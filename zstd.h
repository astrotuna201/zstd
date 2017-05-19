/*
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#if defined (__cplusplus)
extern "C" {
#endif

#ifndef ZSTD_H_235446
#define ZSTD_H_235446

/* ======   Dependency   ======*/
#include <stddef.h>   /* size_t */


/* =====   ZSTDLIB_API : control library symbols visibility   ===== */
#if defined(__GNUC__) && (__GNUC__ >= 4)
#  define ZSTDLIB_VISIBILITY __attribute__ ((visibility ("default")))
#else
#  define ZSTDLIB_VISIBILITY
#endif
#if defined(ZSTD_DLL_EXPORT) && (ZSTD_DLL_EXPORT==1)
#  define ZSTDLIB_API __declspec(dllexport) ZSTDLIB_VISIBILITY
#elif defined(ZSTD_DLL_IMPORT) && (ZSTD_DLL_IMPORT==1)
#  define ZSTDLIB_API __declspec(dllimport) ZSTDLIB_VISIBILITY /* It isn't required but allows to generate better code, saving a function pointer load from the IAT and an indirect jump.*/
#else
#  define ZSTDLIB_API ZSTDLIB_VISIBILITY
#endif


/*******************************************************************************************************
  Introduction

  zstd, short for Zstandard, is a fast lossless compression algorithm,
  targeting real-time compression scenarios at zlib-level and better compression ratios.
  The zstd compression library provides in-memory compression and decompression functions.
  The library supports compression levels from 1 up to ZSTD_maxCLevel() which is currently 22.
  Levels >= 20, labeled `--ultra`, should be used with caution, as they require more memory.
  Compression can be done in:
    - a single step (described as Simple API)
    - a single step, reusing a context (described as Explicit memory management)
    - unbounded multiple steps (described as Streaming compression)
  The compression ratio achievable on small data can be highly improved using compression with a dictionary in:
    - a single step (described as Simple dictionary API)
    - a single step, reusing a dictionary (described as Fast dictionary API)

  Advanced experimental functions can be accessed using #define ZSTD_STATIC_LINKING_ONLY before including zstd.h.
  These APIs shall never be used with a dynamic library.
  They are not "stable", their definition may change in the future. Only static linking is allowed.
*********************************************************************************************************/

/*------   Version   ------*/
#define ZSTD_VERSION_MAJOR    1
#define ZSTD_VERSION_MINOR    3
#define ZSTD_VERSION_RELEASE  0

#define ZSTD_VERSION_NUMBER  (ZSTD_VERSION_MAJOR *100*100 + ZSTD_VERSION_MINOR *100 + ZSTD_VERSION_RELEASE)
ZSTDLIB_API unsigned ZSTD_versionNumber(void);   /**< to be used when checking dll version */

#define ZSTD_LIB_VERSION ZSTD_VERSION_MAJOR.ZSTD_VERSION_MINOR.ZSTD_VERSION_RELEASE
#define ZSTD_QUOTE(str) #str
#define ZSTD_EXPAND_AND_QUOTE(str) ZSTD_QUOTE(str)
#define ZSTD_VERSION_STRING ZSTD_EXPAND_AND_QUOTE(ZSTD_LIB_VERSION)
ZSTDLIB_API const char* ZSTD_versionString(void);


/***************************************
*  Simple API
***************************************/
/*! ZSTD_compress() :
 *  Compresses `src` content as a single zstd compressed frame into already allocated `dst`.
 *  Hint : compression runs faster if `dstCapacity` >=  `ZSTD_compressBound(srcSize)`.
 *  @return : compressed size written into `dst` (<= `dstCapacity),
 *            or an error code if it fails (which can be tested using ZSTD_isError()). */
ZSTDLIB_API size_t ZSTD_compress( void* dst, size_t dstCapacity,
                            const void* src, size_t srcSize,
                                  int compressionLevel);

/*! ZSTD_decompress() :
 *  `compressedSize` : must be the _exact_ size of some number of compressed and/or skippable frames.
 *  `dstCapacity` is an upper bound of originalSize.
 *  If user cannot imply a maximum upper bound, it's better to use streaming mode to decompress data.
 *  @return : the number of bytes decompressed into `dst` (<= `dstCapacity`),
 *            or an errorCode if it fails (which can be tested using ZSTD_isError()). */
ZSTDLIB_API size_t ZSTD_decompress( void* dst, size_t dstCapacity,
                              const void* src, size_t compressedSize);

/*! ZSTD_getDecompressedSize() :
 *  NOTE: This function is planned to be obsolete, in favor of ZSTD_getFrameContentSize().
 *  ZSTD_getFrameContentSize() works the same way,
 *  returning the decompressed size of a single frame,
 *  but distinguishes empty frames from frames with an unknown size, or errors.
 *
 *  'src' is the start of a zstd compressed frame.
 *  @return : content size to be decompressed, as a 64-bits value _if known_, 0 otherwise.
 *   note 1 : decompressed size is an optional field, it may not be present, typically in streaming mode.
 *            When `return==0`, data to decompress could be any size.
 *            In which case, it's necessary to use streaming mode to decompress data.
 *            Optionally, application can use ZSTD_decompress() while relying on implied limits.
 *            (For example, data may be necessarily cut into blocks <= 16 KB).
 *   note 2 : decompressed size is always present when compression is done with ZSTD_compress()
 *   note 3 : decompressed size can be very large (64-bits value),
 *            potentially larger than what local system can handle as a single memory segment.
 *            In which case, it's necessary to use streaming mode to decompress data.
 *   note 4 : If source is untrusted, decompressed size could be wrong or intentionally modified.
 *            Always ensure result fits within application's authorized limits.
 *            Each application can set its own limits.
 *   note 5 : when `return==0`, if precise failure cause is needed, use ZSTD_getFrameParams() to know more. */
ZSTDLIB_API unsigned long long ZSTD_getDecompressedSize(const void* src, size_t srcSize);


/*======  Helper functions  ======*/
ZSTDLIB_API int         ZSTD_maxCLevel(void);               /*!< maximum compression level available */
ZSTDLIB_API size_t      ZSTD_compressBound(size_t srcSize); /*!< maximum compressed size in worst case scenario */
ZSTDLIB_API unsigned    ZSTD_isError(size_t code);          /*!< tells if a `size_t` function result is an error code */
ZSTDLIB_API const char* ZSTD_getErrorName(size_t code);     /*!< provides readable string from an error code */


/***************************************
*  Explicit memory management
***************************************/
/*= Compression context
 *  When compressing many times,
 *  it is recommended to allocate a context just once, and re-use it for each successive compression operation.
 *  This will make workload friendlier for system's memory.
 *  Use one context per thread for parallel execution in multi-threaded environments. */
typedef struct ZSTD_CCtx_s ZSTD_CCtx;
ZSTDLIB_API ZSTD_CCtx* ZSTD_createCCtx(void);
ZSTDLIB_API size_t     ZSTD_freeCCtx(ZSTD_CCtx* cctx);

/*! ZSTD_compressCCtx() :
 *  Same as ZSTD_compress(), requires an allocated ZSTD_CCtx (see ZSTD_createCCtx()). */
ZSTDLIB_API size_t ZSTD_compressCCtx(ZSTD_CCtx* ctx,
                                     void* dst, size_t dstCapacity,
                               const void* src, size_t srcSize,
                                     int compressionLevel);

/*= Decompression context
 *  When decompressing many times,
 *  it is recommended to allocate a context only once,
 *  and re-use it for each successive compression operation.
 *  This will make workload friendlier for system's memory.
 *  Use one context per thread for parallel execution. */
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
ZSTDLIB_API ZSTD_DCtx* ZSTD_createDCtx(void);
ZSTDLIB_API size_t     ZSTD_freeDCtx(ZSTD_DCtx* dctx);

/*! ZSTD_decompressDCtx() :
 *  Same as ZSTD_decompress(), requires an allocated ZSTD_DCtx (see ZSTD_createDCtx()) */
ZSTDLIB_API size_t ZSTD_decompressDCtx(ZSTD_DCtx* ctx,
                                       void* dst, size_t dstCapacity,
                                 const void* src, size_t srcSize);


/**************************
*  Simple dictionary API
***************************/
/*! ZSTD_compress_usingDict() :
*   Compression using a predefined Dictionary (see dictBuilder/zdict.h).
*   Note : This function loads the dictionary, resulting in significant startup delay.
*   Note : When `dict == NULL || dictSize < 8` no dictionary is used. */
ZSTDLIB_API size_t ZSTD_compress_usingDict(ZSTD_CCtx* ctx,
                                           void* dst, size_t dstCapacity,
                                     const void* src, size_t srcSize,
                                     const void* dict,size_t dictSize,
                                           int compressionLevel);

/*! ZSTD_decompress_usingDict() :
*   Decompression using a predefined Dictionary (see dictBuilder/zdict.h).
*   Dictionary must be identical to the one used during compression.
*   Note : This function loads the dictionary, resulting in significant startup delay.
*   Note : When `dict == NULL || dictSize < 8` no dictionary is used. */
ZSTDLIB_API size_t ZSTD_decompress_usingDict(ZSTD_DCtx* dctx,
                                             void* dst, size_t dstCapacity,
                                       const void* src, size_t srcSize,
                                       const void* dict,size_t dictSize);


/****************************
*  Fast dictionary API
****************************/
typedef struct ZSTD_CDict_s ZSTD_CDict;

/*! ZSTD_createCDict() :
*   When compressing multiple messages / blocks with the same dictionary, it's recommended to load it just once.
*   ZSTD_createCDict() will create a digested dictionary, ready to start future compression operations without startup delay.
*   ZSTD_CDict can be created once and used by multiple threads concurrently, as its usage is read-only.
*   `dictBuffer` can be released after ZSTD_CDict creation, as its content is copied within CDict */
ZSTDLIB_API ZSTD_CDict* ZSTD_createCDict(const void* dictBuffer, size_t dictSize,
                                         int compressionLevel);

/*! ZSTD_freeCDict() :
*   Function frees memory allocated by ZSTD_createCDict(). */
ZSTDLIB_API size_t      ZSTD_freeCDict(ZSTD_CDict* CDict);

/*! ZSTD_compress_usingCDict() :
 *  Compression using a digested Dictionary.
 *  Faster startup than ZSTD_compress_usingDict(), recommended when same dictionary is used multiple times.
 *  Note that compression level is decided during dictionary creation.
 *  Frame parameters are hardcoded (dictID=yes, contentSize=yes, checksum=no) */
ZSTDLIB_API size_t ZSTD_compress_usingCDict(ZSTD_CCtx* cctx,
                                            void* dst, size_t dstCapacity,
                                      const void* src, size_t srcSize,
                                      const ZSTD_CDict* cdict);


typedef struct ZSTD_DDict_s ZSTD_DDict;

/*! ZSTD_createDDict() :
*   Create a digested dictionary, ready to start decompression operation without startup delay.
*   dictBuffer can be released after DDict creation, as its content is copied inside DDict */
ZSTDLIB_API ZSTD_DDict* ZSTD_createDDict(const void* dictBuffer, size_t dictSize);

/*! ZSTD_freeDDict() :
*   Function frees memory allocated with ZSTD_createDDict() */
ZSTDLIB_API size_t      ZSTD_freeDDict(ZSTD_DDict* ddict);

/*! ZSTD_decompress_usingDDict() :
*   Decompression using a digested Dictionary.
*   Faster startup than ZSTD_decompress_usingDict(), recommended when same dictionary is used multiple times. */
ZSTDLIB_API size_t ZSTD_decompress_usingDDict(ZSTD_DCtx* dctx,
                                              void* dst, size_t dstCapacity,
                                        const void* src, size_t srcSize,
                                        const ZSTD_DDict* ddict);


/****************************
*  Streaming
****************************/

typedef struct ZSTD_inBuffer_s {
  const void* src;    /**< start of input buffer */
  size_t size;        /**< size of input buffer */
  size_t pos;         /**< position where reading stopped. Will be updated. Necessarily 0 <= pos <= size */
} ZSTD_inBuffer;

typedef struct ZSTD_outBuffer_s {
  void*  dst;         /**< start of output buffer */
  size_t size;        /**< size of output buffer */
  size_t pos;         /**< position where writing stopped. Will be updated. Necessarily 0 <= pos <= size */
} ZSTD_outBuffer;



/*-***********************************************************************
*  Streaming compression - HowTo
*
*  A ZSTD_CStream object is required to track streaming operation.
*  Use ZSTD_createCStream() and ZSTD_freeCStream() to create/release resources.
*  ZSTD_CStream objects can be reused multiple times on consecutive compression operations.
*  It is recommended to re-use ZSTD_CStream in situations where many streaming operations will be achieved consecutively,
*  since it will play nicer with system's memory, by re-using already allocated memory.
*  Use one separate ZSTD_CStream per thread for parallel execution.
*
*  Start a new compression by initializing ZSTD_CStream.
*  Use ZSTD_initCStream() to start a new compression operation.
*  Use ZSTD_initCStream_usingDict() or ZSTD_initCStream_usingCDict() for a compression which requires a dictionary (experimental section)
*
*  Use ZSTD_compressStream() repetitively to consume input stream.
*  The function will automatically update both `pos` fields.
*  Note that it may not consume the entire input, in which case `pos < size`,
*  and it's up to the caller to present again remaining data.
*  @return : a size hint, preferred nb of bytes to use as input for next function call
*            or an error code, which can be tested using ZSTD_isError().
*            Note 1 : it's just a hint, to help latency a little, any other value will work fine.
*            Note 2 : size hint is guaranteed to be <= ZSTD_CStreamInSize()
*
*  At any moment, it's possible to flush whatever data remains within internal buffer, using ZSTD_flushStream().
*  `output->pos` will be updated.
*  Note that some content might still be left within internal buffer if `output->size` is too small.
*  @return : nb of bytes still present within internal buffer (0 if it's empty)
*            or an error code, which can be tested using ZSTD_isError().
*
*  ZSTD_endStream() instructs to finish a frame.
*  It will perform a flush and write frame epilogue.
*  The epilogue is required for decoders to consider a frame completed.
*  ZSTD_endStream() may not be able to flush full data if `output->size` is too small.
*  In which case, call again ZSTD_endStream() to complete the flush.
*  @return : 0 if frame fully completed and fully flushed,
             or >0 if some data is still present within internal buffer
                  (value is minimum size estimation for remaining data to flush, but it could be more)
*            or an error code, which can be tested using ZSTD_isError().
*
* *******************************************************************/

typedef ZSTD_CCtx ZSTD_CStream;  /**< CCtx and CStream are effectively same object */
                                 /* Continue due distinghish them for compatibility with versions <= v1.2.0 */
/*===== ZSTD_CStream management functions =====*/
ZSTDLIB_API ZSTD_CStream* ZSTD_createCStream(void);
ZSTDLIB_API size_t ZSTD_freeCStream(ZSTD_CStream* zcs);

/*===== Streaming compression functions =====*/
ZSTDLIB_API size_t ZSTD_initCStream(ZSTD_CStream* zcs, int compressionLevel);
ZSTDLIB_API size_t ZSTD_compressStream(ZSTD_CStream* zcs, ZSTD_outBuffer* output, ZSTD_inBuffer* input);
ZSTDLIB_API size_t ZSTD_flushStream(ZSTD_CStream* zcs, ZSTD_outBuffer* output);
ZSTDLIB_API size_t ZSTD_endStream(ZSTD_CStream* zcs, ZSTD_outBuffer* output);

ZSTDLIB_API size_t ZSTD_CStreamInSize(void);    /**< recommended size for input buffer */
ZSTDLIB_API size_t ZSTD_CStreamOutSize(void);   /**< recommended size for output buffer. Guarantee to successfully flush at least one complete compressed block in all circumstances. */



/*-***************************************************************************
*  Streaming decompression - HowTo
*
*  A ZSTD_DStream object is required to track streaming operations.
*  Use ZSTD_createDStream() and ZSTD_freeDStream() to create/release resources.
*  ZSTD_DStream objects can be re-used multiple times.
*
*  Use ZSTD_initDStream() to start a new decompression operation,
*   or ZSTD_initDStream_usingDict() if decompression requires a dictionary.
*   @return : recommended first input size
*
*  Use ZSTD_decompressStream() repetitively to consume your input.
*  The function will update both `pos` fields.
*  If `input.pos < input.size`, some input has not been consumed.
*  It's up to the caller to present again remaining data.
*  If `output.pos < output.size`, decoder has flushed everything it could.
*  @return : 0 when a frame is completely decoded and fully flushed,
*            an error code, which can be tested using ZSTD_isError(),
*            any other value > 0, which means there is still some decoding to do to complete current frame.
*            The return value is a suggested next input size (a hint to improve latency) that will never load more than the current frame.
* *******************************************************************************/

typedef struct ZSTD_DStream_s ZSTD_DStream;
/*===== ZSTD_DStream management functions =====*/
ZSTDLIB_API ZSTD_DStream* ZSTD_createDStream(void);
ZSTDLIB_API size_t ZSTD_freeDStream(ZSTD_DStream* zds);

/*===== Streaming decompression functions =====*/
ZSTDLIB_API size_t ZSTD_initDStream(ZSTD_DStream* zds);
ZSTDLIB_API size_t ZSTD_decompressStream(ZSTD_DStream* zds, ZSTD_outBuffer* output, ZSTD_inBuffer* input);

ZSTDLIB_API size_t ZSTD_DStreamInSize(void);    /*!< recommended size for input buffer */
ZSTDLIB_API size_t ZSTD_DStreamOutSize(void);   /*!< recommended size for output buffer. Guarantee to successfully flush at least one complete block in all circumstances. */

#endif  /* ZSTD_H_235446 */


#if defined(ZSTD_STATIC_LINKING_ONLY) && !defined(ZSTD_H_ZSTD_STATIC_LINKING_ONLY)
#define ZSTD_H_ZSTD_STATIC_LINKING_ONLY

/****************************************************************************************
 * START OF ADVANCED AND EXPERIMENTAL FUNCTIONS
 * The definitions in this section are considered experimental.
 * They should never be used with a dynamic library, as they may change in the future.
 * They are provided for advanced usages.
 * Use them only in association with static linking.
 * ***************************************************************************************/

/* --- Constants ---*/
#define ZSTD_MAGICNUMBER            0xFD2FB528   /* >= v0.8.0 */
#define ZSTD_MAGIC_SKIPPABLE_START  0x184D2A50U

#define ZSTD_WINDOWLOG_MAX_32  27
#define ZSTD_WINDOWLOG_MAX_64  27
#define ZSTD_WINDOWLOG_MAX    ((unsigned)(sizeof(size_t) == 4 ? ZSTD_WINDOWLOG_MAX_32 : ZSTD_WINDOWLOG_MAX_64))
#define ZSTD_WINDOWLOG_MIN     10
#define ZSTD_HASHLOG_MAX       ZSTD_WINDOWLOG_MAX
#define ZSTD_HASHLOG_MIN        6
#define ZSTD_CHAINLOG_MAX     (ZSTD_WINDOWLOG_MAX+1)
#define ZSTD_CHAINLOG_MIN      ZSTD_HASHLOG_MIN
#define ZSTD_HASHLOG3_MAX      17
#define ZSTD_SEARCHLOG_MAX    (ZSTD_WINDOWLOG_MAX-1)
#define ZSTD_SEARCHLOG_MIN      1
#define ZSTD_SEARCHLENGTH_MAX   7   /* only for ZSTD_fast, other strategies are limited to 6 */
#define ZSTD_SEARCHLENGTH_MIN   3   /* only for ZSTD_btopt, other strategies are limited to 4 */
#define ZSTD_TARGETLENGTH_MIN   4
#define ZSTD_TARGETLENGTH_MAX 999

#define ZSTD_FRAMEHEADERSIZE_MAX 18    /* for static allocation */
#define ZSTD_FRAMEHEADERSIZE_MIN  6
static const size_t ZSTD_frameHeaderSize_prefix = 5;
static const size_t ZSTD_frameHeaderSize_min = ZSTD_FRAMEHEADERSIZE_MIN;
static const size_t ZSTD_frameHeaderSize_max = ZSTD_FRAMEHEADERSIZE_MAX;
static const size_t ZSTD_skippableHeaderSize = 8;  /* magic number + skippable frame length */


/*--- Advanced types ---*/
typedef enum { ZSTD_fast=1, ZSTD_dfast, ZSTD_greedy, ZSTD_lazy, ZSTD_lazy2,
               ZSTD_btlazy2, ZSTD_btopt, ZSTD_btultra } ZSTD_strategy;   /* from faster to stronger */

typedef struct {
    unsigned windowLog;      /**< largest match distance : larger == more compression, more memory needed during decompression */
    unsigned chainLog;       /**< fully searched segment : larger == more compression, slower, more memory (useless for fast) */
    unsigned hashLog;        /**< dispatch table : larger == faster, more memory */
    unsigned searchLog;      /**< nb of searches : larger == more compression, slower */
    unsigned searchLength;   /**< match length searched : larger == faster decompression, sometimes less compression */
    unsigned targetLength;   /**< acceptable match size for optimal parser (only) : larger == more compression, slower */
    ZSTD_strategy strategy;
} ZSTD_compressionParameters;

typedef struct {
    unsigned contentSizeFlag; /**< 1: content size will be in frame header (when known) */
    unsigned checksumFlag;    /**< 1: generate a 32-bits checksum at end of frame, for error detection */
    unsigned noDictIDFlag;    /**< 1: no dictID will be saved into frame header (if dictionary compression) */
} ZSTD_frameParameters;

typedef struct {
    ZSTD_compressionParameters cParams;
    ZSTD_frameParameters fParams;
} ZSTD_parameters;

typedef struct {
    unsigned long long frameContentSize;
    unsigned windowSize;
    unsigned dictID;
    unsigned checksumFlag;
} ZSTD_frameHeader;

/*= Custom memory allocation functions */
typedef void* (*ZSTD_allocFunction) (void* opaque, size_t size);
typedef void  (*ZSTD_freeFunction) (void* opaque, void* address);
typedef struct { ZSTD_allocFunction customAlloc; ZSTD_freeFunction customFree; void* opaque; } ZSTD_customMem;

/***************************************
*  Frame size functions
***************************************/

/*! ZSTD_findFrameCompressedSize() :
 *  `src` should point to the start of a ZSTD encoded frame or skippable frame
 *  `srcSize` must be at least as large as the frame
 *  @return : the compressed size of the frame pointed to by `src`,
 *            suitable to pass to `ZSTD_decompress` or similar,
 *            or an error code if given invalid input. */
ZSTDLIB_API size_t ZSTD_findFrameCompressedSize(const void* src, size_t srcSize);

/*! ZSTD_getFrameContentSize() :
 *  `src` should point to the start of a ZSTD encoded frame.
 *  `srcSize` must be at least as large as the frame header.
 *       A value >= `ZSTD_frameHeaderSize_max` is guaranteed to be large enough.
 *  @return : - decompressed size of the frame pointed to be `src` if known
 *            - ZSTD_CONTENTSIZE_UNKNOWN if the size cannot be determined
 *            - ZSTD_CONTENTSIZE_ERROR if an error occurred (e.g. invalid magic number, srcSize too small) */
#define ZSTD_CONTENTSIZE_UNKNOWN (0ULL - 1)
#define ZSTD_CONTENTSIZE_ERROR   (0ULL - 2)
ZSTDLIB_API unsigned long long ZSTD_getFrameContentSize(const void *src, size_t srcSize);

/*! ZSTD_findDecompressedSize() :
 *  `src` should point the start of a series of ZSTD encoded and/or skippable frames
 *  `srcSize` must be the _exact_ size of this series
 *       (i.e. there should be a frame boundary exactly `srcSize` bytes after `src`)
 *  @return : - decompressed size of all data in all successive frames
 *            - if the decompressed size cannot be determined: ZSTD_CONTENTSIZE_UNKNOWN
 *            - if an error occurred: ZSTD_CONTENTSIZE_ERROR
 *
 *   note 1 : decompressed size is an optional field, that may not be present, especially in streaming mode.
 *            When `return==ZSTD_CONTENTSIZE_UNKNOWN`, data to decompress could be any size.
 *            In which case, it's necessary to use streaming mode to decompress data.
 *            Optionally, application can still use ZSTD_decompress() while relying on implied limits.
 *            (For example, data may be necessarily cut into blocks <= 16 KB).
 *   note 2 : decompressed size is always present when compression is done with ZSTD_compress()
 *   note 3 : decompressed size can be very large (64-bits value),
 *            potentially larger than what local system can handle as a single memory segment.
 *            In which case, it's necessary to use streaming mode to decompress data.
 *   note 4 : If source is untrusted, decompressed size could be wrong or intentionally modified.
 *            Always ensure result fits within application's authorized limits.
 *            Each application can set its own limits.
 *   note 5 : ZSTD_findDecompressedSize handles multiple frames, and so it must traverse the input to
 *            read each contained frame header.  This is efficient as most of the data is skipped,
 *            however it does mean that all frame data must be present and valid. */
ZSTDLIB_API unsigned long long ZSTD_findDecompressedSize(const void* src, size_t srcSize);


/***************************************
*  Context memory usage
***************************************/

/*! ZSTD_sizeof_*() :
 *  These functions give the current memory usage of selected object.
 *  Object memory usage can evolve if it's re-used multiple times. */
ZSTDLIB_API size_t ZSTD_sizeof_CCtx(const ZSTD_CCtx* cctx);
ZSTDLIB_API size_t ZSTD_sizeof_DCtx(const ZSTD_DCtx* dctx);
ZSTDLIB_API size_t ZSTD_sizeof_CStream(const ZSTD_CStream* zcs);
ZSTDLIB_API size_t ZSTD_sizeof_DStream(const ZSTD_DStream* zds);
ZSTDLIB_API size_t ZSTD_sizeof_CDict(const ZSTD_CDict* cdict);
ZSTDLIB_API size_t ZSTD_sizeof_DDict(const ZSTD_DDict* ddict);

/*! ZSTD_estimate*() :
 *  These functions make it possible to estimate memory usage
 *  of a future target object, before its allocation,
 *  given a set of parameters, which vary depending on target object.
 *  The objective is to guide decision before allocation. */
ZSTDLIB_API size_t ZSTD_estimateCCtxSize(ZSTD_compressionParameters cParams);
ZSTDLIB_API size_t ZSTD_estimateDCtxSize(void);

/*! ZSTD_estimate?StreamSize() :
 *  Note : if streaming is init with function ZSTD_init?Stream_usingDict(),
 *         an internal ?Dict will be created, which size is not estimated.
 *         In this case, get additional size by using ZSTD_estimate?DictSize */
ZSTDLIB_API size_t ZSTD_estimateCStreamSize(ZSTD_compressionParameters cParams);
ZSTDLIB_API size_t ZSTD_estimateDStreamSize(ZSTD_frameHeader fHeader);

/*! ZSTD_estimate?DictSize() :
 *  Note : if dictionary is created "byReference", reduce estimation by dictSize */
ZSTDLIB_API size_t ZSTD_estimateCDictSize(ZSTD_compressionParameters cParams, size_t dictSize);
ZSTDLIB_API size_t ZSTD_estimateDDictSize(size_t dictSize);


/***************************************
*  Advanced compression functions
***************************************/
/*! ZSTD_createCCtx_advanced() :
 *  Create a ZSTD compression context using external alloc and free functions */
ZSTDLIB_API ZSTD_CCtx* ZSTD_createCCtx_advanced(ZSTD_customMem customMem);


/* !!! Soon to be deprecated !!! */
typedef enum {
    ZSTD_p_forceWindow,   /* Force back-references to remain < windowSize, even when referencing Dictionary content (default:0) */
    ZSTD_p_forceRawDict   /* Force loading dictionary in "content-only" mode (no header analysis) */
} ZSTD_CCtxParameter;
/*! ZSTD_setCCtxParameter() :
 *  Set advanced parameters, selected through enum ZSTD_CCtxParameter
 *  @result : 0, or an error code (which can be tested with ZSTD_isError()) */
ZSTDLIB_API size_t ZSTD_setCCtxParameter(ZSTD_CCtx* cctx, ZSTD_CCtxParameter param, unsigned value);

/*! ZSTD_createCDict_byReference() :
 *  Create a digested dictionary for compression
 *  Dictionary content is simply referenced, and therefore stays in dictBuffer.
 *  It is important that dictBuffer outlives CDict, it must remain read accessible throughout the lifetime of CDict */
ZSTDLIB_API ZSTD_CDict* ZSTD_createCDict_byReference(const void* dictBuffer, size_t dictSize, int compressionLevel);

/*! ZSTD_createCDict_advanced() :
 *  Create a ZSTD_CDict using external alloc and free, and customized compression parameters */
ZSTDLIB_API ZSTD_CDict* ZSTD_createCDict_advanced(const void* dict, size_t dictSize, unsigned byReference,
                                                  ZSTD_compressionParameters cParams, ZSTD_customMem customMem);

/*! ZSTD_getCParams() :
*   @return ZSTD_compressionParameters structure for a selected compression level and estimated srcSize.
*   `estimatedSrcSize` value is optional, select 0 if not known */
ZSTDLIB_API ZSTD_compressionParameters ZSTD_getCParams(int compressionLevel, unsigned long long estimatedSrcSize, size_t dictSize);

/*! ZSTD_getParams() :
*   same as ZSTD_getCParams(), but @return a full `ZSTD_parameters` object instead of sub-component `ZSTD_compressionParameters`.
*   All fields of `ZSTD_frameParameters` are set to default (0) */
ZSTDLIB_API ZSTD_parameters ZSTD_getParams(int compressionLevel, unsigned long long estimatedSrcSize, size_t dictSize);

/*! ZSTD_checkCParams() :
*   Ensure param values remain within authorized range */
ZSTDLIB_API size_t ZSTD_checkCParams(ZSTD_compressionParameters params);

/*! ZSTD_adjustCParams() :
*   optimize params for a given `srcSize` and `dictSize`.
*   both values are optional, select `0` if unknown. */
ZSTDLIB_API ZSTD_compressionParameters ZSTD_adjustCParams(ZSTD_compressionParameters cPar, unsigned long long srcSize, size_t dictSize);

/*! ZSTD_compress_advanced() :
*   Same as ZSTD_compress_usingDict(), with fine-tune control over each compression parameter */
ZSTDLIB_API size_t ZSTD_compress_advanced (ZSTD_CCtx* cctx,
                                  void* dst, size_t dstCapacity,
                            const void* src, size_t srcSize,
                            const void* dict,size_t dictSize,
                                  ZSTD_parameters params);

/*! ZSTD_compress_usingCDict_advanced() :
*   Same as ZSTD_compress_usingCDict(), with fine-tune control over frame parameters */
ZSTDLIB_API size_t ZSTD_compress_usingCDict_advanced(ZSTD_CCtx* cctx,
                                  void* dst, size_t dstCapacity,
                            const void* src, size_t srcSize,
                            const ZSTD_CDict* cdict, ZSTD_frameParameters fParams);


/*===   New experimental advanced parameters API   ===*/

/* notes on API design :
 *   In this proposal, parameters are pushed one by one into an existing CCtx,
 *   and then applied on all following compression jobs.
 *   When no parameter is ever provided, CCtx is created with compression level 3.
 *
 *   Another approach could be to load parameters into an intermediate _opaque_ object.
 *   The object would then be loaded into CCtx (like ZSTD_compress_advanced())
 *   This approach would be compatible with ZSTD_createCDict_advanced().
 *   But it's more cumbersome for CCtx, as it requires to manage an additional object.
 *
 *   Below proposal push parameters directly into CCtx.
 *   It's a bit more complex for CDict, as advanced version now requires 3 steps.
 *   (basic CDict API remains the same).
 *   See API details below
 */

/* notes on naming convention :
 *   Initially, the API favored names like ZSTD_setCCtxParameter() .
 *   In this proposal, convention is changed towards ZSTD_CCtx_setParameter() .
 *   The main idea is that it identifies more clearly the target object type.
 *   It feels clearer when considering other potential variants :
 *   ZSTD_CDict_setParameter() (rather than ZSTD_setCDictParameter())
 *   ZSTD_DCtx_setParameter()  (rather than ZSTD_setDCtxParameter() )
 *   The left variant looks clearer.
 */

typedef enum {
    /* compression parameters */
    ZSTD_p_compressionLevel=100, /* Update all compression parameters according to pre-defined cLevel table
                              * Default level is 3.
                              * Special: value 0 means "do not change cLevel". */
    ZSTD_p_windowLog,        /* Maximum allowed back-reference distance, expressed as power of 2.
                              * Must be clamped between ZSTD_WINDOWLOG_MIN and ZSTD_WINDOWLOG_MAX.
                              * default value : set through compressionLevel.
                              * Special: value 0 means "do not change windowLog". */
    ZSTD_p_hashLog,          /* Size of the probe table, as a power of 2.
                              * Resulting table size is (1 << (hashLog+2)).
                              * Must be clamped between ZSTD_HASHLOG_MIN and ZSTD_HASHLOG_MAX.
                              * Larger tables improve compression ratio of strategies <= dFast,
                              * and improve speed of strategies > dFast.
                              * Special: value 0 means "do not change hashLog". */
    ZSTD_p_chainLog,         /* Size of the full-search table, as a power of 2.
                              * Resulting table size is (1 << (chainLog+2)).
                              * Larger tables result in better and slower compression.
                              * This parameter is useless when using "fast" strategy.
                              * Special: value 0 means "do not change chainLog". */
    ZSTD_p_searchLog,        /* Number of search attempts, as a power of 2.
                              * More attempts result in better and slower compression.
                              * This parameter is useless when using "fast" and "dFast" strategies.
                              * Special: value 0 means "do not change searchLog". */
    ZSTD_p_minMatch,         /* Minimum size of searched matches (note : repCode matches can be smaller).
                              * Larger values make faster compression and decompression, but decrease ratio.
                              * Must be clamped between ZSTD_SEARCHLENGTH_MIN and ZSTD_SEARCHLENGTH_MAX.
                              * Note that currently, for all strategies < btopt, effective minimum is 4.
                              * Note that currently, for all strategies > fast, effective maximum is 6.
                              * Special: value 0 means "do not change minMatchLength". */
    ZSTD_p_targetLength,     /* Only useful for strategies >= btopt.
                              * Length of Match considered "good enough" to stop search.
                              * Larger values make compression stronger and slower.
                              * Special: value 0 means "do not change targetLength". */
    ZSTD_p_compressionStrategy, /* See ZSTD_strategy enum definition.
                              * Cast selected strategy as unsigned for ZSTD_CCtx_setParameter() compatibility.
                              * The higher the value of selected strategy, the more complex it is,
                              * resulting in stronger and slower compression.
                              * Special: value 0 means "do not change strategy". */
#if 0
    ZSTD_p_windowSize,       /* Maximum allowed back-reference distance.
                              * Can be set to a more precise value than windowLog.
                              * Will be transparently reduced to closest possible inferior value
                              * (see Zstandard compression format) */
                             /* Not ready yet ! */
#endif

    /* frame parameters */
    ZSTD_p_contentSizeFlag=200, /* Content size is written into frame header _whenever known_ (default:1) */
    ZSTD_p_checksumFlag,     /* A 32-bits checksum of content is written at end of frame (default:0) */
    ZSTD_p_dictIDFlag,       /* When applicable, dictID of dictionary is provided in frame header (default:1) */

    /* dictionary parameters */
    ZSTD_p_refDictContent=300, /* Content of dictionary content will be referenced, instead of copied (default:0).
                              * This avoids duplicating dictionary content.
                              * But it also requires that dictionary buffer outlives its user (CDict) */
                             /* Not ready yet ! */
    ZSTD_p_rawContentDict,   /* load dictionary in "content-only" mode (no header analysis) (default:0) */
                             /* question : should there be an option to load dictionary only in zstd format, rejecting others with an error code ? */

#if 0
    /* multi-threading parameters (not ready yet !) */
    ZSTD_p_nbThreads=400,    /* Select how many threads a compression job can spawn (default:1)
                              * More threads improve speed, but increases also memory usage */
    ZSTDMT_p_jobSize,        /* Size of a compression job. Each job is compressed in parallel.
                              * 0 means default, which is dynamically determined based on compression parameters.
                              * Job size must be a minimum of overlapSize, or 1 KB, whichever is largest
                              * The minimum size is automatically and transparently enforced */
    ZSTDMT_p_overlapSizeLog, /* Size of previous input reloaded at the beginning of each job.
                              * 0 => no overlap, 6(default) => use 1/8th of windowSize, >=9 => use full windowSize */
#endif

    /* advanced parameters - may not remain available after API update */
    ZSTD_p_forceMaxWindow=1100, /* Force back-references to remain < windowSize,
                              * even when referencing into Dictionary content
                              * default : 0 when using a CDict, 1 when using a Prefix */
} ZSTD_cParameter;


/*! ZSTD_CCtx_setParameter() :
 *  Set one compression parameter, selected by enum ZSTD_cParameter.
 *  Note : when `value` is an enum, cast it to unsigned for proper type checking.
 *  @result : 0, or an error code (which can be tested with ZSTD_isError()). */
ZSTDLIB_API size_t ZSTD_CCtx_setParameter(ZSTD_CCtx* cctx, ZSTD_cParameter param, unsigned value);

/*! ZSTD_CCtx_setPledgedSrcSize() :
 *  Total input data size to be compressed into a single frame.
 *  This value will be controlled at the end, and result in error if not respected.
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Note 1 : 0 means zero, empty.
 *           In order to mean "unknown content size", pass constant ZSTD_CONTENTSIZE_UNKNOWN.
 *           Note that ZSTD_CONTENTSIZE_UNKNOWN is default value for all new compression jobs.
 *  Note 2 : If all data is provided and consumed in a single round,
 *           this value is overriden by srcSize instead. */
ZSTDLIB_API size_t ZSTD_CCtx_setPledgedSrcSize(ZSTD_CCtx* cctx, unsigned long long pledgedSrcSize);

/*! ZSTD_CCtx_loadDictionary() :
 *  Create an internal CDict from dict buffer.
 *  Decompression will have to use same buffer.
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Special : Adding a NULL (or 0-size) dictionary invalidates any previous prefix,
 *            meaning "return to no-dictionary mode".
 *  Note 1 : Dictionary content will be copied internally,
 *           except if ZSTD_p_refDictContent is set.
 *  Note 2 : Loading a dictionary involves building tables, which are dependent on compression parameters.
 *           For this reason, compression parameters cannot be changed anymore after loading a prefix.
 *           It's also a CPU-heavy operation, with non-negligible impact on latency.
 *  Note 3 : Dictionary will be used for all future compression jobs.
 *           To return to "no-dictionary" situation, load a NULL dictionary */
ZSTDLIB_API size_t ZSTD_CCtx_loadDictionary(ZSTD_CCtx* cctx, const void* dict, size_t dictSize);   /* Not ready yet ! */

/*! ZSTD_CCtx_refPrefix() :
 *  Reference a prefix (content-only dictionary) to bootstrap next compression job.
 *  Decompression will have to use same prefix.
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Special : Adding a NULL (or 0-size) dictionary invalidates any previous prefix, meaning "return to no-dictionary mode".
 *  Note 1 : Prefix buffer is referenced. It must outlive compression job.
 *  Note 3 : Prefix is only used once. Tables are discarded at end of compression job.
 *           If there is a need to use same prefix multiple times, consider embedding it into a ZSTD_CDict.
 *  Note 2 : Referencing a prefix involves building tables, which are dependent on compression parameters.
 *           For this reason, compression parameters cannot be changed anymore after loading a prefix.
 *           It's also a CPU-heavy operation, with non-negligible impact on latency. */
ZSTDLIB_API size_t ZSTD_CCtx_refPrefix(ZSTD_CCtx* cctx, const void* prefix, size_t prefixSize);   /* Not ready yet ! */


/*! ZSTD_CDict_createEmpty() :
 *  Create a CDict object which is still mutable after creation.
 *  It's the only one case allowing usage of ZSTD_CDict_setParameter().
 *  Once all compression parameters are selected,
 *  it's possible to load the target dictionary, using ZSTD_CDict_loadDictionary().
 *  Dictionary content will be copied internally (except if ZSTD_p_refDictContent is set).
 *  After loading the dictionary, no more change is possible.
 *  The only remaining operation is to free CDict object.
 *  Note : An unfinished CDict behaves the same as a NULL CDict if referenced into a CCtx.
 */
ZSTDLIB_API ZSTD_CDict* ZSTD_CDict_createEmpty(void);   /* Not ready yet ! */
ZSTDLIB_API size_t ZSTD_CDict_setParameter(ZSTD_CDict* cdict, ZSTD_cParameter param, unsigned value);  /* Not ready yet ! */
ZSTDLIB_API size_t ZSTD_CDict_loadDictionary(ZSTD_CDict* cdict, const void* dict, size_t dictSize);    /* Not ready yet ! */

/*! ZSTD_CCtx_refCDict() :
 *  Add a prepared dictionary to cctx, to be used for next compression jobs.
 *  Note that compression parameters will be enforced from within CDict.
 *  Currently, they supercede any compression parameter previously set within CCtx.
 *  The dictionary will remain valid for future compression jobs using same cctx.
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Special : adding a NULL CDict means "return to no-dictionary mode".
 *  Note 1 : Currently, only one dictionary can be managed.
 *           Adding a new dictionary effectively "discards" any previous one.
 *  Note 2 : CDict is just referenced, its lifetime must outlive CCtx.
 */
ZSTDLIB_API size_t ZSTD_CCtx_refCDict(ZSTD_CCtx* cctx, const ZSTD_CDict* cdict);  /* Not ready yet ! */



typedef enum {
    ZSTD_e_continue=0, /* collect more data, encoder transparently decides when to output result, for optimal conditions */
    ZSTD_e_flush,      /* flush any data provided so far - frame will continue, future data can still reference previous data for better compression */
    ZSTD_e_end         /* flush any remaining data and ends current frame. Any future compression starts a new frame. */
} ZSTD_EndDirective;

/*! ZSTD_compressStream_generic() :
 *  Behave about the same as ZSTD_compressStream. To note :
 *  - Compression parameters are pushed into CCtx before starting compression, using ZSTD_setCCtxParameter()
 *  - Compression parameters cannot be changed once compression is started.
 *  - *dstPos must be <= dstCapacity, *srcPos must be <= srcSize
 *  - *dspPos and *srcPos will be updated. They are guaranteed to remain below their respective limit.
 *  - @return provides the amount of data ready to flush within internal buffers
 *            or an error code, which can be tested using ZSTD_isError().
 *            if @return != 0, flush is not fully completed, and must be called again to empty internal buffers.
 *  - after a ZSTD_e_end directive, if internal buffer is not fully flushed,
 *            only ZSTD_e_end and ZSTD_e_flush operations are allowed.
 *            It is necessary to fully flush internal buffers
 *            before changing compression parameters or start a new compression job.
 */
ZSTDLIB_API size_t ZSTD_compress_generic (ZSTD_CCtx* cctx,
                                          ZSTD_outBuffer* output,
                                          ZSTD_inBuffer* input,
                                          ZSTD_EndDirective endOp);

/*! ZSTD_CCtx_reset() :
 *  Return a CCtx to clean state.
 *  Useful after an error, or to interrupt an ongoing compression job and start a new one.
 *  It's possible to modify compression parameters after a reset.
 *  Any internal data not yet flushed is cancelled.
 */
ZSTDLIB_API size_t ZSTD_CCtx_reset(ZSTD_CCtx* cctx);   /* Not ready yet ! */



/*--- Advanced decompression functions ---*/

/*! ZSTD_isFrame() :
 *  Tells if the content of `buffer` starts with a valid Frame Identifier.
 *  Note : Frame Identifier is 4 bytes. If `size < 4`, @return will always be 0.
 *  Note 2 : Legacy Frame Identifiers are considered valid only if Legacy Support is enabled.
 *  Note 3 : Skippable Frame Identifiers are considered valid. */
ZSTDLIB_API unsigned ZSTD_isFrame(const void* buffer, size_t size);

/*! ZSTD_createDCtx_advanced() :
 *  Create a ZSTD decompression context using external alloc and free functions */
ZSTDLIB_API ZSTD_DCtx* ZSTD_createDCtx_advanced(ZSTD_customMem customMem);

/*! ZSTD_createDDict_byReference() :
 *  Create a digested dictionary, ready to start decompression operation without startup delay.
 *  Dictionary content is simply referenced, and therefore stays in dictBuffer.
 *  It is important that dictBuffer outlives DDict, it must remain read accessible throughout the lifetime of DDict */
ZSTDLIB_API ZSTD_DDict* ZSTD_createDDict_byReference(const void* dictBuffer, size_t dictSize);

/*! ZSTD_createDDict_advanced() :
 *  Create a ZSTD_DDict using external alloc and free, optionally by reference */
ZSTDLIB_API ZSTD_DDict* ZSTD_createDDict_advanced(const void* dict, size_t dictSize,
                                                  unsigned byReference, ZSTD_customMem customMem);

/*! ZSTD_getDictID_fromDict() :
 *  Provides the dictID stored within dictionary.
 *  if @return == 0, the dictionary is not conformant with Zstandard specification.
 *  It can still be loaded, but as a content-only dictionary. */
ZSTDLIB_API unsigned ZSTD_getDictID_fromDict(const void* dict, size_t dictSize);

/*! ZSTD_getDictID_fromDDict() :
 *  Provides the dictID of the dictionary loaded into `ddict`.
 *  If @return == 0, the dictionary is not conformant to Zstandard specification, or empty.
 *  Non-conformant dictionaries can still be loaded, but as content-only dictionaries. */
ZSTDLIB_API unsigned ZSTD_getDictID_fromDDict(const ZSTD_DDict* ddict);

/*! ZSTD_getDictID_fromFrame() :
 *  Provides the dictID required to decompressed the frame stored within `src`.
 *  If @return == 0, the dictID could not be decoded.
 *  This could for one of the following reasons :
 *  - The frame does not require a dictionary to be decoded (most common case).
 *  - The frame was built with dictID intentionally removed. Whatever dictionary is necessary is a hidden information.
 *    Note : this use case also happens when using a non-conformant dictionary.
 *  - `srcSize` is too small, and as a result, the frame header could not be decoded (only possible if `srcSize < ZSTD_FRAMEHEADERSIZE_MAX`).
 *  - This is not a Zstandard frame.
 *  When identifying the exact failure cause, it's possible to use ZSTD_getFrameParams(), which will provide a more precise error code. */
ZSTDLIB_API unsigned ZSTD_getDictID_fromFrame(const void* src, size_t srcSize);


/********************************************************************
*  Advanced streaming functions
********************************************************************/

/*=====   Advanced Streaming compression functions  =====*/
ZSTDLIB_API ZSTD_CStream* ZSTD_createCStream_advanced(ZSTD_customMem customMem);
ZSTDLIB_API size_t ZSTD_initCStream_srcSize(ZSTD_CStream* zcs, int compressionLevel, unsigned long long pledgedSrcSize);   /**< pledgedSrcSize must be correct, a size of 0 means unknown.  for a frame size of 0 use initCStream_advanced */
ZSTDLIB_API size_t ZSTD_initCStream_usingDict(ZSTD_CStream* zcs, const void* dict, size_t dictSize, int compressionLevel); /**< note: a dict will not be used if dict == NULL or dictSize < 8. This result in the creation of an internal CDict */
ZSTDLIB_API size_t ZSTD_initCStream_advanced(ZSTD_CStream* zcs, const void* dict, size_t dictSize,
                                             ZSTD_parameters params, unsigned long long pledgedSrcSize);  /**< pledgedSrcSize is optional and can be 0 (meaning unknown). note: if the contentSizeFlag is set, pledgedSrcSize == 0 means the source size is actually 0 */
ZSTDLIB_API size_t ZSTD_initCStream_usingCDict(ZSTD_CStream* zcs, const ZSTD_CDict* cdict);  /**< note : cdict will just be referenced, and must outlive compression session */
ZSTDLIB_API size_t ZSTD_initCStream_usingCDict_advanced(ZSTD_CStream* zcs, const ZSTD_CDict* cdict, unsigned long long pledgedSrcSize, ZSTD_frameParameters fParams);  /**< same as ZSTD_initCStream_usingCDict(), with control over frame parameters */

/*! ZSTD_resetCStream() :
 *  start a new compression job, using same parameters from previous job.
 *  This is typically useful to skip dictionary loading stage, since it will re-use it in-place..
 *  Note that zcs must be init at least once before using ZSTD_resetCStream().
 *  pledgedSrcSize==0 means "srcSize unknown".
 *  If pledgedSrcSize > 0, its value must be correct, as it will be written in header, and controlled at the end.
 *  @return : 0, or an error code (which can be tested using ZSTD_isError()) */
ZSTDLIB_API size_t ZSTD_resetCStream(ZSTD_CStream* zcs, unsigned long long pledgedSrcSize);


/*=====   Advanced Streaming decompression functions  =====*/
typedef enum { DStream_p_maxWindowSize } ZSTD_DStreamParameter_e;
ZSTDLIB_API ZSTD_DStream* ZSTD_createDStream_advanced(ZSTD_customMem customMem);
ZSTDLIB_API size_t ZSTD_setDStreamParameter(ZSTD_DStream* zds, ZSTD_DStreamParameter_e paramType, unsigned paramValue);
ZSTDLIB_API size_t ZSTD_initDStream_usingDict(ZSTD_DStream* zds, const void* dict, size_t dictSize); /**< note: a dict will not be used if dict == NULL or dictSize < 8 */
ZSTDLIB_API size_t ZSTD_initDStream_usingDDict(ZSTD_DStream* zds, const ZSTD_DDict* ddict);  /**< note : ddict will just be referenced, and must outlive decompression session */
ZSTDLIB_API size_t ZSTD_resetDStream(ZSTD_DStream* zds);  /**< re-use decompression parameters from previous init; saves dictionary loading */


/*********************************************************************
*  Buffer-less and synchronous inner streaming functions
*
*  This is an advanced API, giving full control over buffer management, for users which need direct control over memory.
*  But it's also a complex one, with many restrictions (documented below).
*  Prefer using normal streaming API for an easier experience
********************************************************************* */

/**
  Buffer-less streaming compression (synchronous mode)

  A ZSTD_CCtx object is required to track streaming operations.
  Use ZSTD_createCCtx() / ZSTD_freeCCtx() to manage resource.
  ZSTD_CCtx object can be re-used multiple times within successive compression operations.

  Start by initializing a context.
  Use ZSTD_compressBegin(), or ZSTD_compressBegin_usingDict() for dictionary compression,
  or ZSTD_compressBegin_advanced(), for finer parameter control.
  It's also possible to duplicate a reference context which has already been initialized, using ZSTD_copyCCtx()

  Then, consume your input using ZSTD_compressContinue().
  There are some important considerations to keep in mind when using this advanced function :
  - ZSTD_compressContinue() has no internal buffer. It uses externally provided buffer only.
  - Interface is synchronous : input is consumed entirely and produce 1+ (or more) compressed blocks.
  - Caller must ensure there is enough space in `dst` to store compressed data under worst case scenario.
    Worst case evaluation is provided by ZSTD_compressBound().
    ZSTD_compressContinue() doesn't guarantee recover after a failed compression.
  - ZSTD_compressContinue() presumes prior input ***is still accessible and unmodified*** (up to maximum distance size, see WindowLog).
    It remembers all previous contiguous blocks, plus one separated memory segment (which can itself consists of multiple contiguous blocks)
  - ZSTD_compressContinue() detects that prior input has been overwritten when `src` buffer overlaps.
    In which case, it will "discard" the relevant memory section from its history.

  Finish a frame with ZSTD_compressEnd(), which will write the last block(s) and optional checksum.
  It's possible to use srcSize==0, in which case, it will write a final empty block to end the frame.
  Without last block mark, frames will be considered unfinished (corrupted) by decoders.

  `ZSTD_CCtx` object can be re-used (ZSTD_compressBegin()) to compress some new frame.
*/

/*=====   Buffer-less streaming compression functions  =====*/
ZSTDLIB_API size_t ZSTD_compressBegin(ZSTD_CCtx* cctx, int compressionLevel);
ZSTDLIB_API size_t ZSTD_compressBegin_usingDict(ZSTD_CCtx* cctx, const void* dict, size_t dictSize, int compressionLevel);
ZSTDLIB_API size_t ZSTD_compressBegin_advanced(ZSTD_CCtx* cctx, const void* dict, size_t dictSize, ZSTD_parameters params, unsigned long long pledgedSrcSize); /**< pledgedSrcSize is optional and can be 0 (meaning unknown). note: if the contentSizeFlag is set, pledgedSrcSize == 0 means the source size is actually 0 */
ZSTDLIB_API size_t ZSTD_compressBegin_usingCDict(ZSTD_CCtx* cctx, const ZSTD_CDict* cdict); /**< note: fails if cdict==NULL */
ZSTDLIB_API size_t ZSTD_compressBegin_usingCDict_advanced(ZSTD_CCtx* const cctx, const ZSTD_CDict* const cdict, ZSTD_frameParameters const fParams, unsigned long long const pledgedSrcSize);   /* compression parameters are already set within cdict. pledgedSrcSize=0 means null-size */
ZSTDLIB_API size_t ZSTD_copyCCtx(ZSTD_CCtx* cctx, const ZSTD_CCtx* preparedCCtx, unsigned long long pledgedSrcSize); /**<  note: if pledgedSrcSize can be 0, indicating unknown size.  if it is non-zero, it must be accurate.  for 0 size frames, use compressBegin_advanced */

ZSTDLIB_API size_t ZSTD_compressContinue(ZSTD_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
ZSTDLIB_API size_t ZSTD_compressEnd(ZSTD_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);



/*-
  Buffer-less streaming decompression (synchronous mode)

  A ZSTD_DCtx object is required to track streaming operations.
  Use ZSTD_createDCtx() / ZSTD_freeDCtx() to manage it.
  A ZSTD_DCtx object can be re-used multiple times.

  First typical operation is to retrieve frame parameters, using ZSTD_getFrameParams().
  It fills a ZSTD_frameParams structure which provide important information to correctly decode the frame,
  such as the minimum rolling buffer size to allocate to decompress data (`windowSize`),
  and the dictionary ID used.
  (Note : content size is optional, it may not be present. 0 means : content size unknown).
  Note that these values could be wrong, either because of data malformation, or because an attacker is spoofing deliberate false information.
  As a consequence, check that values remain within valid application range, especially `windowSize`, before allocation.
  Each application can set its own limit, depending on local restrictions. For extended interoperability, it is recommended to support at least 8 MB.
  Frame parameters are extracted from the beginning of the compressed frame.
  Data fragment must be large enough to ensure successful decoding, typically `ZSTD_frameHeaderSize_max` bytes.
  @result : 0 : successful decoding, the `ZSTD_frameParams` structure is correctly filled.
           >0 : `srcSize` is too small, please provide at least @result bytes on next attempt.
           errorCode, which can be tested using ZSTD_isError().

  Start decompression, with ZSTD_decompressBegin().
  If decompression requires a dictionary, use ZSTD_decompressBegin_usingDict() or ZSTD_decompressBegin_usingDDict().
  Alternatively, you can copy a prepared context, using ZSTD_copyDCtx().

  Then use ZSTD_nextSrcSizeToDecompress() and ZSTD_decompressContinue() alternatively.
  ZSTD_nextSrcSizeToDecompress() tells how many bytes to provide as 'srcSize' to ZSTD_decompressContinue().
  ZSTD_decompressContinue() requires this _exact_ amount of bytes, or it will fail.

  @result of ZSTD_decompressContinue() is the number of bytes regenerated within 'dst' (necessarily <= dstCapacity).
  It can be zero, which is not an error; it just means ZSTD_decompressContinue() has decoded some metadata item.
  It can also be an error code, which can be tested with ZSTD_isError().

  ZSTD_decompressContinue() needs previous data blocks during decompression, up to `windowSize`.
  They should preferably be located contiguously, prior to current block.
  Alternatively, a round buffer of sufficient size is also possible. Sufficient size is determined by frame parameters.
  ZSTD_decompressContinue() is very sensitive to contiguity,
  if 2 blocks don't follow each other, make sure that either the compressor breaks contiguity at the same place,
  or that previous contiguous segment is large enough to properly handle maximum back-reference.

  A frame is fully decoded when ZSTD_nextSrcSizeToDecompress() returns zero.
  Context can then be reset to start a new decompression.

  Note : it's possible to know if next input to present is a header or a block, using ZSTD_nextInputType().
  This information is not required to properly decode a frame.

  == Special case : skippable frames ==

  Skippable frames allow integration of user-defined data into a flow of concatenated frames.
  Skippable frames will be ignored (skipped) by a decompressor. The format of skippable frames is as follows :
  a) Skippable frame ID - 4 Bytes, Little endian format, any value from 0x184D2A50 to 0x184D2A5F
  b) Frame Size - 4 Bytes, Little endian format, unsigned 32-bits
  c) Frame Content - any content (User Data) of length equal to Frame Size
  For skippable frames ZSTD_decompressContinue() always returns 0.
  For skippable frames ZSTD_getFrameParams() returns fparamsPtr->windowLog==0 what means that a frame is skippable.
    Note : If fparamsPtr->frameContentSize==0, it is ambiguous: the frame might actually be a Zstd encoded frame with no content.
           For purposes of decompression, it is valid in both cases to skip the frame using
           ZSTD_findFrameCompressedSize to find its size in bytes.
  It also returns Frame Size as fparamsPtr->frameContentSize.
*/

/*=====   Buffer-less streaming decompression functions  =====*/
ZSTDLIB_API size_t ZSTD_getFrameHeader(ZSTD_frameHeader* zfhPtr, const void* src, size_t srcSize);   /**< doesn't consume input, see details below */
ZSTDLIB_API size_t ZSTD_decompressBegin(ZSTD_DCtx* dctx);
ZSTDLIB_API size_t ZSTD_decompressBegin_usingDict(ZSTD_DCtx* dctx, const void* dict, size_t dictSize);
ZSTDLIB_API size_t ZSTD_decompressBegin_usingDDict(ZSTD_DCtx* dctx, const ZSTD_DDict* ddict);
ZSTDLIB_API void   ZSTD_copyDCtx(ZSTD_DCtx* dctx, const ZSTD_DCtx* preparedDCtx);

ZSTDLIB_API size_t ZSTD_nextSrcSizeToDecompress(ZSTD_DCtx* dctx);
ZSTDLIB_API size_t ZSTD_decompressContinue(ZSTD_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
typedef enum { ZSTDnit_frameHeader, ZSTDnit_blockHeader, ZSTDnit_block, ZSTDnit_lastBlock, ZSTDnit_checksum, ZSTDnit_skippableFrame } ZSTD_nextInputType_e;
ZSTDLIB_API ZSTD_nextInputType_e ZSTD_nextInputType(ZSTD_DCtx* dctx);

/**
    Block functions

    Block functions produce and decode raw zstd blocks, without frame metadata.
    Frame metadata cost is typically ~18 bytes, which can be non-negligible for very small blocks (< 100 bytes).
    User will have to take in charge required information to regenerate data, such as compressed and content sizes.

    A few rules to respect :
    - Compressing and decompressing require a context structure
      + Use ZSTD_createCCtx() and ZSTD_createDCtx()
    - It is necessary to init context before starting
      + compression : any ZSTD_compressBegin*() variant, including with dictionary
      + decompression : any ZSTD_decompressBegin*() variant, including with dictionary
      + copyCCtx() and copyDCtx() can be used too
    - Block size is limited, it must be <= ZSTD_getBlockSizeMax() <= ZSTD_BLOCKSIZE_ABSOLUTEMAX
      + If input is larger than a block size, it's necessary to split input data into multiple blocks
      + For inputs larger than a single block size, consider using the regular ZSTD_compress() instead.
        Frame metadata is not that costly, and quickly becomes negligible as source size grows larger.
    - When a block is considered not compressible enough, ZSTD_compressBlock() result will be zero.
      In which case, nothing is produced into `dst`.
      + User must test for such outcome and deal directly with uncompressed data
      + ZSTD_decompressBlock() doesn't accept uncompressed data as input !!!
      + In case of multiple successive blocks, should some of them be uncompressed,
        decoder must be informed of their existence in order to follow proper history.
        Use ZSTD_insertBlock() for such a case.
*/

#define ZSTD_BLOCKSIZE_ABSOLUTEMAX (128 * 1024)   /* define, for static allocation */
/*=====   Raw zstd block functions  =====*/
ZSTDLIB_API size_t ZSTD_getBlockSizeMax(ZSTD_CCtx* cctx);
ZSTDLIB_API size_t ZSTD_compressBlock  (ZSTD_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
ZSTDLIB_API size_t ZSTD_decompressBlock(ZSTD_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
ZSTDLIB_API size_t ZSTD_insertBlock(ZSTD_DCtx* dctx, const void* blockStart, size_t blockSize);  /**< insert block into `dctx` history. Useful for uncompressed blocks */


#endif   /* ZSTD_H_ZSTD_STATIC_LINKING_ONLY */

#if defined (__cplusplus)
}
#endif
