# TODO

## Script Auditing & Protection

### Auditing

- [x] **Call tracing** — `lua_sethook` with `LUA_MASKCALL | LUA_MASKRET`. Fires on every
  function call/return. Enables call graph logging, depth tracking, and recording which
  Lua functions ran during a script execution. Implemented as `enable_call_tracing`/
  `disable_call_tracing`, delivering `LuaCallTraceEvent`s to a user-supplied callback
  (not retained by `Lua` itself). Shares a single merged `lua_sethook` installation with
  instruction counting/limit and the recursion depth cap via an internal dispatcher.

- [x] **Instruction counting** — `lua_sethook` with `LUA_MASKCOUNT`. Fires every N
  instructions. Track total ops executed per `run_script` / `call`. Implemented as
  `enable_instruction_counting`/`disable_instruction_counting`/`get_instruction_count`.

- [x] **Memory tracking** — custom allocator via `lua_newstate` instead of `luaL_newstate`.
  Intercepts every alloc/realloc/free and exposes live byte usage. Implemented as
  `get_memory_usage`, backed by a custom `lua_Alloc` (`limited_alloc`) passed to
  `lua_newstate`; also required installing our own panic handler (`lua_atpanic`) since
  switching away from `luaL_newstate` means its automatic default one is no longer set.

- [x] **Output capture** — replace the `print` global with a C function that appends to a
  buffer instead of writing to stdout. Implemented as `enable_output_capture`/
  `disable_output_capture`; `print()` output is discarded by default (never written to
  stdout) until a callback is registered. A separate `enable_error_logging`/
  `disable_error_logging` callback (independent of output capture) reports internal
  LuaCpp problems, currently exceptions thrown by a call-trace callback.

### Protection

- [x] **Instruction limit** (high priority) — same hook as instruction counting; when the
  counter exceeds a threshold call `lua_error` from inside the hook. Script is aborted
  with a catchable error. Prevents infinite loops and runaway scripts. Implemented as
  `set_instruction_limit`/`clear_instruction_limit`.

- [x] **Memory cap** (medium priority) — custom allocator returns `nullptr` when usage
  exceeds a configured limit. Lua raises a memory error that `lua_pcall` catches cleanly.
  Implemented as `set_memory_limit`/`clear_memory_limit`, sharing the same `limited_alloc`
  allocator as memory tracking above; limit breaches (and genuine system OOM) are also
  reported via `enable_error_logging`.

- [x] **Sandboxing** (high priority) — implemented via a `LuaLib` bitmask enum and an
  `explicit Lua(LuaLib libs)` constructor overload (`Lua()` still defaults to
  `LuaLib::All`, opening every library exactly as before, so existing code is
  unaffected). Only the selected libraries are opened via `luaL_requiref`, instead
  of unconditionally calling `luaL_openlibs`. Finer-grained removal within an
  otherwise-opened library (e.g. keeping `os.time` but removing `os.execute`) is
  handled by `sandbox_deny("os.execute")`, which nils out a bare global or a single
  dotted nesting level; it is a silent no-op if the named parent doesn't exist or
  isn't opened, so it composes safely with constructor-level exclusion. LuaCpp does
  not ship a built-in denylist — callers choose exactly which names to remove.

- [x] **Read-only C++ globals** (low priority) — implemented via a permanently-empty
  proxy table installed as the real Lua globals table (`LUA_RIDX_GLOBALS`); its
  `__index`/`__newindex` metamethods transparently forward reads to, and gate writes
  against, the real globals table (kept reachable via a private registry key). Every
  name registered via `assign`/`expose_func` is automatically inserted into an internal
  protected-name set; a script-side write to a protected name raises a catchable Lua
  error ("attempt to modify protected global '...'", also reported via
  `enable_error_logging`) instead of silently succeeding. `protect_global`/
  `unprotect_global` let a caller extend or lift protection for any specific name.
  Reads of protected globals, and writes to any other (unprotected) name, are
  completely unaffected.

- [x] **Bytecode rejection** (high priority) — `run_script` now rejects any input
  beginning with the Lua bytecode signature byte `\x1b` before it ever reaches
  `luaL_loadstring`, returning `{false, "bytecode not allowed"}` (also reported via
  `enable_error_logging`, if enabled). Precompiled chunks skip the lexer/parser
  entirely and can encode indices the parser itself would never produce, so
  untrusted script strings must never be allowed to reach the bytecode loader.

- [x] **Recursion depth cap** (medium priority) — track call depth manually inside the
  `LUA_MASKCALL` hook; call `lua_error` when depth exceeds a threshold. Prevents
  stack overflow via deep mutual recursion. Implemented as `set_recursion_depth_cap`/
  `clear_recursion_depth_cap`.
