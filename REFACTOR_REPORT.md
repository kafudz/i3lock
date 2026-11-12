# i3lock security, performance, and maintainability refactor

Baseline: `1305cc2 release v2.16`  
Scope: complete repository review, targeted refactor, new unit tests, build hardening.  
Status: implemented in the working tree; all automated tests pass.

---

## 1. Executive summary

i3lock is a small, security-sensitive X11 screen locker. Its core design is sound:
a single XCB connection, a libev loop, XKB-based input handling, and PAM (or
OpenBSD `auth_userokay`) authentication. The refactor therefore does **not**
replace the global XCB/libev architecture. Instead it isolates the
security-sensitive logic into small modules, fixes concrete memory-safety and
lock-bypass bugs, removes per-keypress full-screen re-rendering, and adds
repeatable tests plus compiler/linker hardening.

The most important fixes are:

1. **Password buffer overflow** when a composed key sequence produced more
   bytes than the old fixed 512-byte array allowed.
2. **Incomplete password wiping**: the old code only cleared `strlen(password)`
   bytes and left stale characters after backspaces.
3. **Lock bypass on `fork(2)` failure**: the old MapNotify code treated any
   non-zero return value as "parent" and exited, which could unlock the screen
   when `fork()` returned `-1`.
4. **Out-of-bounds / server-controlled allocations** in the RandR and Xinerama
   monitor queries.
5. **Integer overflows** in raw image dimension and size calculations.
6. **Full background re-render on every keypress**, replaced by a cached
   background pixmap and a server-side `xcb_copy_area`.

---

## 2. Evaluation

### 2.1 Architecture

| Area | Observation | Assessment |
| --- | --- | --- |
| Process model | Main process locks the screen, then forks a small `raise_loop` child and later daemonizes after `MapNotify`. | Appropriate for X11, but error paths were unsafe. |
| Event loop | `libev` `ev_io` + `ev_check` + `ev_prepare`, with XCB polled manually. | Correct and low-overhead. Kept. |
| Input | XKB keymap + compose state, UTF-8 conversion, fixed password array. | Good approach, unsafe buffer management. |
| Rendering | Cairo/XCB surfaces, one full-screen pixmap redrawn on every event. | Correct but wasteful. Split into cached background + indicator layer. |
| Monitor discovery | RandR 1.5, RandR 1.4, Xinerama fallbacks. | Good fallback chain, but VLA and unchecked lengths. |
| Authentication | PAM conversation callback or OpenBSD `auth_userokay`. | Functional, but leaked partial responses and logged secrets. |

### 2.2 Security findings

| ID | Severity | Finding | Resolution |
| --- | --- | --- | --- |
| S1 | Critical | Composed input could write past the 512-byte password array (`input_position + 8` check was insufficient for multi-byte compose output). | Bounded `password_append_utf8()` with explicit remaining-space check. |
| S2 | High | Password memory was only wiped up to `strlen()`, and backspace did not wipe removed bytes. | `password_reset()` wipes the whole buffer; `password_backspace()` wipes the removed code point. |
| S3 | High | `fork()` failure in the MapNotify handler was treated as the parent path, calling `exit(0)` and leaving the screen unlocked. | `pid == -1` now keeps the process running and locked. |
| S4 | High | `--debug` logged the entered password. | Removed; man page updated. |
| S5 | Medium | RandR output count from the X server was used for a stack VLA; Xinerama reply length was trusted for a heap loop. | Heap allocation with `calloc`; zero/negative lengths handled; iterator bounded by reply length. |
| S6 | Medium | Raw image width/height and `width * height * bpp` could overflow `size_t`/`int`. | `parse_raw_image_format()` validates dimensions; `size_mul()` checks every multiplication. |
| S7 | Medium | PAM conversation callback leaked earlier responses when a later `strdup()` failed. | Full cleanup on every error path. |
| S8 | Low | `XSS_SLEEP_LOCK_FD` parsing could close `stdin`/`stdout`/`stderr` or a partially parsed number. | Strict `strtol` validation and `fd >= 3` check. |
| S9 | Low | `fork()` failure while creating the raise child was silently ignored. | Still ignored intentionally (non-critical), but documented. |
| S10 | Low | Window grabs/cursor were leaked when no focused window was found at exit. | Cleanup now always runs. |

### 2.3 Performance findings

| ID | Finding | Resolution |
| --- | --- | --- |
| P1 | Every keypress re-rendered the background image and unlock indicator into a full-screen pixmap. | `base_pixmap` caches the static background; `bg_pixmap` receives a server-side copy; only the indicator is drawn. |
| P2 | Background pixmap was freed and reallocated only implicitly, and could be stale after a resolution change. | `ensure_pixmaps()` validates resolution and `free_bg_pixmap()` detaches the window background before freeing. |
| P3 | DPI parsing accepted `NaN`/`Inf`/negative values and could produce absurd indicator sizes. | Clamped to `[1, 1000]`, finite check, safe fallback. |
| P4 | Grab retry loops shared a single `tries` counter, so a slow pointer grab could leave no attempts for the keyboard. | Separate pointer and keyboard retry loops with a monotonic elapsed-time redraw. |

### 2.4 Maintainability findings

| ID | Finding | Resolution |
| --- | --- | --- |
| M1 | Password, image loading, and utility logic lived inside `i3lock.c`. | Extracted `password.c`, `image.c`, `util.c` with headers. |
| M2 | `i3lock.c` contained raw image parsing and PNG validation. | Moved to `image.c`; `i3lock.c` now only calls `load_image()`. |
| M3 | Rendering mixed background, image, and indicator concerns. | Split into `render_background()`, `draw_indicator()`, `ensure_pixmaps()`, `redraw_screen()`. |
| M4 | Error handling was inconsistent (some failures exited, some were ignored). | Security-critical failures stay locked; non-critical failures warn and continue. |
| M5 | No unit tests. | Added `tests/` with util, password, and image tests; CI runs `meson test`. |
| M6 | No compiler/linker hardening. | Added stack protector, PIE, RELRO, `-z now`, and `_FORTIFY_SOURCE=2`. |

---

## 3. Step-by-step refactoring plan

1. **Freeze behavior with a baseline build.** Configure with the CI warning
   flags and confirm the unmodified tree builds.
2. **Extract safe primitives.** Add `util.c` / `include/util.h` with
   `secure_zero()`, overflow-checked `size_mul()` / `size_add()`, hex color
   parsing, and raw-image format parsing.
3. **Replace the password array with a bounded module.** Add `password.c` /
   `include/password.h`; move `mlock()`, reset, append, backspace, length, and
   value access behind an API.
4. **Move image loading out of `i3lock.c`.** Add `image.c` / `include/image.h`;
   validate PNG headers, parse raw formats, and check every allocation size.
5. **Fix the event-loop and process-model error paths.** Handle `fork() == -1`,
   close the sleep-lock fd once, use `_exit()` in forked children, and always
   clean up grabs, the cursor, and the X connection.
6. **Harden monitor discovery.** Replace the RandR VLA with heap allocation,
   bound Xinerama iterations by the reply length, and handle zero/negative
   lengths.
7. **Split rendering.** Cache the static background in `base_pixmap`, copy it
   to `bg_pixmap` server-side, draw only the indicator, and recreate both on
   resolution changes.
8. **Harden input and DPI handling.** Check XKB return values, guard against
   NULL keymap/state, clamp DPI, and remove password logging.
9. **Add build hardening and tests.** Add the new sources to Meson, add
   `tests/meson.build`, and run the tests in CI.
10. **Verify.** Build with CI flags, build with ASan/UBSan, run all tests, and
    run `--version` / `--help` smoke tests.

All ten steps are implemented.

---

## 4. Changes by file

### New files

- `util.c`, `include/util.h` — secure zeroing, checked arithmetic, color and raw
  format parsing.
- `password.c`, `include/password.h` — bounded UTF-8 password buffer, secure
  reset/backspace, `mlock()` wrapper.
- `image.c`, `include/image.h` — PNG validation, raw image parsing, safe
  surface allocation and reading.
- `tests/meson.build`, `tests/test_util.c`, `tests/test_password.c`,
  `tests/test_image.c` — unit tests.

### Modified files

- `i3lock.c` — uses the new modules; fixes fork failure, PAM cleanup, sleep-lock
  fd parsing, grab retries, resize handling, and final cleanup; removes password
  logging.
- `xcb.c` — validates atom/color/geometry inputs; safe pixmap creation; fixed
  grab retry loops; cursor creation uses the correct XCB image header.
- `unlock_indicator.c` — background/indicator split and pixmap caching; NULL
  guards for XKB state; safe DPI-scaled surface sizes.
- `randr.c` — heap-based monitor/output arrays, length validation, error-path
  cleanup, correct RandR 1.5 version test.
- `dpi.c` — finite/clamped DPI parsing and safe fallback.
- `include/xcb.h`, `include/randr.h`, `include/unlock_indicator.h`,
  `include/i3lock.h` — updated prototypes and removed stale declarations.
- `meson.build` — new sources, hardening flags, `HAVE_EXPLICIT_BZERO`, tests
  subdir, Meson minimum `>=0.46.0`.
- `.github/workflows/main.yml` — run `meson test --print-errorlogs`.
- `README.md` — document `meson test`.
- `CHANGELOG` — record the fixes.
- `i3lock.1` — remove the claim that `--debug` logs passwords.

---

## 5. Tests

### New unit tests

| Test | Coverage |
| --- | --- |
| `tests/test_util.c` | `size_mul` / `size_add` overflow and NULL handling; hex color parsing; raw image format parsing (valid, missing height, zero dimensions, overlong/trailing format); `secure_zero`. |
| `tests/test_password.c` | Initial empty state; append; UTF-8 code-point backspace; full-buffer rejection; reset. |
| `tests/test_image.c` | Raw RGB loading and pixel values; zero dimensions; unknown format; non-PNG rejection. |

### Commands run

```sh
# CI-equivalent build
CFLAGS="-Wformat -Wformat-security -Wextra -Wno-unused-parameter -Werror" \
  meson setup /tmp/opencode/i3lock-ci . -Dwarning_level=2
ninja -C /tmp/opencode/i3lock-ci
meson test -C /tmp/opencode/i3lock-ci --print-errorlogs

# ASan + UBSan build
meson setup /tmp/opencode/i3lock-asan . -Db_sanitize=address,undefined
ninja -C /tmp/opencode/i3lock-asan
ASAN_OPTIONS=detect_leaks=1 meson test -C /tmp/opencode/i3lock-asan --print-errorlogs

# Smoke tests
/tmp/opencode/i3lock-ci/i3lock --version
/tmp/opencode/i3lock-ci/i3lock --help
```

### Results

```text
1/3 i3lock:util     OK
2/3 i3lock:password OK
3/3 i3lock:image    OK
Ok: 3   Fail: 0
```

The CI-equivalent build completes with no warnings under
`-Wformat -Wformat-security -Wextra -Wno-unused-parameter -Werror` and
`-Dwarning_level=2`.

---

## 6. Residual risks and follow-ups

1. **GUI behavior was not exercised** because this environment has no X server
   or Xvfb. The rendering split is covered by code review and the non-GUI unit
   tests, but a manual multi-monitor, resize, tiling, and PAM login test is
   still recommended.
2. **`clang-format` is unavailable locally**, so the CI formatting job could
   not be reproduced. The new code follows the existing 4-space, attached-brace
   style; run `clang-format-15 --dry-run --Werror` before merging.
3. **PAM response strings** are allocated with `strdup()` and owned by PAM, so
   they cannot be wiped by i3lock after the callback returns. This is a PAM API
   limitation.
4. **The global XCB/libev architecture remains.** A context-struct rewrite
   would improve testability but carries a large regression risk for a screen
   locker; the module extraction captures most of the maintainability benefit.
5. **`size_add()` is currently only used by tests.** It is kept as a small,
   tested companion to `size_mul()` for future callers.

---

## 7. Complete updated file contents

The following sections contain the complete current contents of every new or
modified source, header, build, test, CI, and documentation file.

### `i3lock.c`

````c
/*
 * vim:ts=4:sw=4:expandtab
 *
 * © 2010 Michael Stapelberg
 *
 * See LICENSE for licensing information
 *
 */
#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/xkb.h>
#include <err.h>
#include <errno.h>
#ifdef __OpenBSD__
#include <bsd_auth.h>
#else
#include <security/pam_appl.h>
#endif
#include <getopt.h>
#include <ev.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <cairo.h>
#include <xcb/xcb_aux.h>
#include <xcb/randr.h>

#include "i3lock.h"
#include "xcb.h"
#include "cursors.h"
#include "unlock_indicator.h"
#include "randr.h"
#include "dpi.h"
#include "image.h"
#include "password.h"
#include "util.h"

#define TSTAMP_N_SECS(n) (n * 1.0)
#define TSTAMP_N_MINS(n) (60 * TSTAMP_N_SECS(n))
#define START_TIMER(timer_obj, timeout, callback) \
    timer_obj = start_timer(timer_obj, timeout, callback)
#define STOP_TIMER(timer_obj) \
    timer_obj = stop_timer(timer_obj)

typedef void (*ev_callback_t)(EV_P_ ev_timer *w, int revents);
static void input_done(void);

char color[7] = "a3a3a3";
uint32_t last_resolution[2];
xcb_window_t win;
static xcb_cursor_t cursor;
#ifndef __OpenBSD__
static pam_handle_t *pam_handle;
static bool pam_started;
#endif
static bool beep = false;
bool debug_mode = false;
bool unlock_indicator = true;
char *modifier_string = NULL;
static bool dont_fork = false;
struct ev_loop *main_loop;
static struct ev_timer *clear_auth_wrong_timeout;
static struct ev_timer *clear_indicator_timeout;
static struct ev_timer *discard_passwd_timeout;
extern unlock_state_t unlock_state;
extern auth_state_t auth_state;
int failed_attempts = 0;
bool show_failed_attempts = false;
bool show_keyboard_layout = false;
bool retry_verification = false;

struct xkb_state *xkb_state;
static struct xkb_context *xkb_context;
struct xkb_keymap *xkb_keymap;
static struct xkb_compose_table *xkb_compose_table;
static struct xkb_compose_state *xkb_compose_state;
static uint8_t xkb_base_event;
static uint8_t xkb_base_error;
static int randr_base = -1;

cairo_surface_t *img = NULL;
bool tile = false;
bool ignore_empty_password = false;
bool skip_repeated_empty_password = false;

/*
 * Loads the XKB keymap from the X11 server and feeds it to xkbcommon.
 * Necessary so that we can properly let xkbcommon track the keyboard state and
 * translate keypresses to utf-8.
 *
 * This function can be called when the user changes the XKB configuration,
 * so it must not leave unusable global state behind
 *
 */
static bool load_keymap(void) {
    if (xkb_context == NULL) {
        if ((xkb_context = xkb_context_new(0)) == NULL) {
            fprintf(stderr, "[i3lock] could not create xkbcommon context\n");
            return false;
        }
    }

    int32_t device_id = xkb_x11_get_core_keyboard_device_id(conn);
    if (device_id < 0) {
        fprintf(stderr, "[i3lock] could not determine the core keyboard device\n");
        return false;
    }
    DEBUG("device = %d\n", device_id);
    struct xkb_keymap *new_keymap = xkb_x11_keymap_new_from_device(xkb_context, conn, device_id, 0);
    if (new_keymap == NULL) {
        fprintf(stderr, "[i3lock] xkb_x11_keymap_new_from_device failed\n");
        return false;
    }

    struct xkb_state *new_state =
        xkb_x11_state_new_from_device(new_keymap, conn, device_id);
    if (new_state == NULL) {
        fprintf(stderr, "[i3lock] xkb_x11_state_new_from_device failed\n");
        xkb_keymap_unref(new_keymap);
        return false;
    }

    /* Only update global state on success */
    xkb_state_unref(xkb_state);
    xkb_keymap_unref(xkb_keymap);
    xkb_state = new_state;
    xkb_keymap = new_keymap;
    return true;
}

/*
 * Loads the XKB compose table from the given locale.
 *
 */
static bool load_compose_table(const char *locale) {
    struct xkb_compose_table *new_table =
        xkb_compose_table_new_from_locale(xkb_context, locale, 0);
    if (new_table == NULL) {
        fprintf(stderr, "[i3lock] xkb_compose_table_new_from_locale failed\n");
        return false;
    }

    struct xkb_compose_state *new_state = xkb_compose_state_new(new_table, 0);
    if (new_state == NULL) {
        fprintf(stderr, "[i3lock] xkb_compose_state_new failed\n");
        xkb_compose_table_unref(new_table);
        return false;
    }

    xkb_compose_table_unref(xkb_compose_table);
    xkb_compose_state_unref(xkb_compose_state);
    xkb_compose_table = new_table;
    xkb_compose_state = new_state;

    return true;
}

static ev_timer *start_timer(ev_timer *timer_obj, ev_tstamp timeout, ev_callback_t callback) {
    if (timer_obj) {
        ev_timer_stop(main_loop, timer_obj);
        ev_timer_set(timer_obj, timeout, 0.);
        ev_timer_start(main_loop, timer_obj);
    } else {
        /* When there is no memory, we just don’t have a timeout. We cannot
         * exit() here, since that would effectively unlock the screen. */
        timer_obj = calloc(1, sizeof(struct ev_timer));
        if (timer_obj) {
            ev_timer_init(timer_obj, callback, timeout, 0.);
            ev_timer_start(main_loop, timer_obj);
        }
    }
    return timer_obj;
}

static ev_timer *stop_timer(ev_timer *timer_obj) {
    if (timer_obj) {
        ev_timer_stop(main_loop, timer_obj);
        free(timer_obj);
    }
    return NULL;
}

/*
 * Neccessary calls after ending input via enter or others
 *
 */
static void finish_input(void) {
    STOP_TIMER(discard_passwd_timeout);
    unlock_state = STATE_KEY_PRESSED;
    redraw_screen();
    input_done();
}

/*
 * Resets auth_state to STATE_AUTH_IDLE 2 seconds after an unsuccessful
 * authentication event.
 *
 */
static void clear_auth_wrong(EV_P_ ev_timer *w, int revents) {
    (void)revents;
    DEBUG("clearing auth wrong\n");
    auth_state = STATE_AUTH_IDLE;
    redraw_screen();

    /* Clear modifier string. */
    if (modifier_string != NULL) {
        free(modifier_string);
        modifier_string = NULL;
    }

    /* Now free this timeout. */
    STOP_TIMER(clear_auth_wrong_timeout);

    /* retry with input done during auth verification */
    if (retry_verification) {
        retry_verification = false;
        finish_input();
    }
}

static void clear_indicator_cb(EV_P_ ev_timer *w, int revents) {
    (void)w;
    (void)revents;
    clear_indicator();
    STOP_TIMER(clear_indicator_timeout);
}

static void clear_input(void) {
    password_reset();
}

static void discard_passwd_cb(EV_P_ ev_timer *w, int revents) {
    (void)w;
    (void)revents;
    clear_input();
    clear_indicator();
    STOP_TIMER(discard_passwd_timeout);
}

static void input_done(void) {
    STOP_TIMER(clear_auth_wrong_timeout);
    auth_state = STATE_AUTH_VERIFY;
    unlock_state = STATE_STARTED;
    redraw_screen();

#ifdef __OpenBSD__
    struct passwd *pw;

    if (!(pw = getpwuid(getuid()))) {
        errx(1, "unknown uid %u.", getuid());
    }

    if (auth_userokay(pw->pw_name, NULL, NULL, (char *)password_value()) != 0) {
        DEBUG("successfully authenticated\n");
        password_reset();

        ev_break(EV_DEFAULT, EVBREAK_ALL);
        return;
    }
#else
    if (pam_authenticate(pam_handle, 0) == PAM_SUCCESS) {
        DEBUG("successfully authenticated\n");
        password_reset();

        /* PAM credentials should be refreshed, this will for example update any kerberos tickets.
         * Related to credentials pam_end() needs to be called to cleanup any temporary
         * credentials like kerberos /tmp/krb5cc_pam_* files which may of been left behind if the
         * refresh of the credentials failed. */
        int ret = pam_setcred(pam_handle, PAM_REFRESH_CRED);
        if (ret != PAM_SUCCESS) {
            fprintf(stderr, "PAM: pam_setcred failed: %s\n", pam_strerror(pam_handle, ret));
        }

        ev_break(EV_DEFAULT, EVBREAK_ALL);
        return;
    }
#endif

    if (debug_mode) {
        fprintf(stderr, "Authentication failure\n");
    }

    auth_state = STATE_AUTH_WRONG;
    /* The unlock indicator displays the number of failed attempts,
     * but caps the attempts to 999, so only increment up to 999. */
    if (failed_attempts < 999) {
        failed_attempts += 1;
    }
    clear_input();
    if (unlock_indicator) {
        redraw_screen();
    }

    /* Clear this state after 2 seconds (unless the user enters another
     * password during that time). */
    ev_now_update(main_loop);
    START_TIMER(clear_auth_wrong_timeout, TSTAMP_N_SECS(2), clear_auth_wrong);

    /* Cancel the clear_indicator_timeout, it would hide the unlock indicator
     * too early. */
    STOP_TIMER(clear_indicator_timeout);

    /* beep on authentication failure, if enabled */
    if (beep) {
        xcb_bell(conn, 100);
        xcb_flush(conn);
    }
}

static void redraw_timeout(EV_P_ ev_timer *w, int revents) {
    (void)revents;
    redraw_screen();
    STOP_TIMER(w);
}

static bool skip_without_validation(void) {
    if (password_length() != 0) {
        return false;
    }

    if (skip_repeated_empty_password || ignore_empty_password) {
        return true;
    }

    return false;
}

/*
 * Handle key presses. Fixes state, then looks up the key symbol for the
 * given keycode, then looks up the key symbol (as UCS-2), converts it to
 * UTF-8 and stores it in the password buffer.
 *
 */
static void handle_key_press(xcb_key_press_event_t *event) {
    xkb_keysym_t ksym;
    char buffer[128];
    int n;
    bool ctrl;
    bool composed = false;

    ksym = xkb_state_key_get_one_sym(xkb_state, event->detail);
    ctrl = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_DEPRESSED);

    /* The buffer will be null-terminated, so n >= 2 for 1 actual character. */
    memset(buffer, '\0', sizeof(buffer));

    if (xkb_compose_state && xkb_compose_state_feed(xkb_compose_state, ksym) == XKB_COMPOSE_FEED_ACCEPTED) {
        switch (xkb_compose_state_get_status(xkb_compose_state)) {
            case XKB_COMPOSE_NOTHING:
                break;
            case XKB_COMPOSE_COMPOSING:
                return;
            case XKB_COMPOSE_COMPOSED: {
                /* xkb_compose_state_get_utf8 doesn't include the terminating byte in the return value
                 * as xkb_keysym_to_utf8 does. Adding one makes the variable n consistent. */
                int written = xkb_compose_state_get_utf8(xkb_compose_state, buffer, sizeof(buffer));
                if (written < 0 || (size_t)written >= sizeof(buffer)) {
                    xkb_compose_state_reset(xkb_compose_state);
                    return;
                }
                n = written + 1;
                ksym = xkb_compose_state_get_one_sym(xkb_compose_state);
                composed = true;
                break;
            }
            case XKB_COMPOSE_CANCELLED:
                xkb_compose_state_reset(xkb_compose_state);
                return;
        }
    }

    if (!composed) {
        n = xkb_keysym_to_utf8(ksym, buffer, sizeof(buffer));
        if (n < 0 || (size_t)n > sizeof(buffer)) {
            n = 0;
        }
    }

    switch (ksym) {
        case XKB_KEY_j:
        case XKB_KEY_m:
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
        case XKB_KEY_XF86ScreenSaver:
            if ((ksym == XKB_KEY_j || ksym == XKB_KEY_m) && !ctrl) {
                break;
            }

            if (auth_state == STATE_AUTH_WRONG) {
                retry_verification = true;
                return;
            }

            if (skip_without_validation()) {
                clear_input();
                return;
            }
            finish_input();
            skip_repeated_empty_password = true;
            return;
        default:
            skip_repeated_empty_password = false;
            // A new password is being entered, but a previous one is pending.
            // Discard the old one and clear the retry_verification flag.
            if (retry_verification) {
                retry_verification = false;
                clear_input();
            }
    }

    switch (ksym) {
        case XKB_KEY_u:
        case XKB_KEY_Escape:
            if ((ksym == XKB_KEY_u && ctrl) ||
                ksym == XKB_KEY_Escape) {
                DEBUG("C-u pressed\n");
                clear_input();
                /* Also hide the unlock indicator */
                if (unlock_indicator) {
                    clear_indicator();
                }
                return;
            }
            break;

        case XKB_KEY_Delete:
        case XKB_KEY_KP_Delete:
            /* Deleting forward doesn’t make sense, as i3lock doesn’t allow you
             * to move the cursor when entering a password. We need to eat this
             * key press so that it won’t be treated as part of the password,
             * see issue #50. */
            return;

        case XKB_KEY_h:
        case XKB_KEY_BackSpace:
            if (ksym == XKB_KEY_h && !ctrl) {
                break;
            }

            if (!password_backspace()) {
                START_TIMER(clear_indicator_timeout, 1.0, clear_indicator_cb);
                unlock_state = STATE_NOTHING_TO_DELETE;
                redraw_screen();
                return;
            }

            /* Hide the unlock indicator after a bit if the password buffer is
             * empty. */
            START_TIMER(clear_indicator_timeout, 1.0, clear_indicator_cb);
            unlock_state = STATE_BACKSPACE_ACTIVE;
            redraw_screen();
            unlock_state = STATE_KEY_PRESSED;
            return;
    }

    if (n < 2) {
        return;
    }

    /* store it in the password buffer as UTF-8 */
    if (!password_append_utf8(buffer, (size_t)(n - 1))) {
        return;
    }

    if (unlock_indicator) {
        unlock_state = STATE_KEY_ACTIVE;
        redraw_screen();
        unlock_state = STATE_KEY_PRESSED;

        struct ev_timer *timeout = NULL;
        START_TIMER(timeout, TSTAMP_N_SECS(0.25), redraw_timeout);
        STOP_TIMER(clear_indicator_timeout);
    }

    START_TIMER(discard_passwd_timeout, TSTAMP_N_MINS(3), discard_passwd_cb);
}

/*
 * A visibility notify event will be received when the visibility (= can the
 * user view the complete window) changes, so for example when a popup overlays
 * some area of the i3lock window.
 *
 * In this case, we raise our window on top so that the popup (or whatever is
 * hiding us) gets hidden.
 *
 */
static void handle_visibility_notify(xcb_connection_t *conn,
                                     xcb_visibility_notify_event_t *event) {
    if (event->state != XCB_VISIBILITY_UNOBSCURED) {
        uint32_t values[] = {XCB_STACK_MODE_ABOVE};
        xcb_configure_window(conn, event->window, XCB_CONFIG_WINDOW_STACK_MODE, values);
        xcb_flush(conn);
    }
}

/*
 * Called when the keyboard mapping changes. We update our symbols.
 *
 * We ignore errors — if the new keymap cannot be loaded it’s better if the
 * screen stays locked and the user intervenes by using killall i3lock.
 *
 */
static void process_xkb_event(xcb_generic_event_t *gevent) {
    union xkb_event {
        struct {
            uint8_t response_type;
            uint8_t xkbType;
            uint16_t sequence;
            xcb_timestamp_t time;
            uint8_t deviceID;
        } any;
        xcb_xkb_new_keyboard_notify_event_t new_keyboard_notify;
        xcb_xkb_map_notify_event_t map_notify;
        xcb_xkb_state_notify_event_t state_notify;
    } *event = (union xkb_event *)gevent;

    DEBUG("process_xkb_event for device %d\n", event->any.deviceID);

    if (event->any.deviceID != xkb_x11_get_core_keyboard_device_id(conn)) {
        return;
    }

    /*
     * XkbNewKkdNotify and XkbMapNotify together capture all sorts of keymap
     * updates (e.g. xmodmap, xkbcomp, setxkbmap), with minimal redundent
     * recompilations.
     */
    switch (event->any.xkbType) {
        case XCB_XKB_NEW_KEYBOARD_NOTIFY:
            if (event->new_keyboard_notify.changed & XCB_XKB_NKN_DETAIL_KEYCODES) {
                (void)load_keymap();
            }
            break;

        case XCB_XKB_MAP_NOTIFY:
            (void)load_keymap();
            break;

        case XCB_XKB_STATE_NOTIFY:
            if (xkb_state != NULL) {
                xkb_state_update_mask(xkb_state,
                                      event->state_notify.baseMods,
                                      event->state_notify.latchedMods,
                                      event->state_notify.lockedMods,
                                      event->state_notify.baseGroup,
                                      event->state_notify.latchedGroup,
                                      event->state_notify.lockedGroup);
            }
            break;
    }
}

/*
 * Called when the properties on the root window change, e.g. when the screen
 * resolution changes. If so we update the window to cover the whole screen
 * and also redraw the image, if any.
 *
 */
static void handle_screen_resize(void) {
    xcb_get_geometry_cookie_t geomc;
    xcb_get_geometry_reply_t *geom;
    geomc = xcb_get_geometry(conn, screen->root);
    if ((geom = xcb_get_geometry_reply(conn, geomc, NULL)) == NULL) {
        return;
    }

    uint32_t width = geom->width;
    uint32_t height = geom->height;
    free(geom);

    if (last_resolution[0] == width && last_resolution[1] == height) {
        return;
    }

    last_resolution[0] = width;
    last_resolution[1] = height;

    /* The pixmaps have the old size and must be recreated. */
    free_bg_pixmap();

    uint32_t mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    xcb_configure_window(conn, win, mask, last_resolution);

    randr_query(screen->root);
    redraw_screen();
    xcb_flush(conn);
}

#ifndef __OpenBSD__
/*
 * Callback function for PAM. We only react on password request callbacks.
 *
 */
static int conv_callback(int num_msg, const struct pam_message **msg,
                         struct pam_response **resp, void *appdata_ptr) {
    (void)appdata_ptr;

    if (num_msg <= 0 || msg == NULL || resp == NULL) {
        return PAM_CONV_ERR;
    }

    /* PAM expects an array of responses, one for each message */
    struct pam_response *responses = calloc((size_t)num_msg, sizeof(struct pam_response));
    if (responses == NULL) {
        return PAM_CONV_ERR;
    }

    for (int c = 0; c < num_msg; c++) {
        if (msg[c] == NULL) {
            for (int i = 0; i < num_msg; i++) {
                free(responses[i].resp);
            }
            free(responses);
            return PAM_CONV_ERR;
        }

        if (msg[c]->msg_style != PAM_PROMPT_ECHO_OFF &&
            msg[c]->msg_style != PAM_PROMPT_ECHO_ON) {
            continue;
        }

        /* return code is currently not used but should be set to zero */
        responses[c].resp_retcode = 0;
        responses[c].resp = strdup(password_value());
        if (responses[c].resp == NULL) {
            for (int i = 0; i < num_msg; i++) {
                free(responses[i].resp);
            }
            free(responses);
            return PAM_CONV_ERR;
        }
    }

    *resp = responses;
    return PAM_SUCCESS;
}
#endif

/*
 * This callback is only a dummy, see xcb_prepare_cb and xcb_check_cb.
 * See also man libev(3): "ev_prepare" and "ev_check" - customise your event loop
 *
 */
static void xcb_got_event(EV_P_ struct ev_io *w, int revents) {
    (void)w;
    (void)revents;
    /* empty, because xcb_prepare_cb and xcb_check_cb are used */
}

/*
 * Flush before blocking (and waiting for new events)
 *
 */
static void xcb_prepare_cb(EV_P_ ev_prepare *w, int revents) {
    (void)w;
    (void)revents;
    xcb_flush(conn);
}

/*
 * Try closing logind sleep lock fd passed over from xss-lock, in case we're
 * being run from there.
 *
 */
static void maybe_close_sleep_lock_fd(void) {
    const char *sleep_lock_fd = getenv("XSS_SLEEP_LOCK_FD");
    if (sleep_lock_fd == NULL || *sleep_lock_fd == '\0') {
        return;
    }

    errno = 0;
    char *endptr = NULL;
    long fd = strtol(sleep_lock_fd, &endptr, 10);
    if (errno != 0 || endptr == sleep_lock_fd || *endptr != '\0') {
        return;
    }

    /* Never close stdin/stdout/stderr by accident. */
    if (fd < 3 || fd > INT_MAX) {
        return;
    }

    close((int)fd);
}

/*
 * Instead of polling the X connection socket we leave this to
 * xcb_poll_for_event() which knows better than we can ever know.
 *
 */
static void xcb_check_cb(EV_P_ ev_check *w, int revents) {
    (void)w;
    (void)revents;
    xcb_generic_event_t *event;

    if (xcb_connection_has_error(conn)) {
        errx(EXIT_FAILURE, "X11 connection broke, did your server terminate?");
    }

    while ((event = xcb_poll_for_event(conn)) != NULL) {
        if (event->response_type == 0) {
            xcb_generic_error_t *error = (xcb_generic_error_t *)event;
            if (debug_mode) {
                fprintf(stderr, "X11 Error received! sequence 0x%x, error_code = %d\n",
                        error->sequence, error->error_code);
            }
            free(event);
            continue;
        }

        /* Strip off the highest bit (set if the event is generated) */
        int type = (event->response_type & 0x7F);

        switch (type) {
            case XCB_KEY_PRESS:
                handle_key_press((xcb_key_press_event_t *)event);
                break;

            case XCB_VISIBILITY_NOTIFY:
                handle_visibility_notify(conn, (xcb_visibility_notify_event_t *)event);
                break;

            case XCB_MAP_NOTIFY:
                if (!dont_fork) {
                    /* After the first MapNotify, we never fork again. We don’t
                     * expect to get another MapNotify, but better be sure… */
                    dont_fork = true;

                    /* The sleep lock fd is only needed once. Closing it more
                     * than once could close an unrelated, reused descriptor. */
                    maybe_close_sleep_lock_fd();

                    pid_t pid = fork();
                    if (pid > 0) {
                        /* In the parent process, we exit. */
                        _exit(EXIT_SUCCESS);
                    } else if (pid == 0) {
                        ev_loop_fork(EV_DEFAULT);
                    } else {
                        /* fork() failed: stay in the foreground and keep the
                         * screen locked instead of exiting (which would unlock
                         * the screen). */
                        fprintf(stderr, "[i3lock] fork failed: %s\n", strerror(errno));
                    }
                }
                break;

            case XCB_CONFIGURE_NOTIFY:
                handle_screen_resize();
                break;

            default:
                if (type == xkb_base_event) {
                    process_xkb_event(event);
                    redraw_screen();
                }
                if (randr_base > -1 &&
                    type == randr_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
                    randr_query(screen->root);
                    handle_screen_resize();
                }
        }

        free(event);
    }
}

/*
 * This function is called from a fork()ed child and will raise the i3lock
 * window when the window is obscured, even when the main i3lock process is
 * blocked due to the authentication backend.
 *
 */
static void raise_loop(xcb_window_t window) {
    xcb_connection_t *raise_conn;
    xcb_generic_event_t *event;
    int screennr = 0;

    if ((raise_conn = xcb_connect(NULL, &screennr)) == NULL ||
        xcb_connection_has_error(raise_conn) != 0) {
        if (raise_conn != NULL) {
            xcb_disconnect(raise_conn);
        }
        errx(EXIT_FAILURE, "Cannot open display");
    }
    (void)screennr;

    /* We need to know about the window being obscured or getting destroyed. */
    xcb_change_window_attributes(raise_conn, window, XCB_CW_EVENT_MASK,
                                 (uint32_t[]){
                                     XCB_EVENT_MASK_VISIBILITY_CHANGE |
                                     XCB_EVENT_MASK_STRUCTURE_NOTIFY});
    xcb_flush(raise_conn);

    DEBUG("Watching window 0x%08x\n", window);
    while ((event = xcb_wait_for_event(raise_conn)) != NULL) {
        if (event->response_type == 0) {
            xcb_generic_error_t *error = (xcb_generic_error_t *)event;
            DEBUG("X11 Error received! sequence 0x%x, error_code = %d\n",
                  error->sequence, error->error_code);
            free(event);
            continue;
        }
        /* Strip off the highest bit (set if the event is generated) */
        int type = (event->response_type & 0x7F);
        DEBUG("Read event of type %d\n", type);
        switch (type) {
            case XCB_VISIBILITY_NOTIFY:
                handle_visibility_notify(raise_conn, (xcb_visibility_notify_event_t *)event);
                break;
            case XCB_UNMAP_NOTIFY:
                DEBUG("UnmapNotify for 0x%08x\n", (((xcb_unmap_notify_event_t *)event)->window));
                if (((xcb_unmap_notify_event_t *)event)->window == window) {
                    _exit(EXIT_SUCCESS);
                }
                break;
            case XCB_DESTROY_NOTIFY:
                DEBUG("DestroyNotify for 0x%08x\n", (((xcb_destroy_notify_event_t *)event)->window));
                if (((xcb_destroy_notify_event_t *)event)->window == window) {
                    _exit(EXIT_SUCCESS);
                }
                break;
            default:
                DEBUG("Unhandled event type %d\n", type);
                break;
        }
        free(event);
    }

    xcb_disconnect(raise_conn);
}

int main(int argc, char *argv[]) {
    struct passwd *pw;
    char *username;
    char *image_path = NULL;
    char *image_raw_format = NULL;
#ifndef __OpenBSD__
    int ret;
    struct pam_conv conv = {conv_callback, NULL};
#endif
    int curs_choice = CURS_NONE;
    int o;
    int longoptind = 0;
    struct option longopts[] = {
        {"version", no_argument, NULL, 'v'},
        {"nofork", no_argument, NULL, 'n'},
        {"beep", no_argument, NULL, 'b'},
        {"dpms", no_argument, NULL, 'd'},
        {"color", required_argument, NULL, 'c'},
        {"pointer", required_argument, NULL, 'p'},
        {"debug", no_argument, NULL, 0},
        {"help", no_argument, NULL, 'h'},
        {"no-unlock-indicator", no_argument, NULL, 'u'},
        {"image", required_argument, NULL, 'i'},
        {"raw", required_argument, NULL, 0},
        {"tiling", no_argument, NULL, 't'},
        {"ignore-empty-password", no_argument, NULL, 'e'},
        {"inactivity-timeout", required_argument, NULL, 'I'},
        {"show-failed-attempts", no_argument, NULL, 'f'},
        {"show-keyboard-layout", no_argument, NULL, 'k'},
        {NULL, no_argument, NULL, 0}};

    int code = EXIT_FAILURE;
    char *optstring = "hvnbdc:p:ui:teI:fk";
    while ((o = getopt_long(argc, argv, optstring, longopts, &longoptind)) != -1) {
        switch (o) {
            case 'v':
                errx(EXIT_SUCCESS, "version " I3LOCK_VERSION " © 2010 Michael Stapelberg");
            case 'n':
                dont_fork = true;
                break;
            case 'b':
                beep = true;
                break;
            case 'd':
                fprintf(stderr, "DPMS support has been removed from i3lock. Please see the manpage i3lock(1).\n");
                break;
            case 'I': {
                fprintf(stderr, "Inactivity timeout only makes sense with DPMS, which was removed. Please see the manpage i3lock(1).\n");
                break;
            }
            case 'c': {
                uint32_t pixel;
                if (!parse_hex_color(optarg, &pixel)) {
                    errx(EXIT_FAILURE, "color is invalid, it must be given in 3-byte hexadecimal format: rrggbb");
                }
                snprintf(color, sizeof(color), "%06" PRIx32, pixel);
                break;
            }
            case 'u':
                unlock_indicator = false;
                break;
            case 'i':
                free(image_path);
                if ((image_path = strdup(optarg)) == NULL) {
                    err(EXIT_FAILURE, "strdup");
                }
                break;
            case 't':
                tile = true;
                break;
            case 'p':
                if (!strcmp(optarg, "win")) {
                    curs_choice = CURS_WIN;
                } else if (!strcmp(optarg, "default")) {
                    curs_choice = CURS_DEFAULT;
                } else {
                    errx(EXIT_FAILURE, "i3lock: Invalid pointer type given. Expected one of \"win\" or \"default\".");
                }
                break;
            case 'e':
                ignore_empty_password = true;
                break;
            case 0:
                if (strcmp(longopts[longoptind].name, "debug") == 0) {
                    debug_mode = true;
                } else if (strcmp(longopts[longoptind].name, "raw") == 0) {
                    free(image_raw_format);
                    if ((image_raw_format = strdup(optarg)) == NULL) {
                        err(EXIT_FAILURE, "strdup");
                    }
                }
                break;
            case 'f':
                show_failed_attempts = true;
                break;
            case 'k':
                show_keyboard_layout = true;
                break;
            case 'h':
                code = EXIT_SUCCESS;
                /* fallthrough */
            default:
                errx(code, "Syntax: i3lock [-v] [-n] [-b] [-d] [-c color] [-u] [-p win|default]"
                           " [-i image.png] [-t] [-e] [-I timeout] [-f] [-k]");
        }
    }

    if (image_raw_format != NULL && image_path == NULL) {
        errx(EXIT_FAILURE, "--raw requires --image");
    }

    if ((pw = getpwuid(getuid())) == NULL) {
        err(EXIT_FAILURE, "getpwuid() failed");
    }
    if ((username = pw->pw_name) == NULL) {
        errx(EXIT_FAILURE, "pw->pw_name is NULL.");
    }
    if (getenv("WAYLAND_DISPLAY") != NULL) {
        errx(EXIT_FAILURE, "i3lock is a program for X11 and does not work on Wayland. Try https://github.com/swaywm/swaylock instead");
    }

    /* We need (relatively) random numbers for highlighting a random part of
     * the unlock indicator upon keypresses. */
    srand((unsigned int)time(NULL));

    /* Using mlock() as non-super-user seems only possible in Linux.
     * Users of other operating systems should use encrypted swap/no swap
     * (or remove the ifdef and run i3lock as super-user).
     * Alas, swap is encrypted by default on OpenBSD so swapping out
     * is not necessarily an issue. */
    if (!password_lock()) {
        err(EXIT_FAILURE, "Could not lock password page in memory, check RLIMIT_MEMLOCK");
    }

#ifndef __OpenBSD__
    /* Initialize PAM */
    if ((ret = pam_start("i3lock", username, &conv, &pam_handle)) != PAM_SUCCESS) {
        errx(EXIT_FAILURE, "PAM: %s", pam_strerror(pam_handle, ret));
    }
    pam_started = true;

    const char *display = getenv("DISPLAY");
    if ((ret = pam_set_item(pam_handle, PAM_TTY, display != NULL ? display : "i3lock")) != PAM_SUCCESS) {
        fprintf(stderr, "PAM: pam_set_item(PAM_TTY) failed: %s\n", pam_strerror(pam_handle, ret));
    }
#endif

    /* Double checking that connection is good and operatable with xcb */
    int screennr;
    if ((conn = xcb_connect(NULL, &screennr)) == NULL ||
        xcb_connection_has_error(conn)) {
        errx(EXIT_FAILURE, "Could not connect to X11, maybe you need to set DISPLAY?");
    }

    if (xkb_x11_setup_xkb_extension(conn,
                                    XKB_X11_MIN_MAJOR_XKB_VERSION,
                                    XKB_X11_MIN_MINOR_XKB_VERSION,
                                    0,
                                    NULL,
                                    NULL,
                                    &xkb_base_event,
                                    &xkb_base_error) != 1) {
        errx(EXIT_FAILURE, "Could not setup XKB extension.");
    }

    static const xcb_xkb_map_part_t required_map_parts =
        (XCB_XKB_MAP_PART_KEY_TYPES |
         XCB_XKB_MAP_PART_KEY_SYMS |
         XCB_XKB_MAP_PART_MODIFIER_MAP |
         XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
         XCB_XKB_MAP_PART_KEY_ACTIONS |
         XCB_XKB_MAP_PART_VIRTUAL_MODS |
         XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP);

    static const xcb_xkb_event_type_t required_events =
        (XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY |
         XCB_XKB_EVENT_TYPE_MAP_NOTIFY |
         XCB_XKB_EVENT_TYPE_STATE_NOTIFY);

    xcb_xkb_select_events(
        conn,
        xkb_x11_get_core_keyboard_device_id(conn),
        required_events,
        0,
        required_events,
        required_map_parts,
        required_map_parts,
        0);

    /* When we cannot initially load the keymap, we better exit */
    if (!load_keymap()) {
        errx(EXIT_FAILURE, "Could not load keymap");
    }

    const char *locale = getenv("LC_ALL");
    if (!locale || !*locale) {
        locale = getenv("LC_CTYPE");
    }
    if (!locale || !*locale) {
        locale = getenv("LANG");
    }
    if (!locale || !*locale) {
        if (debug_mode) {
            fprintf(stderr, "Can't detect your locale, fallback to C\n");
        }
        locale = "C";
    }

    (void)load_compose_table(locale);

    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    if (screen == NULL) {
        errx(EXIT_FAILURE, "Could not determine the X11 screen");
    }

    init_dpi();

    randr_init(&randr_base, screen->root);
    randr_query(screen->root);

    last_resolution[0] = screen->width_in_pixels;
    last_resolution[1] = screen->height_in_pixels;

    xcb_change_window_attributes(conn, screen->root, XCB_CW_EVENT_MASK,
                                 (uint32_t[]){XCB_EVENT_MASK_STRUCTURE_NOTIFY});

    if (image_path != NULL) {
        /* load_image() returns NULL on error, in which case we simply use the
         * background color. */
        img = load_image(image_path, image_raw_format);
    }

    free(image_path);
    free(image_raw_format);

    xcb_window_t stolen_focus = find_focused_window(conn, screen->root);

    /* Open the fullscreen window, initially with the background color. The
     * background image is rendered by redraw_screen(). */
    win = open_fullscreen_window(conn, screen, color, XCB_NONE);

    cursor = create_cursor(conn, screen, win, curs_choice);

    /* Display the "locking…" message while trying to grab the pointer/keyboard. */
    auth_state = STATE_AUTH_LOCK;
    redraw_screen();
    if (!grab_pointer_and_keyboard(conn, screen, cursor, 1000)) {
        DEBUG("stole focus from X11 window 0x%08x\n", stolen_focus);

        /* Set the focus to i3lock, possibly closing context menus which would
         * otherwise prevent us from grabbing keyboard/pointer.
         *
         * We cannot use set_focused_window because _NET_ACTIVE_WINDOW only
         * works for managed windows, but i3lock uses an unmanaged window
         * (override_redirect=1). */
        xcb_set_input_focus(conn, XCB_INPUT_FOCUS_PARENT /* revert_to */, win, XCB_CURRENT_TIME);
        if (!grab_pointer_and_keyboard(conn, screen, cursor, 9000)) {
            auth_state = STATE_I3LOCK_LOCK_FAILED;
            redraw_screen();
            sleep(1);
            errx(EXIT_FAILURE, "Cannot grab pointer/keyboard");
        }
    }

    pid_t pid = fork();
    /* The pid == -1 case is intentionally ignored here:
     * While the child process is useful for preventing other windows from
     * popping up while i3lock blocks, it is not critical. */
    if (pid == 0) {
        /* Child */
        close(xcb_get_file_descriptor(conn));
        maybe_close_sleep_lock_fd();
        raise_loop(win);
        _exit(EXIT_SUCCESS);
    }

    /* Load the keymap again to sync the current modifier state. Since we first
     * loaded the keymap, there might have been changes, but starting from now,
     * we should get all key presses/releases due to having grabbed the
     * keyboard. */
    (void)load_keymap();

    /* Initialize the libev event loop. */
    main_loop = EV_DEFAULT;
    if (main_loop == NULL) {
        errx(EXIT_FAILURE, "Could not initialize libev. Bad LIBEV_FLAGS?");
    }

    /* Explicitly call the screen redraw in case "locking…" message was displayed */
    auth_state = STATE_AUTH_IDLE;
    redraw_screen();

    struct ev_io xcb_watcher;
    struct ev_check xcb_check;
    struct ev_prepare xcb_prepare;

    ev_io_init(&xcb_watcher, xcb_got_event, xcb_get_file_descriptor(conn), EV_READ);
    ev_io_start(main_loop, &xcb_watcher);

    ev_check_init(&xcb_check, xcb_check_cb);
    ev_check_start(main_loop, &xcb_check);

    ev_prepare_init(&xcb_prepare, xcb_prepare_cb);
    ev_prepare_start(main_loop, &xcb_prepare);

    /* Invoke the event callback once to catch all the events which were
     * received up until now. ev will only pick up new events (when the X11
     * file descriptor becomes readable). */
    ev_invoke(main_loop, &xcb_check, 0);
    ev_loop(main_loop, 0);

#ifndef __OpenBSD__
    if (pam_started) {
        pam_end(pam_handle, PAM_SUCCESS);
    }
#endif

    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
    xcb_destroy_window(conn, win);

    if (stolen_focus != XCB_NONE) {
        DEBUG("restoring focus to X11 window 0x%08x\n", stolen_focus);
        set_focused_window(conn, screen->root, stolen_focus);
    }

    if (cursor != XCB_NONE) {
        xcb_free_cursor(conn, cursor);
    }

    xcb_aux_sync(conn);
    xcb_disconnect(conn);

    return 0;
}
````

### `xcb.c`

````c
/*
 * vim:ts=4:sw=4:expandtab
 *
 * © 2010 Michael Stapelberg
 *
 * xcb.c: contains all functions which use XCB to talk to X11. Mostly wrappers
 *        around the rather complicated/ugly parts of the XCB API.
 *
 */
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <time.h>

#include "cursors.h"
#include "unlock_indicator.h"
#include "util.h"
#include "xcb.h"

xcb_connection_t *conn;
xcb_screen_t *screen;

static xcb_atom_t _NET_WM_BYPASS_COMPOSITOR = XCB_NONE;
static xcb_atom_t _NET_ACTIVE_WINDOW = XCB_NONE;

#define curs_invisible_width 8
#define curs_invisible_height 8

static unsigned char curs_invisible_bits[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

#define curs_windows_width 11
#define curs_windows_height 19

static unsigned char curs_windows_bits[] = {
    0xfe, 0x07, 0xfc, 0x07, 0xfa, 0x07, 0xf6, 0x07, 0xee, 0x07, 0xde, 0x07,
    0xbe, 0x07, 0x7e, 0x07, 0xfe, 0x06, 0xfe, 0x05, 0x3e, 0x00, 0xb6, 0x07,
    0x6a, 0x07, 0x6c, 0x07, 0xde, 0x06, 0xdf, 0x06, 0xbf, 0x05, 0xbf, 0x05,
    0x7f, 0x06};

#define mask_windows_width 11
#define mask_windows_height 19

static unsigned char mask_windows_bits[] = {
    0x01, 0x00, 0x03, 0x00, 0x07, 0x00, 0x0f, 0x00, 0x1f, 0x00, 0x3f, 0x00,
    0x7f, 0x00, 0xff, 0x00, 0xff, 0x01, 0xff, 0x03, 0xff, 0x07, 0x7f, 0x00,
    0xf7, 0x00, 0xf3, 0x00, 0xe1, 0x01, 0xe0, 0x01, 0xc0, 0x03, 0xc0, 0x03,
    0x80, 0x01};

static bool intern_atom(xcb_connection_t *conn, const char *name, xcb_atom_t *atom) {
    xcb_generic_error_t *error = NULL;
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        conn,
        xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name),
        &error);

    if (reply == NULL) {
        if (error != NULL) {
            fprintf(stderr, "X11 Error %d while interning %s\n", error->error_code, name);
            free(error);
        }
        return false;
    }

    *atom = reply->atom;
    free(reply);
    return true;
}

static uint32_t get_colorpixel(const char *hex) {
    uint32_t pixel = 0;
    if (!parse_hex_color(hex, &pixel)) {
        fprintf(stderr, "Invalid color: %s\n", hex != NULL ? hex : "(null)");
        return 0;
    }
    return pixel;
}

xcb_visualtype_t *get_root_visual_type(xcb_screen_t *scr) {
    xcb_visualtype_t *visual_type = NULL;
    xcb_depth_iterator_t depth_iter;
    xcb_visualtype_iterator_t visual_iter;

    for (depth_iter = xcb_screen_allowed_depths_iterator(scr);
         depth_iter.rem;
         xcb_depth_next(&depth_iter)) {
        for (visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
             visual_iter.rem;
             xcb_visualtype_next(&visual_iter)) {
            if (scr->root_visual != visual_iter.data->visual_id) {
                continue;
            }

            visual_type = visual_iter.data;
            return visual_type;
        }
    }

    return NULL;
}

xcb_pixmap_t create_bg_pixmap(xcb_connection_t *conn, xcb_screen_t *scr, const uint32_t *resolution, const char *color) {
    if (resolution == NULL || resolution[0] == 0 || resolution[1] == 0 ||
        resolution[0] > UINT16_MAX || resolution[1] > UINT16_MAX) {
        return XCB_NONE;
    }

    xcb_pixmap_t bg_pixmap = xcb_generate_id(conn);
    xcb_create_pixmap(conn, scr->root_depth, bg_pixmap, scr->root,
                      (uint16_t)resolution[0], (uint16_t)resolution[1]);

    /* Generate a Graphics Context and fill the pixmap with background color
     * (for images that are smaller than your screen) */
    xcb_gcontext_t gc = xcb_generate_id(conn);
    uint32_t values[] = {get_colorpixel(color)};
    xcb_create_gc(conn, gc, bg_pixmap, XCB_GC_FOREGROUND, values);
    xcb_rectangle_t rect = {0, 0, (uint16_t)resolution[0], (uint16_t)resolution[1]};
    xcb_poly_fill_rectangle(conn, bg_pixmap, gc, 1, &rect);
    xcb_free_gc(conn, gc);

    return bg_pixmap;
}

xcb_window_t open_fullscreen_window(xcb_connection_t *conn, xcb_screen_t *scr, const char *color, xcb_pixmap_t pixmap) {
    uint32_t mask = 0;
    uint32_t values[3];
    xcb_window_t win = xcb_generate_id(conn);

    if (pixmap == XCB_NONE) {
        mask |= XCB_CW_BACK_PIXEL;
        values[0] = get_colorpixel(color);
    } else {
        mask |= XCB_CW_BACK_PIXMAP;
        values[0] = pixmap;
    }

    mask |= XCB_CW_OVERRIDE_REDIRECT;
    values[1] = 1;

    mask |= XCB_CW_EVENT_MASK;
    values[2] = XCB_EVENT_MASK_EXPOSURE |
                XCB_EVENT_MASK_KEY_PRESS |
                XCB_EVENT_MASK_KEY_RELEASE |
                XCB_EVENT_MASK_VISIBILITY_CHANGE |
                XCB_EVENT_MASK_STRUCTURE_NOTIFY;

    xcb_create_window(conn,
                      XCB_COPY_FROM_PARENT,
                      win,       /* the window id */
                      scr->root, /* parent == root */
                      0, 0,
                      scr->width_in_pixels,
                      scr->height_in_pixels, /* dimensions */
                      0,                     /* border = 0, we draw our own */
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      XCB_WINDOW_CLASS_COPY_FROM_PARENT, /* copy visual from parent */
                      mask,
                      values);

    char *name = "i3lock";
    xcb_change_property(conn,
                        XCB_PROP_MODE_REPLACE,
                        win,
                        XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING,
                        8,
                        (uint32_t)strlen(name),
                        name);

    xcb_change_property(conn,
                        XCB_PROP_MODE_REPLACE,
                        win,
                        XCB_ATOM_WM_CLASS,
                        XCB_ATOM_STRING,
                        8,
                        2 * (uint32_t)(strlen("i3lock") + 1),
                        "i3lock\0i3lock\0");

    const uint32_t bypass_compositor = 1; /* disable compositing */
    if (_NET_WM_BYPASS_COMPOSITOR == XCB_NONE) {
        (void)intern_atom(conn, "_NET_WM_BYPASS_COMPOSITOR", &_NET_WM_BYPASS_COMPOSITOR);
    }
    if (_NET_WM_BYPASS_COMPOSITOR != XCB_NONE) {
        xcb_change_property(conn,
                            XCB_PROP_MODE_REPLACE,
                            win,
                            _NET_WM_BYPASS_COMPOSITOR,
                            XCB_ATOM_CARDINAL,
                            32,
                            1,
                            &bypass_compositor);
    }

    /* Map the window (= make it visible) */
    xcb_map_window(conn, win);

    /* Raise window (put it on top) */
    values[0] = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, values);

    /* Ensure that the window is created and set up before returning */
    xcb_aux_sync(conn);

    return win;
}

static double monotonic_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * Repeatedly tries to grab pointer and keyboard (up to the specified number of
 * tries for each device).
 *
 * Returns true if both grabs succeeded, false if not.
 *
 */
bool grab_pointer_and_keyboard(xcb_connection_t *conn, xcb_screen_t *scr, xcb_cursor_t cursor, int tries) {
    if (tries <= 0) {
        return false;
    }

    const double screen_redraw_timeout = 0.1; /* 100ms */
    bool redrawn = false;
    const double start = monotonic_seconds();

    bool pointer_grabbed = false;
    for (int attempt = 0; attempt < tries; attempt++) {
        xcb_grab_pointer_cookie_t pcookie = xcb_grab_pointer(
            conn,
            false,               /* get all pointer events specified by the following mask */
            scr->root,           /* grab the root window */
            XCB_NONE,            /* which events to let through */
            XCB_GRAB_MODE_ASYNC, /* pointer events should continue as normal */
            XCB_GRAB_MODE_ASYNC, /* keyboard mode */
            XCB_NONE,            /* confine_to = in which window should the cursor stay */
            cursor,              /* we change the cursor to whatever the user wanted */
            XCB_CURRENT_TIME);

        xcb_grab_pointer_reply_t *preply = xcb_grab_pointer_reply(conn, pcookie, NULL);
        bool success = preply != NULL && preply->status == XCB_GRAB_STATUS_SUCCESS;
        free(preply);

        if (success) {
            pointer_grabbed = true;
            break;
        }

        /* Make this quite a bit slower. */
        usleep(50);

        if (!redrawn && (monotonic_seconds() - start) >= screen_redraw_timeout) {
            redraw_screen();
            redrawn = true;
        }
    }

    if (!pointer_grabbed) {
        return false;
    }

    bool keyboard_grabbed = false;
    for (int attempt = 0; attempt < tries; attempt++) {
        xcb_grab_keyboard_cookie_t kcookie = xcb_grab_keyboard(
            conn,
            true,      /* report events */
            scr->root, /* grab the root window */
            XCB_CURRENT_TIME,
            XCB_GRAB_MODE_ASYNC, /* process events as normal, do not require sync */
            XCB_GRAB_MODE_ASYNC);

        xcb_grab_keyboard_reply_t *kreply = xcb_grab_keyboard_reply(conn, kcookie, NULL);
        bool success = kreply != NULL && kreply->status == XCB_GRAB_STATUS_SUCCESS;
        free(kreply);

        if (success) {
            keyboard_grabbed = true;
            break;
        }

        /* Make this quite a bit slower. */
        usleep(50);

        if (!redrawn && (monotonic_seconds() - start) >= screen_redraw_timeout) {
            redraw_screen();
            redrawn = true;
        }
    }

    return keyboard_grabbed;
}

xcb_cursor_t create_cursor(xcb_connection_t *conn, xcb_screen_t *scr, xcb_window_t win, int choice) {
    xcb_pixmap_t bitmap;
    xcb_pixmap_t mask;
    xcb_cursor_t cursor;

    unsigned char *curs_bits;
    unsigned char *mask_bits;
    int curs_w;
    int curs_h;

    switch (choice) {
        case CURS_NONE:
            curs_bits = curs_invisible_bits;
            mask_bits = curs_invisible_bits;
            curs_w = curs_invisible_width;
            curs_h = curs_invisible_height;
            break;
        case CURS_WIN:
            curs_bits = curs_windows_bits;
            mask_bits = mask_windows_bits;
            curs_w = curs_windows_width;
            curs_h = curs_windows_height;
            break;
        case CURS_DEFAULT:
        default:
            return XCB_NONE; /* XCB_NONE is xcb's way of saying "don't change the cursor" */
    }

    bitmap = xcb_create_pixmap_from_bitmap_data(conn,
                                                win,
                                                curs_bits,
                                                curs_w,
                                                curs_h,
                                                1,
                                                scr->white_pixel,
                                                scr->black_pixel,
                                                NULL);

    mask = xcb_create_pixmap_from_bitmap_data(conn,
                                              win,
                                              mask_bits,
                                              curs_w,
                                              curs_h,
                                              1,
                                              scr->white_pixel,
                                              scr->black_pixel,
                                              NULL);

    cursor = xcb_generate_id(conn);

    xcb_create_cursor(conn,
                      cursor,
                      bitmap,
                      mask,
                      65535, 65535, 65535,
                      0, 0, 0,
                      0, 0);

    xcb_free_pixmap(conn, bitmap);
    xcb_free_pixmap(conn, mask);

    return cursor;
}

static void _init_net_active_window(xcb_connection_t *conn) {
    if (_NET_ACTIVE_WINDOW != XCB_NONE) {
        /* already initialized */
        return;
    }
    (void)intern_atom(conn, "_NET_ACTIVE_WINDOW", &_NET_ACTIVE_WINDOW);
}

xcb_window_t find_focused_window(xcb_connection_t *conn, const xcb_window_t root) {
    xcb_window_t result = XCB_NONE;

    _init_net_active_window(conn);
    if (_NET_ACTIVE_WINDOW == XCB_NONE) {
        return XCB_NONE;
    }

    xcb_get_property_reply_t *prop_reply = xcb_get_property_reply(
        conn,
        xcb_get_property_unchecked(
            conn, false, root, _NET_ACTIVE_WINDOW, XCB_GET_PROPERTY_TYPE_ANY, 0, 1 /* word */),
        NULL);
    if (prop_reply == NULL) {
        goto out;
    }
    if (xcb_get_property_value_length(prop_reply) < (int)sizeof(xcb_window_t)) {
        goto out_prop;
    }
    if (prop_reply->type != XCB_ATOM_WINDOW) {
        goto out_prop;
    }

    result = *((xcb_window_t *)xcb_get_property_value(prop_reply));

out_prop:
    free(prop_reply);
out:
    return result;
}

void set_focused_window(xcb_connection_t *conn, const xcb_window_t root, const xcb_window_t window) {
    _init_net_active_window(conn);
    if (_NET_ACTIVE_WINDOW == XCB_NONE) {
        return;
    }

    xcb_client_message_event_t ev;
    memset(&ev, '\0', sizeof(xcb_client_message_event_t));

    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = window;
    ev.type = _NET_ACTIVE_WINDOW;
    ev.format = 32;
    ev.data.data32[0] = 2; /* 2 = pager */

    xcb_send_event(conn, false, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (char *)&ev);
    xcb_flush(conn);
}
````

### `unlock_indicator.c`

````c
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
````

### `randr.c`

````c
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
#include <math.h>
#include <xcb/xcb.h>
#include <xcb/xinerama.h>
#include <xcb/randr.h>

#include "i3lock.h"
#include "xcb.h"
#include "randr.h"

/* Number of Xinerama screens which are currently present. */
int xr_screens = 0;

/* The resolutions of the currently present Xinerama screens. */
Rect *xr_resolutions = NULL;

static bool xinerama_active;
static bool has_randr = false;
static bool has_randr_1_5 = false;
extern bool debug_mode;

static void _xinerama_init(void) {
    const xcb_query_extension_reply_t *extreply = xcb_get_extension_data(conn, &xcb_xinerama_id);
    if (extreply == NULL || !extreply->present) {
        DEBUG("Xinerama extension not found, disabling.\n");
        return;
    }

    xcb_xinerama_is_active_cookie_t cookie;
    xcb_xinerama_is_active_reply_t *reply;

    cookie = xcb_xinerama_is_active(conn);
    reply = xcb_xinerama_is_active_reply(conn, cookie, NULL);
    if (!reply) {
        return;
    }

    if (!reply->state) {
        free(reply);
        return;
    }

    xinerama_active = true;
    free(reply);
}

void randr_init(int *event_base, xcb_window_t root) {
    const xcb_query_extension_reply_t *extreply;

    extreply = xcb_get_extension_data(conn, &xcb_randr_id);
    if (extreply == NULL || !extreply->present) {
        DEBUG("RandR is not present, falling back to Xinerama.\n");
        _xinerama_init();
        return;
    }

    xcb_generic_error_t *err = NULL;
    xcb_randr_query_version_reply_t *randr_version =
        xcb_randr_query_version_reply(
            conn, xcb_randr_query_version(conn, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION), &err);
    if (err != NULL || randr_version == NULL) {
        if (err != NULL) {
            DEBUG("Could not query RandR version: X11 error code %d\n", err->error_code);
            free(err);
        }
        free(randr_version);
        _xinerama_init();
        return;
    }

    has_randr = true;

    has_randr_1_5 = (randr_version->major_version > 1) ||
                    (randr_version->major_version == 1 && randr_version->minor_version >= 5);

    free(randr_version);

    if (event_base != NULL) {
        *event_base = extreply->first_event;
    }

    xcb_randr_select_input(conn, root,
                           XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
                               XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
                               XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
                               XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY);

    xcb_flush(conn);
}

/*
 * randr_query_outputs_15 uses RandR ≥ 1.5 to update outputs.
 *
 */
static bool _randr_query_monitors_15(xcb_window_t root) {
#if XCB_RANDR_MINOR_VERSION < 5
    return false;
#else
    /* RandR 1.5 available at compile-time, i.e. libxcb is new enough */
    if (!has_randr_1_5) {
        return false;
    }
    /* RandR 1.5 available at run-time (supported by the server) */
    DEBUG("Querying monitors using RandR 1.5\n");
    xcb_generic_error_t *err = NULL;
    xcb_randr_get_monitors_reply_t *monitors =
        xcb_randr_get_monitors_reply(
            conn, xcb_randr_get_monitors(conn, root, true), &err);
    if (err != NULL || monitors == NULL) {
        if (err != NULL) {
            DEBUG("Could not get RandR monitors: X11 error code %d\n", err->error_code);
            free(err);
        }
        free(monitors);
        /* Fall back to RandR ≤ 1.4 */
        return false;
    }

    int screens = xcb_randr_get_monitors_monitors_length(monitors);
    DEBUG("%d RandR monitors found (timestamp %d)\n",
          screens, monitors->timestamp);

    if (screens <= 0) {
        free(xr_resolutions);
        xr_resolutions = NULL;
        xr_screens = 0;
        free(monitors);
        return true;
    }

    Rect *resolutions = calloc((size_t)screens, sizeof(Rect));
    /* No memory? Just keep on using the old information. */
    if (!resolutions) {
        free(monitors);
        return true;
    }

    xcb_randr_monitor_info_iterator_t iter;
    int monitor_index;
    for (iter = xcb_randr_get_monitors_monitors_iterator(monitors), monitor_index = 0;
         iter.rem;
         xcb_randr_monitor_info_next(&iter), monitor_index++) {
        const xcb_randr_monitor_info_t *monitor_info = iter.data;

        resolutions[monitor_index].x = monitor_info->x;
        resolutions[monitor_index].y = monitor_info->y;
        resolutions[monitor_index].width = monitor_info->width;
        resolutions[monitor_index].height = monitor_info->height;
        DEBUG("found RandR monitor: %d x %d at %d x %d\n",
              monitor_info->width, monitor_info->height,
              monitor_info->x, monitor_info->y);
    }
    free(xr_resolutions);
    xr_resolutions = resolutions;
    xr_screens = screens;

    free(monitors);
    return true;
#endif
}

/*
 * randr_query_outputs_14 uses RandR ≤ 1.4 to update outputs.
 *
 */
static bool _randr_query_outputs_14(xcb_window_t root) {
    if (!has_randr) {
        return false;
    }
    DEBUG("Querying outputs using RandR ≤ 1.4\n");

    /* Get screen resources (primary output, crtcs, outputs, modes) */
    xcb_randr_get_screen_resources_current_cookie_t rcookie;
    rcookie = xcb_randr_get_screen_resources_current(conn, root);

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn, rcookie, NULL);
    if (res == NULL) {
        DEBUG("Could not query screen resources.\n");
        return false;
    }

    /* timestamp of the configuration so that we get consistent replies to all
     * requests (if the configuration changes between our different calls) */
    const xcb_timestamp_t cts = res->config_timestamp;

    const int len = xcb_randr_get_screen_resources_current_outputs_length(res);

    if (len <= 0) {
        free(xr_resolutions);
        xr_resolutions = NULL;
        xr_screens = 0;
        free(res);
        return true;
    }

    /* an output is VGA-1, LVDS-1, etc. (usually physical video outputs) */
    xcb_randr_output_t *randr_outputs = xcb_randr_get_screen_resources_current_outputs(res);

    /* Request information for each output. Use the heap instead of a VLA:
     * len comes from the X server and may be arbitrarily large. */
    xcb_randr_get_output_info_cookie_t *ocookie = calloc((size_t)len, sizeof(*ocookie));
    if (ocookie == NULL) {
        free(res);
        return true;
    }

    for (int i = 0; i < len; i++) {
        ocookie[i] = xcb_randr_get_output_info(conn, randr_outputs[i], cts);
    }

    Rect *resolutions = calloc((size_t)len, sizeof(Rect));
    /* No memory? Just keep on using the old information. */
    if (!resolutions) {
        free(ocookie);
        free(res);
        return true;
    }

    /* Loop through all outputs available for this X11 screen */
    int screen_count = 0;

    for (int i = 0; i < len; i++) {
        xcb_randr_get_output_info_reply_t *output;

        if ((output = xcb_randr_get_output_info_reply(conn, ocookie[i], NULL)) == NULL) {
            continue;
        }

        if (output->crtc == XCB_NONE) {
            free(output);
            continue;
        }

        xcb_randr_get_crtc_info_cookie_t icookie;
        xcb_randr_get_crtc_info_reply_t *crtc;
        icookie = xcb_randr_get_crtc_info(conn, output->crtc, cts);
        if ((crtc = xcb_randr_get_crtc_info_reply(conn, icookie, NULL)) == NULL) {
            DEBUG("Skipping output: could not get CRTC (0x%08x)\n", output->crtc);
            free(output);
            continue;
        }

        resolutions[screen_count].x = crtc->x;
        resolutions[screen_count].y = crtc->y;
        resolutions[screen_count].width = crtc->width;
        resolutions[screen_count].height = crtc->height;

        DEBUG("found RandR output: %d x %d at %d x %d\n",
              crtc->width, crtc->height,
              crtc->x, crtc->y);

        screen_count++;

        free(crtc);
        free(output);
    }
    free(xr_resolutions);
    xr_resolutions = resolutions;
    xr_screens = screen_count;
    free(ocookie);
    free(res);
    return true;
}

static void _xinerama_query_screens(void) {
    if (!xinerama_active) {
        return;
    }

    xcb_generic_error_t *err = NULL;
    xcb_xinerama_query_screens_cookie_t cookie;
    xcb_xinerama_query_screens_reply_t *reply;
    cookie = xcb_xinerama_query_screens_unchecked(conn);
    reply = xcb_xinerama_query_screens_reply(conn, cookie, &err);
    if (!reply) {
        if (err != NULL) {
            DEBUG("Couldn't get Xinerama screens: X11 error code %d\n", err->error_code);
            free(err);
        }
        return;
    }

    int screens = xcb_xinerama_query_screens_screen_info_length(reply);
    if (screens <= 0) {
        free(xr_resolutions);
        xr_resolutions = NULL;
        xr_screens = 0;
        free(reply);
        return;
    }

    xcb_xinerama_screen_info_t *screen_info = xcb_xinerama_query_screens_screen_info(reply);

    Rect *resolutions = calloc((size_t)screens, sizeof(Rect));
    /* No memory? Just keep on using the old information. */
    if (!resolutions) {
        free(reply);
        return;
    }

    for (int i = 0; i < screens; i++) {
        resolutions[i].x = screen_info[i].x_org;
        resolutions[i].y = screen_info[i].y_org;
        resolutions[i].width = screen_info[i].width;
        resolutions[i].height = screen_info[i].height;
        DEBUG("found Xinerama screen: %d x %d at %d x %d\n",
              screen_info[i].width, screen_info[i].height,
              screen_info[i].x_org, screen_info[i].y_org);
    }

    free(xr_resolutions);
    xr_resolutions = resolutions;
    xr_screens = screens;

    free(reply);
}

void randr_query(xcb_window_t root) {
    if (_randr_query_monitors_15(root)) {
        return;
    }

    if (_randr_query_outputs_14(root)) {
        return;
    }

    _xinerama_query_screens();
}
````

### `dpi.c`

````c
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 */
#include "dpi.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <xcb/xcb_xrm.h>
#include "xcb.h"
#include "i3lock.h"

#define DPI_FALLBACK 96
#define DPI_MIN 1
#define DPI_MAX 1000

extern bool debug_mode;

static long dpi;

extern xcb_screen_t *screen;

static long clamp_dpi(double value) {
    if (!isfinite(value) || value < DPI_MIN) {
        return DPI_MIN;
    }
    if (value > DPI_MAX) {
        return DPI_MAX;
    }
    return lround(value);
}

static long init_dpi_fallback(void) {
    if (screen == NULL || screen->height_in_millimeters == 0) {
        return DPI_FALLBACK;
    }
    double value = (double)screen->height_in_pixels * 25.4 / (double)screen->height_in_millimeters;
    return clamp_dpi(value);
}

/*
 * Initialize the DPI setting.
 * This will use the 'Xft.dpi' X resource if available and fall back to
 * guessing the correct value otherwise.
 */
void init_dpi(void) {
    xcb_xrm_database_t *database = NULL;
    char *resource = NULL;

    if (conn == NULL) {
        goto init_dpi_end;
    }

    database = xcb_xrm_database_from_default(conn);
    if (database == NULL) {
        DEBUG("Failed to open the resource database.\n");
        goto init_dpi_end;
    }

    xcb_xrm_resource_get_string(database, "Xft.dpi", NULL, &resource);
    if (resource == NULL) {
        DEBUG("Resource Xft.dpi not specified, skipping.\n");
        goto init_dpi_end;
    }

    char *endptr = NULL;
    errno = 0;
    double in_dpi = strtod(resource, &endptr);
    if (errno == ERANGE || !isfinite(in_dpi) || in_dpi <= 0 || *endptr != '\0' || endptr == resource) {
        DEBUG("Xft.dpi = %s is an invalid number and couldn't be parsed.\n", resource);
        dpi = 0;
        goto init_dpi_end;
    }
    dpi = clamp_dpi(in_dpi);

    DEBUG("Found Xft.dpi = %ld.\n", dpi);

init_dpi_end:
    if (resource != NULL) {
        free(resource);
    }

    if (database != NULL) {
        xcb_xrm_database_free(database);
    }

    if (dpi == 0) {
        DEBUG("Using fallback for calculating DPI.\n");
        dpi = init_dpi_fallback();
        DEBUG("Using dpi = %ld\n", dpi);
    }
}

/*
 * This function returns the value of the DPI setting.
 *
 */
long get_dpi_value(void) {
    return dpi > 0 ? dpi : DPI_FALLBACK;
}

/*
 * Convert a logical amount of pixels (e.g. 2 pixels on a “standard” 96 DPI
 * screen) to a corresponding amount of physical pixels on a standard or retina
 * screen, e.g. 5 pixels on a 227 DPI MacBook Pro 13" Retina screen.
 *
 */
int logical_px(const int logical) {
    if (screen == NULL) {
        /* Dpi info may not be available when parsing a config without an X
         * server, such as for config file validation. */
        return logical;
    }

    long current_dpi = get_dpi_value();

    /* There are many misconfigurations out there, i.e. systems with screens
     * whose dpi is in fact higher than 96 dpi, but not significantly higher,
     * so software was never adapted. We could tell people to reconfigure their
     * systems to 96 dpi in order to get the behavior they expect/are used to,
     * but since we can easily detect this case in code, let’s do it for them.
     */
    if ((current_dpi / 96.0) < 1.25) {
        return logical;
    }
    return ceil((current_dpi / 96.0) * logical);
}
````

### `util.c`

````c
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
````

### `password.c`

````c
#include <config.h>

#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif

#include "password.h"
#include "util.h"

static char password[PASSWORD_MAX];
static size_t password_len;

bool password_lock(void) {
#if defined(__linux__)
    if (mlock(password, sizeof(password)) != 0) {
        return false;
    }
#endif
    return true;
}

void password_reset(void) {
    secure_zero(password, sizeof(password));
    password_len = 0;
}

bool password_append_utf8(const char *text, size_t len) {
    if (len == 0) {
        return true;
    }
    if (text == NULL) {
        return false;
    }
    if (len > sizeof(password) - 1 - password_len) {
        return false;
    }

    memcpy(password + password_len, text, len);
    password_len += len;
    password[password_len] = '\0';
    return true;
}

bool password_backspace(void) {
    if (password_len == 0) {
        return false;
    }

    size_t i = password_len - 1;
    while (i > 0 && ((unsigned char)password[i] & 0xC0) == 0x80) {
        i--;
    }

    secure_zero(password + i, password_len - i);
    password_len = i;
    password[password_len] = '\0';
    return true;
}

size_t password_length(void) {
    return password_len;
}

const char *password_value(void) {
    return password;
}
````

### `image.c`

````c
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
````

### `include/i3lock.h`

````c
#ifndef _I3LOCK_H
#define _I3LOCK_H

#include <stdio.h>

/* This macro will only print debug output when started with --debug.
 * This is important because xautolock (for example) closes stdout/stderr by
 * default, so just printing something to stdout will lead to the data ending
 * up on the X11 socket (!). */
#define DEBUG(fmt, ...)                                            \
    do {                                                           \
        if (debug_mode) {                                          \
            fprintf(stderr, "[i3lock-debug] " fmt, ##__VA_ARGS__); \
        }                                                          \
    } while (0)

#endif
````

### `include/xcb.h`

````c
#ifndef _XCB_H
#define _XCB_H

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

extern xcb_connection_t *conn;
extern xcb_screen_t *screen;

xcb_visualtype_t *get_root_visual_type(xcb_screen_t *s);
xcb_pixmap_t create_bg_pixmap(xcb_connection_t *conn, xcb_screen_t *scr, const uint32_t *resolution, const char *color);
xcb_window_t open_fullscreen_window(xcb_connection_t *conn, xcb_screen_t *scr, const char *color, xcb_pixmap_t pixmap);
bool grab_pointer_and_keyboard(xcb_connection_t *conn, xcb_screen_t *scr, xcb_cursor_t cursor, int tries);
xcb_cursor_t create_cursor(xcb_connection_t *conn, xcb_screen_t *scr, xcb_window_t win, int choice);
xcb_window_t find_focused_window(xcb_connection_t *conn, const xcb_window_t root);
void set_focused_window(xcb_connection_t *conn, const xcb_window_t root, const xcb_window_t window);

#endif
````

### `include/randr.h`

````c
#ifndef _RANDR_H
#define _RANDR_H

#include <stdint.h>
#include <xcb/xcb.h>

typedef struct Rect {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
} Rect;

extern int xr_screens;
extern Rect *xr_resolutions;

void randr_init(int *event_base, xcb_window_t root);
void randr_query(xcb_window_t root);

#endif
````

### `include/unlock_indicator.h`

````c
#ifndef _UNLOCK_INDICATOR_H
#define _UNLOCK_INDICATOR_H

#include <xcb/xcb.h>

typedef enum {
    STATE_STARTED = 0,           /* default state */
    STATE_KEY_PRESSED = 1,       /* key was pressed, show unlock indicator */
    STATE_KEY_ACTIVE = 2,        /* a key was pressed recently, highlight part
                                   of the unlock indicator. */
    STATE_BACKSPACE_ACTIVE = 3,  /* backspace was pressed recently, highlight
                                   part of the unlock indicator in red. */
    STATE_NOTHING_TO_DELETE = 4, /* backspace was pressed, but there is nothing to delete. */
} unlock_state_t;

typedef enum {
    STATE_AUTH_IDLE = 0,          /* no authenticator interaction at the moment */
    STATE_AUTH_VERIFY = 1,        /* currently verifying the password via authenticator */
    STATE_AUTH_LOCK = 2,          /* currently locking the screen */
    STATE_AUTH_WRONG = 3,         /* the password was wrong */
    STATE_I3LOCK_LOCK_FAILED = 4, /* i3lock failed to load */
} auth_state_t;

void free_bg_pixmap(void);
void redraw_screen(void);
void clear_indicator(void);

#endif
````

### `include/util.h`

````c
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
````

### `include/password.h`

````c
#ifndef _PASSWORD_H
#define _PASSWORD_H

#include <stdbool.h>
#include <stddef.h>

#define PASSWORD_MAX 512

/* Locks the password buffer into memory. Returns false on failure. */
bool password_lock(void);

/* Securely clears the password buffer and resets its length to zero. */
void password_reset(void);

/* Appends len bytes of UTF-8 to the password. Returns false if it does not fit. */
bool password_append_utf8(const char *text, size_t len);

/* Removes the last UTF-8 code point. Returns false if the buffer is empty. */
bool password_backspace(void);

size_t password_length(void);
const char *password_value(void);

#endif
````

### `include/image.h`

````c
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
````

### `meson.build`

````meson
# -*- mode: meson -*-

# Style objective: be consistent with what mesonbuild.com documents/uses, and/or
# the meson book: https://meson-manual.com/

project(
  'i3lock',
  'c',
  version: '2.16',
  default_options: [
    'c_std=c11',
    'warning_level=1',  # enable all warnings (-Wall)
    # TODO(https://github.com/i3/i3/issues/4087): switch to
    # 'buildtype=debugoptimized',
  ],
  # Ubuntu 18.04 (supported until 2023) has meson 0.45.
  # We can revisit our minimum supported meson version
  # if it turns out to be too hard to maintain.
  meson_version: '>=0.46.0',
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-Wunused-value',
  '-Wformat=2',
  '-Wformat-security',
  '-Werror=format-security',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wpointer-arith',
  '-Wno-unused-parameter',
]), language: 'c')

# Hardening flags. get_supported_* keeps the build portable to platforms where
# a particular flag is not available.
add_project_arguments(cc.get_supported_arguments([
  '-fstack-protector-strong',
  '-fPIE',
]), language: 'c')
add_project_link_arguments(cc.get_supported_link_arguments([
  '-pie',
  '-Wl,-z,relro',
  '-Wl,-z,now',
]), language: 'c')

# _FORTIFY_SOURCE only works with optimization enabled. Avoid a preprocessor
# warning (and thus -Werror failures) in debug builds.
if get_option('optimization') != '0'
  add_project_arguments('-D_FORTIFY_SOURCE=2', language: 'c')
endif

if meson.version().version_compare('>=0.48.0')
  # https://github.com/mesonbuild/meson/issues/2166#issuecomment-629696911
  meson.add_dist_script('meson/meson-dist-script')
else
  message('meson <0.48.0 detected, dist tarballs will not be filtered')
endif

################################################################################
# Version handling
################################################################################

cdata = configuration_data()

version_array = meson.project_version().split('.')
cdata.set_quoted('I3LOCK_VERSION', '@VCS_TAG@')
cdata.set_quoted('SYSCONFDIR', join_paths(get_option('prefix'), get_option('sysconfdir')))

if get_option('b_sanitize').split(',').contains('address')
  cdata.set('I3LOCK_ASAN_ENABLED', 1)
endif

cdata.set('HAVE_STRNDUP', cc.has_function('strndup'))
cdata.set('HAVE_MKDIRP', cc.has_function('mkdirp'))
cdata.set('HAVE_EXPLICIT_BZERO', cc.has_function('explicit_bzero'))

# Instead of generating config.h directly, make vcs_tag generate it so that
# @VCS_TAG@ is replaced.
config_h_in = configure_file(
  output: 'config.h.in',
  configuration: cdata,
)
config_h = declare_dependency(
  sources: vcs_tag(
    input: config_h_in,
    output: 'config.h',
    fallback: meson.project_version() + '-non-git',
  )
)

################################################################################
# manpages
################################################################################

install_man('i3lock.1')

# Required for e.g. struct ucred to be defined as per unix(7).
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# https://mesonbuild.com/howtox.html#add-math-library-lm-portably
m_dep = cc.find_library('m', required: false)
rt_dep = cc.find_library('rt', required: false)

xcb_dep = dependency('xcb', method: 'pkg-config')
xcb_xkb_dep = dependency('xcb-xkb', method: 'pkg-config')
xcb_xinerama_dep = dependency('xcb-xinerama', method: 'pkg-config')
xcb_randr_dep = dependency('xcb-randr', method: 'pkg-config')
xcb_image_dep = dependency('xcb-image', method: 'pkg-config')
xcb_util_dep = dependency('xcb-util', method: 'pkg-config')
xcb_util_xrm_dep = dependency('xcb-xrm', method: 'pkg-config')
xkbcommon_dep = dependency('xkbcommon', method: 'pkg-config')
xkbcommon_x11_dep = dependency('xkbcommon-x11', method: 'pkg-config')
cairo_dep = dependency('cairo', version: '>=1.14.4', method: 'pkg-config')

i3lock_srcs = [
  'dpi.c',
  'i3lock.c',
  'image.c',
  'password.c',
  'randr.c',
  'unlock_indicator.c',
  'util.c',
  'xcb.c',
]

ev_dep = cc.find_library('ev')

thread_dep = dependency('threads')

i3lock_deps = [
  thread_dep,
  m_dep,
  rt_dep,
  ev_dep,
  config_h,
  cairo_dep,
  xcb_dep,
  xcb_xkb_dep,
  xcb_xinerama_dep,
  xcb_randr_dep,
  xcb_image_dep,
  xcb_util_dep,
  xcb_util_xrm_dep,
  xkbcommon_dep,
  xkbcommon_x11_dep,
]

host_os = host_machine.system()
if host_os != 'openbsd'
  pam_dep = cc.find_library('pam', required: true)
  i3lock_deps += [pam_dep]
endif

inc = include_directories('include')

executable(
  'i3lock',
  i3lock_srcs,
  install: true,
  include_directories: inc,
  dependencies: i3lock_deps,
)

install_subdir(
  'pam',
  strip_directory: true,
  install_dir: join_paths(get_option('sysconfdir'), 'pam.d'),
)

subdir('tests')
````

### `tests/meson.build`

````meson
test_util = executable(
  'test_util',
  'test_util.c',
  '../util.c',
  include_directories: inc,
  dependencies: [config_h],
)
test('util', test_util)

test_password = executable(
  'test_password',
  'test_password.c',
  '../password.c',
  '../util.c',
  include_directories: inc,
  dependencies: [config_h],
)
test('password', test_password)

test_image = executable(
  'test_image',
  'test_image.c',
  '../image.c',
  '../util.c',
  include_directories: inc,
  dependencies: [config_h, cairo_dep],
)
test('image', test_image)
````

### `tests/test_util.c`

````c
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
````

### `tests/test_password.c`

````c
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "password.h"

static int failures = 0;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    password_reset();
    check(password_length() == 0, "password starts empty");
    check(strcmp(password_value(), "") == 0, "password starts as an empty string");

    check(password_append_utf8("abc", 3), "append accepts text");
    check(password_length() == 3, "append updates the length");
    check(strcmp(password_value(), "abc") == 0, "append stores the text");

    const char utf8_acute[] = "\xc3\xa9"; /* é */
    check(password_append_utf8(utf8_acute, 2), "append accepts UTF-8");
    check(password_length() == 5, "UTF-8 length is counted in bytes");

    check(password_backspace(), "backspace removes a code point");
    check(password_length() == 3, "backspace removes the whole UTF-8 code point");
    check(strcmp(password_value(), "abc") == 0, "backspace leaves the remaining text intact");

    check(password_backspace(), "backspace removes the next character");
    check(password_backspace(), "backspace removes the last character");
    check(password_backspace(), "backspace removes the final character");
    check(!password_backspace(), "backspace reports an empty buffer");
    check(password_length() == 0, "backspace leaves the buffer empty");

    char fill[PASSWORD_MAX];
    memset(fill, 'x', sizeof(fill));
    check(password_append_utf8(fill, PASSWORD_MAX - 1), "append fills the buffer to its limit");
    check(password_length() == PASSWORD_MAX - 1, "filled buffer has the maximum length");
    check(!password_append_utf8("y", 1), "append refuses to overflow the buffer");
    check(password_length() == PASSWORD_MAX - 1, "failed append does not change the length");

    password_reset();
    check(password_length() == 0, "reset empties the buffer");
    check(password_value()[0] == '\0', "reset terminates the buffer");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
````

### `tests/test_image.c`

````c
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
````

### `.github/workflows/main.yml`

````yaml
name: GitHub Actions

on:
  push:
  pull_request:

jobs:
  build:
    name: build and test
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        compiler: [gcc, clang]
    env:
      CC: ${{ matrix.compiler }}

    steps:
    - uses: actions/checkout@v4
    - run: git fetch --prune --unshallow
    - name: fetch or build Docker container
      run: |
        docker build --pull --no-cache --rm -t=i3lock -f ci/Dockerfile .
        docker run -e CC -v $PWD:/usr/src:rw i3lock /bin/sh -c 'git config --global --add safe.directory /usr/src && mkdir build && cd build && CFLAGS="-Wformat -Wformat-security -Wextra -Wno-unused-parameter -Werror" meson .. && ninja && meson test --print-errorlogs'
  formatting:
    name: Check formatting
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v4
    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y clang-format-15
    - name: Check formatting
      run: clang-format-15 --dry-run --Werror $(git ls-files '*.c' 'include/*.h')
````

### `README.md`

````markdown
i3lock - improved screen locker
===============================
[i3lock](https://i3wm.org/i3lock/) is a simple screen locker like slock.
After starting it, you will see a white screen (you can configure the
color/an image). You can return to your screen by entering your password.

Many little improvements have been made to i3lock over time:

- i3lock forks, so you can combine it with an alias to suspend to RAM
  (run "i3lock && echo mem > /sys/power/state" to get a locked screen
   after waking up your computer from suspend to RAM)

- You can specify either a background color or a PNG image which will be
  displayed while your screen is locked. Note that i3lock is not an image
manipulation software. If you need to resize the image to fill the screen
or similar, use existing tooling to do this before passing it to i3lock.

- You can specify whether i3lock should bell upon a wrong password.

- i3lock uses PAM and therefore is compatible with LDAP etc.
  On OpenBSD i3lock uses the bsd_auth(3) framework.

Install
-------

See [the i3lock home page](https://i3wm.org/i3lock/).

Requirements
------------
- pkg-config
- libxcb
- libxcb-util
- libpam-dev
- libcairo-dev
- libxcb-xinerama
- libxcb-randr
- libev
- libx11-dev
- libx11-xcb-dev
- libxkbcommon >= 0.5.0
- libxkbcommon-x11 >= 0.5.0
- libxcb-image
- libxcb-xrm

Running i3lock
-------------

To test i3lock, you can directly run the `i3lock` command. To get out of it,
enter your password and press enter.

For a more permanent setup, we strongly recommend using `xss-lock` so that the
screen is locked *before* your laptop suspends:

```
xss-lock --transfer-sleep-lock -- i3lock --nofork
```

On OpenBSD the `i3lock` binary needs to be setgid `auth` to call the
authentication helpers, e.g. `/usr/libexec/auth/login_passwd`.

Building i3lock
---------------
We recommend you use the provided package from your distribution. Do not build
i3lock unless you have a reason to do so.

First install the dependencies listed in requirements section, then run these
commands (might need to be adapted to your OS):
```
rm -rf build/
mkdir -p build && cd build/

meson setup -Dprefix=/usr
ninja
meson test
```

Upstream
--------
Please submit pull requests to https://github.com/i3/i3lock
````

### `CHANGELOG`

````text
UNRELEASED

 • fix a potential password buffer overflow when appending composed
   characters
 • wipe the complete password buffer, including stale data after
   backspaces
 • keep the screen locked if fork(2) fails while daemonizing
 • fix out-of-bounds access in the Xinerama screen query and avoid a
   server-controlled stack allocation in the RandR query
 • validate raw image dimensions and use overflow-checked arithmetic
 • avoid re-rendering the background image on every keypress
 • harden the build with stack protector, PIE, RELRO and FORTIFY_SOURCE
 • add unit tests for the utility and password modules

2025-10-31 i3lock 2.16

 • fix crash when the user changes the XKB configuration
 • when started on Wayland, display an error and usage
 • switch to clang-format 15 (with InsertBraces)
 • fix -Werror=calloc-transposed-args by swapping calloc args
 • reword: remove "dynamic" TWM
 • update meson setup command in README
 • do not increase failed_attempts beyond 999
 • i3lock.1 man page: fix acute accent
 • declare a development shell in flake.nix
 • fix in_dpi variable checking
 • meson: use explicit_bzero if it is available

2024-03-16 i3lock 2.15

 • unlock indicator: display current keyboard layout when
   user types password (with --show-keyboard-layout flag).

2022-06-21 i3lock 2.14.1

 • unlock indicator: display only caps lock and num lock,
   not all modifiers like shift (which can leak information
   about your password to bystanders)

2022-05-28 i3lock 2.14

 • Change default background color to #a3a3a3
   See https://github.com/i3/i3lock/pull/300 for extensive
   discussion and rationale for this change.
 • Recommend using xss-lock to start i3lock in the README
   and i3lock.1 man page. xss-lock is the best way to ensure
   your screen truly is locked before your computer suspends.
 • Display modifier key warning before unlocking, too,
   not just on failed attempts like before.
 • Switch build system from autotools to meson.

2020-10-27 i3lock 2.13

 • Throw error when trying to start on Wayland
 • Use explicit_bzero() where available, not just on OpenBSD
 • avoid pixmap allocations in the redraw path
 • make --debug output go to stderr
 • unlock_indicator.c: fix build failure against gcc-10
 • fix: call pam_end in cleanup in main, not in event loop
 • set _NET_WM_BYPASS_COMPOSITOR hint to avoid flickering

2019-07-21 i3lock 2.12

 • remove stray \n from error messages
 • capitalize unlock indicator contents
 • set WM_CLASS property
 • reference modifier as “Super”, not “Win”
 • add --raw option to read image as raw bytes

2018-10-18 i3lock 2.11.1

 • Fix dist tarball by including I3LOCK_VERSION

2018-10-10 i3lock 2.11

 • Switch to autotools
 • Display an error when backspace is pressed without any input
 • Print an error when a non-PNG file is opened
   (i3lock only supports PNG files) (Thanks eplanet)
 • Don’t unnecessarily check the xcb_connect return value,
   it is known never to be NULL (Thanks SegFault42)
 • Fix memory leak when grabbing fails (Thanks karulont)
 • Respect Xft.dpi for determining the unlock indicator’s scale factor
 • Discard pending password verification attempts
   when a new password is entered (Thanks layus)

2017-11-25 i3lock 2.10

 • Only use -lpam when not on OpenBSD (Thanks Kaashif)
 • locale: treat empty string same as unset (Thanks Ran)
 • Fix overwrite of getopt optind (Thanks jakob)
 • Immediately hide the unlock indicator after ESC / C-u (Thanks Orestis)
 • Measure wall-clock time instead of CPU time for “locking” indicator.
 • SetInputFocus to the i3lock window to force-close context menus
 • Use RandR for learning about attached monitors

2017-06-21 i3lock 2.9.1

 • Fix version number mechanism (for --version)
 • Revert the fix for composited notifications, as it causes more issues than
   it solves:
   https://github.com/i3/i3lock/issues/130
   https://github.com/i3/i3lock/issues/128

2017-05-26 i3lock 2.9

 • i3lock.1: use signal names without SIG prefix
 • Removed obsolete inactivity timeout
 • Added version files for release tarball.
 • Set font face
 • Automatically unlock (without having to press <Enter>) one attempt which was
   entered while authenticating
 • Stop leaking the image_path dup
 • Displaying locking message when grabbing the pointer/keyboard
 • Display error message when locking failed
 • Add Enter on C-m
 • Change input slices to be exactly pi/3 in size instead of slightly more
 • Fix covering of composited notifications using the XComposite extension
 • Remove last traces of DPMS
 • Use bsd_auth(3) instead of PAM on OpenBSD
 • Restore intended behaviour and don't use mlock(2) on OpenBSD.

2016-06-04 i3lock 2.8

 • Remove DPMS support in favor of a wrapper script and xset(1).
 • Indicate that the --inactivity-timeout option takes an argument. (Thanks
   Kenneth Lyons)
 • fix pam_securetty: set PAM_TTY to getenv("DISPLAY")
 • Eat XKB_KEY_Delete and XKB_KEY_KP_Delete (Thanks bebehei)
 • Show unlock indicator if password was entered during PAM verification
 • Allow CTRL+J as enter and CTRL+H as backspace (Thanks Karl Tarbe)
 • Flush xcb connection after opening fullscreen window (Thanks martin)
 • Add support for `xss-lock --transfer-sleep-lock'

2015-05-20 i3lock 2.7

 • Die when the X11 connection breaks during runtime (Thanks Eduan)
 • Implement logging the number of failed attempts (Thanks koebi)
 • Ignore password validation is pam is in wrong state (Thanks Neodyblue)
 • Get current user with getpwuid() instead of using $ENV{USER} (Thanks Martin)
 • Add support for Compose and dead-keys with libxkbcommon.
   Requires libxkbcommon ≥ 0.5.0 (Thanks Daniel)
 • Format the source using clang-format.
 • Refresh pam credentials on successful authentication (for Kerberos and the
   like) (Thanks James)
 • List pressed modifiers on failed authentication (Thanks Deiz, Alexandre)
 • Only redraw the screen if the unlock indicator is actually used
   (Thanks Ingo)
 • Make pkg-config configurable for cross-compilation (Thanks Nikolay)

2014-07-18 i3lock 2.6

 • NEW DEPENDENCY: use libxkbcommon-x11 instead of libX11
   This helps us get rid of all code that directly uses libX11
 • Use cairo_status_to_string for descriptive errors.
 • Add `-e` option to not validate empty password.
 • Bugfix: update the initial keyboard modifier state (Thanks lee, Ran)
 • Re-raise i3lock when obscured in a separate process
 • Turn on the screen on successful authentication
 • Delay to turn off the screen after wrong passwd
 • Discard half-entered passwd after some inactivity
 • Ignore empty passwd after repeated Enter keypress
 • Scale the unlock indicator (for retina displays)

2013-06-09 i3lock 2.5

 • NEW DEPENDENCY: Use libxkbcommon for input handling
   This makes input handling much better for many edge cases.
 • Bugfix: fix argument parsing on ARM (s/char/int/)
 • Bugfix: free(reply) to avoid memory leak
 • Bugfix: Use ev_loop_fork after fork, fixes forking on kqueue based OSes
 • Bugfix: Fix centering the indicator in the no-xinerama case
 • Only use mlock() on Linux, FreeBSD (for example) requires root
 • promote the "could not load image" message from debug to normal
 • s/pam_message/pam_response/ (Thanks Tucos)
 • remove support for NOLIBCAIRO, cairo-xcb is widespread by now
 • Allow XKB_KEY_XF86ScreenSaver as synonym for enter
   This keysym is generated on convertible tablets by pressing a hardware
   lock/unlock button.
 • Allow passwordless PAM conversations (e.g. fingerprint)
 • Add ctrl+u password reset
 • Set window name to i3lock

2012-06-02 i3lock 2.4.1

 • Bugfix: Correctly center unlock indicator after reconfiguring screens
   (Thanks xeen)
 • Bugfix: Revert shift lock handling (broke uppercase letters)
 • Bugfix: Skip shift when getting the modifier mask (Thanks SardemFF7)

2012-04-01 i3lock 2.4

 • Bugfix: Fix background color when using cairo (Thanks Pascal)
 • Only output text when in debug mode (fixes problems with xautolock)
 • fallback when the image cannot be loaded
 • Use (void) instead of () for functions without args (Thanks fernandotcl)

2012-03-15 i3lock 2.3.1

 • Fix compilation on some systems

2012-03-15 i3lock 2.3

  • Implement a visual unlock indicator
  • Support ISO_Level5_Shift and Caps Lock
  • Lock the password buffer in memory, clear it in RAM after verifying
  • Fork after the window is visible, not before
  • Bugfix: Copy the color depth from parent (root) window instead of
    hardcoding a depth of 24

2011-11-06 i3lock 2.2

  • Don’t re-grab pointer/keyboard on MappingNotify. In some rare situations,
    this lead to some keypresses "slipping through" to the last focused window.
  • Correctly handle Mode_switch/ISO_Level3_Shift
  • Render to a pixmap which is used as background for the window instead of
    copying contents on every expose event
  • Handle screen resolution changes while screen is locked
  • Manpage: document arguments for every option

2011-05-13 i3lock 2.1

  • Accept return/backspace when the buffer of 512 bytes is full
  • Handle numpad keys correctly
  • Handle MappingNotify events
  • Correctly check for errors when connecting to X11
  • Add i3lock.pam to not rely on debian’s /etc/pam.d/other anymore
  • don’t display debug output
  • add NOLIBCAIRO flag to permit compilation without cairo

2010-09-05 i3lock 2.0

  • Complete rewrite of i3lock. Now using xcb instead of Xlib.
  • When a window obscures i3lock, it pushes itself back to the top again.
  • Display version when starting with -v

2009-08-02 i3lock 1.1

  • Implement background pictures (-i) and colors (-c)

2009-05-10 i3lock 1.0

  • Implement PAM support
  • Implement options for forking, beeping, DPMS

2009-05-01 i3lock 0.9

  • First release, forked from slock 0.9
````

### `i3lock.1`

````text
.de Vb \" Begin verbatim text
.ft CW
.nf
.ne \\$1
..
.de Ve \" End verbatim text
.ft R
.fi
..

.TH i3lock 1 "JANUARY 2012" Linux "User Manuals"

.SH NAME
i3lock \- improved screen locker

.SH SYNOPSIS
.B i3lock
.RB [\|\-v\|]
.RB [\|\-n\|]
.RB [\|\-b\|]
.RB [\|\-i
.IR image.png \|]
.RB [\|\-c
.IR color \|]
.RB [\|\-t\|]
.RB [\|\-p
.IR pointer\|]
.RB [\|\-u\|]
.RB [\|\-e\|]
.RB [\|\-f\|]

.SH RECOMMENDED USAGE
.RB xss-lock
.RB --transfer-sleep-lock
.RB --
.RB i3lock
.RB --nofork

Using
.B xss-lock
ensures that your screen is locked before your laptop suspends.

Notably, using a systemd service file is not adequate, as it will not delay
suspend until your screen is locked.

.SH DESCRIPTION
.B i3lock
is a simple screen locker like slock. After starting it, you will see a white
screen (you can configure the color/an image). You can return to your screen by
entering your password.

.SH IMPROVEMENTS

.IP \[bu] 2
i3lock forks, so you can combine it with an alias to suspend to RAM (run "i3lock && echo mem > /sys/power/state" to get a locked screen after waking up your computer from suspend to RAM)
.IP \[bu]
You can specify either a background color or a PNG image which will be displayed while your screen is locked.
.IP \[bu]
You can specify whether i3lock should bell upon a wrong password.
.IP \[bu]
i3lock uses PAM and therefore is compatible with LDAP, etc.


.SH OPTIONS
.TP
.B \-v, \-\-version
Display the version of your
.B i3lock

.TP
.B \-n, \-\-nofork
Don't fork after starting.

.TP
.B \-b, \-\-beep
Enable beeping. Be sure to not do this when you are about to annoy other people,
like when opening your laptop in a boring lecture.

.TP
.B \-u, \-\-no-unlock-indicator
Disable the unlock indicator. i3lock will by default show an unlock indicator
after pressing keys. This will give feedback for every keypress and it will
show you the current PAM state (whether your password is currently being
verified or whether it is wrong).

.TP
.BI \-i\  path \fR,\ \fB\-\-image= path
Display the given PNG image instead of a blank screen.

.TP
.BI \fB\-\-raw= format
Read the image given by \-\-image as a raw image instead of PNG. The argument is the image's format
as <width>x<height>:<pixfmt>. The supported pixel formats are:
\(aqnative', \(aqrgb', \(aqxrgb', \(aqrgbx', \(aqbgr', \(aqxbgr', and \(aqbgrx'.
The "native" pixel format expects a pixel as a 32-bit (4-byte) integer in
the machine's native endianness, with the upper 8 bits unused. Red, green and blue are stored in
the remaining bits, in that order.

.BR Example:
.Vb 6
\&	--raw=1920x1080:rgb
.Ve

.BR
You can use ImageMagick’s
.IR convert(1)
program to feed raw images into i3lock:

.BR
.Vb 6
\&	convert wallpaper.jpg RGB:- | i3lock --raw 3840x2160:rgb --image /dev/stdin
.Ve

This allows you to load a variety of image formats without i3lock having to
support each one explicitly.

.TP
.BI \-c\  rrggbb \fR,\ \fB\-\-color= rrggbb
Turn the screen into the given color instead of white. Color must be given in 3-byte
format: rrggbb (i.e. ff0000 is red).

.TP
.B \-t, \-\-tiling
If an image is specified (via \-i) it will display the image tiled all over the screen
(if it is a multi-monitor setup, the image is visible on all screens).

.TP
.BI \-p\  win|default \fR,\ \fB\-\-pointer= win|default
If you specify "default",
.B i3lock
does not hide your mouse pointer. If you specify "win",
.B i3lock
displays a hardcoded Windows-Pointer (thus enabling you to mess with your
friends by using a screenshot of a Windows desktop as a locking-screen).

.TP
.B \-e, \-\-ignore-empty-password
When an empty password is provided by the user, do not validate
it. Without this option, the empty password will be provided to PAM
and, if invalid, the user will have to wait a few seconds before
another try. This can be useful if the XF86ScreenSaver key is used to
put a laptop to sleep and bounce on resume or if you happen to wake up
your computer with the enter key.

.TP
.B \-f, \-\-show-failed-attempts
Show the number of failed attempts, if any.

.TP
.B \-k, \-\-show-keyboard-layout
Show the current keyboard layout.

.TP
.B \-\-debug
Enables debug logging.

.SH DPMS

The \-d (\-\-dpms) option was removed from i3lock in version 2.8. There were
plenty of use-cases that were not properly addressed, and plenty of bugs
surrounding that feature. While features are not normally removed from i3 and
its tools, we felt the need to make an exception in this case.

Users who wish to explicitly enable DPMS only when their screen is locked can
use a wrapper script around i3lock like the following:

.Vb 6
\&	#!/bin/sh
\&	revert() {
\&	  xset dpms 0 0 0
\&	}
\&	trap revert HUP INT TERM
\&	xset +dpms dpms 5 5 5
\&	i3lock -n
\&	revert
.Ve

The \-I (-\-inactivity-timeout=seconds) was removed because it only makes sense with DPMS.

.SH SEE ALSO
.IR xss-lock(1)
\- hooks up i3lock to the systemd login manager

.IR convert(1)
\- feed a wide variety of image formats to i3lock

.SH AUTHOR
Michael Stapelberg <michael+i3lock at stapelberg dot de>
````
