#include <stdlib.h>
#include <windows.h>
#include "fb_convert.h"
#include "pixel_convert.h"
#include "utils.h"

FORCE_INLINE UNUSED
uint16_t argb8888_to_rgb1555(uint32_t xl)
{
	Pixel32ARGB x;
	x.l = xl;
	return (x.p.b >> 3) | ((x.p.g >> 3) << 5) | ((x.p.r >> 3) << 10);
}

FORCE_INLINE UNUSED
uint8_t argb8888_to_rgb332(uint32_t xl)
{
	Pixel32ARGB x;
	x.l = xl;
	return (x.p.r >> 5) | ((x.p.g >> 5) << 3) | ((x.p.b >> 6) << 6);
}

uint8_t* swrConvert32to24(uint8_t* dest, uint32_t* src, int width, int height)
{
	int size = width * height;
	if (!dest) {
		dest = safeCalloc(3, size);
	}
	
	for (int i = 0, j = 0; i < size; i++)
	{
		Pixel32ARGB x;
		x.l = src[i];
		
		dest[j++] = x.p.b;
		dest[j++] = x.p.g;
		dest[j++] = x.p.r;
	}
	
	return dest;
}

uint16_t* swrConvert32to16(uint16_t* dest, uint32_t* src, int width, int height)
{
	int size = width * height;
	if (!dest) {
		dest = safeCalloc(sizeof(uint16_t), size);
	}
	
	for (int i = 0; i < size; i++)
		dest[i] = argb8888_to_rgb1555(src[i]);
	
	return dest;
}

uint8_t* swrConvert32to8(uint8_t* dest, uint32_t* src, int width, int height)
{
	int size = width * height;
	if (!dest) {
		dest = safeCalloc(sizeof(uint8_t), size);
	}
	
	for (int i = 0; i < size; i++)
		dest[i] = argb8888_to_rgb332(src[i]);
	
	return dest;
}

typedef struct
{
	BITMAPINFOHEADER bmiHeader;
	RGBQUAD bmiColors[256];
}
BITMAPINFO_8BIT;

BITMAPINFO* swrSetup8BitBitmapInfo(int width, int height)
{
	static BITMAPINFO_8BIT* pBmi;
	if (!pBmi) {
		pBmi = safeCalloc(1, sizeof(BITMAPINFO_8BIT));
		
		// set up palette
		int i = 0;
		for (int b = 0; b < 4; b++)
		for (int g = 0; g < 8; g++)
		for (int r = 0; r < 8; r++)
		{
			pBmi->bmiColors[i].rgbRed      = (r << 5) + (r << 2) + (r >> 1);
			pBmi->bmiColors[i].rgbGreen    = (g << 5) + (g << 2) + (g >> 1);
			pBmi->bmiColors[i].rgbBlue     = (b << 6) + (b << 4) + (b << 2) + b;
			pBmi->bmiColors[i].rgbReserved = 255;
			i++;
		}
	}
	
	pBmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	pBmi->bmiHeader.biWidth = width;
	pBmi->bmiHeader.biHeight = -height;
	pBmi->bmiHeader.biPlanes = 1;
	pBmi->bmiHeader.biCompression = BI_RGB;
	pBmi->bmiHeader.biBitCount = 8;
	
	return (BITMAPINFO*) pBmi;
}
