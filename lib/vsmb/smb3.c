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
 * SMB2/SMB3 handler backed by libsmb2. Unlike the native SMBv1 handler in
 * lib/smb.c this speaks the modern dialects up to SMB 3.1.1, including
 * message signing and encryption, and unlike Samba's libsmbclient it is a
 * small portable library that cross-compiles and static-links for Linux,
 * macOS, Windows and FreeBSD.
 *
 * libsmb2 runs its own transport over a socket it owns, so the scheme is
 * registered PROTOPT_NONETWORK and this handler drives libsmb2's async API
 * from curl's multi loop, exposing libsmb2's fd through the pollset exactly
 * as the NFS handler does for libnfs.
 */

#include "curl_setup.h"

#ifdef USE_LIBSMB2

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

/* Fallbacks for the open() flags in case <fcntl.h> is unavailable. libsmb2
   uses the same numeric values as POSIX on all supported platforms. */
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

#include "urldata.h"
#include "vsmb/smb3.h"
#include "transfer.h"
#include "sendf.h"
#include "progress.h"
#include "connect.h"
#include "select.h"
#include "url.h"
#include "creds.h"
#include "curl_range.h"
#include "curl_trc.h"

/* Upper bound for a single SMB2 READ/WRITE. The server advertises a maximum
   which we honor; this only caps our buffer allocation. */
#define SMB3_MAX_BLOCK (1024 * 1024)

#define CURL_META_SMB3_CONN "meta:proto:smb3:conn"
#define CURL_META_SMB3_EASY "meta:proto:smb3:easy"

/* Per-connection state: owns the libsmb2 context and the parsed URL. */
struct smb3_conn {
  struct smb2_context *smb2; /* libsmb2 context; owns its own socket */
  struct smb2_url *url;      /* parsed server / share / path */
  bool connect_done;         /* tree connect callback has fired */
  int connect_status;        /* 0 on success, -errno on failure */
};

/* State machine for a single SMB request. */
typedef enum {
  SMB3_INIT,      /* nothing issued yet */
  SMB3_OPEN,      /* smb2_open_async in flight */
  SMB3_STAT,      /* smb2_fstat_async in flight (download only) */
  SMB3_TRANSFER,  /* pread/pwrite loop */
  SMB3_STOP       /* transfer complete */
} smb3_state;

/* Per-easy request state. */
struct smb3_request {
  struct smb2fh *fh;         /* open file handle */
  char *buf;                 /* read/write staging buffer */
  size_t bufsize;
  curl_off_t offset;         /* current file offset */
  curl_off_t remaining;      /* bytes left to transfer, -1 = until EOF */
  curl_off_t filesize;       /* size reported by fstat (download) */
  struct smb2_stat_64 st;    /* fstat target, written by libsmb2 */
  smb3_state state;
  bool upload;
  bool eos;                  /* client upload reader reached end of stream */
  /* async op bookkeeping - written by the libsmb2 callbacks, read by the
     doing() driver which owns the Curl_easy handle */
  bool op_inflight;
  bool op_done;
  int op_status;             /* bytes moved (>=0) or -errno */
};

static void smb3_conn_dtor(void *key, size_t klen, void *entry)
{
  struct smb3_conn *sc = entry;
  (void)key;
  (void)klen;
  if(sc) {
    if(sc->url)
      smb2_destroy_url(sc->url);
    if(sc->smb2)
      smb2_destroy_context(sc->smb2);
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

/* Give libsmb2 a chance to make progress. The multi loop wakes us when the
   libsmb2 socket is ready (see smb3_pollset); we re-poll with a zero timeout
   to obtain the exact event mask libsmb2 expects and hand it to
   smb2_service(). */
static CURLcode smb3_do_service(struct Curl_easy *data, struct smb3_conn *sc)
{
  struct pollfd pfd;
  t_socket fd = smb2_get_fd(sc->smb2);

  if(fd == (t_socket)-1)
    return CURLE_OK; /* no socket yet, nothing to service */

  pfd.fd = (curl_socket_t)fd;
  pfd.events = (short)smb2_which_events(sc->smb2);
  pfd.revents = 0;

  if(Curl_poll(&pfd, 1, 0) < 0)
    return CURLE_RECV_ERROR;

  if(smb2_service(sc->smb2, pfd.revents) < 0) {
    failf(data, "SMB: %s", smb2_get_error(sc->smb2));
    return CURLE_RECV_ERROR;
  }
  return CURLE_OK;
}

/* ---- libsmb2 async callbacks: record results only ---- */

static void smb3_connect_cb(struct smb2_context *smb2, int status,
                            void *cbdata, void *private_data)
{
  struct smb3_conn *sc = private_data;
  (void)smb2;
  (void)cbdata;
  sc->connect_done = TRUE;
  sc->connect_status = status;
}

static void smb3_open_cb(struct smb2_context *smb2, int status,
                         void *cbdata, void *private_data)
{
  struct smb3_request *req = private_data;
  (void)smb2;
  req->op_done = TRUE;
  req->op_status = status;
  if(!status)
    req->fh = cbdata;
}

static void smb3_generic_cb(struct smb2_context *smb2, int status,
                            void *cbdata, void *private_data)
{
  struct smb3_request *req = private_data;
  (void)smb2;
  (void)cbdata;
  req->op_done = TRUE;
  req->op_status = status; /* bytes moved, or -errno */
}

/* ---- handler callbacks ---- */

static CURLcode smb3_setup_connection(struct Curl_easy *data,
                                      struct connectdata *conn)
{
  struct smb3_conn *sc;
  const char *user = Curl_creds_user(conn->creds);
  const char *passwd = Curl_creds_passwd(conn->creds);
  const char *url = Curl_bufref_ptr(&data->state.url);

  sc = curlx_calloc(1, sizeof(*sc));
  if(!sc)
    return CURLE_OUT_OF_MEMORY;

  sc->smb2 = smb2_init_context();
  if(!sc->smb2) {
    curlx_free(sc);
    return CURLE_OUT_OF_MEMORY;
  }

  /* Split smb://server/share/path into server, share and file. libsmb2 also
     picks up any domain;user:password embedded in the URL. */
  sc->url = smb2_parse_url(sc->smb2, url);
  if(!sc->url) {
    failf(data, "SMB: could not parse URL: %s", smb2_get_error(sc->smb2));
    smb2_destroy_context(sc->smb2);
    curlx_free(sc);
    return CURLE_URL_MALFORMAT;
  }

  if(Curl_conn_meta_set(conn, CURL_META_SMB3_CONN, sc, smb3_conn_dtor))
    return CURLE_OUT_OF_MEMORY;

  /* Credentials from the URL or -u. A domain may be given either as part of
     the username (DOMAIN/user, DOMAIN\user) or in the URL. */
  if(user && user[0]) {
    const char *sep = strchr(user, '/');
    if(!sep)
      sep = strchr(user, '\\');
    if(sep) {
      char *domain = curlx_malloc((size_t)(sep - user) + 1);
      if(!domain)
        return CURLE_OUT_OF_MEMORY;
      memcpy(domain, user, (size_t)(sep - user));
      domain[sep - user] = 0;
      smb2_set_domain(sc->smb2, domain);
      curlx_free(domain);
      smb2_set_user(sc->smb2, sep + 1);
    }
    else
      smb2_set_user(sc->smb2, user);
  }
  else if(sc->url->user)
    smb2_set_user(sc->smb2, sc->url->user);

  if(passwd && passwd[0])
    smb2_set_password(sc->smb2, passwd);

  if(sc->url->domain && sc->url->domain[0])
    smb2_set_domain(sc->smb2, sc->url->domain);

  smb2_set_security_mode(sc->smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);

  /* libsmb2 owns its own transport, so this connection is never pooled. */
  connclose(conn, "SMB connections are not reused");
  return CURLE_OK;
}

static CURLcode smb3_connect(struct Curl_easy *data, bool *done)
{
  struct connectdata *conn = data->conn;
  struct smb3_conn *sc = Curl_conn_meta_get(conn, CURL_META_SMB3_CONN);

  *done = FALSE;
  if(!sc)
    return CURLE_FAILED_INIT;

  if(!sc->url->share || !sc->url->share[0]) {
    failf(data, "SMB: missing share in URL");
    return CURLE_URL_MALFORMAT;
  }

  /* Pass NULL for the user: libsmb2 then uses the username already set on the
     context with smb2_set_user() (smb2_get_user() is not exported by all
     libsmb2 builds, so it must not be relied on here). */
  if(smb2_connect_share_async(sc->smb2, sc->url->server, sc->url->share,
                              NULL, smb3_connect_cb, sc) < 0) {
    failf(data, "SMB: failed to connect to \\\\%s\\%s: %s",
          sc->url->server, sc->url->share, smb2_get_error(sc->smb2));
    return CURLE_COULDNT_CONNECT;
  }
  return CURLE_OK;
}

static CURLcode smb3_connecting(struct Curl_easy *data, bool *done)
{
  struct connectdata *conn = data->conn;
  struct smb3_conn *sc = Curl_conn_meta_get(conn, CURL_META_SMB3_CONN);
  CURLcode result;

  *done = FALSE;
  if(!sc)
    return CURLE_FAILED_INIT;

  result = smb3_do_service(data, sc);
  if(result)
    return result;

  if(sc->connect_done) {
    if(sc->connect_status < 0) {
      failf(data, "SMB: could not connect to \\\\%s\\%s: %s",
            sc->url->server, sc->url->share, smb2_get_error(sc->smb2));
      return CURLE_LOGIN_DENIED;
    }
    infof(data, "SMB: connected to \\\\%s\\%s",
          sc->url->server, sc->url->share);
    *done = TRUE;
  }
  return CURLE_OK;
}

static CURLcode smb3_pollset(struct Curl_easy *data, struct easy_pollset *ps)
{
  struct smb3_conn *sc = Curl_conn_meta_get(data->conn, CURL_META_SMB3_CONN);
  t_socket fd;
  int events;

  if(!sc || !sc->smb2)
    return CURLE_OK;

  fd = smb2_get_fd(sc->smb2);
  if(fd == (t_socket)-1)
    return CURLE_OK;

  events = smb2_which_events(sc->smb2);
  return Curl_pollset_set(data, ps, (curl_socket_t)fd,
                          (events & POLLIN) ? TRUE : FALSE,
                          (events & POLLOUT) ? TRUE : FALSE);
}

static CURLcode smb3_do(struct Curl_easy *data, bool *done)
{
  struct connectdata *conn = data->conn;
  struct smb3_conn *sc = Curl_conn_meta_get(conn, CURL_META_SMB3_CONN);
  struct smb3_request *req;
  uint32_t maxio;

  *done = FALSE;
  if(!sc)
    return CURLE_FAILED_INIT;

  req = curlx_calloc(1, sizeof(*req));
  if(!req)
    return CURLE_OUT_OF_MEMORY;

  req->upload = data->state.upload;
  req->state = SMB3_INIT;
  req->filesize = -1;

  /* Honor --range / --continue-at for downloads so this handler composes with
     the tool's parallel chunked download (each chunk is a byte range). */
  if(!req->upload) {
    CURLcode result = Curl_range(data);
    if(result) {
      curlx_free(req);
      return result;
    }
    req->offset = (data->state.resume_from > 0) ? data->state.resume_from : 0;
    req->remaining = data->req.maxdownload; /* -1 means until EOF */
  }
  else
    req->remaining = data->state.infilesize; /* -1 means unknown */

  maxio = req->upload ? smb2_get_max_write_size(sc->smb2) :
                        smb2_get_max_read_size(sc->smb2);
  if(!maxio || maxio > SMB3_MAX_BLOCK)
    maxio = SMB3_MAX_BLOCK;
  req->bufsize = maxio;
  req->buf = curlx_malloc(req->bufsize);
  if(!req->buf) {
    curlx_free(req);
    return CURLE_OUT_OF_MEMORY;
  }

  if(Curl_meta_set(data, CURL_META_SMB3_EASY, req, smb3_easy_dtor))
    return CURLE_OUT_OF_MEMORY;

  return CURLE_OK;
}

/* Queue the next pread for a download, sized to what remains. */
static CURLcode smb3_read_next(struct Curl_easy *data, struct smb3_conn *sc,
                               struct smb3_request *req)
{
  size_t want = req->bufsize;

  if(req->remaining >= 0 && (curl_off_t)want > req->remaining)
    want = (size_t)req->remaining;

  req->op_done = FALSE;
  req->op_inflight = TRUE;
  if(smb2_pread_async(sc->smb2, req->fh, (uint8_t *)req->buf,
                      (uint32_t)want, (uint64_t)req->offset,
                      smb3_generic_cb, req) < 0) {
    failf(data, "SMB: read failed: %s", smb2_get_error(sc->smb2));
    return CURLE_RECV_ERROR;
  }
  return CURLE_OK;
}

/* Pull the next block from the client and queue a pwrite for an upload. */
static CURLcode smb3_write_next(struct Curl_easy *data, struct smb3_conn *sc,
                                struct smb3_request *req, bool *stop)
{
  size_t nread = 0;
  bool eos = FALSE;
  CURLcode result;

  *stop = FALSE;
  result = Curl_client_read(data, req->buf, req->bufsize, &nread, &eos);
  if(result)
    return result;

  req->eos = eos;
  if(!nread) {
    if(eos)
      *stop = TRUE;
    return CURLE_OK; /* nothing to write this round */
  }

  req->op_done = FALSE;
  req->op_inflight = TRUE;
  if(smb2_pwrite_async(sc->smb2, req->fh, (const uint8_t *)req->buf,
                       (uint32_t)nread, (uint64_t)req->offset,
                       smb3_generic_cb, req) < 0) {
    failf(data, "SMB: write failed: %s", smb2_get_error(sc->smb2));
    return CURLE_SEND_ERROR;
  }
  return CURLE_OK;
}

static CURLcode smb3_transfer_done(struct Curl_easy *data,
                                   struct smb3_request *req, bool *done)
{
  CURLcode result = CURLE_OK;
  if(!req->upload)
    /* flush end-of-stream to the client writer */
    result = Curl_client_write(data, CLIENTWRITE_BODY | CLIENTWRITE_EOS,
                               "", 0);
  req->state = SMB3_STOP;
  *done = TRUE;
  Curl_xfer_setup_nop(data);
  return result;
}

static CURLcode smb3_doing(struct Curl_easy *data, bool *done)
{
  struct connectdata *conn = data->conn;
  struct smb3_conn *sc = Curl_conn_meta_get(conn, CURL_META_SMB3_CONN);
  struct smb3_request *req = Curl_meta_get(data, CURL_META_SMB3_EASY);
  CURLcode result;
  int flags;
  bool stop = FALSE;

  *done = FALSE;
  if(!sc || !req)
    return CURLE_FAILED_INIT;

  result = smb3_do_service(data, sc);
  if(result)
    return result;

  /* Re-entrant state machine: a case either returns (waiting on an async op or
     finished) or advances req->state and breaks to re-evaluate immediately. */
  for(;;) {
    switch(req->state) {
    case SMB3_INIT:
      flags = req->upload ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
      req->op_done = FALSE;
      req->op_inflight = TRUE;
      req->state = SMB3_OPEN;
      if(smb2_open_async(sc->smb2, sc->url->path, flags,
                         smb3_open_cb, req) < 0) {
        failf(data, "SMB: open of %s failed: %s",
              sc->url->path, smb2_get_error(sc->smb2));
        return CURLE_REMOTE_FILE_NOT_FOUND;
      }
      return CURLE_OK;

    case SMB3_OPEN:
      if(!req->op_done)
        return CURLE_OK;
      if(req->op_status < 0) {
        failf(data, "SMB: could not open %s: %s",
              sc->url->path, smb2_get_error(sc->smb2));
        return CURLE_REMOTE_FILE_NOT_FOUND;
      }
      req->op_inflight = FALSE;
      if(req->upload) {
        if(data->state.infilesize >= 0)
          Curl_pgrsSetUploadSize(data, data->state.infilesize);
        req->state = SMB3_TRANSFER;
        break; /* issue the first write */
      }
      /* download: learn the size to bound the transfer and drive progress */
      req->op_done = FALSE;
      req->op_inflight = TRUE;
      req->state = SMB3_STAT;
      if(smb2_fstat_async(sc->smb2, req->fh, &req->st,
                          smb3_generic_cb, req) < 0) {
        failf(data, "SMB: fstat failed: %s", smb2_get_error(sc->smb2));
        return CURLE_RECV_ERROR;
      }
      return CURLE_OK;

    case SMB3_STAT: /* download only */
      if(!req->op_done)
        return CURLE_OK;
      if(req->op_status < 0) {
        failf(data, "SMB: fstat failed: %s", smb2_get_error(sc->smb2));
        return CURLE_RECV_ERROR;
      }
      req->op_inflight = FALSE;
      req->filesize = (curl_off_t)req->st.smb2_size;

      /* a negative resume offset counts back from the end of the file */
      if(data->state.resume_from < 0) {
        data->state.resume_from += req->filesize;
        if(data->state.resume_from < 0)
          data->state.resume_from = 0;
        req->offset = data->state.resume_from;
      }

      if(req->remaining < 0)
        /* whole file from the current offset to EOF */
        req->remaining = (req->filesize > req->offset) ?
                         (req->filesize - req->offset) : 0;
      Curl_pgrsSetDownloadSize(data, req->remaining);
      if(!req->remaining || data->req.no_body)
        /* CURLOPT_NOBODY (e.g. the tool's chunk-size probe): the fstat gave us
           the size, so finish without transferring any data */
        return smb3_transfer_done(data, req, done);
      req->state = SMB3_TRANSFER;
      return smb3_read_next(data, sc, req);

    case SMB3_TRANSFER:
      if(req->upload) {
        if(req->op_inflight) {
          if(!req->op_done)
            return CURLE_OK;
          req->op_inflight = FALSE;
          if(req->op_status < 0) {
            failf(data, "SMB: write failed: %s", smb2_get_error(sc->smb2));
            return CURLE_SEND_ERROR;
          }
          req->offset += req->op_status;
          Curl_pgrs_upload_inc(data, (size_t)req->op_status);
          if(req->eos)
            return smb3_transfer_done(data, req, done);
        }
        result = smb3_write_next(data, sc, req, &stop);
        if(result)
          return result;
        if(stop)
          return smb3_transfer_done(data, req, done);
        return CURLE_OK;
      }
      else {
        curl_off_t n;
        if(!req->op_done)
          return CURLE_OK;
        req->op_inflight = FALSE;
        if(req->op_status < 0) {
          failf(data, "SMB: read failed: %s", smb2_get_error(sc->smb2));
          return CURLE_RECV_ERROR;
        }
        n = req->op_status;
        if(n > 0) {
          result = Curl_client_write(data, CLIENTWRITE_BODY, req->buf,
                                     (size_t)n);
          if(result)
            return result;
          req->offset += n;
          if(req->remaining > 0)
            req->remaining -= n;
        }
        if(!n || !req->remaining)
          return smb3_transfer_done(data, req, done);
        return smb3_read_next(data, sc, req);
      }

    case SMB3_STOP:
      *done = TRUE;
      return CURLE_OK;
    }
  }
}

static CURLcode smb3_done(struct Curl_easy *data, CURLcode status,
                          bool premature)
{
  struct connectdata *conn = data->conn;
  struct smb3_conn *sc = Curl_conn_meta_get(conn, CURL_META_SMB3_CONN);
  struct smb3_request *req = Curl_meta_get(data, CURL_META_SMB3_EASY);
  (void)premature;

  if(req && sc && req->fh) {
    smb2_close(sc->smb2, req->fh);
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
  smb3_connecting,                      /* connecting */
  smb3_doing,                           /* doing */
  smb3_pollset,                         /* proto_pollset */
  smb3_pollset,                         /* doing_pollset */
  ZERO_NULL,                            /* domore_pollset */
  ZERO_NULL,                            /* perform_pollset */
  ZERO_NULL,                            /* disconnect */
  ZERO_NULL,                            /* write_resp */
  ZERO_NULL,                            /* write_resp_hd */
  ZERO_NULL,                            /* connection_is_dead */
  ZERO_NULL,                            /* attach connection */
  ZERO_NULL,                            /* follow */
};

#endif /* USE_LIBSMB2 */
