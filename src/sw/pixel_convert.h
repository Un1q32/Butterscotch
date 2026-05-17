#pragma once

#include "defines.h"

// CONFIG: Change the size of a pixel.
//
// 32-bit: 0xAARRGGBB
// 16-bit: 0b0RRRRRGGGGGBBBBB
// 8-bit:  0bBBGGGRRR
#define PIXEL_SIZE 32
//#define PIXEL_SIZE 16
//#define PIXEL_SIZE 8

#if PIXEL_SIZE == 32
typedef uint32_t uintpixel_t;
#elif PIXEL_SIZE == 16
typedef uint16_t uintpixel_t;
#elif PIXEL_SIZE == 8
typedef uint8_t uintpixel_t;
#define PXL_TRANSPARENT (0xAA)
#else
#error "Unknown pixel size!"
#endif

typedef union
{
	struct {
		uint8_t r, g, b, a;
	} p;
	uint32_t l;
}
Pixel32ABGR;

typedef union
{
	struct {
		uint8_t b, g, r, a;
	} p;
	uint32_t l;
}
Pixel32ARGB;

FORCE_INLINE UNUSED
uint16_t abgr8888_to_rgb1555(uint32_t xl)
{
	Pixel32ABGR x;
	x.l = xl;
	return (x.p.b >> 3) | ((x.p.g >> 3) << 5) | ((x.p.r >> 3) << 10) | ((x.p.a >> 7) << 15);
}

FORCE_INLINE UNUSED
uint8_t abgr8888_to_rgb332(uint32_t xl)
{
	Pixel32ABGR x;
	x.l = xl;
	
#if PIXEL_SIZE == 8
	//check if transparent
	if (x.p.a < 128)
		return PXL_TRANSPARENT;
#endif
	
	uint8_t pxl = (x.p.r >> 5) | ((x.p.g >> 5) << 3) | ((x.p.b >> 6) << 6);
	
#if PIXEL_SIZE == 8
	//hacky fixup
	if (pxl == PXL_TRANSPARENT)
		pxl++;
#endif
	
	return pxl;
}

FORCE_INLINE UNUSED
uintpixel_t swrConvertPixelTexture(uint32_t gmPixel)
{
#if PIXEL_SIZE == 32
	return (gmPixel & 0xFF00FF00) | ((gmPixel & 0xFF) << 16) | ((gmPixel >> 16) & 0xFF);
#elif PIXEL_SIZE == 16
	return abgr8888_to_rgb1555(gmPixel);
#elif PIXEL_SIZE == 8
	return abgr8888_to_rgb332(gmPixel);
#endif
}

#if PIXEL_SIZE == 8
#define swrConvertPixel(x) swrConvertPixelTexture((x) | 0xFF000000)
#else
#define swrConvertPixel swrConvertPixelTexture
#endif
