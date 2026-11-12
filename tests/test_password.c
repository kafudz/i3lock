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
