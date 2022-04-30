/* Copyright (c) 2021, ctrip.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include <rocksdb/c.h>
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define RIO_THREADS_DEFAULT     6
#define RIO_THREADS_MAX         64

#define RIO_NOTIFY_CQ           0
#define RIO_NOTIFY_PIPE         1

#define DEFAULT_PROCESSING_QUEUE_SIZE 1024
#define RIO_NOTIFY_READ_MAX  512

typedef struct {
    int id;
    pthread_t thread_id;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    RIOQueue pending_rios[1];
    RIO *processing_rios;
    int processing_size;
} RIOThread;

typedef struct {
    int notify_recv_fd;
    int notify_send_fd;
    pthread_mutex_t lock;
    RIOQueue complete_queue[1];
    RIO *processing_rios;
    int processing_size;
} RIOCompleteQueue;

typedef struct rocks {
    /* rocksdb */
    int rocksdb_epoch;
    rocksdb_t *rocksdb;
    rocksdb_cache_t *block_cache;
    rocksdb_block_based_table_options_t *block_opts;
    rocksdb_options_t *rocksdb_opts;
    rocksdb_readoptions_t *rocksdb_ropts;
    rocksdb_writeoptions_t *rocksdb_wopts;
    const rocksdb_snapshot_t *rocksdb_snapshot;
    /* rocksdb io threads */
    int threads_num;
    RIOThread threads[RIO_THREADS_MAX];
    RIOCompleteQueue CQ;
} rocks;

int appendToRIOCompleteQueue(RIOCompleteQueue *cq, RIO *rio);

/* --- RIO: rocks io request context  --- */
/* Note that key/val owned by caller(val moved to caller for GET), rocks
 * io thread only ref key/val. */
void _RIOInit(RIO* rio, int type, int notify_type, sds key, sds val,
        rocksIOCallback cb, void *pd, int pipe_fd) {
    rio->type = type;
    rio->notify_type = notify_type;
    rio->key = key;
    rio->val = val;
    rio->cb = cb;
    rio->privdata = pd;
    rio->pipe_fd = pipe_fd;
}

void RIOInitAsync(RIO* rio, int type, sds key, sds val, rocksIOCallback cb, void *pd) {
    _RIOInit(rio, type, RIO_NOTIFY_CQ, key, val, cb, pd, -1);
}

void RIOInitSync(RIO* rio, int type, sds key, sds val, int pipe_fd, void *se) {
    _RIOInit(rio, type, RIO_NOTIFY_PIPE, key, val, NULL, se, pipe_fd);
}

/* --- RIOQueue: singly linked list of RIO chunks  --- */
void RIOQueueInit(RIOQueue *q) {
    q->head = q->tail = NULL;
    q->len = 0;
}

void RIOQueueDeinit(RIOQueue *q) {
    if (q == NULL) return;
    RIOChunk *chunk = q->head, *next;
    while (chunk) {
        next = chunk->next;
        zfree(chunk);
        chunk = next;
    }
    q->head = q->tail = NULL;
    q->len = 0;
}

void RIOQueuePush(RIOQueue *q, RIO *rio) {
    RIO *pushed;
    if (q->tail == NULL || q->tail->pending == q->tail->capacity) {
        RIOChunk *chunk = zmalloc(RIO_CHUNK_SIZE);
        chunk->next = NULL;
        chunk->capacity = RIO_CHUNK_CAPACITY;
        chunk->pending = chunk->processed = 0;

        if (q->tail) q->tail->next = chunk;
        q->tail = chunk;
        if (q->head == NULL) q->head = chunk;
    }
    pushed = q->tail->rios + q->tail->pending++;
    memcpy(pushed,rio,sizeof(RIO));
    q->len++;
    serverLog(LL_WARNING, "[xxx] push : %ld", q->len);
}

RIO* RIOQueuePeek(RIOQueue *q) {
    RIOChunk *chunk = q->head;
    if (q == NULL || chunk == NULL || chunk->processed == chunk->pending)
        return NULL;
    serverAssert(chunk->processed < chunk->pending); //TODO remove assert
    serverAssert(chunk->pending <= chunk->capacity);
    return chunk->rios + chunk->processed;
}

void RIOQueuePop(RIOQueue *q) {
    RIOChunk *chunk = q->head;
    serverAssert(chunk); // TODO remove assert
    serverAssert(chunk->processed < chunk->pending);
    serverAssert(chunk->pending <= chunk->capacity);
    chunk->processed++;
    if (chunk->processed == chunk->capacity) {
        q->head = chunk->next;
        if (q->tail == chunk) q->tail = NULL;
        zfree(chunk);
    }
    q->len--;
    serverLog(LL_WARNING, "[xxx] pop : %ld", q->len);
}

/* --- rio rocksdb operations --- */
int doRIORead(RIO *rio) {
    size_t vallen;
    char *err = NULL, *val;

    serverAssert(rio->type == ROCKS_GET);

    val = rocksdb_get(server.rocks->rocksdb, server.rocks->rocksdb_ropts,
            rio->key, sdslen(rio->key), &vallen, &err);
    if (err != NULL) {
        serverLog(LL_WARNING,"[rocks] do rocksdb get failed: %s", err);
        return -1;
    }

    rio->val = sdsnewlen(val, vallen);
    zlibc_free(val);

    return 0;
}

int doRIOWrite(RIO *rio) {
    char *err = NULL;
    serverAssert(rio->type == ROCKS_PUT);
    rocksdb_put(server.rocks->rocksdb, server.rocks->rocksdb_wopts,
            rio->key, sdslen(rio->key), rio->val, sdslen(rio->val), &err);
    if (err != NULL) {
        serverLog(LL_WARNING,"[rocks] do rocksdb write failed: %s", err);
        return -1;
    }
    return 0;
}

int doRIODel(RIO *rio) {
    char *err = NULL;
    serverAssert(rio->type == ROCKS_DEL);
    rocksdb_delete(server.rocks->rocksdb, server.rocks->rocksdb_wopts,
            rio->key, sdslen(rio->key), &err);
    if (err != NULL) {
        serverLog(LL_WARNING,"[rocks] do rocksdb del failed: %s", err);
        return -1;
    }
    return 0;
}

int doRIO(RIO *rio) {

    if (server.debug_rio_latency) usleep(server.debug_rio_latency*1000);

    switch (rio->type) {
    case ROCKS_GET:
        return doRIORead(rio);
    case ROCKS_PUT:
        return doRIOWrite(rio);
    case ROCKS_DEL:
        return doRIODel(rio);
    default:
        serverPanic("[rocks] Unknown io type: %d", rio->type);
        return -1;
    }
}

void *RIOThreadMain (void *arg) {
    char thdname[16];
    RIOThread *thread = arg;

    snprintf(thdname, sizeof(thdname), "rocks_thd_%d", thread->id);
    redis_set_thread_title(thdname);

    while (1) {
        int processing_count = 0, i;
        RIO *rio;

        pthread_mutex_lock(&thread->lock);
        while (RIOQueueLength(thread->pending_rios) == 0)
            pthread_cond_wait(&thread->cond, &thread->lock);

        while ((rio = RIOQueuePeek(thread->pending_rios)) &&
                processing_count < thread->processing_size) {
            thread->processing_rios[processing_count++] = *rio;
            RIOQueuePop(thread->pending_rios);
        }
        pthread_mutex_unlock(&thread->lock);

        for (i = 0; i < processing_count; i++) {
            RIO *rio = thread->processing_rios+i;

            doRIO(rio); 

            if (rio->notify_type == RIO_NOTIFY_CQ) {
                if (rio->cb != NULL) {
                    appendToRIOCompleteQueue(&server.rocks->CQ, rio); //TODO reduce lock
                }
            } else { /* RIO_NOTIFY_PIPE */
                /* TODO dirty hack, use NOTIFY_CQ to replace NOTIF_PIPE  */
                swapEntry *se = rio->privdata;
                se->rawkey = rio->key;
                se->rawval = rio->val;
                if (write(rio->pipe_fd, "x", 1) < 1 && errno != EAGAIN) {
                    static mstime_t prev_log;
                    if (server.mstime - prev_log >= 1000) {
                        prev_log = server.mstime;
                        serverLog(LL_NOTICE,
                                "[rocks] notify rio finish failed: %s",
                                strerror(errno));
                    }
                }
            }
        }
    }

    return NULL;
}

int processFinishedRIOInCompleteQueue(RIOCompleteQueue *cq) {
    RIO *rio;
    int processing_count = 0, i;

    pthread_mutex_lock(&cq->lock);
    while (RIOQueueLength(cq->complete_queue) && 
            processing_count < cq->processing_size) {
        rio = cq->processing_rios + processing_count;
        memcpy(rio, RIOQueuePeek(cq->complete_queue),sizeof(RIO));
        RIOQueuePop(cq->complete_queue);
        processing_count++;
    }
    pthread_mutex_unlock(&cq->lock);

    for (i = 0; i < processing_count; i++) {
        rio = cq->processing_rios + i;
        serverAssert(rio->cb);
        rio->cb(rio->type, rio->key, rio->val, rio->privdata);
    }

    return processing_count;
}

/* read before unlink clients so that main thread won't miss notify event:
 * rocksb thread: 1. link req; 2. send notify byte;
 * main thread: 1. read notify bytes; 2. unlink req;
 * if main thread read less notify bytes than unlink clients num (e.g. rockdb thread
 * link more clients when , main thread would still be triggered because epoll
 * LT-triggering mode. */
void RIOFinished(aeEventLoop *el, int fd, void *privdata, int mask) {
    char notify_recv_buf[RIO_NOTIFY_READ_MAX];

    UNUSED(el);
    UNUSED(mask);

    int nread = read(fd, notify_recv_buf, sizeof(notify_recv_buf));
    if (nread == 0) {
        serverLog(LL_WARNING, "[rocks] notify recv fd closed.");
    } else if (nread < 0) {
        serverLog(LL_WARNING, "[rocks] read notify failed: %s",
                strerror(errno));
    }

    processFinishedRIOInCompleteQueue(privdata); /* privdata is cq */
}

void rocksIOSubmitAsync(uint32_t dist, int type, sds key, sds val, rocksIOCallback cb, void *pd) {
    struct RIO rio;
    serverAssert(server.rocks->threads_num >= 0);
    RIOThread *rt = &server.rocks->threads[dist % server.rocks->threads_num];
    pthread_mutex_lock(&rt->lock);
    RIOInitAsync(&rio, type, key, val, cb, pd);
    RIOQueuePush(rt->pending_rios, &rio);
    pthread_cond_signal(&rt->cond);
    pthread_mutex_unlock(&rt->lock);
}

void rocksIOSubmitSync(uint32_t dist, int type, sds key, sds val, int notify_fd, void *se) {
    struct RIO rio;
    serverAssert(server.rocks->threads_num >= 0);
    RIOThread *rt = &server.rocks->threads[dist % server.rocks->threads_num];
    RIOInitSync(&rio, type, key, val, notify_fd, se);
    pthread_mutex_lock(&rt->lock);
    RIOQueuePush(rt->pending_rios, &rio);
    pthread_cond_signal(&rt->cond);
    pthread_mutex_unlock(&rt->lock);
}

static int rocksRIODrained(rocks *rocks) {
    RIOThread *rt;
    int i, drained = 1;

    for (i = 0; i < rocks->threads_num; i++) {
        rt = &server.rocks->threads[i];

        pthread_mutex_lock(&rt->lock);
        if (RIOQueueLength(rt->pending_rios)) drained = 0;
        pthread_mutex_unlock(&rt->lock);
    }

    pthread_mutex_lock(&rocks->CQ.lock);
    if (RIOQueueLength(rocks->CQ.complete_queue)) drained = 0;
    pthread_mutex_unlock(&rocks->CQ.lock);

    return drained;
}

int rocksIODrain(rocks *rocks, mstime_t time_limit) {
    int result = 0;
    mstime_t start = mstime();

    while (!rocksRIODrained(rocks)) {
        processFinishedRIOInCompleteQueue(&rocks->CQ);

        if (time_limit >= 0 && mstime() - start > time_limit) {
            result = -1;
            break;
        }
    }

    serverLog(LL_NOTICE,
            "[rocks] drain IO %s: elapsed (%lldms) limit (%lldms)",
            result == 0 ? "ok":"failed", mstime() - start, time_limit);

    return result;
}

int appendToRIOCompleteQueue(RIOCompleteQueue *cq, RIO *rio) {
    pthread_mutex_lock(&cq->lock);
    RIOQueuePush(cq->complete_queue, rio);
    pthread_mutex_unlock(&cq->lock);
    if (write(cq->notify_send_fd, "x", 1) < 1 && errno != EAGAIN) {
        static mstime_t prev_log;
        if (server.mstime - prev_log >= 1000) {
            prev_log = server.mstime;
            serverLog(LL_NOTICE, "[rocks] notify rio finish failed: %s",
                    strerror(errno));
        }
        return -1;
    }
    return 0;
}

unsigned long rocksPendingIOs() {
    int i;
    unsigned long pending = 0;
    for (i = 0; i < server.rocks->threads_num; i++) {
        RIOThread *rt = &server.rocks->threads[i];
        pthread_mutex_lock(&rt->lock);
        pending += RIOQueueLength(rt->pending_rios);
        pthread_mutex_unlock(&rt->lock);
    }
    return pending;
}

static int rocksInitCompleteQueue(rocks *rocks) {
    int fds[2];
    char anetErr[ANET_ERR_LEN];
    RIOCompleteQueue *cq = &rocks->CQ;

    if (pipe(fds)) {
        perror("Can't create notify pipe");
        return -1;
    }

    cq->notify_recv_fd = fds[0];
    cq->notify_send_fd = fds[1];

    pthread_mutex_init(&cq->lock, NULL);

    RIOQueueInit(cq->complete_queue);
    cq->processing_size = DEFAULT_PROCESSING_QUEUE_SIZE;
    cq->processing_rios = zmalloc(sizeof(RIO*)*cq->processing_size);

    if (anetNonBlock(anetErr, cq->notify_recv_fd) != ANET_OK) {
        serverLog(LL_WARNING,
                "Fatal: set notify_recv_fd non-blocking failed: %s",
                anetErr);
        return -1;
    }

    if (anetNonBlock(anetErr, cq->notify_send_fd) != ANET_OK) {
        serverLog(LL_WARNING,
                "Fatal: set notify_recv_fd non-blocking failed: %s",
                anetErr);
        return -1;
    }

    if (aeCreateFileEvent(server.el, cq->notify_recv_fd,
                AE_READABLE, RIOFinished, cq) == AE_ERR) {
        serverLog(LL_WARNING,"Fatal: create notify recv event failed: %s",
                strerror(errno));
        return -1;
    }

    return 0;
}

static void rocksDeinitCompleteQueue(rocks *rocks) {
    RIOCompleteQueue *cq = &rocks->CQ;
    close(cq->notify_recv_fd);
    close(cq->notify_send_fd);
    pthread_mutex_destroy(&cq->lock);
    RIOQueueDeinit(cq->complete_queue);
    zfree(cq->processing_rios);
}

int rocksProcessCompleteQueue(rocks *rocks) {
    return processFinishedRIOInCompleteQueue(&rocks->CQ);
}

#define ROCKS_DIR_MAX_LEN 512

#define ROCKS_DATA "data.rocks"

static int rocksInitDB(rocks *rocks) {
    char *err = NULL, dir[ROCKS_DIR_MAX_LEN];
    rocksdb_cache_t *block_cache;
    rocks->rocksdb_snapshot = NULL;
    rocks->rocksdb_opts = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(rocks->rocksdb_opts, 1); 
    rocksdb_options_enable_statistics(rocks->rocksdb_opts);
    rocksdb_options_set_stats_dump_period_sec(rocks->rocksdb_opts, 60);
    rocksdb_options_set_max_write_buffer_number(rocks->rocksdb_opts, 6);
    rocksdb_options_set_max_bytes_for_level_base(rocks->rocksdb_opts, 512*1024*1024); 
    struct rocksdb_block_based_table_options_t *block_opts = rocksdb_block_based_options_create();
    rocksdb_block_based_options_set_block_size(block_opts, 8192);
    block_cache = rocksdb_cache_create_lru(1*1024*1024);
    rocks->block_cache = block_cache;
    rocksdb_block_based_options_set_block_cache(block_opts, block_cache);
    rocksdb_block_based_options_set_cache_index_and_filter_blocks(block_opts, 0);
    rocksdb_options_set_block_based_table_factory(rocks->rocksdb_opts, block_opts);
    rocks->block_opts = block_opts;

    rocksdb_options_optimize_for_point_lookup(rocks->rocksdb_opts, 1);
    rocksdb_options_optimize_level_style_compaction(rocks->rocksdb_opts, 256*1024*1024);
    rocksdb_options_set_max_background_compactions(rocks->rocksdb_opts, 4); /* default 1 */
    rocksdb_options_compaction_readahead_size(rocks->rocksdb_opts, 2*1024*1024); /* default 0 */
    rocksdb_options_set_optimize_filters_for_hits(rocks->rocksdb_opts, 1); /* default false */

    rocks->rocksdb_ropts = rocksdb_readoptions_create();
    rocksdb_readoptions_set_verify_checksums(rocks->rocksdb_ropts, 0);
    rocksdb_readoptions_set_fill_cache(rocks->rocksdb_ropts, 0);

    rocks->rocksdb_wopts = rocksdb_writeoptions_create();
    rocksdb_writeoptions_disable_WAL(rocks->rocksdb_wopts, 1);

    struct stat statbuf;
    if (!stat(ROCKS_DATA, &statbuf) && S_ISDIR(statbuf.st_mode)) {
        /* "data.rocks" folder already exists, no need to create */
    } else if (mkdir(ROCKS_DATA, 0755)) {
        serverLog(LL_WARNING, "[ROCKS] mkdir %s failed: %s",
                ROCKS_DATA, strerror(errno));
        return -1;
    }

    snprintf(dir, ROCKS_DIR_MAX_LEN, "%s/%d", ROCKS_DATA, rocks->rocksdb_epoch);
    rocks->rocksdb = rocksdb_open(rocks->rocksdb_opts, dir, &err);
    if (err != NULL) {
        serverLog(LL_WARNING, "[ROCKS] rocksdb open failed: %s", err);
        return -1;
    }

    serverLog(LL_NOTICE, "[ROCKS] opened rocks data in (%s).", dir);

    return 0;
}

static void rocksDeinitDB(rocks *rocks) {
    rocksdb_cache_destroy(rocks->block_cache);
    rocksdb_block_based_options_destroy(rocks->block_opts);
    rocksdb_options_destroy(rocks->rocksdb_opts);
    rocksdb_writeoptions_destroy(rocks->rocksdb_wopts);
    rocksdb_readoptions_destroy(rocks->rocksdb_ropts);
    rocksdb_close(rocks->rocksdb);
}

int rocksInitThreads(rocks *rocks) {
    int i, nthd = RIO_THREADS_DEFAULT;

    if (nthd > RIO_THREADS_MAX) nthd = RIO_THREADS_MAX;
    rocks->threads_num = nthd;

    for (i = 0; i < nthd; i++) {
        RIOThread *thread = &rocks->threads[i];

        thread->id = i;
        RIOQueueInit(thread->pending_rios);
        thread->processing_size = DEFAULT_PROCESSING_QUEUE_SIZE;
        thread->processing_rios = zmalloc(sizeof(RIO*)*thread->processing_size);
        pthread_mutex_init(&thread->lock, NULL);
        pthread_cond_init(&thread->cond, NULL);
        if (pthread_create(&thread->thread_id, NULL, RIOThreadMain, thread)) {
            serverLog(LL_WARNING, "Fatal: create rocks IO threads failed.");
            return -1;
        }
    }

    return 0;
}

static void rocksDeinitThreads(rocks *rocks) {
    int i, err;

    for (i = 0; i < rocks->threads_num; i++) {
        RIOThread *thread = &rocks->threads[i];
        RIOQueueDeinit(thread->pending_rios);
        zfree(thread->processing_rios);
        if (thread->thread_id == pthread_self()) continue;
        if (thread->thread_id && pthread_cancel(thread->thread_id) == 0) {
            if ((err = pthread_join(thread->thread_id, NULL)) != 0) {
                serverLog(LL_WARNING, "rocks thread #%d can't be joined: %s",
                        i, strerror(err));
            } else {
                serverLog(LL_WARNING, "rocks thread #%d terminated.", i);
            }
        }
    }
}

struct rocks *rocksCreate() {
    rocks *rocks = zmalloc(sizeof(struct rocks));
    rocks->rocksdb_epoch = 0;
    if (rocksInitDB(rocks)) goto err;
    if (rocksInitCompleteQueue(rocks)) goto err;
    if (rocksInitThreads(rocks)) goto err;
    return rocks;
err:
    if (rocks != NULL) zfree(rocks);
    return NULL;
}

void rocksCreateSnapshot(rocks *rocks) {
    if (rocks->rocksdb_snapshot) {
        serverLog(LL_WARNING, "[rocks] release snapshot before create.");
        rocksdb_release_snapshot(rocks->rocksdb, rocks->rocksdb_snapshot);
    }
    rocks->rocksdb_snapshot = rocksdb_create_snapshot(rocks->rocksdb);
    serverLog(LL_NOTICE, "[rocks] create rocksdb snapshot ok.");
}

void rocksUseSnapshot(rocks *rocks) {
    if (rocks->rocksdb_snapshot) {
        rocksdb_readoptions_set_snapshot(rocks->rocksdb_ropts, rocks->rocksdb_snapshot);
        serverLog(LL_NOTICE, "[rocks] use snapshot read ok.");
    } else {
        serverLog(LL_WARNING, "[rocks] use snapshot read failed: snapshot not exists.");
    }
}

void rocksReleaseSnapshot(rocks *rocks) {
    if (rocks->rocksdb_snapshot) {
        serverLog(LL_NOTICE, "[rocks] relase snapshot ok.");
        rocksdb_release_snapshot(rocks->rocksdb, rocks->rocksdb_snapshot);
        rocks->rocksdb_snapshot = NULL;
    }
}

/* --- rocks iter ---- */
static int rocksIterWaitReady(rocksIter* it) {
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    pthread_mutex_lock(&cq->buffer_lock);
    while (1) {
        /* iterResult ready */
        if (cq->processed_count < cq->buffered_count)
            break;
        /* iter finished */
        if (cq->iter_finished) {
            pthread_mutex_unlock(&cq->buffer_lock);
            return 0;
        }
        /* wait io thread */
        pthread_cond_wait(&cq->ready_cond, &cq->buffer_lock);
    }
    pthread_mutex_unlock(&cq->buffer_lock);
    return 1;
}

static void rocksIterNotifyReady(rocksIter* it) {
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    pthread_mutex_lock(&cq->buffer_lock);
    cq->buffered_count++;
    pthread_cond_signal(&cq->ready_cond);
    pthread_mutex_unlock(&cq->buffer_lock);
}

static void rocksIterNotifyFinshed(rocksIter* it) {
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    pthread_mutex_lock(&cq->buffer_lock);
    cq->iter_finished = 1;
    pthread_cond_signal(&cq->ready_cond);
    pthread_mutex_unlock(&cq->buffer_lock);
}

static int rocksIterWaitVacant(rocksIter *it) {
    int64_t slots, occupied;
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    /* wait untill there are vacant slots in buffer. */
    pthread_mutex_lock(&cq->buffer_lock);
    while (1) {
        occupied = cq->buffered_count - cq->processed_count;
        slots = cq->buffer_capacity - occupied;
        if (slots < 0) {
            serverPanic("CQ slots is negative.");
        } else if (slots == 0) {
            pthread_cond_wait(&cq->vacant_cond, &cq->buffer_lock);
        } else {
            break;
        }
    }
    pthread_mutex_unlock(&cq->buffer_lock);
    return slots;
}

static void rocksIterNotifyVacant(rocksIter* it) {
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    pthread_mutex_lock(&cq->buffer_lock);
    cq->processed_count++;
    pthread_cond_signal(&cq->vacant_cond);
    pthread_mutex_unlock(&cq->buffer_lock);
}

void *rocksIterIOThreadMain(void *arg) {
    rocksIter *it = arg;
    bufferedIterCompleteQueue *cq = it->buffered_cq;

    redis_set_thread_title("rocks_iter");

    while (!cq->iter_finished) {
        int64_t slots = rocksIterWaitVacant(it);

        /* there only one producer, slots will decrease only by current
         * thread, we can produce multiple iterResult in one loop. */
        while (slots--) {
            iterResult *cur;
            int curidx;
            const char *rawkey, *rawval;
            size_t rklen, rvlen;

            if (!rocksdb_iter_valid(it->rocksdb_iter)) {
                rocksIterNotifyFinshed(it);
                break;
            }

            curidx = cq->buffered_count % cq->buffer_capacity;
            cur = cq->buffered + curidx;

            rawkey = rocksdb_iter_key(it->rocksdb_iter,&rklen);
            rawval = rocksdb_iter_value(it->rocksdb_iter,&rvlen);

            if (rklen > CACHED_MAX_KEY_LEN) {
                cur->rawkey = sdsnewlen(rawkey, rklen);
            } else {
                memcpy(cur->cached_key, rawkey, rklen);
                cur->cached_key[rklen] = '\0';
                sdssetlen(cur->cached_key, rklen);
                cur->rawkey = cur->cached_key;
            }

            if (rvlen > CACHED_MAX_VAL_LEN) {
                cur->rawval = sdsnewlen(rawval, rvlen);
            } else {
                memcpy(cur->cached_val, rawval, rvlen);
                cur->cached_val[rvlen] = '\0';
                sdssetlen(cur->cached_val, rvlen);
                cur->rawval = cur->cached_val;
            }

            rocksIterNotifyReady(it);

            rocksdb_iter_next(it->rocksdb_iter);
        }
    }
    serverLog(LL_WARNING, "Rocks iter thread exit.");

    return NULL;
}

bufferedIterCompleteQueue *bufferedIterCompleteQueueNew(int capacity) {
    int i;
    bufferedIterCompleteQueue* buffered_cq;

    buffered_cq = zmalloc(sizeof(bufferedIterCompleteQueue));
    memset(buffered_cq, 0 ,sizeof(*buffered_cq));
    buffered_cq->buffer_capacity = capacity;

    buffered_cq->buffered = zmalloc(capacity*sizeof(iterResult));
    for (i = 0; i < capacity; i++) {
        iterResult *iter_result = buffered_cq->buffered+i;
        iter_result->cached_key = sdsnewlen(NULL, CACHED_MAX_KEY_LEN);
        iter_result->rawkey = NULL;
        iter_result->cached_val = sdsnewlen(NULL, CACHED_MAX_VAL_LEN);
        iter_result->rawval = NULL;
    }

    pthread_mutex_init(&buffered_cq->buffer_lock, NULL);
    pthread_cond_init(&buffered_cq->ready_cond, NULL);
    pthread_mutex_init(&buffered_cq->buffer_lock, NULL);
    pthread_cond_init(&buffered_cq->vacant_cond, NULL);
    return buffered_cq;
}

void bufferedIterCompleteQueueFree(bufferedIterCompleteQueue *buffered_cq) {
    if (buffered_cq == NULL) return;
    for (int i = 0; i < buffered_cq->buffer_capacity;i++) {
        iterResult *res = buffered_cq->buffered+i;
        if (res->rawkey != res->cached_key) sdsfree(res->rawkey);
        if (res->rawval != res->cached_val) sdsfree(res->rawval);
        sdsfree(res->rawkey);
        sdsfree(res->rawval);
    }
    zfree(buffered_cq->buffered);
    pthread_mutex_destroy(&buffered_cq->buffer_lock);
    pthread_cond_destroy(&buffered_cq->ready_cond);
    pthread_mutex_destroy(&buffered_cq->buffer_lock);
    pthread_cond_destroy(&buffered_cq->vacant_cond);
    zfree(buffered_cq);
}

rocksIter *rocksCreateIter(rocks *rocks, redisDb *db) {
    int err;
    rocksdb_iterator_t *rocksdb_iter;
    rocksIter *it = zmalloc(sizeof(rocksIter));

    it->rocks = rocks;
    it->db = db;
    rocksdb_iter = rocksdb_create_iterator(rocks->rocksdb, rocks->rocksdb_ropts);
    if (rocksdb_iter == NULL) {
        serverLog(LL_WARNING, "Create rocksdb iterator failed.");
        goto err;
    }
    rocksdb_iter_seek_to_first(rocksdb_iter);
    it->rocksdb_iter = rocksdb_iter;

    it->buffered_cq = bufferedIterCompleteQueueNew(DEFAULT_BUFFERED_ITER_CAPACITY);

    if ((err = pthread_create(&it->io_thread, NULL, rocksIterIOThreadMain, it))) {
        serverLog(LL_WARNING, "Create rocksdb iterator thread failed: %s.", strerror(err));
        goto err;
    }

    return it;
err:
    rocksReleaseIter(it);
    return NULL;
}

int rocksIterSeekToFirst(rocksIter *it) {
    return rocksIterWaitReady(it);
}

void rocksIterKeyValue(rocksIter *it, sds *rawkey, sds *rawval) {
    int idx;
    iterResult *cur;
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    idx = cq->processed_count % cq->buffer_capacity;
    cur = it->buffered_cq->buffered+idx;
    if (rawkey) *rawkey = cur->rawkey;
    if (rawval) *rawval = cur->rawval;
}

/* Will block untill at least one result is ready.
 * note that rawkey and rawval are owned by rocksIter. */
int rocksIterNext(rocksIter *it) {
    int idx;
    iterResult *cur;
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    idx = cq->processed_count % cq->buffer_capacity;
    cur = it->buffered_cq->buffered+idx;
    /* clear previos state */
    if (cur->rawkey != cur->cached_key) sdsfree(cur->rawkey);
    if (cur->rawval != cur->cached_val) sdsfree(cur->rawval);
    rocksIterNotifyVacant(it);
    return rocksIterWaitReady(it);
}

void rocksReleaseIter(rocksIter *it) {
    int err;

    if (it == NULL) return;

    if (it->io_thread) {
        if (pthread_cancel(it->io_thread) == 0) {
            if ((err = pthread_join(it->io_thread, NULL)) != 0) {
                serverLog(LL_WARNING, "Iter io thread can't be joined: %s",
                         strerror(err));
            } else {
                serverLog(LL_WARNING, "Iter io thread terminated.");
            }
        }
        it->io_thread = 0;
    }

   if (it->buffered_cq) {
       bufferedIterCompleteQueueFree(it->buffered_cq);
       it->buffered_cq = NULL;
   }

   if (it->rocksdb_iter) {
       rocksdb_iter_destroy(it->rocksdb_iter);
       it->rocksdb_iter = NULL;
   }

   zfree(it);
}

void rocksIterGetError(rocksIter *it, char **error) {
    rocksdb_iter_get_error(it->rocksdb_iter, error);
}

void rocksDestroy(rocks *rocks) {
    rocksDeinitThreads(rocks);
    rocksDeinitCompleteQueue(rocks);
    rocksDeinitDB(rocks);
    zfree(rocks);
}

static int rmdirRecursive(const char *path) {
	struct dirent *p;
	DIR *d = opendir(path);
	size_t path_len = strlen(path);
	int r = 0;

	if (d == NULL) return -1;

	while (!r && (p=readdir(d))) {
		int r2 = -1;
		char *buf;
		size_t len;
		struct stat statbuf;

		/* Skip the names "." and ".." as we don't want to recurse on them. */
		if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, ".."))
			continue;

		len = path_len + strlen(p->d_name) + 2; 
		buf = zmalloc(len);

		snprintf(buf, len, "%s/%s", path, p->d_name);
		if (!stat(buf, &statbuf)) {
			if (S_ISDIR(statbuf.st_mode))
				r2 = rmdirRecursive(buf);
			else
				r2 = unlink(buf);
		}

		zfree(buf);
		r = r2;
	}
	closedir(d);

	if (!r) r = rmdir(path);

	return r;
}

int rocksFlushAll() {
    char odir[ROCKS_DIR_MAX_LEN];

    snprintf(odir, ROCKS_DIR_MAX_LEN, "%s/%d", ROCKS_DATA, server.rocks->rocksdb_epoch);
    rocksIODrain(server.rocks, -1);
    rocksDeinitDB(server.rocks);
    server.rocks->rocksdb_epoch++;
    if (rocksInitDB(server.rocks)) {
        serverLog(LL_WARNING, "[ROCKS] init new rocksdb failed, trying to resume old one.");
        server.rocks->rocksdb_epoch--;
        if (rocksInitDB(server.rocks)) {
            serverLog(LL_WARNING, "[ROCKS] resume old one failed, oops.");
        } else {
            serverLog(LL_WARNING, "[ROCKS] resume old one success.");
        }
        return -1;
    }
    rmdirRecursive(odir);
    serverLog(LL_NOTICE, "[ROCKS] remove rocks data in (%s).", odir);
    return 0;
}

rocksdb_t *rocksGetDb(rocks *rocks) {
    return rocks->rocksdb;
}

void rocksCron(void) {
    uint64_t property_int = 0;
    if (!rocksdb_property_int(server.rocks->rocksdb,
                "rocksdb.total-sst-files-size", &property_int)) {
        server.rocksdb_disk_used = property_int;
    }
    if (server.maxdisk && server.rocksdb_disk_used > server.maxdisk) {
        serverLog(LL_WARNING, "Rocksdb disk usage exceeds maxdisk %lld > %lld.",
                server.rocksdb_disk_used, server.maxdisk);
    }
}
