/*
 * vim:ts=4:sw=4:expandtab
 *
 * © 2010 Michael Stapelberg
 *
 * See LICENSE for licensing information
 *
 */
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo.h>
#include <cairo/cairo-xcb.h>

#include "i3lock.h"
#include "xcb.h"
#include "unlock_indicator.h"
#include "randr.h"
#include "dpi.h"
#include "password.h"
#include "util.h"

#define BUTTON_RADIUS 90
#define BUTTON_SPACE (BUTTON_RADIUS + 5)
#define BUTTON_CENTER (BUTTON_RADIUS + 5)
#define BUTTON_DIAMETER (2 * BUTTON_SPACE)

/*******************************************************************************
 * Variables defined in i3lock.c.
 ******************************************************************************/

extern bool debug_mode;

/* The lock window. */
extern xcb_window_t win;

/* The current resolution of the X11 root window. */
extern uint32_t last_resolution[2];

/* Whether the unlock indicator is enabled (defaults to true). */
extern bool unlock_indicator;

/* List of pressed modifiers, or NULL if none are pressed. */
extern char *modifier_string;
/* Name of the current keyboard layout or NULL if not initialized. */
char *layout_string = NULL;

/* A Cairo surface containing the specified image (-i), if any. */
extern cairo_surface_t *img;

/* Whether the image should be tiled. */
extern bool tile;
/* The background color to use (in hex). */
extern char color[7];

/* Whether the failed attempts should be displayed. */
extern bool show_failed_attempts;
/* Whether keyboard layout should be displayed. */
extern bool show_keyboard_layout;
/* Number of failed unlock attempts. */
extern int failed_attempts;

extern struct xkb_keymap *xkb_keymap;
extern struct xkb_state *xkb_state;

/*******************************************************************************
 * Variables defined in xcb.c.
 ******************************************************************************/

/* The root screen, to determine the DPI. */
extern xcb_screen_t *screen;

/*******************************************************************************
 * Local variables.
 ******************************************************************************/

/* Cache the screen’s visual, necessary for creating a Cairo context. */
static xcb_visualtype_t *vistype;

/* Maintain the current unlock/PAM state to draw the appropriate unlock
 * indicator. */
unlock_state_t unlock_state;
auth_state_t auth_state;

/* The background pixmap without any indicator, and the pixmap which is
 * currently displayed (background plus indicator). Keeping both avoids
 * re-rendering the background image on every keypress. */
static xcb_pixmap_t base_pixmap = XCB_NONE;
static xcb_pixmap_t bg_pixmap = XCB_NONE;
static xcb_gcontext_t bg_gc = XCB_NONE;
static uint32_t bg_resolution[2];

static void string_append(char **string_ptr, const char *appended) {
    char *tmp = NULL;
    if (*string_ptr == NULL) {
        if (asprintf(&tmp, "%s", appended) != -1) {
            *string_ptr = tmp;
        }
    } else if (asprintf(&tmp, "%s, %s", *string_ptr, appended) != -1) {
        free(*string_ptr);
        *string_ptr = tmp;
    }
}

static void display_button_text(
    cairo_t *ctx, const char *text, double y_offset, bool use_dark_text) {
    cairo_text_extents_t extents;
    double x, y;

    cairo_text_extents(ctx, text, &extents);
    x = BUTTON_CENTER - ((extents.width / 2) + extents.x_bearing);
    y = BUTTON_CENTER - ((extents.height / 2) + extents.y_bearing) + y_offset;

    cairo_move_to(ctx, x, y);
    if (use_dark_text) {
        cairo_set_source_rgb(ctx, 0., 0., 0.);
    } else {
        cairo_set_source_rgb(ctx, 1., 1., 1.);
    }
    cairo_show_text(ctx, text);
    cairo_close_path(ctx);
}

static void update_layout_string(void) {
    if (layout_string) {
        free(layout_string);
        layout_string = NULL;
    }

    if (xkb_keymap == NULL || xkb_state == NULL) {
        return;
    }

    xkb_layout_index_t num_layouts = xkb_keymap_num_layouts(xkb_keymap);
    for (xkb_layout_index_t i = 0; i < num_layouts; ++i) {
        if (xkb_state_layout_index_is_active(xkb_state, i, XKB_STATE_LAYOUT_EFFECTIVE)) {
            const char *name = xkb_keymap_layout_get_name(xkb_keymap, i);
            if (name) {
                string_append(&layout_string, name);
            }
        }
    }
}

/* check_modifier_keys describes the currently active modifiers (Caps Lock, Alt,
   Num Lock or Super) in the modifier_string variable. */
static void check_modifier_keys(void) {
    if (xkb_keymap == NULL || xkb_state == NULL) {
        return;
    }

    xkb_mod_index_t idx, num_mods;
    const char *mod_name;

    num_mods = xkb_keymap_num_mods(xkb_keymap);

    for (idx = 0; idx < num_mods; idx++) {
        if (!xkb_state_mod_index_is_active(xkb_state, idx, XKB_STATE_MODS_EFFECTIVE)) {
            continue;
        }

        mod_name = xkb_keymap_mod_get_name(xkb_keymap, idx);
        if (mod_name == NULL) {
            continue;
        }

        /* Replace certain xkb names with nicer, human-readable ones. */
        if (strcmp(mod_name, XKB_MOD_NAME_CAPS) == 0) {
            mod_name = "Caps Lock";
        } else if (strcmp(mod_name, XKB_MOD_NAME_NUM) == 0) {
            mod_name = "Num Lock";
        } else {
            /* Show only Caps Lock and Num Lock, other modifiers (e.g. Shift)
             * leak state about the password. */
            continue;
        }
        string_append(&modifier_string, mod_name);
    }
}

static bool ensure_visual(void) {
    if (vistype == NULL) {
        vistype = get_root_visual_type(screen);
    }
    return vistype != NULL;
}

static void get_background_rgb(double *red, double *green, double *blue) {
    uint32_t pixel = 0;
    if (!parse_hex_color(color, &pixel)) {
        pixel = 0;
    }

    *red = ((pixel >> 16) & 0xff) / 255.0;
    *green = ((pixel >> 8) & 0xff) / 255.0;
    *blue = (pixel & 0xff) / 255.0;
}

/*
 * Renders the static background (color and optional image) into the given
 * pixmap. This is only done when the pixmap is (re)created, not on every
 * redraw.
 */
static void render_background(xcb_pixmap_t pixmap) {
    cairo_surface_t *xcb_output = cairo_xcb_surface_create(conn, pixmap, vistype, (int)last_resolution[0], (int)last_resolution[1]);
    if (cairo_surface_status(xcb_output) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Could not create background surface: %s\n",
                cairo_status_to_string(cairo_surface_status(xcb_output)));
        cairo_surface_destroy(xcb_output);
        return;
    }

    cairo_t *ctx = cairo_create(xcb_output);

    /* Explicitly clear the entire pixmap with the background color to get a
     * defined state. */
    double red, green, blue;
    get_background_rgb(&red, &green, &blue);
    cairo_set_source_rgb(ctx, red, green, blue);
    cairo_rectangle(ctx, 0, 0, last_resolution[0], last_resolution[1]);
    cairo_fill(ctx);

    if (img) {
        if (!tile) {
            cairo_set_source_surface(ctx, img, 0, 0);
            cairo_paint(ctx);
        } else {
            /* create a pattern and fill a rectangle as big as the screen */
            cairo_pattern_t *pattern = cairo_pattern_create_for_surface(img);
            cairo_set_source(ctx, pattern);
            cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
            cairo_rectangle(ctx, 0, 0, last_resolution[0], last_resolution[1]);
            cairo_fill(ctx);
            cairo_pattern_destroy(pattern);
        }
    }

    cairo_destroy(ctx);
    cairo_surface_flush(xcb_output);
    cairo_surface_destroy(xcb_output);
}

/*
 * Draws the unlock indicator on top of the already-copied background pixmap.
 */
static void draw_indicator(xcb_pixmap_t pixmap) {
    if (!unlock_indicator ||
        (unlock_state < STATE_KEY_PRESSED && auth_state <= STATE_AUTH_IDLE)) {
        return;
    }

    if (vistype == NULL) {
        return;
    }

    const double scaling_factor = get_dpi_value() / 96.0;
    int button_diameter_physical = (int)ceil(scaling_factor * BUTTON_DIAMETER);
    if (button_diameter_physical <= 0) {
        return;
    }

    DEBUG("scaling_factor is %.f, physical diameter is %d px\n",
          scaling_factor, button_diameter_physical);

    /* Initialize cairo: Create one in-memory surface to render the unlock
     * indicator on, create one XCB surface to actually draw (one or more,
     * depending on the amount of screens) unlock indicators on. */
    cairo_surface_t *output = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, button_diameter_physical, button_diameter_physical);
    if (cairo_surface_status(output) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Could not create indicator surface: %s\n",
                cairo_status_to_string(cairo_surface_status(output)));
        cairo_surface_destroy(output);
        return;
    }

    cairo_t *ctx = cairo_create(output);
    cairo_scale(ctx, scaling_factor, scaling_factor);

    /* Draw a (centered) circle with transparent background. */
    cairo_set_line_width(ctx, 10.0);
    cairo_arc(ctx,
              BUTTON_CENTER /* x */,
              BUTTON_CENTER /* y */,
              BUTTON_RADIUS /* radius */,
              0 /* start */,
              2 * M_PI /* end */);

    /* Use the appropriate color for the different PAM states
     * (currently verifying, wrong password, or default) */
    switch (auth_state) {
        case STATE_AUTH_VERIFY:
        case STATE_AUTH_LOCK:
            cairo_set_source_rgba(ctx, 0, 114.0 / 255, 255.0 / 255, 0.75);
            break;
        case STATE_AUTH_WRONG:
        case STATE_I3LOCK_LOCK_FAILED:
            cairo_set_source_rgba(ctx, 250.0 / 255, 0, 0, 0.75);
            break;
        default:
            if (unlock_state == STATE_NOTHING_TO_DELETE) {
                cairo_set_source_rgba(ctx, 250.0 / 255, 0, 0, 0.75);
                break;
            }
            cairo_set_source_rgba(ctx, 0, 0, 0, 0.75);
            break;
    }
    cairo_fill_preserve(ctx);

    bool use_dark_text = true;

    switch (auth_state) {
        case STATE_AUTH_VERIFY:
        case STATE_AUTH_LOCK:
            cairo_set_source_rgb(ctx, 51.0 / 255, 0, 250.0 / 255);
            break;
        case STATE_AUTH_WRONG:
        case STATE_I3LOCK_LOCK_FAILED:
            cairo_set_source_rgb(ctx, 125.0 / 255, 51.0 / 255, 0);
            break;
        case STATE_AUTH_IDLE:
            if (unlock_state == STATE_NOTHING_TO_DELETE) {
                cairo_set_source_rgb(ctx, 125.0 / 255, 51.0 / 255, 0);
                break;
            }

            cairo_set_source_rgb(ctx, 51.0 / 255, 125.0 / 255, 0);
            use_dark_text = false;
            break;
    }
    cairo_stroke(ctx);

    /* Draw an inner seperator line. */
    cairo_set_source_rgb(ctx, 0, 0, 0);
    cairo_set_line_width(ctx, 2.0);
    cairo_arc(ctx,
              BUTTON_CENTER /* x */,
              BUTTON_CENTER /* y */,
              BUTTON_RADIUS - 5 /* radius */,
              0,
              2 * M_PI);
    cairo_stroke(ctx);

    cairo_set_line_width(ctx, 10.0);

    /* Display a (centered) text of the current PAM state. */
    char *text = NULL;
    /* We don't want to show more than a 3-digit number. */
    char buf[4];

    cairo_set_source_rgb(ctx, 0, 0, 0);
    cairo_select_font_face(ctx, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(ctx, 28.0);
    switch (auth_state) {
        case STATE_AUTH_VERIFY:
            text = "Verifying…";
            break;
        case STATE_AUTH_LOCK:
            text = "Locking…";
            break;
        case STATE_AUTH_WRONG:
            text = "Wrong!";
            break;
        case STATE_I3LOCK_LOCK_FAILED:
            text = "Lock failed!";
            break;
        default:
            if (unlock_state == STATE_NOTHING_TO_DELETE) {
                text = "No input";
            }
            if (show_failed_attempts && failed_attempts > 0) {
                if (failed_attempts > 999) {
                    text = "> 999";
                } else {
                    snprintf(buf, sizeof(buf), "%d", failed_attempts);
                    text = buf;
                }
                cairo_set_source_rgb(ctx, 1, 0, 0);
                cairo_set_font_size(ctx, 32.0);
            }
            break;
    }

    if (text) {
        display_button_text(ctx, text, 0., use_dark_text);
    }

    if (modifier_string != NULL) {
        cairo_set_font_size(ctx, 14.0);
        display_button_text(ctx, modifier_string, 28., use_dark_text);
    }
    if (show_keyboard_layout && layout_string != NULL) {
        cairo_set_font_size(ctx, 14.0);
        display_button_text(ctx, layout_string, -28., use_dark_text);
    }

    /* After the user pressed any valid key or the backspace key, we
     * highlight a random part of the unlock indicator to confirm this
     * keypress. */
    if (unlock_state == STATE_KEY_ACTIVE ||
        unlock_state == STATE_BACKSPACE_ACTIVE) {
        cairo_new_sub_path(ctx);
        double highlight_start = (rand() % (int)(2 * M_PI * 100)) / 100.0;
        cairo_arc(ctx,
                  BUTTON_CENTER /* x */,
                  BUTTON_CENTER /* y */,
                  BUTTON_RADIUS /* radius */,
                  highlight_start,
                  highlight_start + (M_PI / 3.0));
        if (unlock_state == STATE_KEY_ACTIVE) {
            /* For normal keys, we use a lighter green. */
            cairo_set_source_rgb(ctx, 51.0 / 255, 219.0 / 255, 0);
        } else {
            /* For backspace, we use red. */
            cairo_set_source_rgb(ctx, 219.0 / 255, 51.0 / 255, 0);
        }
        cairo_stroke(ctx);

        /* Draw two little separators for the highlighted part of the
         * unlock indicator. */
        cairo_set_source_rgb(ctx, 0, 0, 0);
        cairo_arc(ctx,
                  BUTTON_CENTER /* x */,
                  BUTTON_CENTER /* y */,
                  BUTTON_RADIUS /* radius */,
                  highlight_start /* start */,
                  highlight_start + (M_PI / 128.0) /* end */);
        cairo_stroke(ctx);
        cairo_arc(ctx,
                  BUTTON_CENTER /* x */,
                  BUTTON_CENTER /* y */,
                  BUTTON_RADIUS /* radius */,
                  (highlight_start + (M_PI / 3.0)) - (M_PI / 128.0) /* start */,
                  highlight_start + (M_PI / 3.0) /* end */);
        cairo_stroke(ctx);
    }

    cairo_surface_t *xcb_output = cairo_xcb_surface_create(conn, pixmap, vistype, (int)last_resolution[0], (int)last_resolution[1]);
    if (cairo_surface_status(xcb_output) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Could not create output surface: %s\n",
                cairo_status_to_string(cairo_surface_status(xcb_output)));
        cairo_surface_destroy(xcb_output);
        cairo_destroy(ctx);
        cairo_surface_destroy(output);
        return;
    }

    cairo_t *xcb_ctx = cairo_create(xcb_output);

    if (xr_screens > 0) {
        /* Composite the unlock indicator in the middle of each screen. */
        for (int monitor = 0; monitor < xr_screens; monitor++) {
            int x = (xr_resolutions[monitor].x + ((xr_resolutions[monitor].width / 2) - (button_diameter_physical / 2)));
            int y = (xr_resolutions[monitor].y + ((xr_resolutions[monitor].height / 2) - (button_diameter_physical / 2)));
            cairo_set_source_surface(xcb_ctx, output, x, y);
            cairo_rectangle(xcb_ctx, x, y, button_diameter_physical, button_diameter_physical);
            cairo_fill(xcb_ctx);
        }
    } else {
        /* We have no information about the screen sizes/positions, so we just
         * place the unlock indicator in the middle of the X root window and
         * hope for the best. */
        int x = (last_resolution[0] / 2) - (button_diameter_physical / 2);
        int y = (last_resolution[1] / 2) - (button_diameter_physical / 2);
        cairo_set_source_surface(xcb_ctx, output, x, y);
        cairo_rectangle(xcb_ctx, x, y, button_diameter_physical, button_diameter_physical);
        cairo_fill(xcb_ctx);
    }

    cairo_destroy(xcb_ctx);
    cairo_surface_flush(xcb_output);
    cairo_surface_destroy(xcb_output);
    cairo_destroy(ctx);
    cairo_surface_destroy(output);
}

/*
 * Makes sure the background pixmaps exist and match the current resolution.
 */
static bool ensure_pixmaps(void) {
    if (base_pixmap != XCB_NONE && bg_pixmap != XCB_NONE &&
        bg_resolution[0] == last_resolution[0] &&
        bg_resolution[1] == last_resolution[1]) {
        return true;
    }

    free_bg_pixmap();

    if (!ensure_visual()) {
        fprintf(stderr, "Could not find the root visual\n");
        return false;
    }

    base_pixmap = create_bg_pixmap(conn, screen, last_resolution, color);
    bg_pixmap = create_bg_pixmap(conn, screen, last_resolution, color);
    if (base_pixmap == XCB_NONE || bg_pixmap == XCB_NONE) {
        fprintf(stderr, "Could not allocate background pixmaps\n");
        free_bg_pixmap();
        return false;
    }

    bg_resolution[0] = last_resolution[0];
    bg_resolution[1] = last_resolution[1];

    bg_gc = xcb_generate_id(conn);
    xcb_create_gc(conn, bg_gc, base_pixmap, 0, NULL);

    render_background(base_pixmap);
    return true;
}

/*
 * Releases the current background pixmaps so that the next redraw_screen() call
 * will allocate new ones with the updated resolution.
 *
 */
void free_bg_pixmap(void) {
    /* Detach the pixmap from the window before freeing it, otherwise the
     * window would briefly reference a destroyed X resource. */
    if (bg_pixmap != XCB_NONE && win != XCB_NONE) {
        xcb_change_window_attributes(conn, win, XCB_CW_BACK_PIXMAP, (uint32_t[]){XCB_NONE});
    }

    if (bg_gc != XCB_NONE) {
        xcb_free_gc(conn, bg_gc);
        bg_gc = XCB_NONE;
    }
    if (base_pixmap != XCB_NONE) {
        xcb_free_pixmap(conn, base_pixmap);
        base_pixmap = XCB_NONE;
    }
    if (bg_pixmap != XCB_NONE) {
        xcb_free_pixmap(conn, bg_pixmap);
        bg_pixmap = XCB_NONE;
    }
    bg_resolution[0] = 0;
    bg_resolution[1] = 0;
}

/*
 * Copies the static background, draws the indicator on top of it and swaps the
 * result in as the window background.
 *
 */
void redraw_screen(void) {
    DEBUG("redraw_screen(unlock_state = %d, auth_state = %d)\n", unlock_state, auth_state);

    if (modifier_string) {
        free(modifier_string);
        modifier_string = NULL;
    }
    check_modifier_keys();
    update_layout_string();

    if (!ensure_pixmaps()) {
        return;
    }

    xcb_copy_area(conn, base_pixmap, bg_pixmap, bg_gc,
                  0, 0, 0, 0,
                  (uint16_t)last_resolution[0], (uint16_t)last_resolution[1]);
    draw_indicator(bg_pixmap);

    xcb_change_window_attributes(conn, win, XCB_CW_BACK_PIXMAP, (uint32_t[]){bg_pixmap});
    /* XXX: Possible optimization: Only update the area in the middle of the
     * screen instead of the whole screen. */
    xcb_clear_area(conn, 0, win, 0, 0, (uint16_t)last_resolution[0], (uint16_t)last_resolution[1]);
    xcb_flush(conn);
}

/*
 * Hides the unlock indicator completely when there is no content in the
 * password buffer.
 *
 */
void clear_indicator(void) {
    if (password_length() == 0) {
        unlock_state = STATE_STARTED;
    } else {
        unlock_state = STATE_KEY_PRESSED;
    }
    redraw_screen();
}
