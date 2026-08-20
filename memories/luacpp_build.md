# LuaCpp project: "compile Lua as C++" saga

See `/memories/luacpp_build.md` (memory-tool file) for the full detailed
history of this project's build/feature work. This repo-tracked copy exists
so memory content can be included in commits; sync it from the memory-tool
file whenever asked to "include memory in the commit".

## Latest update: expose_mutable_method extended with owner keep-alive overloads

Closed a previously-noted gap: `expose_mutable_method` now has the same
three-overload shape as `expose_func`/`expose_method` - no-owner,
single-owner (`std::shared_ptr<Owner>`), and multi-owner
(`std::tuple<std::shared_ptr<Owners>...>` via `Lua::keep_alive()`). Added
`make_mutable_method_wrapper` (mirrors `make_method_wrapper` but with the
copy-mutate-write-back body), consolidated the public overloads so only the
tuple one has real logic (the other two forward via `keep_alive(...)`).
Added 3 new tests (single-owner keep-alive, multi-owner `keep_alive()`,
owner survives after local shared_ptr reset) placed after
`LUA_REGISTER_STRUCT(Point, ...)`. Updated README.md (`expose_mutable_method`
section + `expose_func`'s "same overloads exist for..." line) and
LIFETIME.md item 1 to mention `expose_mutable_method` has the identical
overload set. Test count: 248/248 passing (up from 245).

Net result: `expose_func`/`expose_method`/`expose_mutable_method` all now
have the identical no-owner/single-owner/keep_alive()-tuple 3-overload
shape, each with exactly one real registration code path (the tuple
overload); the other two are thin one-line forwarders. `NoOwner` sentinel
type is gone entirely (superseded by an empty tuple).

Prior chain that led here (multi-owner keep_alive() added for
expose_func/expose_method, then single-owner and no-owner overloads
consolidated into the tuple-based one, then a README wording fix after the
user questioned whether "chaining single-owner calls" was ever a real
alternative - confirmed it isn't, since a second expose_func/expose_method
call under the same name REPLACES the registration rather than merging
owners) - see the memory-tool file for full details of each step.

## Key recurring facts about this project

- Repo: C:\code\LuaCpp (also /home/permal/code/LuaCpp in another env).
- Lua 5.5.0 compiled as C++ (single unity wrapper TU, extern "C" wrapped),
  NOT using LUA_USE_LONGJMP - Lua's own native C++ exception handling is
  used at the lua_pcall boundary instead, which correctly unwinds
  destructors. MSVC needs /EHa (not default /EHsc) applied as a PUBLIC
  usage requirement on `lua_static` so it propagates to consumers too.
- CMake targets: `lua_static`, `luacpp_example`, `luacpp_tests` (build via
  `cmake -B build && cmake --build build`, test via `ctest --test-dir
  build`). LUACPP_BUILD_EXAMPLES/LUACPP_BUILD_TESTS/LUACPP_ENABLE_ASAN
  options default to ON only when LuaCpp is the top-level project.
- `Lua.hpp` is the single-header wrapper; `registered_funcs` is a
  `std::list<std::shared_ptr<LuaFunc>>`; every registered closure holds a
  `std::weak_ptr<LuaFunc>` upvalue via `trampoline()`, which locks() it
  before calling and raises a catchable Lua error if expired.
- Lifetime risks and mitigations are documented in `LIFETIME.md` (5 items):
  1) raw-reference captures in expose_func/expose_method/
     expose_mutable_method closures - mitigated via shared_ptr<Owner> and
     multi-owner keep_alive() overloads (this session's work).
  2) Lua outliving registered callables - weak_ptr trampoline (currently
     dormant/forward-looking since nothing prunes registered_funcs early).
  3) teardown/destruction order - `close()` nils globals + forces GC.
  4) struct method self-by-value pitfalls (documented only, no code fix
     possible/needed).
  5) coroutine resumption after C++ teardown - same hazard as item 1,
     reachable via a resumed coroutine instead of a direct call.
- Full test suite is currently 248/248 passing.