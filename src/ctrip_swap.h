#ifndef  __CTRIP_SWAP_H__
#define  __CTRIP_SWAP_H__

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

/* rocks io thread */
#define ROCKS_GET             	1
#define ROCKS_PUT            	  2
#define ROCKS_DEL              	3

#define RIO_CHUNK_SIZE          (16*1024)
#define RIO_CHUNK_CAPACITY      ((RIO_CHUNK_SIZE - offsetof(RIOChunk,capacity)) / sizeof(RIO))

typedef void (*rocksIOCallback)(int action, sds key, sds val, void *privdata);

typedef struct RIO {
    unsigned type:4;        /* io type, GET/PUT/DEL */
    unsigned notify_type:4; /* notify complete type, CQ/PIPE */
    unsigned reserved:24;   /* reserved */
    int pipe_fd;            /* PIPE: fd to notify io finished */
    sds key;                /* rocks key */
    sds val;                /* rocks val */
    rocksIOCallback cb;     /* CQ: io finished callback */
    void *privdata;         /* CQ: io finished privdata */
} RIO;

typedef struct RIOChunk {
  struct RIOChunk *next;
  size_t processed;
  size_t pending;
  size_t capacity;
  RIO rios[];
} RIOChunk;

typedef struct RIOQueue {
  RIOChunk *head;
  RIOChunk *tail;
  size_t len;
} RIOQueue;

void RIOQueueInit(RIOQueue *q);
void RIOQueueDeinit(RIOQueue *q);
void RIOQueuePush(RIOQueue *q, RIO *rio);
RIO *RIOQueuePeek(RIOQueue *q);
void RIOQueuePop(RIOQueue *q);
#define RIOQueueLength(q) ((q)->len)

void rocksIOSubmitAsync(uint32_t dist, int type, sds key, sds val, rocksIOCallback cb, void *privdata);
void rocksIOSubmitSync(uint32_t dist, int type, sds key, sds val, int notify_fd, void *se);
void RIOReap(struct RIO *r, sds *key, sds *val);
unsigned long rocksPendingIOs();
int rocksIODrain(struct rocks *rocks, mstime_t time_limit);

struct rocks *rocksCreate(void);
void rocksDestroy(struct rocks *rocks);
int rocksEvictionsInprogress(void);
int rocksDelete(redisDb *db, robj *key);
int rocksFlushAll();
rocksdb_t *rocksGetDb(struct rocks *rocks);
int rocksProcessCompleteQueue(struct rocks *rocks);
void rocksCron();
int rocksInitThreads(struct rocks *rocks);
void rocksCreateSnapshot(struct rocks *rocks);
void rocksUseSnapshot(struct rocks *rocks);
void rocksReleaseSnapshot(struct rocks *rocks);

/* rocks iter thread */
#define DEFAULT_BUFFERED_ITER_CAPACITY 256
#define CACHED_MAX_KEY_LEN 1000
#define CACHED_MAX_VAL_LEN 4000

typedef struct iterResult {
    sds cached_key;
    sds cached_val;
    sds rawkey;
    sds rawval;
} iterResult;

typedef struct bufferedIterCompleteQueue {
    int buffer_capacity;
    iterResult *buffered;
    int iter_finished;
    int64_t buffered_count;
    int64_t processed_count;
    pthread_mutex_t buffer_lock;
    pthread_cond_t ready_cond;
    pthread_cond_t vacant_cond;
} bufferedIterCompleteQueue;

typedef struct rocksIter{
    redisDb *db;
    struct rocks *rocks;
    pthread_t io_thread;
    bufferedIterCompleteQueue *buffered_cq;
    rocksdb_iterator_t *rocksdb_iter;
} rocksIter;

rocksIter *rocksCreateIter(struct rocks *rocks, redisDb *db);
int rocksIterSeekToFirst(rocksIter *it);
int rocksIterNext(rocksIter *it);
void rocksIterKeyValue(rocksIter *it, sds *rawkey, sds *rawval);
void rocksReleaseIter(rocksIter *it);
void rocksIterGetError(rocksIter *it, char **error);

/* rdb */
int rdbSaveRocks(rio *rdb, redisDb *db, int rdbflags);

/* parallel swap */
typedef int (*parallelSwapFinishedCb)(sds rawkey, sds rawval, void *pd);

typedef struct {
    int inprogress;         /* swap entry in progress? */
    int pipe_read_fd;       /* read end to wait rio swap finish. */
    int pipe_write_fd;      /* write end to notify swap finish by rio. */
    parallelSwapFinishedCb cb; /* swap finished callback. */
    void *pd;               /* swap finished private data. */
    sds rawkey;
    sds rawval;
} swapEntry;

typedef struct parallelSwap {
    list *entries;
    int parallel;
} parallelSwap;

parallelSwap *parallelSwapNew(int parallel);
void parallelSwapFree(parallelSwap *ps);
int parallelSwapSubmit(parallelSwap *ps, int action, sds rawkey, sds rawval, parallelSwapFinishedCb cb, void *pd);
int parallelSwapDrain();
int parallelSwapGet(sds rawkey, parallelSwapFinishedCb cb, void *pd);
int parallelSwapPut(sds rawkey, sds rawval, parallelSwapFinishedCb cb, void *pd);
int parallelSwapDel(sds rawkey, parallelSwapFinishedCb cb, void *pd);

/* encoding */
sds rocksEncodeKey(int type, sds key);
int rocksDecodeKey(const char *rawkey, size_t rawlen, const char **key, size_t *klen);

/* Whole key swap (string, hash) */
int swapAnaWk(struct redisCommand *cmd, redisDb *db, robj *key, int *action, char **rawkey, char **rawval, dataSwapFinishedCallback *cb, void **pd);
void getDataSwapsWk(robj *key, int mode, getSwapsResult *result);
void setupSwappingClientsWk(redisDb *db, robj *key, void *scs);
void *lookupSwappingClientsWk(redisDb *db, robj *key);
void *getComplementSwapsWk(redisDb *db, robj *key, int mode, int *type, getSwapsResult *result, complementObjectFunc *comp, void **pd);

#endif
