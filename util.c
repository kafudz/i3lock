#include <config.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_EXPLICIT_BZERO
#include <strings.h> /* explicit_bzero(3) */
#endif

#include "util.h"

void secure_zero(void *ptr, size_t len) {
    if (ptr == NULL || len == 0) {
        return;
    }

#ifdef HAVE_EXPLICIT_BZERO
    explicit_bzero(ptr, len);
#else
    volatile unsigned char *p = ptr;
    while (len-- > 0) {
        *p++ = 0;
    }
#endif
}

bool size_mul(size_t a, size_t b, size_t *result) {
    if (result == NULL) {
        return false;
    }
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *result = a * b;
    return true;
}

bool size_add(size_t a, size_t b, size_t *result) {
    if (result == NULL) {
        return false;
    }
    if (b > SIZE_MAX - a) {
        return false;
    }
    *result = a + b;
    return true;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool parse_hex_color(const char *hex, uint32_t *pixel) {
    if (hex == NULL || pixel == NULL) {
        return false;
    }

    if (*hex == '#') {
        hex++;
    }

    if (strlen(hex) != 6) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < 6; i++) {
        int digit = hex_value(hex[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (uint32_t)digit;
    }

    *pixel = value;
    return true;
}

bool parse_raw_image_format(const char *spec, size_t *width, size_t *height, char *pixfmt, size_t pixfmt_size) {
    if (spec == NULL || width == NULL || height == NULL || pixfmt == NULL || pixfmt_size == 0) {
        return false;
    }

    char format[RAW_PIXFMT_MAXLEN + 1];
    size_t w = 0;
    size_t h = 0;
    int consumed = 0;

    if (sscanf(spec, "%zux%zu:%6s%n", &w, &h, format, &consumed) != 3) {
        return false;
    }

    if (consumed <= 0 || spec[consumed] != '\0') {
        return false;
    }

    if (w == 0 || h == 0 || w > INT_MAX || h > INT_MAX) {
        return false;
    }

    size_t format_len = strlen(format);
    if (format_len >= pixfmt_size) {
        return false;
    }

    memcpy(pixfmt, format, format_len + 1);
    *width = w;
    *height = h;
    return true;
}
