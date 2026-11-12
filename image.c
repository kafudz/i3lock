#include <config.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <cairo.h>

#include "image.h"
#include "util.h"

struct raw_pixel_format {
    size_t bpp;
    size_t red;
    size_t green;
    size_t blue;
};

static const struct raw_pixel_format raw_fmt_rgb = {3, 0, 1, 2};
static const struct raw_pixel_format raw_fmt_rgbx = {4, 0, 1, 2};
static const struct raw_pixel_format raw_fmt_xrgb = {4, 1, 2, 3};
static const struct raw_pixel_format raw_fmt_bgr = {3, 2, 1, 0};
static const struct raw_pixel_format raw_fmt_bgrx = {4, 2, 1, 0};
static const struct raw_pixel_format raw_fmt_xbgr = {4, 3, 2, 1};

static ssize_t read_raw_image_native(uint32_t *dest, FILE *src, size_t width, size_t height, size_t pixstride) {
    size_t row_bytes;
    if (!size_mul(width, 4, &row_bytes)) {
        return -1;
    }

    ssize_t count = 0;
    for (size_t y = 0; y < height; y++) {
        size_t n = fread(dest + y * pixstride, 1, row_bytes, src);
        count += (ssize_t)n;
        if (n < row_bytes) {
            break;
        }
    }

    return count;
}

static ssize_t read_raw_image_fmt(uint32_t *dest, FILE *src, size_t width, size_t height, size_t pixstride,
                                  const struct raw_pixel_format *fmt) {
    size_t row_bytes;
    if (!size_mul(width, fmt->bpp, &row_bytes)) {
        return -1;
    }

    unsigned char *buf = malloc(row_bytes);
    if (buf == NULL) {
        return -1;
    }

    ssize_t count = 0;
    for (size_t y = 0; y < height; y++) {
        size_t n = fread(buf, 1, row_bytes, src);
        count += (ssize_t)n;
        if (n < row_bytes) {
            break;
        }

        for (size_t x = 0; x < width; x++) {
            size_t idx = x * fmt->bpp;
            dest[y * pixstride + x] = 0 |
                                      (uint32_t)buf[idx + fmt->red] << 16 |
                                      (uint32_t)buf[idx + fmt->green] << 8 |
                                      (uint32_t)buf[idx + fmt->blue];
        }
    }

    free(buf);
    return count;
}

static const struct raw_pixel_format *lookup_raw_format(const char *name) {
    if (strcmp(name, "rgb") == 0) {
        return &raw_fmt_rgb;
    }
    if (strcmp(name, "rgbx") == 0) {
        return &raw_fmt_rgbx;
    }
    if (strcmp(name, "xrgb") == 0) {
        return &raw_fmt_xrgb;
    }
    if (strcmp(name, "bgr") == 0) {
        return &raw_fmt_bgr;
    }
    if (strcmp(name, "bgrx") == 0) {
        return &raw_fmt_bgrx;
    }
    if (strcmp(name, "xbgr") == 0) {
        return &raw_fmt_xbgr;
    }
    return NULL;
}

static cairo_surface_t *read_raw_image(const char *image_path, const char *image_raw_format) {
    size_t width;
    size_t height;
    char pixfmt[RAW_PIXFMT_MAXLEN + 1];

    if (!parse_raw_image_format(image_raw_format, &width, &height, pixfmt, sizeof(pixfmt))) {
        fprintf(stderr, "Invalid image format: \"%s\"\n", image_raw_format);
        return NULL;
    }

    const struct raw_pixel_format *fmt = NULL;
    size_t bpp = 4;
    if (strcmp(pixfmt, "native") != 0) {
        fmt = lookup_raw_format(pixfmt);
        if (fmt == NULL) {
            fprintf(stderr, "Unknown raw pixel format: %s\n", pixfmt);
            return NULL;
        }
        bpp = fmt->bpp;
    }

    size_t pixel_count;
    size_t surface_size;
    size_t expected_size;
    if (!size_mul(width, height, &pixel_count) ||
        !size_mul(pixel_count, 4, &surface_size) ||
        !size_mul(pixel_count, bpp, &expected_size) ||
        surface_size > (size_t)SSIZE_MAX ||
        expected_size > (size_t)SSIZE_MAX) {
        fprintf(stderr, "Image dimensions are too large: %zux%zu\n", width, height);
        return NULL;
    }

    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)width, (int)height);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Could not create surface: %s\n",
                cairo_status_to_string(cairo_surface_status(img)));
        cairo_surface_destroy(img);
        return NULL;
    }
    cairo_surface_flush(img);

    uint32_t *data = (uint32_t *)cairo_image_surface_get_data(img);
    int stride = cairo_image_surface_get_stride(img);
    if (data == NULL || stride <= 0) {
        fprintf(stderr, "Could not access image surface data\n");
        cairo_surface_destroy(img);
        return NULL;
    }
    size_t pixstride = (size_t)stride / 4;

    FILE *f = fopen(image_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "Could not open image \"%s\": %s\n", image_path, strerror(errno));
        cairo_surface_destroy(img);
        return NULL;
    }

    ssize_t count;
    if (fmt == NULL) {
        count = read_raw_image_native(data, f, width, height, pixstride);
    } else {
        count = read_raw_image_fmt(data, f, width, height, pixstride, fmt);
    }

    cairo_surface_mark_dirty(img);

    if (count < 0 || (size_t)count < expected_size) {
        if (count < 0 || ferror(f)) {
            fprintf(stderr, "Failed to read image \"%s\": %s\n", image_path, strerror(errno));
            fclose(f);
            cairo_surface_destroy(img);
            return NULL;
        }

        /* Print a warning if the file contains less data than expected, but
         * don't abort. It's useful to see how the image looks even if it is
         * wrong. */
        fprintf(stderr, "Warning: expected to read %zu bytes from \"%s\", read %zd\n",
                expected_size, image_path, count);
    }

    fclose(f);
    return img;
}

static bool verify_png_image(const char *image_path) {
    if (image_path == NULL) {
        return false;
    }

    /* Check file exists and has the correct PNG header. */
    FILE *png_file = fopen(image_path, "rb");
    if (png_file == NULL) {
        fprintf(stderr, "Image file path \"%s\" cannot be opened: %s\n", image_path, strerror(errno));
        return false;
    }

    unsigned char png_header[8];
    size_t bytes_read = fread(png_header, 1, sizeof(png_header), png_file);
    fclose(png_file);

    if (bytes_read != sizeof(png_header)) {
        fprintf(stderr, "Could not read PNG header from \"%s\"\n", image_path);
        return false;
    }

    /* Check the PNG header according to the specification, available at:
     * https://www.w3.org/TR/2003/REC-PNG-20031110/#5PNG-file-signature */
    static const unsigned char png_reference_header[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (memcmp(png_reference_header, png_header, sizeof(png_header)) != 0) {
        fprintf(stderr, "File \"%s\" does not start with a PNG header. i3lock currently only supports loading PNG files.\n", image_path);
        return false;
    }

    return true;
}

cairo_surface_t *load_image(const char *path, const char *raw_format) {
    if (path == NULL) {
        return NULL;
    }

    if (raw_format != NULL) {
        return read_raw_image(path, raw_format);
    }

    if (!verify_png_image(path)) {
        return NULL;
    }

    cairo_surface_t *img = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Could not load image \"%s\": %s\n",
                path, cairo_status_to_string(cairo_surface_status(img)));
        cairo_surface_destroy(img);
        return NULL;
    }

    return img;
}
