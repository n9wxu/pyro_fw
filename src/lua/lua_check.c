/*
 * Static validation -- see lua_check.h for what this is and is not.
 *
 * The method is to compile the chunk and walk its constant tables. Lua interns
 * every string literal into Proto->k, and nested functions hang off Proto->p,
 * so a recursive walk yields every literal in the program without executing
 * any of it. Resource names arrive at the API as strings, so for real scripts
 * that walk is the set of names the program can use.
 *
 * The comparison runs in the direction that admits no false positives: for
 * each resource the configuration provides, ask whether its name appears in
 * the chunk. The other question -- "is this string a resource name?" -- would
 * need dataflow to avoid flagging every message the script sends, so the
 * unknown-name check is deliberately narrow: a near miss of a real name.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_check.h"
#include "lua_arena.h"
#include "lua_platform.h"
#include "lua.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lstate.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* What lua_check() compiles in: less than the VM's 32 kB arena, which must
 * also hold the environment and the running program. A script too big for
 * it is reported as "not enough memory". Half as much again on a 64-bit
 * host, as for the VM's arena (pyro_lua.c). */
#ifndef LUA_CHECK_SCRATCH_BYTES
#define LUA_CHECK_SCRATCH_BYTES (16 * 1024 * (sizeof(void *) == 8 ? 3 : 2) / 2)
#endif

/* Distinct literals remembered. Beyond this a per-name report stops being
 * useful; longer strings cannot be names (LUA_NAME_MAX) or API words. */
#define SEEN_MAX 128
#define SEEN_LEN 12

typedef struct {
    char s[SEEN_MAX][SEEN_LEN];
    int n;
} seen_t;

#define SEEN_BYTES ((sizeof(seen_t) + 7u) & ~(size_t)7u)

static void seen_add(seen_t *sn, const char *s) {
    if (sn->n >= SEEN_MAX || strlen(s) >= SEEN_LEN) {
        return;
    }
    for (int i = 0; i < sn->n; i++) {
        if (strcmp(sn->s[i], s) == 0) {
            return;
        }
    }
    strcpy(sn->s[sn->n++], s);
}

static bool seen_has(const seen_t *sn, const char *s) {
    for (int i = 0; i < sn->n; i++) {
        if (strcmp(sn->s[i], s) == 0) {
            return true;
        }
    }
    return false;
}

/* Recursion is bounded by the parser's own nesting limit (LUAI_MAXCCALLS). */
static void walk(const Proto *p, seen_t *sn) {
    for (int i = 0; i < p->sizek; i++) {
        const TValue *v = &p->k[i];
        if (ttisstring(v)) {
            seen_add(sn, getstr(tsvalue(v)));
        }
    }
    for (int i = 0; i < p->sizep; i++) {
        walk(p->p[i], sn);
    }
}

static void add(lua_chk_result_t *r, lua_chk_kind_t kind, const char *fmt, ...) {
    if (r->count >= LUA_CHK_MAX) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->items[r->count].detail, sizeof(r->items[0].detail), fmt, ap);
    va_end(ap);
    r->items[r->count].kind = kind;
    r->count++;
    if (kind == LUA_CHK_SYNTAX || kind == LUA_CHK_MISSING) {
        r->green = false;
    }
}

/* Names the API itself uses, plus the namespaces. A constant matching one of
 * these is structure, not a resource reference. */
static const char *api_words[] = {
    "output",   "input",  "serial", "pixel", "sensor", "flight",   "pyro",  "set",    "get",    "write",
    "read",     "show",   "fill",   "clear", "count",  "state",    "alt",   "speed",  "maxalt", "pressure",
    "time",     "status", "print",  "tick",  "init",   "on_event", "math",  "string", "table",  "tostring",
    "tonumber", "pairs",  "ipairs", "type",  "select", "error",    "pcall", "assert", "len",    "format",
    "log",      "line",   "adc",    "seq",   "apogee", "thrust",
};

static bool is_api_word(const char *s) {
    for (size_t i = 0; i < sizeof(api_words) / sizeof(api_words[0]); i++) {
        if (strcmp(api_words[i], s) == 0) {
            return true;
        }
    }
    return false;
}

/* Levenshtein distance, capped. "Close to something real" rather than "looks
 * like a word": flagging every identifier-shaped constant would report the
 * 'hi' in serial.write('radio', 'hi'), and a checker that cries wolf on
 * message text is one operators learn to ignore. */
static int edit_distance(const char *a, const char *b, int cap) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la - lb > cap || lb - la > cap) {
        return cap + 1;
    }
    int prev[SEEN_LEN], cur[SEEN_LEN];
    if (lb + 1 > SEEN_LEN) {
        return cap + 1;
    }
    for (int j = 0; j <= lb; j++) {
        prev[j] = j;
    }
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int m = prev[j] + 1;
            if (cur[j - 1] + 1 < m) {
                m = cur[j - 1] + 1;
            }
            if (prev[j - 1] + cost < m) {
                m = prev[j - 1] + cost;
            }
            cur[j] = m;
        }
        for (int j = 0; j <= lb; j++) {
            prev[j] = cur[j];
        }
    }
    return prev[lb];
}

/* Three characters minimum: below that almost anything is within edit
 * distance 1 of almost anything else. */
static bool identifier_shaped(const char *s) {
    size_t n = strlen(s);
    if (n < 3 || n > LUA_NAME_MAX - 1) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            return false;
        }
    }
    return true;
}

static bool provides(const lua_chk_env_t *env, const char *s) {
    for (int i = 0; i < env->n; i++) {
        if (env->names[i][0] && strcmp(env->names[i], s) == 0) {
            return true;
        }
    }
    return false;
}

void lua_chk_env_from_platform(lua_chk_env_t *env) {
    memset(env, 0, sizeof(*env));
    static const lua_iface_kind_t named[] = {LUA_IF_OUTPUT, LUA_IF_INPUT, LUA_IF_SERIAL};
    for (unsigned k = 0; k < sizeof(named) / sizeof(named[0]); k++) {
        for (int i = 0; i < lua_iface_count_kind(named[k]) && env->n < LUA_CHK_MAX_NAMES; i++) {
            strncpy(env->names[env->n++], lua_iface_nth_of_kind(named[k], i)->name, LUA_NAME_MAX - 1);
        }
    }
    env->has_output = lua_iface_count_kind(LUA_IF_OUTPUT) > 0;
    env->has_input = lua_iface_count_kind(LUA_IF_INPUT) > 0;
    env->has_serial = lua_iface_count_kind(LUA_IF_SERIAL) > 0;
    env->has_pixel = lua_iface_count_kind(LUA_IF_PIXEL) > 0;
}

/* Compiles into sn. Returns false, with the reason added, if it did not. */
static bool collect_literals(void *mem, size_t mem_len, const char *src, size_t len, seen_t *sn,
                             lua_chk_result_t *out) {
    lua_arena_t arena;
    lua_arena_init(&arena, mem, mem_len);
    lua_State *L = lua_newstate(lua_arena_alloc, &arena);
    if (!L) {
        add(out, LUA_CHK_SYNTAX, "no memory to compile");
        return false;
    }
    if (luaL_loadbufferx(L, src, len, "check", "t") != LUA_OK) {
        const char *e = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
        add(out, LUA_CHK_SYNTAX, "%s", e ? e : "syntax error");
        lua_close(L);
        return false;
    }
    const LClosure *cl = (const LClosure *)lua_topointer(L, -1);
    walk(cl->p, sn);
    lua_close(L);
    return true;
}

void lua_check_in(void *mem, size_t mem_len, const char *src, size_t len, const lua_chk_env_t *env,
                  lua_chk_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->green = true;

    if (mem_len <= SEEN_BYTES) {
        add(out, LUA_CHK_SYNTAX, "no memory to compile");
        return;
    }
    seen_t *sn = mem;
    sn->n = 0;
    if (!collect_literals((uint8_t *)mem + SEEN_BYTES, mem_len - SEEN_BYTES, src, len, sn, out)) {
        return;
    }

    /* Identifier-shaped constants within one edit of a real name: a typo like
     * output.set('beacn', ...). A warning, since a bare word in a message
     * could match too. */
    for (int i = 0; i < sn->n; i++) {
        const char *s = sn->s[i];
        if (!identifier_shaped(s) || is_api_word(s) || provides(env, s)) {
            continue;
        }
        for (int j = 0; j < env->n; j++) {
            if (env->names[j][0] && edit_distance(s, env->names[j], 1) <= 1) {
                add(out, LUA_CHK_UNKNOWN, "'%s' matches no resource -- did you mean '%s'?", s, env->names[j]);
                break;
            }
        }
    }

    /* The gap that matters: the script uses the API but configuration granted
     * nothing of that kind. */
    if (seen_has(sn, "serial") && !env->has_serial) {
        add(out, LUA_CHK_MISSING, "script uses serial.*, but no pin is assigned TX or RX");
    }
    if (seen_has(sn, "pixel") && !env->has_pixel) {
        add(out, LUA_CHK_MISSING, "script uses pixel.*, but no pin is assigned the LED string");
    }
    if (seen_has(sn, "output") && !env->has_output) {
        add(out, LUA_CHK_MISSING, "script uses output.*, but no pin is assigned an output");
    }
    if (seen_has(sn, "input") && !env->has_input) {
        add(out, LUA_CHK_MISSING, "script uses input.*, but no pin is assigned an input");
    }

    if (out->count == 0) {
        int named = 0;
        for (int i = 0; i < env->n; i++) {
            named += env->names[i][0] ? 1 : 0;
        }
        add(out, LUA_CHK_OK, "%d resources, all references resolved", named);
    }
}

void lua_check(const char *src, size_t len, const lua_chk_env_t *env, lua_chk_result_t *out) {
    static uint8_t scratch[LUA_CHECK_SCRATCH_BYTES] __attribute__((aligned(8)));
    lua_check_in(scratch, sizeof(scratch), src, len, env, out);
}
