#ifndef _UTIL_H
#define _UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Maximum length of a pixel format name in a raw image specification. */
#define RAW_PIXFMT_MAXLEN 6

/*
 * Overwrites len bytes at ptr with zeros in a way the compiler is not allowed
 * to optimize away. Use this for passwords and other sensitive data.
 */
void secure_zero(void *ptr, size_t len);

/*
 * Overflow-checked size_t arithmetic. Both functions return false and leave
 * *result untouched when the operation would overflow.
 */
bool size_mul(size_t a, size_t b, size_t *result);
bool size_add(size_t a, size_t b, size_t *result);

/*
 * Parses a color in rrggbb (or #rrggbb) notation into a 0xRRGGBB pixel value.
 */
bool parse_hex_color(const char *hex, uint32_t *pixel);

/*
 * Parses a raw image specification of the form <width>x<height>:<pixfmt>.
 * The pixel format is copied into pixfmt (including the terminating NUL).
 * Returns false for malformed input or dimensions that do not fit into an int.
 */
bool parse_raw_image_format(const char *spec, size_t *width, size_t *height, char *pixfmt, size_t pixfmt_size);

#endif
