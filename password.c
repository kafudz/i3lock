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
