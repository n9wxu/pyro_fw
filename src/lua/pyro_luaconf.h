/*
 * Lua build configuration overrides, injected via LUA_USER_H.
 *
 * luaconf.h defines LUA_32BITS unconditionally, so a -D on the command line
 * collides with it and loses. This header is included at the END of
 * luaconf.h, which is the hook upstream provides for exactly this.
 *
 * 32-bit numbers halve the size of every Lua value, which matters against a
 * fixed arena, and keep the simulator numerically identical to the target
 * rather than subtly more precise.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_LUACONF_H
#define PYRO_LUACONF_H

#undef LUA_32BITS
#define LUA_32BITS 1

/* No C library locale, no dynamic loading, no popen. */
#undef LUA_USE_C89

#endif
