// ---------------------------------------------------------------------------
// Minimal Allwinner CedarX video-encoder interface (subset of vencoder.h).
//
// libvencoder.so on the W10 is the standard CedarX encoder wrapper: it is
// NEEDED-linked against libvenc_codec / libvenc_base / libVE / libcdc_base /
// libMemAdapter and exposes the well-known VideoEnc* C API. We dlopen it at
// runtime and call these symbols. Struct field offsets used below were checked
// against a disassembly of VideoEncInit / GetOneBitstreamFrame (see
// docs/REVERSE_ENGINEERING.md): nDstWidth@0x0c, nDstHeight@0x10, and the
// memops/veops pointers the wrapper fills at 0x20/0x28/0x30.
//
// NOTE: H264 rate/GOP parameter *index* values are the classic Allwinner enum
// values and should be confirmed on-device (they vary slightly by BSP). The
// frame plumbing (create/init/alloc/encode/get-bitstream) is stable.
// ---------------------------------------------------------------------------
#ifndef VENCODER_H
#define VENCODER_H
#include <stdint.h>

typedef enum {
	VENC_CODEC_H264 = 0,
	VENC_CODEC_JPEG,
	VENC_CODEC_H265,
} VENC_CODEC_TYPE;

typedef enum {
	VENC_PIXEL_YUV420SP = 0,   // NV12
	VENC_PIXEL_YVU420SP = 1,   // NV21  <-- our input
	VENC_PIXEL_YUV420P  = 2,
	VENC_PIXEL_YVU420P  = 3,
} VENC_PIXEL_FMT;

// VideoEncSetParameter indices (classic CedarX). Confirm on device if needed.
typedef enum {
	VENC_IndexParamBitrate        = 0,   // int, bits/sec
	VENC_IndexParamFramerate      = 1,   // int, fps
	VENC_IndexParamMaxKeyInterval = 2,   // int, GOP length (frames)
} VENC_INDEXTYPE;

// --- VencBaseConfig (offsets verified for the fields we set) ---------------
typedef struct {
	uint32_t nInputWidth;    // 0x00
	uint32_t nInputHeight;   // 0x04
	uint32_t nStride;        // 0x08
	uint32_t nDstWidth;      // 0x0c
	uint32_t nDstHeight;     // 0x10
	uint32_t eInputFormat;   // 0x14  (VENC_PIXEL_FMT)
	uint32_t _pad18;         // 0x18
	uint32_t _pad1c;
	void    *memops;         // 0x20  (filled by VideoEncInit)
	void    *veOpsS;         // 0x28  (filled by VideoEncInit)
	void    *pVeOpsSelf;     // 0x30  (filled by VideoEncInit)
	uint8_t  _rest[0x80];    // remaining CedarX fields, zeroed
} VencBaseConfig;

typedef struct {
	uint32_t nBufferNum;
	uint32_t nSizeY;
	uint32_t nSizeC;
} VencAllocateBufferParam;

// Returned by VideoEncGetParameter for the SPS/PPS header index.
typedef struct {
	uint8_t *pBuffer;
	uint32_t nLength;
} VencHeaderData;

// --- VencInputBuffer (standard CedarX layout) ------------------------------
typedef struct {
	unsigned long  nID;
	int64_t        nPts;
	uint32_t       nFlag;
	uint8_t       *pAddrPhyY;
	uint8_t       *pAddrPhyC;
	uint8_t       *pAddrVirY;   // <-- copy Y plane here
	uint8_t       *pAddrVirC;   // <-- copy VU plane here
	int            bEnableCorp;
	int            sCropInfo[4];
	int            bAllocMemSelf;
	int            nShareBufFd;
	uint8_t        _rest[0x40];
} VencInputBuffer;

// --- VencOutputBuffer (standard CedarX layout) -----------------------------
typedef struct {
	int            nID;
	int64_t        nPts;
	uint32_t       nFlag;
	uint32_t       nSize0;      // bytes at pData0
	uint32_t       nSize1;      // bytes at pData1 (ring wrap)
	uint8_t       *pData0;      // <-- H264 Annex-B NAL
	uint8_t       *pData1;
} VencOutputBuffer;

// --- function pointer types for the symbols we dlsym -----------------------
typedef void* (*fn_VideoEncCreate)(VENC_CODEC_TYPE);
typedef void  (*fn_VideoEncDestroy)(void*);
typedef int   (*fn_VideoEncInit)(void*, VencBaseConfig*);
typedef int   (*fn_VideoEncUnInit)(void*);
typedef int   (*fn_VideoEncSetParameter)(void*, VENC_INDEXTYPE, void*);
typedef int   (*fn_VideoEncGetParameter)(void*, int, void*);
typedef int   (*fn_AllocInputBuffer)(void*, VencAllocateBufferParam*);
typedef int   (*fn_GetOneAllocInputBuffer)(void*, VencInputBuffer*);
typedef int   (*fn_FlushCacheAllocInputBuffer)(void*, VencInputBuffer*);
typedef int   (*fn_AddOneInputBuffer)(void*, VencInputBuffer*);
typedef int   (*fn_VideoEncodeOneFrame)(void*);
typedef int   (*fn_AlreadyUsedInputBuffer)(void*, VencInputBuffer*);
typedef int   (*fn_ReturnOneAllocInputBuffer)(void*, VencInputBuffer*);
typedef int   (*fn_GetOneBitstreamFrame)(void*, VencOutputBuffer*);
typedef int   (*fn_FreeOneBitStreamFrame)(void*, VencOutputBuffer*);

#endif
