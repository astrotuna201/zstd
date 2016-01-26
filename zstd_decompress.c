/*
    zstd - standard compression library
    Copyright (C) 2014-2015, Yann Collet.

    BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the
    distribution.
    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

    You can contact the author at :
    - zstd source repository : https://github.com/Cyan4973/zstd
    - ztsd public forum : https://groups.google.com/forum/#!forum/lz4c
*/

/* ***************************************************************
*  Tuning parameters
*****************************************************************/
/*!
 * HEAPMODE :
 * Select how default decompression function ZSTD_decompress() will allocate memory,
 * in memory stack (0), or in memory heap (1, requires malloc())
 */
#ifndef ZSTD_HEAPMODE
#  define ZSTD_HEAPMODE 1
#endif

/*!
*  LEGACY_SUPPORT :
*  ZSTD_decompress() can decode older formats (v0.1+) if set to 1
*/
#ifndef ZSTD_LEGACY_SUPPORT
#  define ZSTD_LEGACY_SUPPORT 0
#endif


/* *******************************************************
*  Includes
*********************************************************/
#include <stdlib.h>      /* calloc */
#include <string.h>      /* memcpy, memmove */
#include <stdio.h>       /* debug : printf */
#include "mem.h"         /* low level memory routines */
#include "zstd_static.h"
#include "zstd_internal.h"
#include "fse_static.h"
#include "huff0_static.h"

#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT==1)
#  include "zstd_legacy.h"
#endif

/* *******************************************************
*  Compiler specifics
*********************************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  define FORCE_INLINE static __forceinline
#  include <intrin.h>                    /* For Visual 2005 */
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4324)        /* disable: C4324: padded structure */
#else
#  define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)
#  ifdef __GNUC__
#    define FORCE_INLINE static inline __attribute__((always_inline))
#  else
#    define FORCE_INLINE static inline
#  endif
#endif


/* *************************************
*  Local types
***************************************/
typedef struct
{
    blockType_t blockType;
    U32 origSize;
} blockProperties_t;


/* *******************************************************
*  Memory operations
**********************************************************/
static void ZSTD_copy4(void* dst, const void* src) { memcpy(dst, src, 4); }


/* *************************************
*  Error Management
***************************************/
unsigned ZSTD_versionNumber (void) { return ZSTD_VERSION_NUMBER; }

/*! ZSTD_isError
*   tells if a return value is an error code */
unsigned ZSTD_isError(size_t code) { return ERR_isError(code); }

/*! ZSTD_getErrorName
*   provides error code string (useful for debugging) */
const char* ZSTD_getErrorName(size_t code) { return ERR_getErrorName(code); }


/* *************************************************************
*   Context management
***************************************************************/
typedef enum { ZSTDds_getFrameHeaderSize, ZSTDds_decodeFrameHeader,
               ZSTDds_decodeBlockHeader, ZSTDds_decompressBlock } ZSTD_dStage;

struct ZSTD_DCtx_s
{
    U32 LLTable[FSE_DTABLE_SIZE_U32(LLFSELog)];
    U32 OffTable[FSE_DTABLE_SIZE_U32(OffFSELog)];
    U32 MLTable[FSE_DTABLE_SIZE_U32(MLFSELog)];
    U32 hufTableX4[HUF_DTABLE_SIZE(HufLog)];
    const void* previousDstEnd;
    const void* base;
    const void* vBase;
    const void* dictEnd;
    size_t expected;
    size_t headerSize;
    ZSTD_parameters params;
    blockType_t bType;
    ZSTD_dStage stage;
    const BYTE* litPtr;
    size_t litBufSize;
    size_t litSize;
    BYTE litBuffer[BLOCKSIZE + WILDCOPY_OVERLENGTH];
    BYTE headerBuffer[ZSTD_frameHeaderSize_max];
};  /* typedef'd to ZSTD_DCtx within "zstd_static.h" */

size_t ZSTD_resetDCtx(ZSTD_DCtx* dctx)
{
    dctx->expected = ZSTD_frameHeaderSize_min;
    dctx->stage = ZSTDds_getFrameHeaderSize;
    dctx->previousDstEnd = NULL;
    dctx->base = NULL;
    dctx->vBase = NULL;
    dctx->dictEnd = NULL;
    dctx->hufTableX4[0] = HufLog;
    return 0;
}

ZSTD_DCtx* ZSTD_createDCtx(void)
{
    ZSTD_DCtx* dctx = (ZSTD_DCtx*)malloc(sizeof(ZSTD_DCtx));
    if (dctx==NULL) return NULL;
    ZSTD_resetDCtx(dctx);
    return dctx;
}

size_t ZSTD_freeDCtx(ZSTD_DCtx* dctx)
{
    free(dctx);
    return 0;
}


/* *************************************************************
*   Decompression section
***************************************************************/

/* Frame format description
   Frame Header -  [ Block Header - Block ] - Frame End
   1) Frame Header
      - 4 bytes - Magic Number : ZSTD_MAGICNUMBER (defined within zstd_internal.h)
      - 1 byte  - Window Descriptor
   2) Block Header
      - 3 bytes, starting with a 2-bits descriptor
                 Uncompressed, Compressed, Frame End, unused
   3) Block
      See Block Format Description
   4) Frame End
      - 3 bytes, compatible with Block Header
*/

/* Block format description
   Literal Section - Sequences Section
   1) Literal Section
   1.1) Header : up to 5 bytes
        flags:
            00 compressed by Huff0
            01 is Raw (uncompressed)
            10 is Rle
            11 unused
            Note : using 11 for Huff0 with precomputed table ?
            Note : delta map ? => compressed ?
            Note 2 : 19 bits for sizes, seems a bit larger than necessary
            Note 3 : RLE blocks ?

   1.2.1) Huff0 block, using sizes from header
        See Huff0 format

   1.2.2) Huff0 block, using precomputed DTable
        _usingDTable variants

   1.2.3) uncompressed blocks
        as the name says (both 2 or 3 bytes variants)

   2) Sequences section
      TO DO
*/


/** ZSTD_decodeFrameHeader_Part1
*   decode the 1st part of the Frame Header, which tells Frame Header size.
*   srcSize must be == ZSTD_frameHeaderSize_min
*   @return : the full size of the Frame Header */
static size_t ZSTD_decodeFrameHeader_Part1(ZSTD_DCtx* zc, const void* src, size_t srcSize)
{
    U32 magicNumber;
    if (srcSize != ZSTD_frameHeaderSize_min) return ERROR(srcSize_wrong);
    magicNumber = MEM_readLE32(src);
    if (magicNumber != ZSTD_MAGICNUMBER) return ERROR(prefix_unknown);
    zc->headerSize = ZSTD_frameHeaderSize_min;
    return zc->headerSize;
}


size_t ZSTD_getFrameParams(ZSTD_parameters* params, const void* src, size_t srcSize)
{
    U32 magicNumber;
    if (srcSize < ZSTD_frameHeaderSize_min) return ZSTD_frameHeaderSize_max;
    magicNumber = MEM_readLE32(src);
    if (magicNumber != ZSTD_MAGICNUMBER) return ERROR(prefix_unknown);
    memset(params, 0, sizeof(*params));
    params->windowLog = (((const BYTE*)src)[4] & 15) + ZSTD_WINDOWLOG_ABSOLUTEMIN;
    if ((((const BYTE*)src)[4] >> 4) != 0) return ERROR(frameParameter_unsupported);   /* reserved bits */
    return 0;
}

/** ZSTD_decodeFrameHeader_Part2
*   decode the full Frame Header
*   srcSize must be the size provided by ZSTD_decodeFrameHeader_Part1
*   @return : 0, or an error code, which can be tested using ZSTD_isError() */
static size_t ZSTD_decodeFrameHeader_Part2(ZSTD_DCtx* zc, const void* src, size_t srcSize)
{
    size_t result;
    if (srcSize != zc->headerSize) return ERROR(srcSize_wrong);
    result = ZSTD_getFrameParams(&(zc->params), src, srcSize);
    if ((MEM_32bits()) && (zc->params.windowLog > 25)) return ERROR(frameParameter_unsupportedBy32bitsImplementation);
    return result;
}


size_t ZSTD_getcBlockSize(const void* src, size_t srcSize, blockProperties_t* bpPtr)
{
    const BYTE* const in = (const BYTE* const)src;
    BYTE headerFlags;
    U32 cSize;

    if (srcSize < 3) return ERROR(srcSize_wrong);

    headerFlags = *in;
    cSize = in[2] + (in[1]<<8) + ((in[0] & 7)<<16);

    bpPtr->blockType = (blockType_t)(headerFlags >> 6);
    bpPtr->origSize = (bpPtr->blockType == bt_rle) ? cSize : 0;

    if (bpPtr->blockType == bt_end) return 0;
    if (bpPtr->blockType == bt_rle) return 1;
    return cSize;
}


static size_t ZSTD_copyRawBlock(void* dst, size_t maxDstSize, const void* src, size_t srcSize)
{
    if (srcSize > maxDstSize) return ERROR(dstSize_tooSmall);
    memcpy(dst, src, srcSize);
    return srcSize;
}


/** ZSTD_decodeLiteralsBlock
    @return : nb of bytes read from src (< srcSize ) */
size_t ZSTD_decodeLiteralsBlock(ZSTD_DCtx* dctx,
                          const void* src, size_t srcSize)   /* note : srcSize < BLOCKSIZE */
{
    const BYTE* const istart = (const BYTE*) src;

    /* any compressed block with literals segment must be at least this size */
    if (srcSize < MIN_CBLOCK_SIZE) return ERROR(corruption_detected);

    switch(istart[0]>> 6)
    {
    case IS_HUF:
        {
            size_t litSize, litCSize, singleStream=0;
            U32 lhSize = ((istart[0]) >> 4) & 3;
            switch(lhSize)
            {
            case 0: case 1: default:   /* note : default is impossible, since lhSize into [0..3] */
                /* 2 - 2 - 10 - 10 */
                lhSize=3;
                singleStream = istart[0] & 16;
                litSize  = ((istart[0] & 15) << 6) + (istart[1] >> 2);
                litCSize = ((istart[1] &  3) << 8) + istart[2];
                break;
            case 2:
                /* 2 - 2 - 14 - 14 */
                lhSize=4;
                litSize  = ((istart[0] & 15) << 10) + (istart[1] << 2) + (istart[2] >> 6);
                litCSize = ((istart[2] & 63) <<  8) + istart[3];
                break;
            case 3:
                /* 2 - 2 - 18 - 18 */
                lhSize=5;
                litSize  = ((istart[0] & 15) << 14) + (istart[1] << 6) + (istart[2] >> 2);
                litCSize = ((istart[2] &  3) << 16) + (istart[3] << 8) + istart[4];
                break;
            }
            if (litSize > BLOCKSIZE) return ERROR(corruption_detected);;

            if (HUF_isError(singleStream ?
                            HUF_decompress1X2(dctx->litBuffer, litSize, istart+lhSize, litCSize) :
                            HUF_decompress   (dctx->litBuffer, litSize, istart+lhSize, litCSize) ))
                return ERROR(corruption_detected);

            dctx->litPtr = dctx->litBuffer;
            dctx->litBufSize = BLOCKSIZE+8;
            dctx->litSize = litSize;
            return litCSize + lhSize;
        }
    case IS_PCH:
        {
            size_t errorCode;
            size_t litSize, litCSize;
            U32 lhSize = ((istart[0]) >> 4) & 3;
            if (lhSize != 1)  /* only case supported for now : small litSize, single stream */
                return ERROR(corruption_detected);

            /* 2 - 2 - 10 - 10 */
            lhSize=3;
            litSize  = ((istart[0] & 15) << 6) + (istart[1] >> 2);
            litCSize = ((istart[1] &  3) << 8) + istart[2];

            errorCode = HUF_decompress1X4_usingDTable(dctx->litBuffer, litSize, istart+lhSize, litCSize, dctx->hufTableX4);
            if (HUF_isError(errorCode)) return ERROR(corruption_detected);

            dctx->litPtr = dctx->litBuffer;
            dctx->litBufSize = BLOCKSIZE+WILDCOPY_OVERLENGTH;
            dctx->litSize = litSize;
            return litCSize + lhSize;
        }
    case IS_RAW:
        {
            size_t litSize;
            U32 lhSize = ((istart[0]) >> 4) & 3;
            switch(lhSize)
            {
            case 0: case 1: default:   /* note : default is impossible, since lhSize into [0..3] */
                lhSize=1;
                litSize = istart[0] & 31;
                break;
            case 2:
                litSize = ((istart[0] & 15) << 8) + istart[1];
                break;
            case 3:
                litSize = ((istart[0] & 15) << 16) + (istart[1] << 8) + istart[2];
                break;
            }

            if (litSize+WILDCOPY_OVERLENGTH > srcSize)   /* risk reading beyond src buffer with wildcopy */
            {
                if (litSize > srcSize-lhSize) return ERROR(corruption_detected);
                memcpy(dctx->litBuffer, istart+lhSize, litSize);
                dctx->litPtr = dctx->litBuffer;
                dctx->litBufSize = BLOCKSIZE+8;
                dctx->litSize = litSize;
                return lhSize+litSize;
            }
            /* direct reference into compressed stream */
            dctx->litPtr = istart+lhSize;
            dctx->litBufSize = srcSize-lhSize;
            dctx->litSize = litSize;
            return lhSize+litSize;
        }
    case IS_RLE:
        {
            size_t litSize;
            U32 lhSize = ((istart[0]) >> 4) & 3;
            switch(lhSize)
            {
            case 0: case 1: default:   /* note : default is impossible, since lhSize into [0..3] */
                lhSize = 1;
                litSize = istart[0] & 31;
                break;
            case 2:
                litSize = ((istart[0] & 15) << 8) + istart[1];
                break;
            case 3:
                litSize = ((istart[0] & 15) << 16) + (istart[1] << 8) + istart[2];
                break;
            }
            if (litSize > BLOCKSIZE) return ERROR(corruption_detected);
            memset(dctx->litBuffer, istart[lhSize], litSize);
            dctx->litPtr = dctx->litBuffer;
            dctx->litBufSize = BLOCKSIZE+WILDCOPY_OVERLENGTH;
            dctx->litSize = litSize;
            return lhSize+1;
        }
    default:
        return ERROR(corruption_detected);   /* impossible */
    }
}


size_t ZSTD_decodeSeqHeaders(int* nbSeq, const BYTE** dumpsPtr, size_t* dumpsLengthPtr,
                         FSE_DTable* DTableLL, FSE_DTable* DTableML, FSE_DTable* DTableOffb,
                         const void* src, size_t srcSize)
{
    const BYTE* const istart = (const BYTE* const)src;
    const BYTE* ip = istart;
    const BYTE* const iend = istart + srcSize;
    U32 LLtype, Offtype, MLtype;
    U32 LLlog, Offlog, MLlog;
    size_t dumpsLength;

    /* check */
    if (srcSize < 5) return ERROR(srcSize_wrong);

    /* SeqHead */
    *nbSeq = MEM_readLE16(ip); ip+=2;
    LLtype  = *ip >> 6;
    Offtype = (*ip >> 4) & 3;
    MLtype  = (*ip >> 2) & 3;
    if (*ip & 2)
    {
        dumpsLength  = ip[2];
        dumpsLength += ip[1] << 8;
        ip += 3;
    }
    else
    {
        dumpsLength  = ip[1];
        dumpsLength += (ip[0] & 1) << 8;
        ip += 2;
    }
    *dumpsPtr = ip;
    ip += dumpsLength;
    *dumpsLengthPtr = dumpsLength;

    /* check */
    if (ip > iend-3) return ERROR(srcSize_wrong); /* min : all 3 are "raw", hence no header, but at least xxLog bits per type */

    /* sequences */
    {
        S16 norm[MaxML+1];    /* assumption : MaxML >= MaxLL >= MaxOff */
        size_t headerSize;

        /* Build DTables */
        switch(LLtype)
        {
        U32 max;
        case bt_rle :
            LLlog = 0;
            FSE_buildDTable_rle(DTableLL, *ip++); break;
        case bt_raw :
            LLlog = LLbits;
            FSE_buildDTable_raw(DTableLL, LLbits); break;
        default :
            max = MaxLL;
            headerSize = FSE_readNCount(norm, &max, &LLlog, ip, iend-ip);
            if (FSE_isError(headerSize)) return ERROR(GENERIC);
            if (LLlog > LLFSELog) return ERROR(corruption_detected);
            ip += headerSize;
            FSE_buildDTable(DTableLL, norm, max, LLlog);
        }

        switch(Offtype)
        {
        U32 max;
        case bt_rle :
            Offlog = 0;
            if (ip > iend-2) return ERROR(srcSize_wrong);   /* min : "raw", hence no header, but at least xxLog bits */
            FSE_buildDTable_rle(DTableOffb, *ip++ & MaxOff); /* if *ip > MaxOff, data is corrupted */
            break;
        case bt_raw :
            Offlog = Offbits;
            FSE_buildDTable_raw(DTableOffb, Offbits); break;
        default :
            max = MaxOff;
            headerSize = FSE_readNCount(norm, &max, &Offlog, ip, iend-ip);
            if (FSE_isError(headerSize)) return ERROR(GENERIC);
            if (Offlog > OffFSELog) return ERROR(corruption_detected);
            ip += headerSize;
            FSE_buildDTable(DTableOffb, norm, max, Offlog);
        }

        switch(MLtype)
        {
        U32 max;
        case bt_rle :
            MLlog = 0;
            if (ip > iend-2) return ERROR(srcSize_wrong); /* min : "raw", hence no header, but at least xxLog bits */
            FSE_buildDTable_rle(DTableML, *ip++); break;
        case bt_raw :
            MLlog = MLbits;
            FSE_buildDTable_raw(DTableML, MLbits); break;
        default :
            max = MaxML;
            headerSize = FSE_readNCount(norm, &max, &MLlog, ip, iend-ip);
            if (FSE_isError(headerSize)) return ERROR(GENERIC);
            if (MLlog > MLFSELog) return ERROR(corruption_detected);
            ip += headerSize;
            FSE_buildDTable(DTableML, norm, max, MLlog);
        }
    }

    return ip-istart;
}


typedef struct {
    size_t litLength;
    size_t offset;
    size_t matchLength;
} seq_t;

typedef struct {
    BIT_DStream_t DStream;
    FSE_DState_t stateLL;
    FSE_DState_t stateOffb;
    FSE_DState_t stateML;
    size_t prevOffset;
    const BYTE* dumps;
    const BYTE* dumpsEnd;
} seqState_t;


static void ZSTD_decodeSequence(seq_t* seq, seqState_t* seqState)
{
    size_t litLength;
    size_t prevOffset;
    size_t offset;
    size_t matchLength;
    const BYTE* dumps = seqState->dumps;
    const BYTE* const de = seqState->dumpsEnd;

    /* Literal length */
    litLength = FSE_decodeSymbol(&(seqState->stateLL), &(seqState->DStream));
    prevOffset = litLength ? seq->offset : seqState->prevOffset;
    if (litLength == MaxLL)
    {
        U32 add = *dumps++;
        if (add < 255) litLength += add;
        else
        {
            litLength = MEM_readLE32(dumps) & 0xFFFFFF;  /* no pb : dumps is always followed by seq tables > 1 byte */
            dumps += 3;
        }
        if (dumps >= de) dumps = de-1;   /* late correction, to avoid read overflow (data is now corrupted anyway) */
    }

    /* Offset */
    {
        static const U32 offsetPrefix[MaxOff+1] = {
                1 /*fake*/, 1, 2, 4, 8, 16, 32, 64, 128, 256,
                512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144,
                524288, 1048576, 2097152, 4194304, 8388608, 16777216, 33554432, /*fake*/ 1, 1, 1, 1, 1 };
        U32 offsetCode, nbBits;
        offsetCode = FSE_decodeSymbol(&(seqState->stateOffb), &(seqState->DStream));   /* <= maxOff, by table construction */
        if (MEM_32bits()) BIT_reloadDStream(&(seqState->DStream));
        nbBits = offsetCode - 1;
        if (offsetCode==0) nbBits = 0;   /* cmove */
        offset = offsetPrefix[offsetCode] + BIT_readBits(&(seqState->DStream), nbBits);
        if (MEM_32bits()) BIT_reloadDStream(&(seqState->DStream));
        if (offsetCode==0) offset = prevOffset;   /* cmove */
        if (offsetCode | !litLength) seqState->prevOffset = seq->offset;   /* cmove */
    }

    /* MatchLength */
    matchLength = FSE_decodeSymbol(&(seqState->stateML), &(seqState->DStream));
    if (matchLength == MaxML)
    {
        U32 add = *dumps++;
        if (add < 255) matchLength += add;
        else
        {
            matchLength = MEM_readLE32(dumps) & 0xFFFFFF;  /* no pb : dumps is always followed by seq tables > 1 byte */
            dumps += 3;
        }
        if (dumps >= de) dumps = de-1;   /* late correction, to avoid read overflow (data is now corrupted anyway) */
    }
    matchLength += MINMATCH;

    /* save result */
    seq->litLength = litLength;
    seq->offset = offset;
    seq->matchLength = matchLength;
    seqState->dumps = dumps;
}


FORCE_INLINE size_t ZSTD_execSequence(BYTE* op,
                                BYTE* const oend, seq_t sequence,
                                const BYTE** litPtr, const BYTE* const litLimit_8,
                                const BYTE* const base, const BYTE* const vBase, const BYTE* const dictEnd)
{
    static const int dec32table[] = { 0, 1, 2, 1, 4, 4, 4, 4 };   /* added */
    static const int dec64table[] = { 8, 8, 8, 7, 8, 9,10,11 };   /* substracted */
    BYTE* const oLitEnd = op + sequence.litLength;
    const size_t sequenceLength = sequence.litLength + sequence.matchLength;
    BYTE* const oMatchEnd = op + sequenceLength;   /* risk : address space overflow (32-bits) */
    BYTE* const oend_8 = oend-8;
    const BYTE* const litEnd = *litPtr + sequence.litLength;
    const BYTE* match = oLitEnd - sequence.offset;

    /* check */
    if (oLitEnd > oend_8) return ERROR(dstSize_tooSmall);   /* last match must start at a minimum distance of 8 from oend */
    if (oMatchEnd > oend) return ERROR(dstSize_tooSmall);   /* overwrite beyond dst buffer */
    if (litEnd > litLimit_8) return ERROR(corruption_detected);   /* risk read beyond lit buffer */

    /* copy Literals */
    ZSTD_wildcopy(op, *litPtr, sequence.litLength);   /* note : oLitEnd <= oend-8 : no risk of overwrite beyond oend */
    op = oLitEnd;
    *litPtr = litEnd;   /* update for next sequence */

    /* copy Match */
    if (sequence.offset > (size_t)(oLitEnd - base))
    {
        /* offset beyond prefix */
        if (sequence.offset > (size_t)(oLitEnd - vBase))
            return ERROR(corruption_detected);
        match = dictEnd - (base-match);
        if (match + sequence.matchLength <= dictEnd)
        {
            memmove(oLitEnd, match, sequence.matchLength);
            return sequenceLength;
        }
        /* span extDict & currentPrefixSegment */
        {
            size_t length1 = dictEnd - match;
            memmove(oLitEnd, match, length1);
            op = oLitEnd + length1;
            sequence.matchLength -= length1;
            match = base;
        }
    }

    /* match within prefix */
    if (sequence.offset < 8)
    {
        /* close range match, overlap */
        const int sub2 = dec64table[sequence.offset];
        op[0] = match[0];
        op[1] = match[1];
        op[2] = match[2];
        op[3] = match[3];
        match += dec32table[sequence.offset];
        ZSTD_copy4(op+4, match);
        match -= sub2;
    }
    else
    {
        ZSTD_copy8(op, match);
    }
    op += 8; match += 8;

    if (oMatchEnd > oend-12)
    {
        if (op < oend_8)
        {
            ZSTD_wildcopy(op, match, oend_8 - op);
            match += oend_8 - op;
            op = oend_8;
        }
        while (op < oMatchEnd) *op++ = *match++;
    }
    else
    {
        ZSTD_wildcopy(op, match, sequence.matchLength-8);   /* works even if matchLength < 8 */
    }
    return sequenceLength;
}


static size_t ZSTD_decompressSequences(
                               ZSTD_DCtx* dctx,
                               void* dst, size_t maxDstSize,
                         const void* seqStart, size_t seqSize)
{
    const BYTE* ip = (const BYTE*)seqStart;
    const BYTE* const iend = ip + seqSize;
    BYTE* const ostart = (BYTE* const)dst;
    BYTE* op = ostart;
    BYTE* const oend = ostart + maxDstSize;
    size_t errorCode, dumpsLength;
    const BYTE* litPtr = dctx->litPtr;
    const BYTE* const litLimit_8 = litPtr + dctx->litBufSize - 8;
    const BYTE* const litEnd = litPtr + dctx->litSize;
    int nbSeq;
    const BYTE* dumps;
    U32* DTableLL = dctx->LLTable;
    U32* DTableML = dctx->MLTable;
    U32* DTableOffb = dctx->OffTable;
    const BYTE* const base = (const BYTE*) (dctx->base);
    const BYTE* const vBase = (const BYTE*) (dctx->vBase);
    const BYTE* const dictEnd = (const BYTE*) (dctx->dictEnd);

    /* Build Decoding Tables */
    errorCode = ZSTD_decodeSeqHeaders(&nbSeq, &dumps, &dumpsLength,
                                      DTableLL, DTableML, DTableOffb,
                                      ip, iend-ip);
    if (ZSTD_isError(errorCode)) return errorCode;
    ip += errorCode;

    /* Regen sequences */
    {
        seq_t sequence;
        seqState_t seqState;

        memset(&sequence, 0, sizeof(sequence));
        sequence.offset = 4;
        seqState.dumps = dumps;
        seqState.dumpsEnd = dumps + dumpsLength;
        seqState.prevOffset = 4;
        errorCode = BIT_initDStream(&(seqState.DStream), ip, iend-ip);
        if (ERR_isError(errorCode)) return ERROR(corruption_detected);
        FSE_initDState(&(seqState.stateLL), &(seqState.DStream), DTableLL);
        FSE_initDState(&(seqState.stateOffb), &(seqState.DStream), DTableOffb);
        FSE_initDState(&(seqState.stateML), &(seqState.DStream), DTableML);

        for ( ; (BIT_reloadDStream(&(seqState.DStream)) <= BIT_DStream_completed) && nbSeq ; )
        {
            size_t oneSeqSize;
            nbSeq--;
            ZSTD_decodeSequence(&sequence, &seqState);
            oneSeqSize = ZSTD_execSequence(op, oend, sequence, &litPtr, litLimit_8, base, vBase, dictEnd);
            if (ZSTD_isError(oneSeqSize)) return oneSeqSize;
            op += oneSeqSize;
        }

        /* check if reached exact end */
        if ( !BIT_endOfDStream(&(seqState.DStream)) ) return ERROR(corruption_detected);   /* DStream should be entirely and exactly consumed; otherwise data is corrupted */

        /* last literal segment */
        {
            size_t lastLLSize = litEnd - litPtr;
            if (litPtr > litEnd) return ERROR(corruption_detected);
            if (op+lastLLSize > oend) return ERROR(dstSize_tooSmall);
            if (op != litPtr) memcpy(op, litPtr, lastLLSize);
            op += lastLLSize;
        }
    }

    return op-ostart;
}


static void ZSTD_checkContinuity(ZSTD_DCtx* dctx, const void* dst)
{
    if (dst != dctx->previousDstEnd)   /* not contiguous */
    {
        dctx->dictEnd = dctx->previousDstEnd;
        dctx->vBase = (const char*)dst - ((const char*)(dctx->previousDstEnd) - (const char*)(dctx->base));
        dctx->base = dst;
        dctx->previousDstEnd = dst;
    }
}


static size_t ZSTD_decompressBlock_internal(ZSTD_DCtx* dctx,
                            void* dst, size_t maxDstSize,
                      const void* src, size_t srcSize)
{
    /* blockType == blockCompressed */
    const BYTE* ip = (const BYTE*)src;

    /* Decode literals sub-block */
    size_t litCSize = ZSTD_decodeLiteralsBlock(dctx, src, srcSize);
    if (ZSTD_isError(litCSize)) return litCSize;
    ip += litCSize;
    srcSize -= litCSize;

    return ZSTD_decompressSequences(dctx, dst, maxDstSize, ip, srcSize);
}


size_t ZSTD_decompressBlock(ZSTD_DCtx* dctx,
                            void* dst, size_t maxDstSize,
                      const void* src, size_t srcSize)
{
    ZSTD_checkContinuity(dctx, dst);
    return ZSTD_decompressBlock_internal(dctx, dst, maxDstSize, src, srcSize);
}


size_t ZSTD_decompress_usingDict(ZSTD_DCtx* dctx,
                                 void* dst, size_t maxDstSize,
                                 const void* src, size_t srcSize,
                                 const void* dict, size_t dictSize)
{
    const BYTE* ip = (const BYTE*)src;
    const BYTE* iend = ip + srcSize;
    BYTE* const ostart = (BYTE* const)dst;
    BYTE* op = ostart;
    BYTE* const oend = ostart + maxDstSize;
    size_t remainingSize = srcSize;
    blockProperties_t blockProperties;

    /* init */
    ZSTD_resetDCtx(dctx);
    if (dict)
    {
        size_t errorCode = ZSTD_decompress_insertDictionary(dctx, dict, dictSize);
        if (ZSTD_isError(errorCode)) return ERROR(dictionary_corrupted);
        dctx->dictEnd = dctx->previousDstEnd;
        dctx->vBase = (const char*)dst - ((const char*)(dctx->previousDstEnd) - (const char*)(dctx->base));
        dctx->base = dst;
    }
    else
    {
        dctx->vBase = dctx->base = dctx->dictEnd = dst;
    }

    /* Frame Header */
    {
        size_t frameHeaderSize;
        if (srcSize < ZSTD_frameHeaderSize_min+ZSTD_blockHeaderSize) return ERROR(srcSize_wrong);
#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT==1)
        {
            const U32 magicNumber = MEM_readLE32(src);
            if (ZSTD_isLegacy(magicNumber))
                return ZSTD_decompressLegacy(dst, maxDstSize, src, srcSize, magicNumber);
        }
#endif
        frameHeaderSize = ZSTD_decodeFrameHeader_Part1(dctx, src, ZSTD_frameHeaderSize_min);
        if (ZSTD_isError(frameHeaderSize)) return frameHeaderSize;
        if (srcSize < frameHeaderSize+ZSTD_blockHeaderSize) return ERROR(srcSize_wrong);
        ip += frameHeaderSize; remainingSize -= frameHeaderSize;
        frameHeaderSize = ZSTD_decodeFrameHeader_Part2(dctx, src, frameHeaderSize);
        if (ZSTD_isError(frameHeaderSize)) return frameHeaderSize;
    }

    /* Loop on each block */
    while (1)
    {
        size_t decodedSize=0;
        size_t cBlockSize = ZSTD_getcBlockSize(ip, iend-ip, &blockProperties);
        if (ZSTD_isError(cBlockSize)) return cBlockSize;

        ip += ZSTD_blockHeaderSize;
        remainingSize -= ZSTD_blockHeaderSize;
        if (cBlockSize > remainingSize) return ERROR(srcSize_wrong);

        switch(blockProperties.blockType)
        {
        case bt_compressed:
            decodedSize = ZSTD_decompressBlock_internal(dctx, op, oend-op, ip, cBlockSize);
            break;
        case bt_raw :
            decodedSize = ZSTD_copyRawBlock(op, oend-op, ip, cBlockSize);
            break;
        case bt_rle :
            return ERROR(GENERIC);   /* not yet supported */
            break;
        case bt_end :
            /* end of frame */
            if (remainingSize) return ERROR(srcSize_wrong);
            break;
        default:
            return ERROR(GENERIC);   /* impossible */
        }
        if (cBlockSize == 0) break;   /* bt_end */

        if (ZSTD_isError(decodedSize)) return decodedSize;
        op += decodedSize;
        ip += cBlockSize;
        remainingSize -= cBlockSize;
    }

    return op-ostart;
}


size_t ZSTD_decompressDCtx(ZSTD_DCtx* dctx, void* dst, size_t maxDstSize, const void* src, size_t srcSize)
{
    return ZSTD_decompress_usingDict(dctx, dst, maxDstSize, src, srcSize, NULL, 0);
}

size_t ZSTD_decompress(void* dst, size_t maxDstSize, const void* src, size_t srcSize)
{
#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
    size_t regenSize;
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (dctx==NULL) return ERROR(memory_allocation);
    regenSize = ZSTD_decompressDCtx(dctx, dst, maxDstSize, src, srcSize);
    ZSTD_freeDCtx(dctx);
    return regenSize;
#else
    ZSTD_DCtx dctx;
    return ZSTD_decompressDCtx(&dctx, dst, maxDstSize, src, srcSize);
#endif
}


/* ******************************
*  Streaming Decompression API
********************************/
size_t ZSTD_nextSrcSizeToDecompress(ZSTD_DCtx* dctx)
{
    return dctx->expected;
}

size_t ZSTD_decompressContinue(ZSTD_DCtx* dctx, void* dst, size_t maxDstSize, const void* src, size_t srcSize)
{
    /* Sanity check */
    if (srcSize != dctx->expected) return ERROR(srcSize_wrong);
    ZSTD_checkContinuity(dctx, dst);

    /* Decompress : frame header; part 1 */
    switch (dctx->stage)
    {
    case ZSTDds_getFrameHeaderSize :
        {
            /* get frame header size */
            if (srcSize != ZSTD_frameHeaderSize_min) return ERROR(srcSize_wrong);   /* impossible */
            dctx->headerSize = ZSTD_decodeFrameHeader_Part1(dctx, src, ZSTD_frameHeaderSize_min);
            if (ZSTD_isError(dctx->headerSize)) return dctx->headerSize;
            memcpy(dctx->headerBuffer, src, ZSTD_frameHeaderSize_min);
            if (dctx->headerSize > ZSTD_frameHeaderSize_min)
            {
                dctx->expected = dctx->headerSize - ZSTD_frameHeaderSize_min;
                dctx->stage = ZSTDds_decodeFrameHeader;
                return 0;
            }
            dctx->expected = 0;   /* not necessary to copy more */
        }
    case ZSTDds_decodeFrameHeader:
        {
            /* get frame header */
            size_t result;
            memcpy(dctx->headerBuffer + ZSTD_frameHeaderSize_min, src, dctx->expected);
            result = ZSTD_decodeFrameHeader_Part2(dctx, dctx->headerBuffer, dctx->headerSize);
            if (ZSTD_isError(result)) return result;
            dctx->expected = ZSTD_blockHeaderSize;
            dctx->stage = ZSTDds_decodeBlockHeader;
            return 0;
        }
    case ZSTDds_decodeBlockHeader:
        {
            /* Decode block header */
            blockProperties_t bp;
            size_t blockSize = ZSTD_getcBlockSize(src, ZSTD_blockHeaderSize, &bp);
            if (ZSTD_isError(blockSize)) return blockSize;
            if (bp.blockType == bt_end)
            {
                dctx->expected = 0;
                dctx->stage = ZSTDds_getFrameHeaderSize;
            }
            else
            {
                dctx->expected = blockSize;
                dctx->bType = bp.blockType;
                dctx->stage = ZSTDds_decompressBlock;
            }
            return 0;
        }
    case ZSTDds_decompressBlock:
        {
            /* Decompress : block content */
            size_t rSize;
            switch(dctx->bType)
            {
            case bt_compressed:
                rSize = ZSTD_decompressBlock_internal(dctx, dst, maxDstSize, src, srcSize);
                break;
            case bt_raw :
                rSize = ZSTD_copyRawBlock(dst, maxDstSize, src, srcSize);
                break;
            case bt_rle :
                return ERROR(GENERIC);   /* not yet handled */
                break;
            case bt_end :   /* should never happen (filtered at phase 1) */
                rSize = 0;
                break;
            default:
                return ERROR(GENERIC);
            }
            dctx->stage = ZSTDds_decodeBlockHeader;
            dctx->expected = ZSTD_blockHeaderSize;
            dctx->previousDstEnd = (char*)dst + rSize;
            return rSize;
        }
    default:
        return ERROR(GENERIC);   /* impossible */
    }
}


static void ZSTD_refDictContent(ZSTD_DCtx* dctx, const void* dict, size_t dictSize)
{
    dctx->dictEnd = dctx->previousDstEnd;
    dctx->vBase = (const char*)dict - ((const char*)(dctx->previousDstEnd) - (const char*)(dctx->base));
    dctx->base = dict;
    dctx->previousDstEnd = (const char*)dict + dictSize;
}


static size_t ZSTD_loadEntropy(ZSTD_DCtx* dctx, const void* dict, size_t dictSize)
{
    size_t hSize = HUF_readDTableX4(dctx->hufTableX4, dict, dictSize);
    if (HUF_isError(hSize)) return ERROR(dictionary_corrupted);
    return hSize;
}

size_t ZSTD_decompress_insertDictionary(ZSTD_DCtx* dctx, const void* dict, size_t dictSize)
{
    size_t eSize;
    U32 magic = MEM_readLE32(dict);
    if (magic != ZSTD_DICT_MAGIC) {
        /* pure content mode */
        ZSTD_refDictContent(dctx, dict, dictSize);
        return 0;
    }
    /* load entropy tables */
    dict = (const char*)dict + 4;
    dictSize -= 4;
    eSize = ZSTD_loadEntropy(dctx, dict, dictSize);
    if (ZSTD_isError(eSize)) return ERROR(dictionary_corrupted);

    /* reference dictionary content */
    dict = (const char*)dict + eSize;
    dictSize -= eSize;
    ZSTD_refDictContent(dctx, dict, dictSize);

    return 0;
}

