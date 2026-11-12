/*
 * The one tokenizer for config.ini, pins.ini and beep.ini.
 *
 * A line is key=value, ended by CR+LF, LF or the end of the text [CFG-09].
 * Spaces and tabs around the key and the value are not part of either. Blank
 * lines, [section] headers and lines starting ';' or '#' carry nothing, and
 * neither does a line with no '=' or an empty key. Each pair goes to fn in
 * file order; what a key means, and whether it is known [CFG-08], is fn's.
 *
 * Header-only, so each user links it without a build change. Mutates buf.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef INI_TOKENIZER_H
#define INI_TOKENIZER_H

#include <stdbool.h>
#include <string.h>

typedef void (*ini_pair_fn)(const char *key, const char *val, void *ctx);

static inline bool ini_is_blank(char c) {
    return (c == ' ') || (c == '\t');
}

/* s with its leading and trailing blanks cut, in place. */
static inline char *ini_trim(char *s) {
    size_t start = 0u;
    while (ini_is_blank(s[start])) {
        start++;
    }
    char *text = &s[start];
    size_t len = strlen(text);
    while ((len > 0u) && ini_is_blank(text[len - 1u])) {
        len--;
        text[len] = '\0';
    }
    return text;
}

static inline void ini_for_each(char *buf, ini_pair_fn fn, void *ctx) {
    char *line = buf;
    while (*line != '\0') {
        size_t eol = 0u;
        while ((line[eol] != '\0') && (line[eol] != '\r') && (line[eol] != '\n')) {
            eol++;
        }
        size_t next = eol;
        while ((line[next] == '\r') || (line[next] == '\n')) {
            next++;
        }
        line[eol] = '\0';

        char *text = ini_trim(line);
        char *eq = strchr(text, '=');
        if ((eq != NULL) && (text[0] != '[') && (text[0] != ';') && (text[0] != '#')) {
            *eq = '\0';
            char *key = ini_trim(text);
            if (key[0] != '\0') {
                fn(key, ini_trim(&eq[1]), ctx);
            }
        }
        line = &line[next];
    }
}

#endif
