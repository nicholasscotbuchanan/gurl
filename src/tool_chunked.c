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
#include "tool_setup.h"

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef _WIN32
#include <io.h>
#endif

#include "tool_cfgable.h"
#include "tool_operate.h"
#include "tool_chunked.h"
#include "tool_msgs.h"

/* Real OS threads: each chunk stream is a worker thread running a blocking
   curl_easy_perform, so the concurrent transfers progress on separate cores
   (their own TLS/decrypt/write work) instead of being multiplexed onto one
   thread. HAVE_THREADS_POSIX comes from curl_config.h. */
#if defined(HAVE_THREADS_POSIX)
#include <pthread.h>
#define CHUNK_THREADS 1
typedef pthread_t       chunk_thread_t;
typedef pthread_mutex_t chunk_mutex_t;
#define chunk_mutex_init(m)    pthread_mutex_init(m, NULL)
#define chunk_mutex_lock(m)    pthread_mutex_lock(m)
#define chunk_mutex_unlock(m)  pthread_mutex_unlock(m)
#define chunk_mutex_destroy(m) pthread_mutex_destroy(m)
#elif defined(_WIN32)
#include <process.h>
#define CHUNK_THREADS 1
typedef uintptr_t        chunk_thread_t;
typedef CRITICAL_SECTION chunk_mutex_t;
#define chunk_mutex_init(m)    InitializeCriticalSection(m)
#define chunk_mutex_lock(m)    EnterCriticalSection(m)
#define chunk_mutex_unlock(m)  LeaveCriticalSection(m)
#define chunk_mutex_destroy(m) DeleteCriticalSection(m)
#endif

#ifdef _WIN32
#define CHUNK_OPENMODE (_S_IREAD | _S_IWRITE)
#else
#define CHUNK_OPENMODE \
  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
#endif

/* Non-configurable policy: a download larger than CHUNK_THRESHOLD is split into
   byte-range pieces fetched with at most CHUNK_STREAMS running at any instant (a
   sliding window over a shared work queue).

   The piece size is not fixed. A fixed 1 GiB chunk wastes the pool on anything
   under 8 GiB (a 4 GiB file would only ever fill 4 of 8 streams) and leaves a
   fat tail: the final round hands whole 1 GiB pieces to a couple of stragglers
   while the rest of the pool sits idle. Instead the size is chosen from the
   total so the file always splits into roughly CHUNK_TARGET pieces -- enough
   that every stream stays fed and the tail is never larger than one small piece
   -- clamped so each request is still big enough to saturate the link (CHUNK_MIN)
   yet bounded in memory and tail cost (CHUNK_MAX). Because a worker reuses its
   keep-alive connection across the pieces it pulls, the finer split costs a new
   request, not a new connection. */
#define CHUNK_STREAMS   8                       /* concurrent chunk streams */
#define CHUNK_THRESHOLD ((curl_off_t)1 << 30)   /* only chunk files over 1 GiB */
#define CHUNK_MIN       ((curl_off_t)64 << 20)  /* 64 MiB: floor per request */
#define CHUNK_MAX       ((curl_off_t)1 << 30)   /* 1 GiB: ceiling per request */
#define CHUNK_TARGET    (CHUNK_STREAMS * 8)     /* aim for ~this many pieces */
#define CHUNK_BUFSIZE   (1024 * 1024)           /* receive buffer per worker */
#define CHUNK_QUEUE_MAX ((curl_off_t)256 << 20) /* max bytes queued for writer */

/* Adaptive selection: before committing to parallel chunks, download this much
   on a single stream and time it. Splitting a download into concurrent byte
   ranges only helps when the network is the bottleneck; against a source fast
   enough to saturate local disk, one *sequential* stream is faster than the
   parallel writer's *scattered* writes, so parallelizing would slow it down.
   If the probe stream sustains at least CHUNK_FAST_MIBPS, keep it single. */
#define CHUNK_PROBE      ((curl_off_t)64 << 20)  /* single-stream probe segment */
#define CHUNK_FAST_MIBPS 200.0                   /* one-stream speed that beats
                                                    the parallel scattered writer */

/* Pick the per-piece size for a download of 'total' bytes: about CHUNK_TARGET
   pieces, clamped to [CHUNK_MIN, CHUNK_MAX]. */
static curl_off_t chunk_size_for(curl_off_t total)
{
  curl_off_t c = total / CHUNK_TARGET;
  if(c < CHUNK_MIN)
    c = CHUNK_MIN;
  else if(c > CHUNK_MAX)
    c = CHUNK_MAX;
  return c;
}

/* ---- learn the size up front, without downloading the body ------------- */

/* Run one cheap metadata request on per->curl -- HEAD for HTTP(S), SIZE for
   FTP, a GETATTR-backed size probe for NFS -- so the download size and range
   support are known before a single body byte is transferred. Returns TRUE and
   sets *total when the file is larger than CHUNK_SIZE and the protocol honors
   byte ranges, meaning the caller should fetch it as concurrent chunks. A
   1 GiB-or-smaller file (or a failed/unsupported probe) returns FALSE, so the
   caller performs an ordinary single transfer. Leaves per->curl reset to a
   normal GET so the caller can reuse it. */
bool tool_chunk_probe(struct per_transfer *per, curl_off_t *total)
{
  curl_off_t clen = -1;
  long code = 0;
  const char *scheme = NULL;
  CURLcode res;

  /* NOBODY turns this into HEAD/SIZE/GETATTR: metadata only, no transfer. Arm
     chunk_watch so the header callback records Accept-Ranges for HTTP. Silence
     the progress meter for the probe: otherwise it paints a "0 / <size>" line
     that then sits frozen for the whole chunked download and looks like a
     hang, since the chunk transfers run with their own progress disabled. */
  per->chunk_accept_ranges = FALSE;
  per->chunk_watch = TRUE;
  (void)curl_easy_setopt(per->curl, CURLOPT_NOBODY, 1L);
  (void)curl_easy_setopt(per->curl, CURLOPT_NOPROGRESS, 1L);
  res = curl_easy_perform(per->curl);
  per->chunk_watch = FALSE;
  /* back to a normal bodyful GET for the chunk transfers that reuse this handle
     (and the duphandles made from it) */
  (void)curl_easy_setopt(per->curl, CURLOPT_NOBODY, 0L);
  (void)curl_easy_setopt(per->curl, CURLOPT_HTTPGET, 1L);
  /* restore the meter to what the config asked for, so a sub-1-GiB file that
     falls back to a normal single GET still shows its progress */
  (void)curl_easy_setopt(per->curl, CURLOPT_NOPROGRESS,
                         (global->noprogress || global->silent) ? 1L : 0L);

  if(res)
    return FALSE;             /* probe failed: fall back to a normal download */

  curl_easy_getinfo(per->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &clen);
  curl_easy_getinfo(per->curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_getinfo(per->curl, CURLINFO_SCHEME, &scheme);

  if(clen <= CHUNK_THRESHOLD) /* 1 GiB or smaller (also covers unknown -1) */
    return FALSE;
  if(scheme &&
     (curl_strequal(scheme, "HTTP") || curl_strequal(scheme, "HTTPS"))) {
    /* only trust an explicit Accept-Ranges: bytes on a 2xx HEAD */
    if(!per->chunk_accept_ranges || (code / 100 != 2))
      return FALSE;
  }
  else if(!(scheme &&
            (curl_strequal(scheme, "FTP") || curl_strequal(scheme, "FTPS") ||
             curl_strequal(scheme, "NFS"))))
    /* FTP REST / NFS pread honor a byte offset when the size is known; other
       protocols (e.g. SMB) report a size but ignore ranges, so exclude them
       to avoid corrupting the output. */
    return FALSE;
  *total = clen;
  return TRUE;
}

/* ---- offset writes ----------------------------------------------------- */

static curl_off_t chunk_raw_write(int fd, const char *buf, size_t len)
{
#ifdef _WIN32
  return (curl_off_t)_write(fd, buf, (unsigned int)len);
#else
  return (curl_off_t)write(fd, buf, len);
#endif
}

/* Create the output file as a fresh, empty file. Deliberately does NOT grow it
   to its final size: on APFS, pre-sizing (ftruncate) turns every subsequent
   write into a copy-on-write overwrite of an existing block, ~4x slower than
   letting the file grow (measured 36s vs 8s for a 10 GiB sequential write). The
   chunks together cover every byte of [0,total), and the last byte is always
   written, so the file still ends at exactly 'total' bytes without pre-sizing. */
static bool chunk_presize(const char *outfile, curl_off_t total)
{
  int fd = curlx_open(outfile, O_CREAT | O_TRUNC | O_WRONLY | CURL_O_BINARY,
                      CHUNK_OPENMODE);
  (void)total;
  if(fd < 0)
    return FALSE;
  curlx_close(fd);
  return TRUE;
}

/* ---- pool of worker threads, one chunk each ---------------------------- */

/* ---- single writer thread ---------------------------------------------- */

/* A unit of work handed from a network worker to the writer: 'len' body bytes
   (owned, freed by the writer after the write) destined for absolute file
   offset 'offset'. */
struct chunk_wjob {
  struct chunk_wjob *next;
  curl_off_t offset;
  char *buf;
  size_t len;
};

/* The sole owner of the output descriptor. Network workers are producers that
   copy each received buffer into a job and enqueue it; this one thread is the
   only consumer and the only thread that ever writes the file.

   On macOS/APFS, N threads writing one shared inode serialize on the kernel's
   per-file write lock -- measured ~5x slower than a single writer for a 10 GiB
   file (43s vs 9s, 8 writers vs 1). Funnelling every write through one thread
   removes that contention while the network keeps running on all CHUNK_STREAMS
   worker threads. */
struct chunk_writer {
  chunk_mutex_t lock;
  struct chunk_wjob *head, *tail; /* FIFO of pending writes */
  curl_off_t queued;              /* bytes queued (for backpressure) */
  int fd;
  bool done;                      /* set once every producer has finished */
  CURLcode result;                /* first write error, if any */
  chunk_thread_t thread;
  bool threaded;
};

/* Chunk indices are handed out from a single shared counter guarded by 'lock'.
   Each worker grabs the next index, downloads that byte range with a blocking
   transfer, and comes back for more until the counter is exhausted or another
   worker has recorded an error. */
struct chunk_pool {
  chunk_mutex_t lock;
  unsigned int next;         /* next chunk index to hand out */
  unsigned int n;            /* total number of chunks */
  curl_off_t base;           /* file offset where chunk 0 starts */
  curl_off_t chunk_size;     /* bytes per chunk (last one may be shorter) */
  curl_off_t total;          /* file size in bytes */
  unsigned int finished;     /* worker loops that have exited */
  CURLcode result;           /* first error any worker hit (guarded by lock) */
  struct chunk_writer *writer; /* single writer, or NULL for inline direct I/O */
};

/* One worker thread: its own easy handle. With a writer it hands received
   buffers off; without one (the no-threads fallback) it writes directly through
   its own descriptor 'fd'. */
struct chunk_worker {
  CURL *easy;
  int fd;                 /* only used in the writer-less fallback path */
  struct chunk_pool *pool;
  chunk_thread_t thread;
  curl_off_t cur_offset;  /* absolute file offset of the next received byte */
  curl_off_t written;     /* bytes this worker has received, for the progress
                             line -- private to this worker's thread so the hot
                             path needs no lock; the driver sums the workers'
                             counters and a slightly stale read is fine */
  bool threaded;          /* TRUE if a thread was actually spawned for it */
};

/* Append fully at 'offset' on the single writer descriptor. Only the writer
   thread calls this, so a plain lseek+write (no pwrite) is safe. */
static CURLcode chunk_write_at(int fd, const char *buf, size_t len,
                               curl_off_t offset)
{
  size_t done = 0;
  if(curl_lseek(fd, offset, SEEK_SET) == LSEEK_ERROR)
    return CURLE_WRITE_ERROR;
  while(done < len) {
    curl_off_t rc = chunk_raw_write(fd, buf + done, len - done);
    if(rc < 0)
      return CURLE_WRITE_ERROR;
    done += (size_t)rc;
  }
  return CURLE_OK;
}

/* ---- single-stream segments (adaptive probe + fast-path remainder) ------ */

/* Sequential write callback: append to the descriptor the caller positioned.
   Used by the single-stream download of a contiguous range. */
static size_t chunk_seq_write_cb(char *buffer, size_t sz, size_t nitems,
                                 void *userdata)
{
  int *fdp = userdata;
  size_t len = sz * nitems, done = 0;
  while(done < len) {
    curl_off_t rc = chunk_raw_write(*fdp, buffer + done, len - done);
    if(rc < 0)
      return 0;
    done += (size_t)rc;
  }
  return len;
}

/* Download the byte range [start,end] on 'easy' straight to 'fd', writing
   sequentially from 'start'. Used for the throughput probe and, when one stream
   already saturates the disk, for the whole remainder. */
static CURLcode chunk_stream_range(CURL *easy, int fd,
                                   curl_off_t start, curl_off_t end)
{
  char range[80];
  if(curl_lseek(fd, start, SEEK_SET) == LSEEK_ERROR)
    return CURLE_WRITE_ERROR;
  curl_msnprintf(range, sizeof(range),
                 "%" CURL_FORMAT_CURL_OFF_T "-%" CURL_FORMAT_CURL_OFF_T,
                 start, end);
  (void)curl_easy_setopt(easy, CURLOPT_RANGE, range);
  (void)curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, chunk_seq_write_cb);
  (void)curl_easy_setopt(easy, CURLOPT_WRITEDATA, &fd);
  (void)curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 1L);
  return curl_easy_perform(easy);
}

/* The writer thread body: drain the queue, writing each job at its offset, until
   every producer has finished and the queue is empty. */
static void chunk_writer_loop(struct chunk_writer *wr)
{
  for(;;) {
    struct chunk_wjob *job = NULL;

    chunk_mutex_lock(&wr->lock);
    if(wr->head) {
      job = wr->head;
      wr->head = job->next;
      if(!wr->head)
        wr->tail = NULL;
      wr->queued -= (curl_off_t)job->len;
    }
    else if(wr->done) {
      chunk_mutex_unlock(&wr->lock);
      break;              /* producers done and nothing left: exit */
    }
    chunk_mutex_unlock(&wr->lock);

    if(!job) {
      curlx_wait_ms(1);   /* queue momentarily empty: wait for more */
      continue;
    }

    if(!wr->result) {
      CURLcode res = chunk_write_at(wr->fd, job->buf, job->len, job->offset);
      if(res) {
        chunk_mutex_lock(&wr->lock);
        if(!wr->result)
          wr->result = res;
        chunk_mutex_unlock(&wr->lock);
      }
    }
    curlx_free(job->buf);
    curlx_free(job);
  }
}

/* libcurl write callback. With a writer: copy the bytes into a job and enqueue
   it for the single writer thread (applying backpressure so at most
   CHUNK_QUEUE_MAX bytes are buffered). Without a writer (fallback): write
   straight to this worker's own descriptor. */
static size_t chunk_write_cb(char *buffer, size_t sz, size_t nitems,
                             void *userdata)
{
  struct chunk_worker *w = userdata;
  struct chunk_writer *wr = w->pool->writer;
  size_t len = sz * nitems;

  if(!len)
    return 0;

  if(!wr) {
    /* fallback: no writer thread, write directly (single-threaded caller) */
    if(chunk_write_at(w->fd, buffer, len, w->cur_offset))
      return 0;
  }
  else {
    struct chunk_wjob *job;
    char *copy;

    /* bail out immediately if the writer or a peer worker already failed */
    if(wr->result || w->pool->result)
      return 0;

    copy = curlx_malloc(len);
    if(!copy)
      return 0;
    memcpy(copy, buffer, len);
    job = curlx_malloc(sizeof(*job));
    if(!job) {
      curlx_free(copy);
      return 0;
    }
    job->next = NULL;
    job->offset = w->cur_offset;
    job->buf = copy;
    job->len = len;

    /* enqueue, waiting while the queue is full so the whole file is never held
       in memory; a full queue simply throttles this transfer's socket reads */
    for(;;) {
      chunk_mutex_lock(&wr->lock);
      if(wr->result) {
        chunk_mutex_unlock(&wr->lock);
        curlx_free(copy);
        curlx_free(job);
        return 0;
      }
      if(wr->queued < CHUNK_QUEUE_MAX) {
        if(wr->tail)
          wr->tail->next = job;
        else
          wr->head = job;
        wr->tail = job;
        wr->queued += (curl_off_t)len;
        chunk_mutex_unlock(&wr->lock);
        break;
      }
      chunk_mutex_unlock(&wr->lock);
      curlx_wait_ms(1);
    }
  }

  w->cur_offset += (curl_off_t)len;
  w->written += (curl_off_t)len;
  return len;
}

/* Download one chunk (byte range idx*chunk_size .. +chunk_size-1, capped at
   total) on this worker's handle, blocking until it finishes. */
static CURLcode chunk_fetch_one(struct chunk_worker *w, unsigned int idx)
{
  struct chunk_pool *pool = w->pool;
  curl_off_t start = pool->base + (curl_off_t)idx * pool->chunk_size;
  curl_off_t end = start + pool->chunk_size - 1;
  char range[80];

  if(end >= pool->total)
    end = pool->total - 1;

  /* the write callback tags each received buffer with this running offset */
  w->cur_offset = start;

  curl_msnprintf(range, sizeof(range),
                 "%" CURL_FORMAT_CURL_OFF_T "-%" CURL_FORMAT_CURL_OFF_T,
                 start, end);
  (void)curl_easy_setopt(w->easy, CURLOPT_RANGE, range);
  return curl_easy_perform(w->easy);
}

/* The body every worker runs (whether on its own thread or, for the fallback
   worker, on the calling thread): pull chunk indices until the queue drains or
   someone records an error. */
static void chunk_worker_loop(struct chunk_worker *w)
{
  struct chunk_pool *pool = w->pool;

  for(;;) {
    unsigned int idx;
    CURLcode res;

    chunk_mutex_lock(&pool->lock);
    if(pool->result || pool->next >= pool->n) {
      chunk_mutex_unlock(&pool->lock);
      break;      /* queue drained (or another worker errored): stop pulling */
    }
    idx = pool->next++;
    chunk_mutex_unlock(&pool->lock);

    res = chunk_fetch_one(w, idx);
    if(res) {
      chunk_mutex_lock(&pool->lock);
      if(!pool->result)
        pool->result = res;
      chunk_mutex_unlock(&pool->lock);
      break;
    }
  }
  /* reached on every exit path -- normal drain AND error -- so the driving
     thread's `finished >= launched` check always eventually holds. Missing this
     on the normal path made the driver spin forever after all bytes arrived. */
  chunk_mutex_lock(&pool->lock);
  pool->finished++;
  chunk_mutex_unlock(&pool->lock);
}

/* Render a single in-place progress line for the whole chunked download. Only
   called on an interactive stderr, so a plain '\r' rewrite is fine. Sums the
   workers' private byte counters -- an unlocked read of counters other threads
   are advancing, which is fine for a progress display (worst case the total is
   a hair stale for one refresh). */
static void chunk_progress(const struct chunk_worker *workers,
                           unsigned int nworkers, curl_off_t total,
                           unsigned int threads, struct curltime start)
{
  curl_off_t got = 0;
  timediff_t ms;
  double mibps;
  int pct;
  unsigned int i;

  for(i = 0; i < nworkers; i++)
    got += workers[i].written;

  ms = curlx_timediff_ms(curlx_now(), start);
  mibps = ms > 0 ? ((double)got / (1024.0 * 1024.0)) / ((double)ms / 1000.0) : 0;
  pct = total > 0 ? (int)((got * 100) / total) : 0;
  fprintf(stderr,
          "\rChunked %3d%%  %.2f / %.2f GiB  %.0f MiB/s  %u threads   ",
          pct, (double)got / (1 << 30), (double)total / (1 << 30),
          mibps, threads);
  fflush(stderr);
}

#ifdef CHUNK_THREADS
#ifdef _WIN32
static unsigned __stdcall chunk_worker_thread(void *arg)
#else
static void *chunk_worker_thread(void *arg)
#endif
{
  chunk_worker_loop(arg);
  return 0;
}

static bool chunk_thread_start(struct chunk_worker *w)
{
#ifdef _WIN32
  uintptr_t h = _beginthreadex(NULL, 0, chunk_worker_thread, w, 0, NULL);
  if(!h)
    return FALSE;
  w->thread = h;
#else
  if(pthread_create(&w->thread, NULL, chunk_worker_thread, w))
    return FALSE;
#endif
  w->threaded = TRUE;
  return TRUE;
}

static void chunk_thread_join(struct chunk_worker *w)
{
  if(!w->threaded)
    return;
#ifdef _WIN32
  WaitForSingleObject((HANDLE)w->thread, INFINITE);
  CloseHandle((HANDLE)w->thread);
#else
  pthread_join(w->thread, NULL);
#endif
  w->threaded = FALSE;
}

#ifdef _WIN32
static unsigned __stdcall chunk_writer_thread(void *arg)
#else
static void *chunk_writer_thread(void *arg)
#endif
{
  chunk_writer_loop(arg);
  return 0;
}

static bool chunk_writer_start(struct chunk_writer *wr)
{
#ifdef _WIN32
  uintptr_t h = _beginthreadex(NULL, 0, chunk_writer_thread, wr, 0, NULL);
  if(!h)
    return FALSE;
  wr->thread = h;
#else
  if(pthread_create(&wr->thread, NULL, chunk_writer_thread, wr))
    return FALSE;
#endif
  wr->threaded = TRUE;
  return TRUE;
}

static void chunk_writer_join(struct chunk_writer *wr)
{
  if(!wr->threaded)
    return;
#ifdef _WIN32
  WaitForSingleObject((HANDLE)wr->thread, INFINITE);
  CloseHandle((HANDLE)wr->thread);
#else
  pthread_join(wr->thread, NULL);
#endif
  wr->threaded = FALSE;
}
#endif /* CHUNK_THREADS */

/* Prepare 'w': a private easy handle (duphandle of per->curl) and a private
   descriptor into the output file. The handle is deliberately NOT attached to
   the shared CURLSH -- that share carries no lock callbacks, so touching it
   from several threads would race; independent handles each open their own
   connection instead. */
static CURLcode chunk_worker_open(struct chunk_worker *w,
                                  struct per_transfer *per,
                                  struct chunk_pool *pool)
{
  w->pool = pool;
  w->written = 0;
  w->cur_offset = 0;
  w->fd = -1;
  w->threaded = FALSE;
  w->easy = curl_easy_duphandle(per->curl);
  if(!w->easy)
    return CURLE_OUT_OF_MEMORY;
  /* Only the writer-less fallback path writes through a per-worker descriptor;
     when a writer thread owns the file, workers never touch the fd. */
  if(!pool->writer) {
    w->fd = curlx_open(per->outfile, O_WRONLY | CURL_O_BINARY, CHUNK_OPENMODE);
    if(w->fd < 0) {
      curl_easy_cleanup(w->easy);
      w->easy = NULL;
      return CURLE_WRITE_ERROR;
    }
  }
  (void)curl_easy_setopt(w->easy, CURLOPT_SHARE, NULL);
  (void)curl_easy_setopt(w->easy, CURLOPT_WRITEFUNCTION, chunk_write_cb);
  (void)curl_easy_setopt(w->easy, CURLOPT_WRITEDATA, w);
  (void)curl_easy_setopt(w->easy, CURLOPT_NOPROGRESS, 1L);
  /* A large receive buffer means the write callback fires far less often: fewer
     write() syscalls and, on the shared output file, far fewer acquisitions of
     the kernel's per-file write lock that serialize the worker threads. libcurl
     clamps this to its own maximum if it is larger. */
  (void)curl_easy_setopt(w->easy, CURLOPT_BUFFERSIZE, (long)CHUNK_BUFSIZE);
  return CURLE_OK;
}

static void chunk_worker_close(struct chunk_worker *w)
{
  if(w->fd >= 0) {
    curlx_close(w->fd);
    w->fd = -1;
  }
  if(w->easy) {
    curl_easy_cleanup(w->easy);
    w->easy = NULL;
  }
}

/* Download the region [base,total) as fixed-size chunks using a pool of at most
   CHUNK_STREAMS worker threads, each fetching one chunk at a time. Bytes before
   'base' have already been written (the adaptive probe). Any chunk failing fails
   the whole download. */
static CURLcode chunk_download(struct per_transfer *per, curl_off_t total,
                               curl_off_t base)
{
  struct chunk_pool pool;
  struct chunk_worker *workers;
  curl_off_t region = total - base;
  curl_off_t chunk_size = chunk_size_for(region);
  unsigned int n = (unsigned int)((region + chunk_size - 1) / chunk_size);
  unsigned int nworkers = (n < CHUNK_STREAMS) ? n : CHUNK_STREAMS;
  unsigned int i, opened = 0;
  CURLcode result = CURLE_OK;
#ifdef CHUNK_THREADS
  struct chunk_writer writer;
  bool have_writer = FALSE;
#endif

  notef("Downloading '%s' (%" CURL_FORMAT_CURL_OFF_T " bytes) as %u chunks of "
        "%" CURL_FORMAT_CURL_OFF_T " MiB across %d threads",
        per->outfile, total, n, chunk_size >> 20, CHUNK_STREAMS);

  workers = curlx_calloc(nworkers, sizeof(*workers));
  if(!workers)
    return CURLE_OUT_OF_MEMORY;

  pool.next = 0;
  pool.n = n;
  pool.base = base;
  pool.chunk_size = chunk_size;
  pool.total = total;
  pool.finished = 0;
  pool.result = CURLE_OK;
  pool.writer = NULL;
  chunk_mutex_init(&pool.lock);

#ifdef CHUNK_THREADS
  /* Stand up the single writer thread that owns the only descriptor to the
     output file, so the worker threads never write the shared inode
     concurrently. If its descriptor or thread cannot be created, fall back to
     the old model where each worker writes through its own descriptor. */
  writer.head = writer.tail = NULL;
  writer.queued = 0;
  writer.done = FALSE;
  writer.result = CURLE_OK;
  writer.threaded = FALSE;
  chunk_mutex_init(&writer.lock);
  writer.fd = curlx_open(per->outfile, O_WRONLY | CURL_O_BINARY,
                         CHUNK_OPENMODE);
  if(writer.fd >= 0 && chunk_writer_start(&writer)) {
    pool.writer = &writer;
    have_writer = TRUE;
  }
  else {
    if(writer.fd >= 0)
      curlx_close(writer.fd);
    chunk_mutex_destroy(&writer.lock);
  }
#endif

  for(i = 0; i < nworkers; i++) {
    result = chunk_worker_open(&workers[i], per, &pool);
    if(result)
      break;
    opened++;
  }

  if(!result && opened) {
#ifdef CHUNK_THREADS
    /* Run every worker on its own thread and keep THIS thread free to paint a
       live progress line -- the chunk handles all run with NOPROGRESS, so
       without this the terminal would sit at 0% for the whole download and look
       hung. A worker whose thread fails to start simply does no work; the
       others drain the shared queue, so the download still finishes. */
    bool show = !global->noprogress && !global->silent && global->isatty;
    unsigned int launched = 0;
    struct curltime start = curlx_now();

    for(i = 0; i < opened; i++)
      if(chunk_thread_start(&workers[i]))
        launched++;

    if(!launched)
      /* nothing threaded (all creations failed): fetch inline, no live line */
      chunk_worker_loop(&workers[0]);
    else {
      for(;;) {
        unsigned int fin;
        chunk_mutex_lock(&pool.lock);
        fin = pool.finished;
        chunk_mutex_unlock(&pool.lock);
        if(show)
          chunk_progress(workers, opened, pool.total, launched, start);
        if(fin >= launched)
          break;
        curlx_wait_ms(200);
      }
    }
    for(i = 0; i < opened; i++)
      chunk_thread_join(&workers[i]);
    if(show)
      fputc('\n', stderr);
#else
    /* No thread support: fetch the chunks sequentially on this thread. */
    chunk_worker_loop(&workers[0]);
#endif
    result = pool.result;
  }

#ifdef CHUNK_THREADS
  if(have_writer) {
    /* every producer has stopped: let the writer drain what is queued, then
       finish. Its write error, if any, fails the download when nothing else
       already did. */
    chunk_mutex_lock(&writer.lock);
    writer.done = TRUE;
    chunk_mutex_unlock(&writer.lock);
    chunk_writer_join(&writer);
    if(!result)
      result = writer.result;
    curlx_close(writer.fd);
    chunk_mutex_destroy(&writer.lock);
  }
#endif

  for(i = 0; i < opened; i++)
    chunk_worker_close(&workers[i]);
  chunk_mutex_destroy(&pool.lock);
  curlx_free(workers);
  return result;
}

/* ---- public entry: fixed-size chunked download of a known size --------- */

/* The caller has already learned (from the size probe) that this is a
   range-capable download of 'total' bytes larger than 1 GiB. Adaptively pick the
   fastest path for this source: probe one stream's throughput, then either
   finish on that single stream (fast source -- sequential writes win) or split
   the remainder into parallel chunks (network-bound source -- more streams win).
   Either way this owns the whole transfer and writes 'per->outfile' directly. */
CURLcode tool_chunk_download(struct per_transfer *per, CURLSH *share,
                             curl_off_t total)
{
  int fd;
  CURLcode res;
  curl_off_t probe_len;
  timediff_t ms;
  double mibps;
  struct curltime t0;

  (void)share; /* worker handles are independent; see chunk_worker_open */

  if(!chunk_presize(per->outfile, total)) {
    /* the initial transfer was already aborted, so there is no normal fallback
       left: report the failure */
    errorf("chunked: cannot prepare output '%s'", per->outfile);
    return CURLE_WRITE_ERROR;
  }
  fd = curlx_open(per->outfile, O_WRONLY | CURL_O_BINARY, CHUNK_OPENMODE);
  if(fd < 0) {
    errorf("chunked: cannot open output '%s'", per->outfile);
    return CURLE_WRITE_ERROR;
  }

  /* Probe: download the first segment on one stream, timed, written as real
     data (never re-fetched). Its rate tells us whether this source is fast
     enough that a single sequential stream already beats the parallel writer. */
  probe_len = (total < CHUNK_PROBE) ? total : CHUNK_PROBE;
  t0 = curlx_now();
  res = chunk_stream_range(per->curl, fd, 0, probe_len - 1);
  ms = curlx_timediff_ms(curlx_now(), t0);
  if(res) {
    curlx_close(fd);
    return res;
  }
  if(probe_len >= total) {         /* whole file already fetched by the probe */
    curlx_close(fd);
    return CURLE_OK;
  }

  mibps = (ms > 0) ?
    ((double)probe_len / (1024.0 * 1024.0)) / ((double)ms / 1000.0) : 1.0e9;

  if(mibps >= CHUNK_FAST_MIBPS) {
    /* Fast source: one sequential stream is optimal; parallel byte ranges would
       only scatter the writes and slow the disk down. Finish on this stream. */
    notef("Downloading '%s' (%" CURL_FORMAT_CURL_OFF_T " bytes) on one stream: "
          "source is fast (%.0f MiB/s), not parallelizing",
          per->outfile, total, mibps);
    res = chunk_stream_range(per->curl, fd, probe_len, total - 1);
    curlx_close(fd);
    return res;
  }

  /* Network-bound source: parallelize the remainder across worker threads. */
  curlx_close(fd);   /* the worker pool opens its own writer descriptor */
  notef("Source is network-bound (%.0f MiB/s on one stream); parallelizing",
        mibps);
  return chunk_download(per, total, probe_len);
}
