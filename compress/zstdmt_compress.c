#include <stdlib.h>   /* malloc */
#include <string.h>   /* memcpy */
#include <pool.h>     /* threadpool */
#include "threading.h"  /* mutex */
#include "zstd_internal.h"   /* MIN, ERROR, ZSTD_* */
#include "zstdmt_compress.h"

#if 0
#  include <stdio.h>
#  include <unistd.h>
#  include <sys/times.h>
   static unsigned g_debugLevel = 2;
#  define DEBUGLOG(l, ...) if (l<=g_debugLevel) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, " \n"); }

static unsigned long long GetCurrentClockTimeMicroseconds()
{
   static clock_t _ticksPerSecond = 0;
   if (_ticksPerSecond <= 0) _ticksPerSecond = sysconf(_SC_CLK_TCK);

   struct tms junk; clock_t newTicks = (clock_t) times(&junk);
   return ((((unsigned long long)newTicks)*(1000000))/_ticksPerSecond);
}

#define MUTEX_WAIT_TIME_DLEVEL 5
#define PTHREAD_MUTEX_LOCK(mutex) \
if (g_debugLevel>=MUTEX_WAIT_TIME_DLEVEL) { \
   unsigned long long beforeTime = GetCurrentClockTimeMicroseconds(); \
   pthread_mutex_lock(mutex); \
   unsigned long long afterTime = GetCurrentClockTimeMicroseconds(); \
   unsigned long long elapsedTime = (afterTime-beforeTime); \
   if (elapsedTime > 1000) {  /* or whatever threshold you like; I'm using 1 millisecond here */ \
      DEBUGLOG(MUTEX_WAIT_TIME_DLEVEL, "Thread took %llu microseconds to acquire mutex %s \n", \
               elapsedTime, #mutex); \
  } \
} else pthread_mutex_lock(mutex);

#else

#  define DEBUGLOG(l, ...)   /* disabled */
#  define PTHREAD_MUTEX_LOCK(m) pthread_mutex_lock(m)

#endif


#define ZSTDMT_NBTHREADS_MAX 128

/* =====   Buffer Pool   ===== */

typedef struct buffer_s {
    void* start;
    size_t size;
} buffer_t;

typedef struct ZSTDMT_bufferPool_s {
    unsigned totalBuffers;;
    unsigned nbBuffers;
    buffer_t bTable[1];   /* variable size */
} ZSTDMT_bufferPool;

static ZSTDMT_bufferPool* ZSTDMT_createBufferPool(unsigned nbThreads)
{
    unsigned const maxNbBuffers = 2*nbThreads + 2;
    ZSTDMT_bufferPool* const bufPool = (ZSTDMT_bufferPool*)calloc(1, sizeof(ZSTDMT_bufferPool) + maxNbBuffers * sizeof(buffer_t));
    if (bufPool==NULL) return NULL;
    bufPool->totalBuffers = maxNbBuffers;
    return bufPool;
}

static void ZSTDMT_freeBufferPool(ZSTDMT_bufferPool* bufPool)
{
    unsigned u;
    if (!bufPool) return;   /* compatibility with free on NULL */
    for (u=0; u<bufPool->totalBuffers; u++)
        free(bufPool->bTable[u].start);
    free(bufPool);
}

/* assumption : invocation from main thread only ! */
static buffer_t ZSTDMT_getBuffer(ZSTDMT_bufferPool* pool, size_t bSize)
{
    if (pool->nbBuffers) {   /* try to use an existing buffer */
        buffer_t const buf = pool->bTable[--(pool->nbBuffers)];
        size_t const availBufferSize = buf.size;
        if ((availBufferSize >= bSize) & (availBufferSize <= 10*bSize))   /* large enough, but not too much */
            return buf;
        free(buf.start);   /* size conditions not respected : scratch this buffer and create a new one */
    }
    /* create new buffer */
    {   void* const start = malloc(bSize);
        if (start==NULL) bSize = 0;
        return (buffer_t) { start, bSize };   /* note : start can be NULL if malloc fails ! */
    }
}

/* store buffer for later re-use, up to pool capacity */
static void ZSTDMT_releaseBuffer(ZSTDMT_bufferPool* pool, buffer_t buf)
{
    if (buf.start == NULL) return;   /* release on NULL */
    if (pool->nbBuffers < pool->totalBuffers) {
        pool->bTable[pool->nbBuffers++] = buf;   /* store for later re-use */
        return;
    }
    /* Reached bufferPool capacity (should not happen) */
    free(buf.start);
}


/* =====   CCtx Pool   ===== */

typedef struct {
    unsigned totalCCtx;
    unsigned availCCtx;
    ZSTD_CCtx* cctx[1];   /* variable size */
} ZSTDMT_CCtxPool;

/* assumption : CCtxPool invocation only from main thread */

/* note : all CCtx borrowed from the pool should be released back to the pool _before_ freeing the pool */
static void ZSTDMT_freeCCtxPool(ZSTDMT_CCtxPool* pool)
{
    unsigned u;
    for (u=0; u<pool->availCCtx; u++)  /* note : availCCtx is supposed == totalCCtx; otherwise, some CCtx are still in use */
        ZSTD_freeCCtx(pool->cctx[u]);
    free(pool);
}

static ZSTDMT_CCtxPool* ZSTDMT_createCCtxPool(unsigned nbThreads)
{
    ZSTDMT_CCtxPool* const cctxPool = (ZSTDMT_CCtxPool*) calloc(1, sizeof(ZSTDMT_CCtxPool) + nbThreads*sizeof(ZSTD_CCtx*));
    if (!cctxPool) return NULL;
    {   unsigned threadNb;
        for (threadNb=0; threadNb<nbThreads; threadNb++) {
            cctxPool->cctx[threadNb] = ZSTD_createCCtx();
            if (cctxPool->cctx[threadNb]==NULL) {   /* failed cctx allocation : abort cctxPool creation */
                cctxPool->totalCCtx = cctxPool->availCCtx = threadNb;
                ZSTDMT_freeCCtxPool(cctxPool);
                return NULL;
    }   }   }
    cctxPool->totalCCtx = cctxPool->availCCtx = nbThreads;
    return cctxPool;
}

static ZSTD_CCtx* ZSTDMT_getCCtx(ZSTDMT_CCtxPool* pool)
{
    if (pool->availCCtx) {
        pool->availCCtx--;
        return pool->cctx[pool->availCCtx];
    }
    /* note : should not be possible, since totalCCtx==nbThreads */
    return ZSTD_createCCtx();   /* note : can be NULL is creation fails ! */
}

static void ZSTDMT_releaseCCtx(ZSTDMT_CCtxPool* pool, ZSTD_CCtx* cctx)
{
    if (cctx==NULL) return;   /* release on NULL */
    if (pool->availCCtx < pool->totalCCtx)
        pool->cctx[pool->availCCtx++] = cctx;
    else
    /* note : should not be possible, since totalCCtx==nbThreads */
        ZSTD_freeCCtx(cctx);
}


/* =====   Thread worker   ===== */

typedef struct {
    ZSTD_CCtx* cctx;
    const void* srcStart;
    size_t srcSize;
    buffer_t dstBuff;
    size_t cSize;
    size_t dstFlushed;
    unsigned long long fullFrameSize;
    unsigned firstChunk;
    unsigned lastChunk;
    unsigned jobCompleted;
    pthread_mutex_t* jobCompleted_mutex;
    pthread_cond_t* jobCompleted_cond;
    ZSTD_parameters params;
} ZSTDMT_jobDescription;

/* ZSTDMT_compressChunk() : POOL_function type */
void ZSTDMT_compressChunk(void* jobDescription)
{
    ZSTDMT_jobDescription* const job = (ZSTDMT_jobDescription*)jobDescription;
    buffer_t dstBuff = job->dstBuff;
    size_t hSize = ZSTD_compressBegin_advanced(job->cctx, NULL, 0, job->params, job->fullFrameSize);
    if (ZSTD_isError(hSize)) { job->cSize = hSize; goto _endJob; }
    hSize = ZSTD_compressContinue(job->cctx, dstBuff.start, dstBuff.size, job->srcStart, 0);   /* flush frame header */
    if (ZSTD_isError(hSize)) { job->cSize = hSize; goto _endJob; }
    if (job->firstChunk) {   /* preserve frame header when it is first chunk */
        dstBuff.start = (char*)dstBuff.start + hSize;
        dstBuff.size -= hSize;
    } else                  /* otherwise, overwrite */
        hSize = 0;

    job->cSize = (job->lastChunk) ?   /* last chunk signal */
                 ZSTD_compressEnd(job->cctx, dstBuff.start, dstBuff.size, job->srcStart, job->srcSize) :
                 ZSTD_compressContinue(job->cctx, dstBuff.start, dstBuff.size, job->srcStart, job->srcSize);
    if (!ZSTD_isError(job->cSize)) job->cSize += hSize;
    DEBUGLOG(5, "chunk %u : compressed %u bytes into %u bytes  ", (unsigned)job->lastChunk, (unsigned)job->srcSize, (unsigned)job->cSize);

_endJob:
    PTHREAD_MUTEX_LOCK(job->jobCompleted_mutex);
    job->jobCompleted = 1;
    pthread_cond_signal(job->jobCompleted_cond);
    pthread_mutex_unlock(job->jobCompleted_mutex);
}


/* =====   Multi-threaded compression   ===== */

struct ZSTDMT_CCtx_s {
    POOL_ctx* factory;
    ZSTDMT_bufferPool* buffPool;
    ZSTDMT_CCtxPool* cctxPool;
    unsigned nbThreads;
    pthread_mutex_t jobCompleted_mutex;
    pthread_cond_t jobCompleted_cond;
    ZSTDMT_jobDescription jobs[1];   /* variable size */
};

ZSTDMT_CCtx *ZSTDMT_createCCtx(unsigned nbThreads)
{
    ZSTDMT_CCtx* cctx;
    if ((nbThreads < 1) | (nbThreads > ZSTDMT_NBTHREADS_MAX)) return NULL;
    cctx = (ZSTDMT_CCtx*) calloc(1, sizeof(ZSTDMT_CCtx) + nbThreads*sizeof(ZSTDMT_jobDescription));
    if (!cctx) return NULL;
    cctx->nbThreads = nbThreads;
    cctx->factory = POOL_create(nbThreads, 1);
    cctx->buffPool = ZSTDMT_createBufferPool(nbThreads);
    cctx->cctxPool = ZSTDMT_createCCtxPool(nbThreads);
    if (!cctx->factory | !cctx->buffPool | !cctx->cctxPool) {  /* one object was not created */
        ZSTDMT_freeCCtx(cctx);
        return NULL;
    }
    pthread_mutex_init(&cctx->jobCompleted_mutex, NULL);   /* Todo : check init function return */
    pthread_cond_init(&cctx->jobCompleted_cond, NULL);
    return cctx;
}

size_t ZSTDMT_freeCCtx(ZSTDMT_CCtx* mtctx)
{
    POOL_free(mtctx->factory);
    ZSTDMT_freeBufferPool(mtctx->buffPool);
    ZSTDMT_freeCCtxPool(mtctx->cctxPool);
    pthread_mutex_destroy(&mtctx->jobCompleted_mutex);
    pthread_cond_destroy(&mtctx->jobCompleted_cond);
    free(mtctx);
    return 0;
}


size_t ZSTDMT_compressCCtx(ZSTDMT_CCtx* mtctx,
                           void* dst, size_t dstCapacity,
                     const void* src, size_t srcSize,
                           int compressionLevel)
{
    ZSTD_parameters params = ZSTD_getParams(compressionLevel, srcSize, 0);
    size_t const chunkTargetSize = (size_t)1 << (params.cParams.windowLog + 2);
    unsigned const nbChunksMax = (unsigned)(srcSize / chunkTargetSize) + (srcSize < chunkTargetSize) /* min 1 */;
    unsigned nbChunks = MIN(nbChunksMax, mtctx->nbThreads);
    size_t const proposedChunkSize = (srcSize + (nbChunks-1)) / nbChunks;
    size_t const avgChunkSize = ((proposedChunkSize & 0x1FFFF) < 0xFFFF) ? proposedChunkSize + 0xFFFF : proposedChunkSize;   /* avoid too small last block */
    size_t remainingSrcSize = srcSize;
    const char* const srcStart = (const char*)src;
    size_t frameStartPos = 0;

    DEBUGLOG(3, "windowLog : %2u => chunkTargetSize : %u bytes  ", params.cParams.windowLog, (U32)chunkTargetSize);
    DEBUGLOG(2, "nbChunks  : %2u   (chunkSize : %u bytes)   ", nbChunks, (U32)avgChunkSize);
    params.fParams.contentSizeFlag = 1;

    {   unsigned u;
        for (u=0; u<nbChunks; u++) {
            size_t const chunkSize = MIN(remainingSrcSize, avgChunkSize);
            size_t const dstBufferCapacity = u ? ZSTD_compressBound(chunkSize) : dstCapacity;
            buffer_t const dstBuffer = u ? ZSTDMT_getBuffer(mtctx->buffPool, dstBufferCapacity) : (buffer_t){ dst, dstCapacity };
            ZSTD_CCtx* const cctx = ZSTDMT_getCCtx(mtctx->cctxPool);

            if ((cctx==NULL) || (dstBuffer.start==NULL)) {
                mtctx->jobs[u].cSize = ERROR(memory_allocation);   /* job result */
                mtctx->jobs[u].jobCompleted = 1;
                nbChunks = u+1;
                break;   /* let's wait for previous jobs to complete, but don't start new ones */
            }

            mtctx->jobs[u].srcStart = srcStart + frameStartPos;
            mtctx->jobs[u].srcSize = chunkSize;
            mtctx->jobs[u].fullFrameSize = srcSize;
            mtctx->jobs[u].params = params;
            mtctx->jobs[u].dstBuff = dstBuffer;
            mtctx->jobs[u].cctx = cctx;
            mtctx->jobs[u].firstChunk = (u==0);
            mtctx->jobs[u].lastChunk = (u==nbChunks-1);
            mtctx->jobs[u].jobCompleted = 0;
            mtctx->jobs[u].jobCompleted_mutex = &mtctx->jobCompleted_mutex;
            mtctx->jobs[u].jobCompleted_cond = &mtctx->jobCompleted_cond;

            DEBUGLOG(3, "posting job %u   (%u bytes)", u, (U32)chunkSize);
            POOL_add(mtctx->factory, ZSTDMT_compressChunk, &mtctx->jobs[u]);

            frameStartPos += chunkSize;
            remainingSrcSize -= chunkSize;
    }   }
    /* note : since nbChunks <= nbThreads, all jobs should be running immediately in parallel */

    {   unsigned chunkID;
        size_t error = 0, dstPos = 0;
        for (chunkID=0; chunkID<nbChunks; chunkID++) {
            DEBUGLOG(3, "ready to write chunk %u ", chunkID);

            PTHREAD_MUTEX_LOCK(&mtctx->jobCompleted_mutex);
            while (mtctx->jobs[chunkID].jobCompleted==0) {
                DEBUGLOG(4, "waiting for jobCompleted signal from chunk %u", chunkID);
                pthread_cond_wait(&mtctx->jobCompleted_cond, &mtctx->jobCompleted_mutex);
            }
            pthread_mutex_unlock(&mtctx->jobCompleted_mutex);

            ZSTDMT_releaseCCtx(mtctx->cctxPool, mtctx->jobs[chunkID].cctx);
            {   size_t const cSize = mtctx->jobs[chunkID].cSize;
                if (ZSTD_isError(cSize)) error = cSize;
                if ((!error) && (dstPos + cSize > dstCapacity)) error = ERROR(dstSize_tooSmall);
                if (chunkID) {   /* note : chunk 0 is already written directly into dst */
                    if (!error) memcpy((char*)dst + dstPos, mtctx->jobs[chunkID].dstBuff.start, cSize);
                    ZSTDMT_releaseBuffer(mtctx->buffPool, mtctx->jobs[chunkID].dstBuff);
                }
                dstPos += cSize ;
            }
        }
        if (!error) DEBUGLOG(3, "compressed size : %u  ", (U32)dstPos);
        return error ? error : dstPos;
    }

}


/* ====================================== */
/* =======      Streaming API     ======= */
/* ====================================== */

#if 0

size_t ZSTDMT_initCStream(ZSTDMT_CCtx* zcs, int compressionLevel) {
    zcs->params = ZSTD_getParams(compressionLevel, 0, 0);
    zcs->targetSectionSize = 1 << (zcs->params.cParams.windowLog + 2);
    zcs->inBuffSize = 5 * (1 << zcs->params.cParams.windowLog);
    zcs->inBuff.buffer = ZSTDMT_getBuffer(zcs->buffPool, zcs->inBuffSize);   /* check for NULL ! */
    zcs->inBuff.current = 0;
    zcs->doneJobID = 0;
    zcs->nextJobID = 0;
    return 0;
}

typedef struct {
    buffer_t buffer;
    unsigned current;
} inBuff_t;


size_t ZSTDMT_compressStream(ZSTDMT_CCtx* zcs, ZSTD_outBuffer* output, ZSTD_inBuffer* input)
{
    /* fill input buffer */
    {   size_t const toLoad = MIN(input->size - input->pos, zcs->inBuffSize - zcs->inBuff.current);
        memcpy((char*)zcs->inBuff.buffer.start + zcs->inBuff.current, input->src, toLoad);
        input->pos += toLoad;
    }

    if (zcs->inBuff.current == zcs->inBuffSize) {   /* filled enough : let's compress */
        size_t const dstBufferCapacity = ZSTD_compressBound(zcs->targetSectionSize);
        buffer_t const dstBuffer = ZSTDMT_getBuffer(zcs->buffPool, zcs->targetSectionSize); /* should check for NULL */
        ZSTD_CCtx* const cctx = ZSTDMT_getCCtx(zcs->cctxPool);   /* should check for NULL */
        unsigned const jobID = zcs->nextJobID & zcs->jobIDmask;

        zcs->jobs[jobID].srcStart = zcs->inBuff.start;
        zcs->jobs[jobID].srcSize = zcs->targetSectionSize;
        zcs->jobs[jobID].fullFrameSize = 0;
        zcs->jobs[jobID].compressionLevel = zcs->compressionLevel;
        zcs->jobs[jobID].dstBuff = dstBuffer;
        zcs->jobs[jobID].cctx = cctx;
        zcs->jobs[jobID].frameID = (jobID>0);
        zcs->jobs[jobID].jobCompleted = 0;
        zcs->jobs[jobID].dstFlushed = 0;
        zcs->jobs[jobID].jobCompleted_mutex = &zcs->jobCompleted_mutex;
        zcs->jobs[jobID].jobCompleted_cond = &zcs->jobCompleted_cond;

        /* get a new buffer for next input - save remaining into it */
        zcs->inBuff.buffer = ZSTDMT_getBuffer(zcs->buffPool, zcs->inBuffSize);   /* check for NULL ! */
        zcs->inBuff.current = zcs->inBuffSize - zcs->targetSectionSize;
        memcpy(zcs->inBuff.buffer.start, (char*)zcs->jobs[jobID].srcStart + zcs->targetSectionSize, zcs->inBuff.current);

        DEBUGLOG(3, "posting job %u   (%u bytes)", jobID, (U32)zcs->jobs[jobID].srcSize);
        POOL_add(zcs->factory, ZSTDMT_compressChunk, &zcs->jobs[jobID]);
        zcs->nextJobID++;
    }

    /* check if there is any data available to flush */
    {   unsigned const jobID = zcs->doneJobID & zcs->jobIDmask;
        ZSTDMT_jobDescription job = zcs->jobs[jobID];
        if (job.jobCompleted) {   /* job completed : output can be flushed */
            size_t const toWrite = MIN(job.cSize - job.dstFlushed, output->size - output->pos);
            ZSTDMT_releaseCCtx(zcs->cctxPool, job.cctx); zcs->jobs[jobID].cctx = NULL;   /* release cctx for future task */
            free(job.srcStart); zcs->jobs[jobID].srcStart = NULL; /* note : need a buff_t for release */
            memcpy((char*)output->dst + output->pos, job.dstBuff.start + job.dstFlushed, toWrite);
            output->pos += toWrite;
            job.dstFlushed += toWrite;
            if (job.dstFlushed == job.cSize) {   /* output buffer fully flushed => next one */
                ZSTDMT_releaseBuffer(zcs->buffPool, job.dstBuff);
                zcs->doneJobID++;
            } else
                zcs->jobs[jobID].dstFlushed = job.dstFlushed;
    }   }

    /* recommended next input size : fill current input buffer */
    return zcs->inBuffSize - zcs->inBuff.current;
}

size_t ZSTDMT_flushStream(ZSTDMT_CCtx* zcs, ZSTD_outBuffer* output);
size_t ZSTDMT_endStream(ZSTDMT_CCtx* zcs, ZSTD_outBuffer* output);

#endif
