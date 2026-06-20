#ifndef PUBLIC_FPDF_JPEG_H_
#define PUBLIC_FPDF_JPEG_H_

#include "fpdfview.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compress RGBA to JPEG. Returns byte size, or 0 on error.
// quality: JPEG quality [0..100]. A value of 85 is a good default.
// Caller MUST free the returned buffer with the module's free().
FPDF_EXPORT size_t FPDF_CALLCONV
EPDF_JPEG_EncodeRGBA(uint8_t* rgba,
                     int width,
                     int height,
                     int stride,
                     int quality,
                     uint8_t** out_ptr);

// Compress BGRA to JPEG. Returns byte size, or 0 on error.
// quality: JPEG quality [0..100]. A value of 85 is a good default.
// Caller MUST free the returned buffer with the module's free().
FPDF_EXPORT size_t FPDF_CALLCONV
EPDF_JPEG_EncodeBGRA(uint8_t* bgra,
                     int width,
                     int height,
                     int stride,
                     int quality,
                     uint8_t** out_ptr);

// Compress FPDF_BITMAP directly to JPEG.
// Supports FPDFBitmap_Gray (1), FPDFBitmap_BGR (2), FPDFBitmap_BGRx (3), and FPDFBitmap_BGRA (4) formats.
// Returns byte size, or 0 on error.
// Caller MUST free the returned buffer with the module's free().
FPDF_EXPORT size_t FPDF_CALLCONV
EPDF_JPEG_EncodeBitmap(FPDF_BITMAP bitmap,
                       int quality,
                       uint8_t** out_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PUBLIC_FPDF_JPEG_H_        