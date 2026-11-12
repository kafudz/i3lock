#ifndef _IMAGE_H
#define _IMAGE_H

#include <cairo.h>

/*
 * Loads a background image. If raw_format is non-NULL, the file is interpreted
 * as a raw image with the given <width>x<height>:<pixfmt> specification.
 * Otherwise the file must be a PNG. Returns NULL on error.
 */
cairo_surface_t *load_image(const char *path, const char *raw_format);

#endif
