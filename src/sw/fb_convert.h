#pragma once

#include <stdint.h>

typedef struct tagBITMAPINFO BITMAPINFO;

uint8_t* swrConvert32to24(uint8_t* dest, uint32_t* src, int width, int height);

uint16_t* swrConvert32to16(uint16_t* dest, uint32_t* src, int width, int height);

uint8_t* swrConvert32to8(uint8_t* dest, uint32_t* src, int width, int height);

BITMAPINFO* swrSetup8BitBitmapInfo(int width, int height);
