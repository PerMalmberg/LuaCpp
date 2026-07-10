# TODO

## Script Auditing & Protection

### Auditing

- [ ] **Call tracing** — `lua_sethook` with `LUA_MASKCALL | LUA_MASKRET`. Fires on every
  function call/return. Enables call graph logging, depth tracking, and recording which
  Lua functions ran during a script execution.

- [ ] **Instruction counting** — `lua_sethook` with `LUA_MASKCOUNT`. Fires every N
  instructions. Track total ops executed per `run_script` / `call`.

- [ ] **Memory tracking** — custom allocator via `lua_newstate` instead of `luaL_newstate`.
  Intercepts every alloc/realloc/free and exposes live byte usage.

- [ ] **Output capture** — replace the `print` global with a C function that appends to a
  buffer instead of writing to stdout.

### Protection

- [ ] **Instruction limit** (high priority) — same hook as instruction counting; when the
  counter exceeds a threshold call `lua_error` from inside the hook. Script is aborted
  with a catchable error. Prevents infinite loops and runaway scripts.

- [ ] **Memory cap** (medium priority) — custom allocator returns `nullptr` when usage
  exceeds a configured limit. Lua raises a memory error that `lua_pcall` catches cleanly.

- [ ] **Sandboxing** (high priority) — after `luaL_openlibs`, nil out globals scripts must
  never reach:
  ```lua
  os.execute = nil
  io         = nil
  dofile     = nil
  load       = nil
  require    = nil
  package    = nil
  debug      = nil
  ```
  Alternatively, skip `luaL_openlibs` entirely and open only safe libs
  (`luaopen_math`, `luaopen_string`, `luaopen_table`).

- [ ] **Read-only C++ globals** (low priority) — attach a `__newindex` metamethod to `_G`
  so scripts cannot overwrite globals set from C++ via `assign`.

- [ ] **Bytecode rejection** (high priority) — `luaL_loadstring` accepts pre-compiled
  bytecode when the string starts with `\x1b`. Bytecode skips the lexer/parser and can
  contain crafted instructions. Reject before loading:
  ```cpp
  if (script[0] == '\x1b') return {false, "bytecode not allowed"};
  ```

- [ ] **Recursion depth cap** (medium priority) — track call depth manually inside the
  `LUA_MASKCALL` hook; call `lua_error` when depth exceeds a threshold. Prevents
  stack overflow via deep mutual recursion.
