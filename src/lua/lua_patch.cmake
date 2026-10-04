# PATCH_COMMAND for the Lua 5.4 sources; runs in their directory. Idempotent,
# so a re-run over an already patched tree changes nothing.
#
# luaconf.h: LUA_32BITS 1. luaconf.h defines it unconditionally, so neither a
# -D nor LUA_USER_H (included at the end of lua.h, after the type selection)
# can set it.
#
# lstrlib.c: a luai_matchstep(L) call at the top of match(), so the matcher's
# work can be charged to the instruction budget (pyro_luaconf.h). Defaults to
# nothing when LUA_USER_H does not define it.

function(patch_file file pattern replacement done_marker)
    file(READ ${file} text)
    if(text MATCHES "${done_marker}")
        return()
    endif()
    string(REGEX REPLACE "${pattern}" "${replacement}" patched "${text}")
    if(NOT patched MATCHES "${done_marker}")
        message(FATAL_ERROR "${file}: pattern not found, Lua version changed? (${pattern})")
    endif()
    file(WRITE ${file} "${patched}")
endfunction()

patch_file(luaconf.h "#define LUA_32BITS[ \t]+0" "#define LUA_32BITS\t1" "#define LUA_32BITS[ \t]+1")

patch_file(lstrlib.c
    "static const char \\*match \\(MatchState \\*ms, const char \\*s, const char \\*p\\) {\n"
    "#if !defined(luai_matchstep)\n#define luai_matchstep(L) ((void)0)\n#endif\nstatic const char *match (MatchState *ms, const char *s, const char *p) {\n  luai_matchstep(ms->L);\n"
    "luai_matchstep\\(ms->L\\);")
