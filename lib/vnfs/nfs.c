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

#include "curl_setup.h"

#ifdef USE_NFS

#include <nfsc/libnfs.h>

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "urldata.h"
#include "vnfs/nfs.h"
#include "transfer.h"
#include "sendf.h"
#include "progress.h"
#include "connect.h"
#include "select.h"
#include "url.h"
#include "curl_range.h"
#include "curl_trc.h"

/* Fallbacks for the open() flags in case <fcntl.h> is unavailable. libnfs
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

/* Upper bound for a single NFS READ/WRITE RPC. The server may advertise a
   smaller maximum which we honor; this only caps our buffer allocation. */
#define NFS_MAX_BLOCK (1024 * 1024)

#define CURL_META_NFS_CONN "meta:proto:nfs:conn"
#define CURL_META_NFS_EASY "meta:proto:nfs:easy"

/* Per-connection state: owns the libnfs context and the parsed URL. */
struct nfs_conn {
  struct nfs_context *nfs;   /* libnfs context; owns its own socket(s) */
  struct nfs_url *url;       /* parsed server / export / file */
  bool mount_done;           /* mount callback has fired */
  int mount_status;          /* 0 on success, -errno on failure */
};

/* State machine for a single NFS request. */
typedef enum {
  NFS_INIT,      /* nothing issued yet */
  NFS_OPEN,      /* nfs_open_async in flight */
  NFS_STAT,      /* nfs_fstat64_async in flight (download only) */
  NFS_TRANSFER,  /* pread/pwrite loop */
  NFS_STOP       /* transfer complete */
} nfs_state;

/* Per-easy request state. */
struct nfs_request {
  struct nfsfh *fh;          /* open file handle */
  char *buf;                 /* read/write staging buffer */
  size_t bufsize;            /* size of buf */
  curl_off_t offset;         /* current file offset */
  curl_off_t remaining;      /* bytes left to transfer, -1 = until EOF */
  curl_off_t filesize;       /* size reported by fstat (download) */
  nfs_state state;
  bool upload;
  bool eos;                  /* client upload reader reached end of stream */
  /* async op bookkeeping - written by the libnfs callbacks, read by the
     doing() driver which owns the Curl_easy handle */
  bool op_inflight;          /* an async op is queued and not yet completed */
  bool op_done;              /* the in-flight op has completed */
  int op_status;             /* callback status: bytes moved (>=0) or -errno */
};

static void nfs_conn_dtor(void *key, size_t klen, void *entry)
{
  struct nfs_conn *nfsc = entry;
  (void)key;
  (void)klen;
  if(nfsc) {
    if(nfsc->url)
      nfs_destroy_url(nfsc->url);
    if(nfsc->nfs)
      nfs_destroy_context(nfsc->nfs);
    curlx_free(nfsc);
  }
}

static void nfs_easy_dtor(void *key, size_t klen, void *entry)
{
  struct nfs_request *req = entry;
  (void)key;
  (void)klen;
  if(req) {
    curlx_free(req->buf);
    curlx_free(req);
  }
}

/* Give libnfs a chance to make progress. The multi loop wakes us when the
   libnfs socket is ready (see nfs_pollset); we re-poll with a zero timeout to
   obtain the exact event mask libnfs expects and then hand it to
   nfs_service(). */
static CURLcode nfs_do_service(struct Curl_easy *data, struct nfs_conn *nfsc)
{
  struct pollfd pfd;
  int fd = nfs_get_fd(nfsc->nfs);

  if(fd < 0)
    return CURLE_OK; /* no socket yet, nothing to service */

  pfd.fd = (curl_socket_t)fd;
  pfd.events = (short)nfs_which_events(nfsc->nfs);
  pfd.revents = 0;

  if(Curl_poll(&pfd, 1, 0) < 0)
    return CURLE_RECV_ERROR;

  if(nfs_service(nfsc->nfs, pfd.revents) < 0) {
    failf(data, "NFS: %s", nfs_get_error(nfsc->nfs));
    return CURLE_RECV_ERROR;
  }
  return CURLE_OK;
}

/* ---- libnfs async callbacks: record results only ---- */

static void nfs_mount_cb(int status, struct nfs_context *nfs,
                         void *data, void *private_data)
{
  struct nfs_conn *nfsc = private_data;
  (void)nfs;
  (void)data;
  nfsc->mount_done = TRUE;
  nfsc->mount_status = status;
}

static void nfs_open_cb(int status, struct nfs_context *nfs,
                        void *data, void *private_data)
{
  struct nfs_request *req = private_data;
  (void)nfs;
  req->op_done = TRUE;
  req->op_status = status;
  if(status == 0)
    req->fh = data;
}

static void nfs_stat_cb(int status, struct nfs_context *nfs,
                        void *data, void *private_data)
{
  struct nfs_request *req = private_data;
  (void)nfs;
  req->op_done = TRUE;
  req->op_status = status;
  if(status == 0) {
    struct nfs_stat_64 *st = data;
    req->filesize = (curl_off_t)st->nfs_size;
  }
}

static void nfs_rw_cb(int status, struct nfs_context *nfs,
                      void *data, void *private_data)
{
  struct nfs_request *req = private_data;
  (void)nfs;
  (void)data; /* for pread libnfs filled req->buf directly */
  req->op_done = TRUE;
  req->op_status = status; /* bytes moved, or -errno */
}

/* ---- handler callbacks ---- */

static CURLcode nfs_setup_connection(struct Curl_easy *data,
                                     struct connectdata *conn)
{
  struct nfs_conn *nfsc;
  const char *url = Curl_bufref_ptr(&data->state.url);

  nfsc = curlx_calloc(1, sizeof(*nfsc));
  if(!nfsc)
    return CURLE_OUT_OF_MEMORY;

  nfsc->nfs = nfs_init_context();
  if(!nfsc->nfs) {
    curlx_free(nfsc);
    return CURLE_OUT_OF_MEMORY;
  }

  /* Split nfs://server/export/path into server, export and file. */
  nfsc->url = nfs_parse_url_full(nfsc->nfs, url);
  if(!nfsc->url) {
    failf(data, "NFS: could not parse URL: %s", nfs_get_error(nfsc->nfs));
    nfs_destroy_context(nfsc->nfs);
    curlx_free(nfsc);
    return CURLE_URL_MALFORMAT;
  }

  if(Curl_conn_meta_set(conn, CURL_META_NFS_CONN, nfsc, nfs_conn_dtor))
    return CURLE_OUT_OF_MEMORY;

  /* libnfs runs its own RPC transport (portmapper/mount/nfs) over sockets it
     owns, so this connection is never pooled or reused. */
  connclose(conn, "NFS connections are not reused");
  return CURLE_OK;
}

static CURLcode nfs_connect(struct Curl_easy *data, bool *done)
{
  struct connectdata *conn = data->conn;
  struct nfs_conn *nfsc = Curl_conn_meta_get(conn, CURL_META_NFS_CONN);

  *done = FALSE;
  if(!nfsc)
    return CURLE_FAILED_INIT;

  if(nfs_mount_async(nfsc->nfs, nfsc->url->server, nfsc->url->path,
                     nfs_mount_cb, nfsc) < 0) {
    failf(data, "NFS: mount failed: %s", nfs_get_error(nfsc->nfs));
    return CURLE_COULDNT_CONNECT;
  }
  return CURLE_OK;
}

static CURLcode nfs_connecting(struct Curl_easy *data, bool *done)
{
  struct connectdata *conn = data->conn;
  struct nfs_conn *nfsc = Curl_conn_meta_get(conn, CURL_META_NFS_CONN);
  CURLcode result;

  *done = FALSE;
  if(!nfsc)
    return CURLE_FAILED_INIT;

  result = nfs_do_service(data, nfsc);
  if(result)
    return result;

  if(nfsc->mount_done) {
    if(nfsc->mount_status < 0) {
      failf(data, "NFS: mount of %s:%s failed: %s",
            nfsc->url->server, nfsc->url->path, nfs_get_error(nfsc->nfs));
      return CURLE_COULDNT_CONNECT;
    }
    infof(data, "NFS: mounted %s:%s", nfsc->url->server, nfsc->url->path);
    *done = TRUE;
  }
  return CURLE_OK;
}

static CURLcode nfs_pollset(struct Curl_easy *data, struct easy_pollset *ps)
{
  struct nfs_conn *nfsc = Curl_conn_meta_get(data->conn, CURL_META_NFS_CONN);
  int fd, events;

  if(!nfsc || !nfsc->nfs)
    return CURLE_OK;

  fd = nfs_get_fd(nfsc->nfs);
  if(fd < 0)
    return CURLE_OK;

  events = nfs_which_events(nfsc->nfs);
  return Curl_pollset_set(data, ps, (curl_socket_t)fd,
                          (events & POLLIN) ? TRUE : FALSE,
                          (events & POLLOUT) ? TRUE : FALSE);
}

static CURLcode nfs_do(struct Curl_easy *data, bool *done)
{
  struct connectdata *conn = data->conn;
  struct nfs_conn *nfsc = Curl_conn_meta_get(conn, CURL_META_NFS_CONN);
  struct nfs_request *req;
  size_t maxio;

  *done = FALSE;
  if(!nfsc)
    return CURLE_FAILED_INIT;

  req = curlx_calloc(1, sizeof(*req));
  if(!req)
    return CURLE_OUT_OF_MEMORY;

  req->upload = data->state.upload;
  req->state = NFS_INIT;
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
  else {
    req->remaining = data->state.infilesize; /* -1 means unknown */
  }

  maxio = req->upload ? nfs_get_writemax(nfsc->nfs) :
                        nfs_get_readmax(nfsc->nfs);
  if(!maxio || maxio > NFS_MAX_BLOCK)
    maxio = NFS_MAX_BLOCK;
  req->bufsize = maxio;
  req->buf = curlx_malloc(req->bufsize);
  if(!req->buf) {
    curlx_free(req);
    return CURLE_OUT_OF_MEMORY;
  }

  if(Curl_meta_set(data, CURL_META_NFS_EASY, req, nfs_easy_dtor))
    return CURLE_OUT_OF_MEMORY;

  return CURLE_OK;
}

/* Queue the next pread for a download, sized to what remains. */
static CURLcode nfs_read_next(struct Curl_easy *data, struct nfs_conn *nfsc,
                              struct nfs_request *req)
{
  size_t want = req->bufsize;

  if(req->remaining >= 0 && (curl_off_t)want > req->remaining)
    want = (size_t)req->remaining;

  req->op_done = FALSE;
  req->op_inflight = TRUE;
  if(nfs_pread_async(nfsc->nfs, req->fh, req->buf, want,
                     (uint64_t)req->offset, nfs_rw_cb, req) < 0) {
    failf(data, "NFS: read failed: %s", nfs_get_error(nfsc->nfs));
    return CURLE_RECV_ERROR;
  }
  return CURLE_OK;
}

/* Pull the next block from the client and queue a pwrite for an upload. */
static CURLcode nfs_write_next(struct Curl_easy *data, struct nfs_conn *nfsc,
                               struct nfs_request *req, bool *stop)
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
  if(nfs_pwrite_async(nfsc->nfs, req->fh, req->buf, nread,
                      (uint64_t)req->offset, nfs_rw_cb, req) < 0) {
    failf(data, "NFS: write failed: %s", nfs_get_error(nfsc->nfs));
    return CURLE_SEND_ERROR;
  }
  return CURLE_OK;
}

static CURLcode nfs_transfer_done(struct Curl_easy *data,
                                  struct nfs_request *req, bool *done)
{
  CURLcode result = CURLE_OK;
  if(!req->upload)
    /* flush end-of-stream to the client writer */
    result = Curl_client_write(data, CLIENTWRITE_BODY | CLIENTWRITE_EOS,
                               "", 0);
  req->state = NFS_STOP;
  *done = TRUE;
  Curl_xfer_setup_nop(data);
  return result;
}

static CURLcode nfs_doing(struct Curl_easy *data, bool *done)
{
  struct connectdata *conn = data->conn;
  struct nfs_conn *nfsc = Curl_conn_meta_get(conn, CURL_META_NFS_CONN);
  struct nfs_request *req = Curl_meta_get(data, CURL_META_NFS_EASY);
  CURLcode result;
  int flags;
  bool stop = FALSE;

  *done = FALSE;
  if(!nfsc || !req)
    return CURLE_FAILED_INIT;

  result = nfs_do_service(data, nfsc);
  if(result)
    return result;

  /* Re-entrant state machine: a case either returns (waiting on an async op or
     finished) or advances req->state and breaks to re-evaluate immediately. */
  for(;;) {
    switch(req->state) {
    case NFS_INIT:
      flags = req->upload ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
      req->op_done = FALSE;
      req->op_inflight = TRUE;
      req->state = NFS_OPEN;
      if(nfs_open_async(nfsc->nfs, nfsc->url->file, flags,
                        nfs_open_cb, req) < 0) {
        failf(data, "NFS: open of %s failed: %s",
              nfsc->url->file, nfs_get_error(nfsc->nfs));
        return CURLE_REMOTE_FILE_NOT_FOUND;
      }
      return CURLE_OK;

    case NFS_OPEN:
      if(!req->op_done)
        return CURLE_OK;
      if(req->op_status < 0) {
        failf(data, "NFS: could not open %s: %s",
              nfsc->url->file, nfs_get_error(nfsc->nfs));
        return CURLE_REMOTE_FILE_NOT_FOUND;
      }
      req->op_inflight = FALSE;
      if(req->upload) {
        req->state = NFS_TRANSFER;
        break; /* issue the first write */
      }
      /* download: learn the size to bound the transfer and drive progress */
      req->op_done = FALSE;
      req->op_inflight = TRUE;
      req->state = NFS_STAT;
      if(nfs_fstat64_async(nfsc->nfs, req->fh, nfs_stat_cb, req) < 0) {
        failf(data, "NFS: fstat failed: %s", nfs_get_error(nfsc->nfs));
        return CURLE_RECV_ERROR;
      }
      return CURLE_OK;

    case NFS_STAT: /* download only */
      if(!req->op_done)
        return CURLE_OK;
      if(req->op_status < 0) {
        failf(data, "NFS: fstat failed: %s", nfs_get_error(nfsc->nfs));
        return CURLE_RECV_ERROR;
      }
      if(req->remaining < 0)
        /* whole file from the current offset to EOF */
        req->remaining = (req->filesize > req->offset) ?
                         (req->filesize - req->offset) : 0;
      Curl_pgrsSetDownloadSize(data, req->remaining);
      if(req->remaining == 0 || data->req.no_body)
        /* CURLOPT_NOBODY (e.g. the tool's chunk-size probe): the fstat gave us
           the size, so finish without transferring any data */
        return nfs_transfer_done(data, req, done);
      req->state = NFS_TRANSFER;
      result = nfs_read_next(data, nfsc, req);
      if(result)
        return result;
      return CURLE_OK;

    case NFS_TRANSFER:
      if(req->upload) {
        if(req->op_inflight) {
          if(!req->op_done)
            return CURLE_OK;
          req->op_inflight = FALSE;
          if(req->op_status < 0) {
            failf(data, "NFS: write failed: %s", nfs_get_error(nfsc->nfs));
            return CURLE_SEND_ERROR;
          }
          req->offset += req->op_status;
          if(req->eos)
            return nfs_transfer_done(data, req, done);
        }
        result = nfs_write_next(data, nfsc, req, &stop);
        if(result)
          return result;
        if(stop)
          return nfs_transfer_done(data, req, done);
        return CURLE_OK;
      }
      else {
        curl_off_t n;
        if(!req->op_done)
          return CURLE_OK;
        req->op_inflight = FALSE;
        if(req->op_status < 0) {
          failf(data, "NFS: read failed: %s", nfs_get_error(nfsc->nfs));
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
        if(n == 0 || req->remaining == 0)
          return nfs_transfer_done(data, req, done);
        return nfs_read_next(data, nfsc, req);
      }

    case NFS_STOP:
      *done = TRUE;
      return CURLE_OK;
    }
  }
}

static CURLcode nfs_done(struct Curl_easy *data, CURLcode status,
                         bool premature)
{
  struct connectdata *conn = data->conn;
  struct nfs_conn *nfsc = Curl_conn_meta_get(conn, CURL_META_NFS_CONN);
  struct nfs_request *req = Curl_meta_get(data, CURL_META_NFS_EASY);
  (void)premature;

  if(req && nfsc && req->fh) {
    nfs_close(nfsc->nfs, req->fh);
    req->fh = NULL;
  }
  return status;
}

const struct Curl_protocol Curl_protocol_nfs = {
  nfs_setup_connection,                 /* setup_connection */
  nfs_do,                               /* do_it */
  nfs_done,                             /* done */
  ZERO_NULL,                            /* do_more */
  nfs_connect,                          /* connect_it */
  nfs_connecting,                       /* connecting */
  nfs_doing,                            /* doing */
  nfs_pollset,                          /* proto_pollset */
  nfs_pollset,                          /* doing_pollset */
  ZERO_NULL,                            /* domore_pollset */
  ZERO_NULL,                            /* perform_pollset */
  ZERO_NULL,                            /* disconnect */
  ZERO_NULL,                            /* write_resp */
  ZERO_NULL,                            /* write_resp_hd */
  ZERO_NULL,                            /* connection_is_dead */
  ZERO_NULL,                            /* attach connection */
  ZERO_NULL,                            /* follow */
};

#endif /* USE_NFS */
