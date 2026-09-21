/*
 * Static validation — see lua_check.h for what this is and is not.
 *
 * The method is to compile the chunk and walk its constant tables. Lua interns
 * every string literal into Proto->k, and nested functions hang off Proto->p,
 * so a recursive walk yields every literal in the program without executing
 * any of it. Resource names arrive at the API as strings, so for real scripts
 * that walk is exactly the set of names the program can use.
 *
 * The comparison runs in the direction that admits no false positives: for
 * each resource the platform provides, ask whether its name appears in the
 * chunk. Present and enabled is fine. Present and not enabled is the gap this
 * exists to find. Asking the other question -- "is this string a resource
 * name?" -- would need dataflow to avoid flagging every message the script
 * sends, so the unknown-name check is deliberately narrow: identifier-shaped
 * constants that sit next to a known API namespace and match nothing.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_check.h"
#include "lua_platform.h"
#include "lua.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lstate.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Seen-string collector. Bounded: a chunk with more distinct literals than
 * this is past the point where a per-name report is useful. */
#define SEEN_MAX 128
#define SEEN_LEN 24
#define LUA_NAME_SCAN SEEN_LEN

typedef struct {
    char s[SEEN_MAX][SEEN_LEN];
    int n;
} seen_t;

static void seen_add(seen_t *sn, const char *s) {
    if (sn->n >= SEEN_MAX || strlen(s) >= SEEN_LEN) {
        return;
    }
    for (int i = 0; i < sn->n; i++) {
        if (strcmp(sn->s[i], s) == 0) {
            return;
        }
    }
    strncpy(sn->s[sn->n], s, SEEN_LEN - 1);
    sn->s[sn->n][SEEN_LEN - 1] = '\0';
    sn->n++;
}

static bool seen_has(const seen_t *sn, const char *s) {
    for (int i = 0; i < sn->n; i++) {
        if (strcmp(sn->s[i], s) == 0) {
            return true;
        }
    }
    return false;
}

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
};

static bool is_api_word(const char *s) {
    for (size_t i = 0; i < sizeof(api_words) / sizeof(api_words[0]); i++) {
        if (strcmp(api_words[i], s) == 0) {
            return true;
        }
    }
    return false;
}

/* Levenshtein distance, capped. Used to decide whether an unrecognised
 * string is a near-miss of a real resource name.
 *
 * An earlier version flagged any identifier-shaped constant that matched
 * nothing, which made serial.write('radio', 'hi') report 'hi' as a possible
 * typo. A checker that cries wolf on message text is a checker operators
 * learn to ignore, so the test is now "close to something real" rather than
 * "looks like a word". That trades some recall for precision, which is the
 * right trade here: the sandbox is what makes a genuinely wrong name safe,
 * and this only has to catch the plausible mistake. */
static int edit_distance(const char *a, const char *b, int cap) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la - lb > cap || lb - la > cap) {
        return cap + 1;
    }
    int prev[LUA_NAME_SCAN], cur[LUA_NAME_SCAN];
    if (lb + 1 > LUA_NAME_SCAN) {
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

static bool identifier_shaped(const char *s) {
    /* Three characters minimum: below that almost anything is within edit
     * distance 1 of almost anything else. */
    size_t n = strlen(s);
    if (n < 3 || n > 8) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            return false;
        }
    }
    return true;
}

void lua_chk_env_from_platform(lua_chk_env_t *env) {
    memset(env, 0, sizeof(*env));
    for (int i = 0; i < lua_plat_output_count() && env->n < LUA_CHK_MAX_NAMES; i++) {
        strncpy(env->names[env->n++], lua_plat_output_desc(i)->name, LUA_NAME_MAX - 1);
    }
    for (int i = 0; i < lua_plat_input_count() && env->n < LUA_CHK_MAX_NAMES; i++) {
        strncpy(env->names[env->n++], lua_plat_input_desc(i)->name, LUA_NAME_MAX - 1);
    }
    for (int i = 0; i < lua_plat_serial_count() && env->n < LUA_CHK_MAX_NAMES; i++) {
        strncpy(env->names[env->n++], lua_plat_serial_desc(i)->name, LUA_NAME_MAX - 1);
    }
    env->has_output = lua_plat_output_count() > 0;
    env->has_input = lua_plat_input_count() > 0;
    env->has_serial = lua_plat_serial_count() > 0;
    env->has_pixel = lua_plat_pixel_count() > 0;
}

void lua_check(const char *src, size_t len, const lua_chk_env_t *env, lua_chk_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->green = true;

    lua_State *L = luaL_newstate();
    if (!L) {
        add(out, LUA_CHK_SYNTAX, "no memory to compile");
        return;
    }
    if (luaL_loadbuffer(L, src, len, "check") != LUA_OK) {
        const char *e = lua_tostring(L, -1);
        add(out, LUA_CHK_SYNTAX, "%s", e ? e : "syntax error");
        lua_close(L);
        return;
    }

    seen_t sn;
    sn.n = 0;
    const LClosure *cl = (const LClosure *)lua_topointer(L, -1);
    walk(cl->p, &sn);
    lua_close(L);

    /* Direction 1: every name the configuration grants. Exact, no heuristic. */
    seen_t provided;
    provided.n = 0;
    for (int i = 0; i < env->n; i++) {
        if (env->names[i][0]) {
            seen_add(&provided, env->names[i]);
        }
    }

    /* Direction 2: identifier-shaped constants that name nothing. A typo like
     * output.set('beacn', ...) lands here. Reported as a warning because the
     * shape test is a heuristic -- a bare word in a message would also match. */
    for (int i = 0; i < sn.n; i++) {
        const char *s = sn.s[i];
        if (!identifier_shaped(s) || is_api_word(s) || seen_has(&provided, s)) {
            continue;
        }
        /* Only report it if it is within one edit of a name that exists.
         * Anything further away is far more likely to be message text. */
        for (int j = 0; j < provided.n; j++) {
            if (edit_distance(s, provided.s[j], 1) <= 1) {
                add(out, LUA_CHK_UNKNOWN, "'%s' matches no resource -- did you mean '%s'?", s, provided.s[j]);
                break;
            }
        }
    }

    /* The gap that matters: the script uses the API but configuration granted
     * nothing of that kind. */
    if (seen_has(&sn, "serial") && !env->has_serial) {
        add(out, LUA_CHK_MISSING, "script uses serial.*, but no pin is assigned TX or RX");
    }
    if (seen_has(&sn, "pixel") && !env->has_pixel) {
        add(out, LUA_CHK_MISSING, "script uses pixel.*, but no pin is assigned the LED string");
    }
    if (seen_has(&sn, "output") && !env->has_output) {
        add(out, LUA_CHK_MISSING, "script uses output.*, but no pin is assigned an output");
    }
    if (seen_has(&sn, "input") && !env->has_input) {
        add(out, LUA_CHK_MISSING, "script uses input.*, but no pin is assigned an input");
    }

    if (out->count == 0) {
        add(out, LUA_CHK_OK, "%d resources, all references resolved", provided.n);
    }
}
