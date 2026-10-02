/*
 * The POST routes.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HTTP_ROUTES_H
#define HTTP_ROUTES_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    R_NONE,
    R_FILE,
    R_UPLOAD,
    R_OTA,
    R_CONFIG,
    R_PINS,
    R_BEEPS,
    R_SERIAL,
    R_ERASE,
    R_BEEP_PLAY,
    R_LUA_CHECK,
    R_STATUS,
    R_FLOG,
} route_t;

typedef struct {
    const char *path;
    bool prefix;
    route_t route;
    uint32_t gather;
    bool flash;
    bool fs;
} post_route_t;

static const post_route_t post_routes[] = {
    {"/api/ota", false, R_OTA, 0, true, false},
    {"/www/", true, R_UPLOAD, 0, true, true},
#if PYRO_HAS_LUA
    {"/api/lua/script", false, R_UPLOAD, 0, true, true},
    {"/api/lua/check", false, R_LUA_CHECK, 2047, false, false},
#endif
    {"/api/serial", false, R_SERIAL, 12, true, true},
    {"/api/config", false, R_CONFIG, 511, true, true},
    {"/api/beeps/play", false, R_BEEP_PLAY, 63, false, false},
    {"/api/beeps", false, R_BEEPS, BEEP_STORE_MAX - 1, true, true},
    {"/api/pins", false, R_PINS, PIN_STORE_MAX - 1, true, true},
};

static inline const post_route_t *find_post_route(const char *path) {
    for (unsigned i = 0; i < sizeof(post_routes) / sizeof(post_routes[0]); i++) {
        const post_route_t *r = &post_routes[i];
        if (r->prefix ? strncmp(path, r->path, strlen(r->path)) == 0 : strcmp(path, r->path) == 0) {
            return r;
        }
    }
    return NULL;
}

#endif
