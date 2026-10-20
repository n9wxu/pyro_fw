/*
 * A JSON reader for the host tests, strict enough to reject what a browser
 * rejects: assert_json() fails the test unless the text is one JSON value.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JSON_CHECK_H
#define JSON_CHECK_H

#include "unity.h"
#include <stdbool.h>
#include <string.h>

static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
        p++;
    }
    return p;
}

static const char *value(const char *p);

static const char *string(const char *p) {
    if (*p != '"') {
        return NULL;
    }
    for (p++; *p != '"'; p++) {
        if ((unsigned char)*p < 0x20) {
            return NULL;
        }
        if (*p == '\\') {
            p++;
            if (!strchr("\"\\/bfnrtu", *p)) {
                return NULL;
            }
        }
    }
    return p + 1;
}

static const char *number(const char *p) {
    const char *s = p;
    if (*p == '-') {
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    return p > s && p[-1] != '-' ? p : NULL;
}

static const char *container(const char *p, char close, bool keyed) {
    p = ws(p + 1);
    if (*p == close) {
        return p + 1;
    }
    for (;;) {
        if (keyed) {
            p = string(ws(p));
            if (!p || *(p = ws(p)) != ':') {
                return NULL;
            }
            p++;
        }
        p = value(ws(p));
        if (!p) {
            return NULL;
        }
        p = ws(p);
        if (*p == close) {
            return p + 1;
        }
        if (*p != ',') {
            return NULL;
        }
        p++;
    }
}

static const char *value(const char *p) {
    if (*p == '{') {
        return container(p, '}', true);
    }
    if (*p == '[') {
        return container(p, ']', false);
    }
    if (*p == '"') {
        return string(p);
    }
    if (strncmp(p, "true", 4) == 0) {
        return p + 4;
    }
    if (strncmp(p, "false", 5) == 0) {
        return p + 5;
    }
    if (strncmp(p, "null", 4) == 0) {
        return p + 4;
    }
    return number(p);
}

static void assert_json(const char *text) {
    const char *end = value(ws(text));
    TEST_ASSERT_NOT_NULL_MESSAGE(end, "not JSON");
    TEST_ASSERT_EQUAL_CHAR_MESSAGE('\0', *ws(end), "text after the JSON value");
}

#endif
