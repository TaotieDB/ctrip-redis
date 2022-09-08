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

#include "ctrip_swap.h"

/*-----------------------------------------------------------------------------
 * db.evict related API
 *----------------------------------------------------------------------------*/

void dbSetDirty(redisDb *db, robj *key) {
    robj *o = lookupKey(db,key,LOOKUP_NOTOUCH);
    if (o) setObjectDirty(o);
}

int objectIsDirty(robj *o) {
    return o->dirty;
}

int buildObjectMeta(objectMetaType *omtype, const char *extend,
        size_t extlen, OUT objectMeta **pobject_meta) {
    objectMeta *object_meta;

    if (omtype == NULL || omtype->decodeObjectMeta == NULL || extend == NULL) {
        if (pobject_meta) *pobject_meta = NULL;
        return 0;
    }

    object_meta = zmalloc(sizeof(objectMeta));
    if (omtype->decodeObjectMeta(object_meta,extend,extlen)) {
        zfree(object_meta);
        if (pobject_meta) *pobject_meta = NULL;
        return -1;
    }

    if (pobject_meta) *pobject_meta = object_meta;
    return 0;
}

objectMeta *createLenObjectMeta(int object_type, size_t len) {
	objectMeta *m = zmalloc(sizeof(objectMeta));
    m->object_type = object_type;
    m->reserved = 0;
	m->len = len;
	return m;
}

objectMeta *createPtrObjectMeta(int object_type, void *ptr) {
	objectMeta *m = zmalloc(sizeof(objectMeta));
    m->object_type = object_type;
	m->ptr = (unsigned long)ptr;
	return m;
}

static inline objectMetaType *getObjectMetaType(objectMeta *object_meta) {
    objectMetaType *omtype = NULL;
    switch (object_meta->object_type) {
    case OBJ_STRING:
        omtype = NULL;
        break;
    case OBJ_HASH:
    case OBJ_SET:
    case OBJ_ZSET:
        omtype = &lenObjectMetaType;
        break;
    case OBJ_LIST:
        omtype = NULL; //TODO impl
        break;
    default:
        break;
    }
    return omtype;
}

void freeObjectMeta(objectMeta *object_meta) {
    objectMetaType *omtype;
    if (object_meta == NULL) return;
    omtype = getObjectMetaType(object_meta);
    if (omtype != NULL && omtype->free) omtype->free(object_meta);
    zfree(object_meta);
}

objectMeta *dupObjectMeta(objectMeta *object_meta) {
    objectMeta *dup_meta;
    objectMetaType *omtype;
    if (object_meta == NULL) return NULL;
    omtype = getObjectMetaType(object_meta);
    dup_meta = zmalloc(sizeof(objectMeta));
    memcpy(dup_meta,object_meta,sizeof(objectMeta));
    if (omtype != NULL && omtype->duplicate) omtype->duplicate(dup_meta,object_meta);
    return dup_meta;
}

sds encodeLenObjectMeta(struct objectMeta *object_meta) {
    return rocksEncodeObjectMetaLen(object_meta->len);
}

int decodeLenObjectMeta(struct objectMeta *object_meta, const char *extend, size_t extlen) {
    long len = rocksDecodeObjectMetaLen(extend,extlen);
    if (len < 0) return -1;
    object_meta->len = len;
    return 0;
}

int lenObjectMetaIsHot(objectMeta *object_meta, robj *value) {
    if (value == NULL) return 0;
    if (object_meta && object_meta->len > 0) return 0;
    return 1;
}

objectMetaType lenObjectMetaType = {
    .encodeObjectMeta = encodeLenObjectMeta,
    .decodeObjectMeta = decodeLenObjectMeta,
    .objectIsHot = lenObjectMetaIsHot,
    .free = NULL,
    .duplicate = NULL,
};

/* Note that db.meta is a satellite dict just like db.expire. */ 
/* Db->meta */
int dictExpandAllowed(size_t moreMem, double usedRatio);

void dictObjectMetaFree(void *privdata, void *val) {
    DICT_NOTUSED(privdata);
    freeObjectMeta(val);
}

dictType objectMetaDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    dictObjectMetaFree,         /* val destructor */
    dictExpandAllowed           /* allow to expand */
};

objectMeta *lookupMeta(redisDb *db, robj *key) {
    return dictFetchValue(db->meta,key->ptr);
}

void dbAddMeta(redisDb *db, robj *key, objectMeta *m) {
    dictEntry *kde;
    kde = dictFind(db->dict,key->ptr);
    serverAssertWithInfo(NULL,key,kde != NULL);
    serverAssert(dictAdd(db->meta,dictGetKey(kde),m) == DICT_OK);
}

int dbDeleteMeta(redisDb *db, robj *key) {
    if (dictSize(db->meta) == 0) return 0;
    return dictDelete(db->meta,key->ptr) == DICT_OK ? 1 : 0;
}

#ifdef REDIS_TEST
int swapObjectTest(int argc, char *argv[], int accurate) {
    initTestRedisServer();
    redisDb* db = server.db + 0;
    int error = 0;
    UNUSED(argc), UNUSED(argv), UNUSED(accurate);

    TEST("object: meta can be deleted specificly or by effect") {
        char *key1raw = "key1", *val1raw = "val1";
        robj *key1 = createStringObject(key1raw, strlen(key1raw)); 
        robj *val1 = createStringObject(val1raw, strlen(val1raw)); 

        dbAdd(db,key1,val1);
        dbAddMeta(db,key1,createHashObjectMeta(1));
        test_assert(lookupMeta(db,key1) != NULL);
        dbDeleteMeta(db,key1);
        test_assert(lookupMeta(db,key1) == NULL);
        dbAddMeta(db,key1,createHashObjectMeta(1));
        test_assert(lookupMeta(db,key1) != NULL);
        dbDelete(db,key1);
        test_assert(lookupMeta(db,key1) == NULL);
    }
    return error;
}
#endif