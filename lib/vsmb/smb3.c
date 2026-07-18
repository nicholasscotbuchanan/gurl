/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/

/*
 * SMB2/3 handler backed by Samba's libsmbclient. Unlike the native SMBv1
 * handler in lib/smb.c, this speaks the modern SMB2/3 dialects up to
 * SMB 3.1.1, including message signing and AES-GCM encryption, all of which
 * libsmbclient implements. libsmbclient owns its own network transport, so
 * the scheme is registered as PROTOPT_NONETWORK and the whole (blocking)
 * transfer is performed in do_it, mirroring the file:// handler.
 */

#include "curl_setup.h"

#ifdef USE_LIBSMBCLIENT

#include <libsmbclient.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

/* Fallbacks in case the system headers are unavailable; libsmbclient uses
   the same numeric values as POSIX on all supported platforms. */
#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_WRONLY
#define O_WRONLY 1
#endif
#ifndef O_CREAT
#define O_CREAT  0100
#endif
#ifndef O_TRUNC
#define O_TRUNC  01000
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#endif

#include "urldata.h"
#include "vsmb/smb3.h"
#include "transfer.h"
#include "sendf.h"
#include "progress.h"
#include "url.h"
#include "creds.h"
#include "connect.h"
#include "curl_range.h"
#include "curl_trc.h"
#include "curlx/strerr.h"

/* Upper bound for a single SMB read/write staging buffer. */
#define SMB3_MAX_BLOCK (1024 * 1024)

#define CURL_META_SMB3_CONN "meta:proto:smb3:conn"
#define CURL_META_SMB3_EASY "meta:proto:smb3:easy"

/* Per-connection state: owns the libsmbclient context and parsed URL. */
struct smb3_conn {
  SMBCCTX *ctx;             /* libsmbclient context; owns its own socket(s) */
  char *url;               /* smb://host/share/path passed to libsmbclient */
  char *user;              /* username with any DOMAIN/ prefix removed */
  char *domain;            /* workgroup/domain, or NULL */
  char *passwd;            /* password, or NULL */
  /* cached libsmbclient method pointers (valid after init) */
  smbc_open_fn fn_open;
  smbc_read_fn fn_read;
  smbc_write_fn fn_write;
  smbc_close_fn fn_close;
  smbc_stat_fn fn_stat;
  smbc_lseek_fn fn_lseek;
};

/* Per-easy request state. */
struct smb3_request {
  SMBCFILE *fh;            /* open file handle */
  char *buf;               /* staging buffer */
  size_t bufsize;
};

static void smb3_conn_dtor(void *key, size_t klen, void *entry)
{
  struct smb3_conn *sc = entry;
  (void)key;
  (void)klen;
  if(sc) {
    if(sc->ctx)
      smbc_free_context(sc->ctx, 1); /* 1 = shut down active connections */
    curlx_free(sc->url);
    curlx_free(sc->user);
    curlx_free(sc->domain);
    curlx_free(sc->passwd);
    curlx_free(sc);
  }
}

static void smb3_easy_dtor(void *key, size_t klen, void *entry)
{
  struct smb3_request *req = entry;
  (void)key;
  (void)klen;
  if(req) {
    curlx_free(req->buf);
    curlx_free(req);
  }
}

/* libsmbclient calls this to obtain the credentials to authenticate with.
   The connection state pointer is stashed as the context user data. */
static void smb3_auth_cb(SMBCCTX *c,
                         const char *srv, const char *shr,
                         char *wg, int wglen,
                         char *un, int unlen,
                         char *pw, int pwlen)
{
  struct smb3_conn *sc = smbc_getOptionUserData(c);
  (void)srv;
  (void)shr;
  if(!sc)
    return;
  if(sc->domain && wglen > 0)
    curl_msnprintf(wg, (size_t)wglen, "%s", sc->domain);
  if(sc->user && unlen > 0)
    curl_msnprintf(un, (size_t)unlen, "%s", sc->user);
  if(sc->passwd && pwlen > 0)
    curl_msnprintf(pw, (size_t)pwlen, "%s", sc->passwd);
}

/* Split "DOMAIN/user" or "DOMAIN\user" into domain + user (both strdup'd). */
static CURLcode smb3_parse_user(struct smb3_conn *sc, const char *userpwd)
{
  const char *slash = strchr(userpwd, '/');
  if(!slash)
    slash = strchr(userpwd, '\\');

  if(slash) {
    sc->domain = curlx_malloc((size_t)(slash - userpwd) + 1);
    if(!sc->domain)
      return CURLE_OUT_OF_MEMORY;
    memcpy(sc->domain, userpwd, (size_t)(slash - userpwd));
    sc->domain[slash - userpwd] = 0;
    sc->user = curlx_strdup(slash + 1);
  }
  else
    sc->user = curlx_strdup(userpwd);

  if(!sc->user)
    return CURLE_OUT_OF_MEMORY;
  return CURLE_OK;
}

static CURLcode smb3_setup_connection(struct Curl_easy *data,
                                      struct connectdata *conn)
{
  struct smb3_conn *sc;
  const char *user = Curl_creds_user(conn->creds);
  const char *passwd = Curl_creds_passwd(conn->creds);
  CURLcode result;

  sc = curlx_calloc(1, sizeof(*sc));
  if(!sc)
    return CURLE_OUT_OF_MEMORY;

  if(Curl_conn_meta_set(conn, CURL_META_SMB3_CONN, sc, smb3_conn_dtor))
    return CURLE_OUT_OF_MEMORY;

  /* Build the smb://host/share/path URL for libsmbclient. state.up.path
     already begins with '/' and holds "/share/dir/file". */
  sc->url = curl_maprintf("smb://%s%s", conn->origin->hostname,
                          data->state.up.path);
  if(!sc->url)
    return CURLE_OUT_OF_MEMORY;

  if(user && user[0]) {
    result = smb3_parse_user(sc, user);
    if(result)
      return result;
  }
  if(passwd && passwd[0]) {
    sc->passwd = curlx_strdup(passwd);
    if(!sc->passwd)
      return CURLE_OUT_OF_MEMORY;
  }

  sc->ctx = smbc_new_context();
  if(!sc->ctx) {
    failf(data, "SMB: could not create libsmbclient context");
    return CURLE_FAILED_INIT;
  }

  smbc_setOptionUserData(sc->ctx, sc);
  smbc_setFunctionAuthDataWithContext(sc->ctx, smb3_auth_cb);
  /* Honor the port from the URL. libsmbclient's URL parser does not accept a
     port, so it has to be set on the context instead. */
  smbc_setPort(sc->ctx, conn->origin->port);
  /* Restrict to SMB2 .. SMB 3.1.1: never fall back to insecure SMBv1. */
  smbc_setOptionProtocols(sc->ctx, "SMB2", "SMB3_11");
  /* Use SMB3 encryption (AES-GCM/CCM) when the server offers it. */
  smbc_setOptionSmbEncryptionLevel(sc->ctx, SMBC_ENCRYPTLEVEL_REQUEST);
  smbc_setDebug(sc->ctx, 0);

  if(!smbc_init_context(sc->ctx)) {
    failf(data, "SMB: could not initialize libsmbclient context");
    return CURLE_FAILED_INIT;
  }

  /* Cache the method pointers now that the context is initialized. */
  sc->fn_open = smbc_getFunctionOpen(sc->ctx);
  sc->fn_read = smbc_getFunctionRead(sc->ctx);
  sc->fn_write = smbc_getFunctionWrite(sc->ctx);
  sc->fn_close = smbc_getFunctionClose(sc->ctx);
  sc->fn_stat = smbc_getFunctionStat(sc->ctx);
  sc->fn_lseek = smbc_getFunctionLseek(sc->ctx);

  /* libsmbclient owns its transport, so this connection is never reused. */
  connclose(conn, "SMB connections are not reused");
  return CURLE_OK;
}

static CURLcode smb3_connect(struct Curl_easy *data, bool *done)
{
  /* libsmbclient connects lazily on the first open(); nothing to do here. */
  (void)data;
  *done = TRUE;
  return CURLE_OK;
}

static CURLcode smb3_upload(struct Curl_easy *data, struct smb3_conn *sc,
                            struct smb3_request *req)
{
  CURLcode result = CURLE_OK;
  char errbuf[STRERROR_LEN];

  req->fh = sc->fn_open(sc->ctx, sc->url, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(!req->fh) {
    failf(data, "SMB: could not open %s for writing: %s",
          sc->url, curlx_strerror(errno, errbuf, sizeof(errbuf)));
    return CURLE_UPLOAD_FAILED;
  }

  if(data->state.infilesize >= 0)
    Curl_pgrsSetUploadSize(data, data->state.infilesize);

  for(;;) {
    size_t nread = 0;
    bool eos = FALSE;
    const char *p;
    size_t left;

    result = Curl_client_read(data, req->buf, req->bufsize, &nread, &eos);
    if(result)
      break;

    p = req->buf;
    left = nread;
    while(left) {
      ssize_t w = sc->fn_write(sc->ctx, req->fh, p, left);
      if(w < 0) {
        failf(data, "SMB: write failed: %s",
              curlx_strerror(errno, errbuf, sizeof(errbuf)));
        return CURLE_SEND_ERROR;
      }
      p += w;
      left -= (size_t)w;
      Curl_pgrs_upload_inc(data, (size_t)w);
    }

    result = Curl_pgrsUpdate(data);
    if(result)
      break;

    if(eos)
      break;
  }
  return result;
}

static CURLcode smb3_download(struct Curl_easy *data, struct smb3_conn *sc,
                              struct smb3_request *req)
{
  CURLcode result;
  struct stat st;
  curl_off_t filesize = -1;
  curl_off_t offset;
  curl_off_t remaining;
  char errbuf[STRERROR_LEN];

  /* Learn the size up front (satisfies HEAD/size probes and bounds the
     transfer) without downloading any data. */
  if(sc->fn_stat(sc->ctx, sc->url, &st) == 0)
    filesize = (curl_off_t)st.st_size;

  req->fh = sc->fn_open(sc->ctx, sc->url, O_RDONLY, 0);
  if(!req->fh) {
    failf(data, "SMB: could not open %s: %s", sc->url,
          curlx_strerror(errno, errbuf, sizeof(errbuf)));
    return CURLE_REMOTE_FILE_NOT_FOUND;
  }

  /* Honor --range / --continue-at so this handler composes with the tool's
     parallel chunked download (each chunk is a byte range). */
  result = Curl_range(data);
  if(result)
    return result;

  if(data->state.resume_from < 0) {
    /* last N bytes: needs a known size */
    if(filesize < 0) {
      failf(data, "SMB: cannot get the size of %s", sc->url);
      return CURLE_READ_ERROR;
    }
    data->state.resume_from += filesize;
    if(data->state.resume_from < 0)
      data->state.resume_from = 0;
  }
  offset = (data->state.resume_from > 0) ? data->state.resume_from : 0;

  if(offset) {
    if(sc->fn_lseek(sc->ctx, req->fh, (off_t)offset, SEEK_SET) < 0) {
      failf(data, "SMB: could not seek in %s: %s", sc->url,
            curlx_strerror(errno, errbuf, sizeof(errbuf)));
      return CURLE_BAD_DOWNLOAD_RESUME;
    }
  }

  remaining = data->req.maxdownload; /* -1 = until EOF */
  if(remaining < 0 && filesize >= 0)
    remaining = (filesize > offset) ? (filesize - offset) : 0;

  if(remaining >= 0)
    Curl_pgrsSetDownloadSize(data, remaining);

  /* CURLOPT_NOBODY (e.g. the tool's chunk-size probe): stat already gave us
     the size, so finish without transferring any body. */
  if(data->req.no_body)
    return CURLE_OK;

  while(remaining) {
    size_t want = req->bufsize;
    ssize_t n;

    if(remaining > 0 && (curl_off_t)want > remaining)
      want = (size_t)remaining;

    n = sc->fn_read(sc->ctx, req->fh, req->buf, want);
    if(n < 0) {
      failf(data, "SMB: read failed: %s",
            curlx_strerror(errno, errbuf, sizeof(errbuf)));
      return CURLE_RECV_ERROR;
    }
    if(n == 0)
      break; /* EOF */

    result = Curl_client_write(data, CLIENTWRITE_BODY, req->buf, (size_t)n);
    if(result)
      return result;

    if(remaining > 0)
      remaining -= n;

    result = Curl_pgrsUpdate(data);
    if(result)
      return result;
  }

  return Curl_client_write(data, CLIENTWRITE_BODY | CLIENTWRITE_EOS, "", 0);
}

static CURLcode smb3_do(struct Curl_easy *data, bool *done)
{
  struct connectdata *conn = data->conn;
  struct smb3_conn *sc = Curl_conn_meta_get(conn, CURL_META_SMB3_CONN);
  struct smb3_request *req;
  CURLcode result;

  *done = TRUE; /* the whole transfer completes synchronously here */

  if(!sc)
    return CURLE_FAILED_INIT;

  req = curlx_calloc(1, sizeof(*req));
  if(!req)
    return CURLE_OUT_OF_MEMORY;

  req->bufsize = SMB3_MAX_BLOCK;
  req->buf = curlx_malloc(req->bufsize);
  if(!req->buf) {
    curlx_free(req);
    return CURLE_OUT_OF_MEMORY;
  }
  if(Curl_meta_set(data, CURL_META_SMB3_EASY, req, smb3_easy_dtor))
    return CURLE_OUT_OF_MEMORY;

  /* No socket transfer follows; libsmbclient does the I/O below. */
  Curl_xfer_setup_nop(data);

  if(data->state.upload)
    result = smb3_upload(data, sc, req);
  else
    result = smb3_download(data, sc, req);

  return result;
}

static CURLcode smb3_done(struct Curl_easy *data, CURLcode status,
                          bool premature)
{
  struct connectdata *conn = data->conn;
  struct smb3_conn *sc = Curl_conn_meta_get(conn, CURL_META_SMB3_CONN);
  struct smb3_request *req = Curl_meta_get(data, CURL_META_SMB3_EASY);
  (void)premature;

  if(req && sc && req->fh) {
    sc->fn_close(sc->ctx, req->fh);
    req->fh = NULL;
  }
  return status;
}

const struct Curl_protocol Curl_protocol_smb3 = {
  smb3_setup_connection,                /* setup_connection */
  smb3_do,                              /* do_it */
  smb3_done,                            /* done */
  ZERO_NULL,                            /* do_more */
  smb3_connect,                         /* connect_it */
  ZERO_NULL,                            /* connecting */
  ZERO_NULL,                            /* doing */
  ZERO_NULL,                            /* proto_pollset */
  ZERO_NULL,                            /* doing_pollset */
  ZERO_NULL,                            /* domore_pollset */
  ZERO_NULL,                            /* perform_pollset */
  ZERO_NULL,                            /* disconnect */
  ZERO_NULL,                            /* write_resp */
  ZERO_NULL,                            /* write_resp_hd */
  ZERO_NULL,                            /* connection_is_dead */
  ZERO_NULL,                            /* attach connection */
  ZERO_NULL,                            /* follow */
};

#endif /* USE_LIBSMBCLIENT */
