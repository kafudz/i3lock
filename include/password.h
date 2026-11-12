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
