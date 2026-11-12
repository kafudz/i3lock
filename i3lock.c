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
