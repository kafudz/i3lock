#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cairo.h>

#include "image.h"

static int failures = 0;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    unsigned char pixels[12] = {
        255, 0, 0,
        0, 255, 0,
        0, 0, 255,
        255, 255, 255,
    };

    char path[] = "/tmp/i3lock-image-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }

    ssize_t written = write(fd, pixels, sizeof(pixels));
    close(fd);
    if (written != (ssize_t)sizeof(pixels)) {
        perror("write");
        unlink(path);
        return EXIT_FAILURE;
    }

    cairo_surface_t *img = load_image(path, "2x2:rgb");
    check(img != NULL, "load_image loads a valid raw RGB image");
    if (img != NULL) {
        check(cairo_image_surface_get_width(img) == 2, "raw image width");
        check(cairo_image_surface_get_height(img) == 2, "raw image height");

        uint32_t *data = (uint32_t *)cairo_image_surface_get_data(img);
        size_t stride = (size_t)cairo_image_surface_get_stride(img) / 4;
        check(data[0] == 0x00ff0000, "raw image red pixel");
        check(data[1] == 0x0000ff00, "raw image green pixel");
        check(data[stride] == 0x000000ff, "raw image blue pixel");
        check(data[stride + 1] == 0x00ffffff, "raw image white pixel");

        cairo_surface_destroy(img);
    }

    check(load_image(path, "0x2:rgb") == NULL, "load_image rejects zero dimensions");
    check(load_image(path, "2x2:notafmt") == NULL, "load_image rejects an unknown pixel format");
    check(load_image(path, NULL) == NULL, "load_image rejects non-PNG input");

    unlink(path);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
