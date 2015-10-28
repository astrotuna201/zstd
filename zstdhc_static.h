/*
    zstdhc - high compression variant
    Header File - Experimental API, static linking only
    Copyright (C) 2015, Yann Collet.

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
    - zstd source repository : http://www.zstd.net
*/
#pragma once

#if defined (__cplusplus)
extern "C" {
#endif

/* *************************************
*  Includes
***************************************/
#include "mem.h"
#include "zstdhc.h"


/* *************************************
*  Types
***************************************/
typedef struct
{
    U32 windowLog;    /* largest match distance : impact decompression buffer size */
    U32 chainLog;     /* full search distance : larger == more compression, slower, more memory*/
    U32 hashLog;      /* dispatch table : larger == more memory, faster*/
    U32 searchLog;    /* nb of searches : larger == more compression, slower*/
} ZSTD_HC_parameters;

/* parameters boundaries */
#define ZSTD_HC_WINDOWLOG_MAX 26
#define ZSTD_HC_WINDOWLOG_MIN 18
#define ZSTD_HC_CHAINLOG_MAX ZSTD_HC_WINDOWLOG_MAX
#define ZSTD_HC_CHAINLOG_MIN 4
#define ZSTD_HC_HASHLOG_MAX 28
#define ZSTD_HC_HASHLOG_MIN 4
#define ZSTD_HC_SEARCHLOG_MAX (ZSTD_HC_CHAINLOG_MAX-1)
#define ZSTD_HC_SEARCHLOG_MIN 1


/* *************************************
*  Functions
***************************************/
/** ZSTD_HC_compress_advanced
*   Same as ZSTD_HC_compressCCtx(), but can fine-tune each compression parameter */
size_t ZSTD_HC_compress_advanced (ZSTD_HC_CCtx* ctx,
                                 void* dst, size_t maxDstSize,
                           const void* src, size_t srcSize,
                                 ZSTD_HC_parameters params);


/* *************************************
*  Pre-defined compression levels
***************************************/
#define ZSTD_HC_MAX_CLEVEL 25
static const ZSTD_HC_parameters ZSTD_HC_defaultParameters[ZSTD_HC_MAX_CLEVEL+1] = {
    /* W,  C,  H,  S */
    { 18, 12, 14,  1 },   /* level  0 - never used */
    { 18, 12, 15,  2 },   /* level  1 */
    { 19, 14, 16,  3 },   /* level  2 */
    { 20, 19, 19,  2 },   /* level  3 */
    { 20, 19, 19,  3 },   /* level  4 */
    { 20, 19, 19,  4 },   /* level  5 */
    { 20, 20, 19,  4 },   /* level  6 */
    { 20, 19, 19,  5 },   /* level  7 */
    { 20, 19, 19,  6 },   /* level  8 */
    { 20, 20, 20,  6 },   /* level  9 */
    { 21, 20, 21,  6 },   /* level 10 */
    { 21, 20, 21,  7 },   /* level 11 */
    { 21, 20, 22,  7 },   /* level 12 */
    { 21, 21, 23,  7 },   /* level 13 */
    { 21, 21, 23,  7 },   /* level 14 */
    { 21, 21, 23,  8 },   /* level 15 */
    { 21, 21, 23,  9 },   /* level 16 */
    { 21, 21, 23,  9 },   /* level 17 */
    { 21, 21, 23, 10 },   /* level 18 */
    { 22, 22, 23,  9 },   /* level 19 */
    { 22, 22, 23,  9 },   /* level 20 */
    { 22, 22, 23, 10 },   /* level 21 */
    { 22, 22, 23, 10 },   /* level 22 */
    { 22, 22, 23, 11 },   /* level 23 */
    { 22, 22, 23, 12 },   /* level 24 */
    { 23, 23, 23, 11 },   /* level 25 */
};




#if defined (__cplusplus)
}
#endif
