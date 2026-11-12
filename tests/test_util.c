#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static int failures = 0;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    size_t result = 0;

    check(size_mul(2, 3, &result) && result == 6, "size_mul computes the product");
    check(size_mul(0, SIZE_MAX, &result) && result == 0, "size_mul handles zero");
    check(!size_mul(SIZE_MAX, 2, &result), "size_mul detects overflow");
    check(!size_mul(1, 1, NULL), "size_mul rejects NULL result");

    check(size_add(2, 3, &result) && result == 5, "size_add computes the sum");
    check(size_add(0, 0, &result) && result == 0, "size_add handles zero");
    check(!size_add(SIZE_MAX, 1, &result), "size_add detects overflow");
    check(!size_add(1, 1, NULL), "size_add rejects NULL result");

    uint32_t pixel = 0;
    check(parse_hex_color("a3a3a3", &pixel) && pixel == 0xa3a3a3, "parse_hex_color parses lowercase");
    check(parse_hex_color("#FF0000", &pixel) && pixel == 0xff0000, "parse_hex_color accepts a leading # and uppercase");
    check(!parse_hex_color("12345", &pixel), "parse_hex_color rejects short input");
    check(!parse_hex_color("gg0000", &pixel), "parse_hex_color rejects non-hex input");
    check(!parse_hex_color(NULL, &pixel), "parse_hex_color rejects NULL input");

    size_t width = 0;
    size_t height = 0;
    char pixfmt[RAW_PIXFMT_MAXLEN + 1];

    check(parse_raw_image_format("1920x1080:rgb", &width, &height, pixfmt, sizeof(pixfmt)) &&
              width == 1920 && height == 1080 && strcmp(pixfmt, "rgb") == 0,
          "parse_raw_image_format parses a valid specification");
    check(parse_raw_image_format("1x2:native", &width, &height, pixfmt, sizeof(pixfmt)) &&
              width == 1 && height == 2 && strcmp(pixfmt, "native") == 0,
          "parse_raw_image_format parses native");
    check(!parse_raw_image_format("1920:rgb", &width, &height, pixfmt, sizeof(pixfmt)),
          "parse_raw_image_format rejects a missing height");
    check(!parse_raw_image_format("0x10:rgb", &width, &height, pixfmt, sizeof(pixfmt)),
          "parse_raw_image_format rejects a zero width");
    check(!parse_raw_image_format("10x0:rgb", &width, &height, pixfmt, sizeof(pixfmt)),
          "parse_raw_image_format rejects a zero height");
    check(!parse_raw_image_format("10x10:toolong", &width, &height, pixfmt, sizeof(pixfmt)),
          "parse_raw_image_format rejects an overlong pixel format");
    check(!parse_raw_image_format("10x10:rgbjunk", &width, &height, pixfmt, sizeof(pixfmt)),
          "parse_raw_image_format rejects trailing data");
    check(!parse_raw_image_format("10x10:", &width, &height, pixfmt, sizeof(pixfmt)),
          "parse_raw_image_format rejects an empty pixel format");

    char secret[16] = "top secret";
    secure_zero(secret, sizeof(secret));
    bool all_zero = true;
    for (size_t i = 0; i < sizeof(secret); i++) {
        if (secret[i] != 0) {
            all_zero = false;
        }
    }
    check(all_zero, "secure_zero wipes the whole buffer");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
