/*
 * The server's routes, and the POST table. Data and a lookup only, so the
 * host tests what each route is promised.
 *
 * The includer defines PYRO_HAS_LUA, PYRO_HAS_SD, PYRO_HAS_BENCH_FLIGHT,
 * BEEP_STORE_MAX and PIN_STORE_MAX.
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
    R_FILE,   /* GET of a file, streamed by fill() */
    R_STATUS, /* answered by HTTP_UNIT_STATUS */
    R_FLOG,   /* the binary flight log, rendered as CSV by fill() */
    /* POSTs with a body */
    R_UPLOAD, /* a file: /www/..., the Lua program */
    R_OTA,
    R_CONFIG,
    R_PINS,
    R_BEEPS,
    R_SERIAL,
    R_BEEP_PLAY,
    R_LUA_CHECK,
    /* POSTs that act on the path alone */
    R_ERASE,
    R_REBOOT,
    R_TEST_MODE_ON,
    R_TEST_MODE_OFF,
    R_SIM_FLIGHT,
    R_SIM_STOP,
    R_SD_BENCH,
    R_SD_INIT,
    R_SD_IDLE,
    R_HR_START,
    R_HR_STOP,
} route_t;

typedef enum {
    MATCH_EXACT,
    MATCH_PREFIX, /* the path begins with it */
    MATCH_QUERY,  /* exactly it, or it followed by '?' and a query */
} route_match_t;

typedef enum {
    BODY_NONE,   /* acts on the path; any body is discarded */
    BODY_STREAM, /* handed to on_body as it arrives */
    BODY_GATHER, /* gathered whole into the work buffer, at most `gather` bytes */
} route_body_t;

typedef struct {
    const char *path;
    route_match_t match;
    route_t route;
    route_body_t body;
    uint32_t gather;
    /* [WEB-API-08] It touches the filesystem or the card, so it is refused
     * while the flight log holds them. */
    bool fs;
} post_route_t;

static const post_route_t post_routes[] = {
    {"/api/ota", MATCH_EXACT, R_OTA, BODY_STREAM, 0, false},
    {"/www/", MATCH_PREFIX, R_UPLOAD, BODY_STREAM, 0, true},
#if PYRO_HAS_LUA
    {"/api/lua/script", MATCH_EXACT, R_UPLOAD, BODY_STREAM, 0, true},
    {"/api/lua/check", MATCH_EXACT, R_LUA_CHECK, BODY_GATHER, 2047, false},
#endif
    {"/api/serial", MATCH_EXACT, R_SERIAL, BODY_GATHER, 12, true},
    {"/api/config", MATCH_EXACT, R_CONFIG, BODY_GATHER, 511, true},
    {"/api/beeps/play", MATCH_EXACT, R_BEEP_PLAY, BODY_GATHER, 63, false},
    {"/api/beeps", MATCH_EXACT, R_BEEPS, BODY_GATHER, BEEP_STORE_MAX - 1, true},
    {"/api/pins", MATCH_EXACT, R_PINS, BODY_GATHER, PIN_STORE_MAX - 1, true},
    {"/api/flight/erase", MATCH_EXACT, R_ERASE, BODY_NONE, 0, true},
    {"/api/reboot", MATCH_EXACT, R_REBOOT, BODY_NONE, 0, false},
    {"/api/test_mode/on", MATCH_EXACT, R_TEST_MODE_ON, BODY_NONE, 0, false},
    {"/api/test_mode/off", MATCH_EXACT, R_TEST_MODE_OFF, BODY_NONE, 0, false},
#if PYRO_HAS_BENCH_FLIGHT
    {"/api/sim/flight", MATCH_QUERY, R_SIM_FLIGHT, BODY_NONE, 0, false},
    {"/api/sim/stop", MATCH_EXACT, R_SIM_STOP, BODY_NONE, 0, false},
#endif
#if PYRO_HAS_SD
    {"/api/sd/bench", MATCH_QUERY, R_SD_BENCH, BODY_NONE, 0, true},
    {"/api/sd/init", MATCH_QUERY, R_SD_INIT, BODY_NONE, 0, true},
    {"/api/sd/idle", MATCH_QUERY, R_SD_IDLE, BODY_NONE, 0, true},
    {"/api/hr/start", MATCH_QUERY, R_HR_START, BODY_NONE, 0, true},
    {"/api/hr/stop", MATCH_EXACT, R_HR_STOP, BODY_NONE, 0, false},
#endif
};

static inline bool route_matches(const post_route_t *r, const char *path) {
    size_t n = strlen(r->path);
    switch (r->match) {
    case MATCH_PREFIX:
        return strncmp(path, r->path, n) == 0;
    case MATCH_QUERY:
        return strncmp(path, r->path, n) == 0 && (path[n] == '\0' || path[n] == '?');
    default:
        return strcmp(path, r->path) == 0;
    }
}

static inline const post_route_t *find_post_route(const char *path) {
    for (unsigned i = 0; i < sizeof(post_routes) / sizeof(post_routes[0]); i++) {
        if (route_matches(&post_routes[i], path)) {
            return &post_routes[i];
        }
    }
    return NULL;
}

#endif
