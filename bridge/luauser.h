/*
 * Build configuration for the embedded Lua (third_party/lua), included at
 * the end of lua.h through -DLUA_USER_H. Lua's own files stay unmodified.
 */
#ifndef WINE_ATSPI_LUAUSER_H
#define WINE_ATSPI_LUAUSER_H

/*
 * Wine's msvcrt <signal.h> has no sig_atomic_t. Lua only needs an integer
 * that a hook may set; the bridge sets no Lua hooks from signals.
 */
#define l_signalT int

/*
 * Lua raises errors with setjmp/longjmp. In a Winelib module these come
 * from Wine's msvcrt, whose x86_64 longjmp unwinds the stack with SEH
 * (_setjmpex with a frame address), which need not work across ELF
 * frames. GCC's builtins only restore registers, as Lua expects. The two
 * are never in the same function (luaD_throw, luaD_rawrunprotected).
 */
typedef void *wine_atspi_jmpbuf[5];
#define luai_jmpbuf wine_atspi_jmpbuf
#define LUAI_THROW(L, c) __builtin_longjmp((c)->b, 1)
#define LUAI_TRY(L, c, a) if (__builtin_setjmp((c)->b) == 0) { a }

/*
 * Wine's msvcrt <vadefs.h> makes va_list the ms_abi one on x86_64, but a
 * Winelib ELF function is sysv, so va_start in Lua's variadic functions
 * (lua_pushfstring, luaL_error, luaG_runerror, ...) read garbage and
 * crashed. Lua passes its va_list to no msvcrt function, only to its own
 * luaO_pushvfstring, so it can use GCC's sysv va_list throughout. lua.h
 * includes <stdarg.h> before this file. A bridge file that includes lua.h
 * gets this va_list too, so it must not pass one to Win32 (wvsprintfW).
 */
#if defined(__x86_64__) && defined(__WINE__)
#undef va_start
#undef va_arg
#undef va_end
#undef va_copy
#define va_list __builtin_va_list
#define va_start(v, l) __builtin_va_start(v, l)
#define va_arg(v, l) __builtin_va_arg(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_copy(d, s) __builtin_va_copy(d, s)
#endif

#endif
