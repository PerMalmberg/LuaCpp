# LuaCpp project: "compile Lua as C++" saga

See `/memories/luacpp_build.md` (memory-tool file) for the full detailed
history of this project's build/feature work. This repo-tracked copy exists
so memory content can be included in commits; sync it from the memory-tool
file whenever asked to "include memory in the commit".

## Latest update: corrected diagnosis - the failure was in the codeql job,
## not the test job; fixed by disabling ASan there instead

Initial (wrong) diagnosis: assumed the failure was in the `test` job's
ubuntu-latest legs, attributed to a GitHub-image-update-induced ASan
`verify_asan_link_order` false positive, and "fixed" it by adding
`env: ASAN_OPTIONS: verify_asan_link_order=0` at the `test` job level.

User corrected this: the `test` job's ubuntu-latest legs actually pass fine
- only the `codeql` job's build fails. Re-diagnosed: `LUACPP_ENABLE_ASAN`
defaults to `ON` whenever LuaCpp is the top-level CMake project (see
`CMakeLists.txt`'s `LUACPP_IS_TOP_LEVEL` option defaults), which is true in
*both* jobs - so ASan being enabled isn't the differentiator, since both
jobs build with it ON. The actual differentiator: `github/codeql-action/
init` installs a build tracer that, for every process spawned in the rest
of that job, injects its own `LD_PRELOAD`-based interception mechanism so
CodeQL can observe compiler/linker invocations for its extraction database.
That tracer library loads ahead of `libasan.so` in the process's initial
library list - which is exactly what trips ASan's `verify_asan_link_order`
startup self-check. The plain `test` job has no such tracer, so
`libasan.so` loads first there without issue, even with the identical
`-DLUACPP_ENABLE_ASAN=ON`.

Fix applied: reverted the `ASAN_OPTIONS` env block from the `test` job
(unnecessary - it was never actually broken), and instead added
`-DLUACPP_ENABLE_ASAN=OFF` to the `codeql` job's Configure step, with a
comment explaining the tracer-vs-ASan conflict and noting CodeQL's static
analysis only needs the code to compile (not run), so ASan brings no
benefit there anyway. This is the more correct/minimal fix: it avoids
fighting an inherent incompatibility (CodeQL tracer + ASan's LD_PRELOAD
requirement) rather than trying to suppress just the symptom on the wrong
job.

Not independently verifiable in this sandboxed dev environment (no access
to GitHub Actions runs) - reasoning is based on documented CodeQL tracer
behavior and ASan's known startup self-check semantics.

## Prior update: CodeQL merged into ci.yml as a job (not a separate workflow)

Initial approach (added, then superseded within the same session before
ever being committed): a standalone `.github/workflows/codeql.yml` file
with its own triggers (push/pull_request to `main` + weekly cron) and a
separate `[![CodeQL]]` README badge.

Final approach, per explicit follow-up ("make codeql part of the regular
workflow instead of a separate build"): deleted `codeql.yml` entirely and
added a `codeql` job directly inside the existing `.github/workflows/ci.yml`
(alongside the `test` matrix job), so it shares `ci.yml`'s existing
triggers (plain `push`/`pull_request`, no branch filter, no schedule) rather
than having its own. The README's existing single `[![CI]]` badge now
covers both jobs, so no separate badge was added/kept. `codeql` job details
(unchanged from the standalone version): `runs-on: ubuntu-latest`,
`permissions: security-events: write, contents: read`, steps
`actions/checkout@v7` -> `lukka/get-cmake@latest` ->
`github/codeql-action/init@v4` (languages: cpp) -> manual `cmake -B build
-DCMAKE_BUILD_TYPE=Release` + `cmake --build build` (not autobuild, so it
traces the same compile invocations as the `test` job - notably Lua
compiled as C++ via the single unity wrapper TU) -> `github/codeql-action/
analyze@v4` (category `/language:cpp`). Note: user corrected an initial v3
pin to v4 (the latest `codeql-action` major version) before this was
committed.

Net tracked-file diff for this whole CodeQL effort ended up being just the
new `codeql` job appended to `ci.yml` - the README badge add+revert and the
codeql.yml create+delete cancelled out to no diff, since none of that was
committed before the "merge into regular workflow" follow-up arrived.
Purely additive/CI-only change - no source, test, or build-affecting files
touched; full suite unaffected: 269/269 passing (1267 assertions).

## Prior update: expose_namespace extended with owner keep-alive overloads

Closed the previously-noted gap ("no keep_alive() overload for
expose_namespace yet"). `expose_namespace` now has the same three-overload
shape as `expose_func`/`expose_method`/`expose_mutable_method`: a no-owner
overload, a single-owner overload (`std::shared_ptr<Owner>`), and a
multi-owner overload (`std::tuple<std::shared_ptr<Owners>...>` via
`Lua::keep_alive()`) - the real implementation. Key difference from the
other three functions: the owner(s) are shared across **every entry** in
one `expose_namespace(...)` call (one copy of the keep_alive tuple, not one
per entry), since a module conceptually groups several functions that
often close over the same underlying object (e.g. a shared document).

Implementation in `src/Lua.hpp`:
- `register_module_entry(entry, const KeepAlive&)` (private) now takes the
  keep_alive tuple by const& and forwards a copy into `make_func_wrapper`
  for each entry (cheap - just shared_ptr refcount bumps).
- `expose_namespace(module_name, entries...)` - delegates to
  `expose_namespace(module_name, keep_alive(), entries...)`.
- `expose_namespace(module_name, shared_ptr<Owner> owner, entries...)` -
  delegates to `expose_namespace(module_name, keep_alive(std::move(owner)),
  entries...)`.
- `expose_namespace(module_name, tuple<shared_ptr<Owners>...> owners,
  entries...)` - the one real registration path: builds the table, folds
  `register_module_entry(entry, owners)` over every entry (owners copied,
  not moved, since the same tuple is reused per entry), then
  `write_real_global` + `registered_global_names`/`protected_globals` as
  before.

Added 2 new `[expose_namespace][lifetime]` tests in `src/test.cpp` (now 9
`[expose_namespace]` tests total, 28 assertions): single-owner (`counter`
dropped locally after registration, module's own shared_ptr copy keeps it
alive across repeated calls to a wrapped `example_ns.bump()`), and
multi-owner via `Lua::keep_alive(counter, multiplier)` with TWO entries
(`bump_and_scale` and `peek`) in the same `expose_namespace` call, proving
both entries share the *same* underlying counter instance (not independent
copies) and both owners stay alive after the locals go out of scope.

Updated the README "expose_namespace" section: replaced the "no
keep_alive() overload yet" caveat with single-owner and
`Lua::keep_alive()` multi-owner code examples (mirroring the
`expose_mutable_method` README treatment), and updated the pitfalls list
accordingly.

Verified: full rebuild, `[expose_namespace]` tests 9/9 (28 assertions, up
from 7/20), full suite 269/269 (1267 assertions, up from 267/1259).

## Prior update: implemented expose_namespace/lua_module_func (the proposed
## namespacing helper) with tests and README docs

Implemented the module/namespace-table registration helper previously only
proposed in discussion (for APIs like `ndi.find`/`ndi.replace`/
`ndi.append`). New public surface in `src/Lua.hpp`:

- `LuaModuleFunc<std::tuple<ReturnTypes...>, std::tuple<Args...>>` -
  namespace-scope descriptor struct (primary template undefined; only the
  tuple-wrapped partial specialization is instantiated) holding a
  `const char* name` + `std::function<lua_return_t<ReturnTypes...>(Args...)>`.
  Placed right after `LUA_REGISTER_STRUCT`'s macro definition.
- `lua_module_func<ReturnTypes...>(name, func)` - free factory function
  building one `LuaModuleFunc` entry; `ReturnTypes...` explicit (same
  convention as `expose_func`/`call<>`), `Args...` deduced from `func`.
- `Lua::expose_namespace(module_name, entries...)` - public variadic member
  template. Pushes a fresh table, folds over `register_module_entry(...)`
  for each entry (each may have a totally different signature from the
  others - that's why LuaModuleFunc needs the tuple-wrapping trick, to let
  a single parameter pack `Entries...` hold heterogeneously-typed
  descriptors), then `write_real_global(module_name)` + adds the name to
  `registered_global_names`/`protected_globals` - so the module table gets
  the exact same overwrite-protection and `close()`-nils-it treatment as a
  flat `expose_func` global.
- `Lua::register_module_entry(LuaModuleFunc<...> entry)` (private) - mirrors
  `register_global_func` but does `lua_setfield(table, entry.name)` instead
  of `write_real_global`, and does NOT track the entry itself in
  `registered_global_names`/`protected_globals` (only the module table as a
  whole is tracked) - added right after `register_global_func` in the
  source.
- No `keep_alive()` overload for `expose_namespace` itself (documented as a
  known limitation) - captures must be by-value (e.g. a `std::shared_ptr`
  captured by value into each lambda, which is what the tests/README do);
  an entry needing reference-owner keep-alive should be registered
  individually via `expose_func` and spliced into the table with a small
  `run_script()` instead.

Added a `[struct][xml][expose_namespace]`-tagged block of 7 tests in
`src/test.cpp` (right after the `expose_mutable_method` XmlNode tests, +20
assertions): a shared `std::shared_ptr<std::vector<XmlNode>> document`
captured by value in each closure models a tiny in-memory "database";
`xml_node_matches()` (anonymous-namespace free function) matches
`params["element_name"]` against `n.name` and every other key against
`n.attributes`. Tests: `ndi.find` returns matching top-level nodes;
`ndi.replace` removes matches + inserts new_data, returning `(true, "")`;
`ndi.replace` returns `(false, "no matching nodes found")` when nothing
matches; `ndi.append` adds without disturbing existing nodes; all three
entries reachable as `type(ndi.find/replace/append) == 'function'` on one
table; the `ndi` global itself is protected (`ndi = nil` rejected with
"protected global"); `close()` nils `ndi` out like any other expose_func
registration. Note: `call<>()` only takes a bare global name (no dotted
paths), so each test defines a tiny Lua wrapper function
(`do_find`/`do_replace`/`do_append`) that internally calls `ndi.xxx(...)`,
then invokes that wrapper via `call<>()` - this is not a limitation of
`expose_namespace` itself, just of how test bodies drive calls into
dotted-path globals generically.

Added a matching "### expose_namespace" README section (right after
`expose_mutable_method`, before "### Exception Handling"): explains why
`expose_func` alone can't do this (dotted name would literally become one
flat global named `"ndi.find"`), documents `lua_module_func<ReturnTypes...>`
convention, gives the full `ndi.find/replace/append` example against a
shared `std::vector<XmlNode>` document, explains why `search_params` should
be a plain `unordered_map<string,string>` rather than a
`LUA_REGISTER_STRUCT` (partial-table matching vs. every-field-required
struct semantics), and lists the same pitfalls as `expose_func` plus the
"no keep_alive() overload yet" caveat.

Verified: full rebuild, `[expose_namespace]` tests 7/7 (20 assertions),
full suite 267/267 (1259 assertions, up from 260/1239). Used Python one-off
scripts (via run_command) to splice large test/README blocks into place
after insert_edit_into_file repeatedly failed to parse the JSON payload for
this particular chunk (unescaped quotes inside code/comments in the
newText) - written to a /tmp scratch file first, then spliced in with a
short inline Python script matched against a stable surrounding-text
marker, which was more reliable than manually escaping a large block by
hand.

Follow-up: user asked to rename the example module name "ndi" to
"example_ns" throughout (both `src/test.cpp` and `README.md`). Done via
targeted `sed -i '<line-range> s/\bndi\b/example_ns/g'` restricted to the
exact line ranges containing the expose_namespace example/tests, since a
blanket whole-file replace would have been safe anyway (word-boundary
regex correctly skips substring false positives like "unwinding",
"binding", "individually") but line-scoping was used as an extra
precaution. Re-verified: full rebuild, `[expose_namespace]` tests still
7/7 (20 assertions), full suite 267/267 (1259 assertions) - unchanged
counts, purely a rename.

## Prior update: data-centric XML tree struct-binding tests + README section

User asked whether a tree-shaped class (tag name, string attributes map,
list of child pointers) could be exchanged with Lua tables. Answer: yes,
but `std::vector<Node*>` must become `std::vector<Node>` - struct exchange
is always by value (no code path turns a Lua table into a raw pointer), and
a self-referential `std::vector<T>` member is legal C++17 (incomplete type
at declaration, complete by first use after the class body closes).
Registered a new `XmlNode` struct (`name`, `text`,
`unordered_map<string,string> attributes`, `vector<XmlNode> children`) in
`src/test.cpp` right after the existing `Bag`/`Registry` container-field
struct examples, tagged `[struct][xml]`, with 9 new TEST_CASEs (34
assertions): reading nested fields/attributes from a C++-built tree in Lua;
attribute value changed in Lua reflected back via `call<XmlNode>`; new
attribute added in Lua; child appended in Lua (table literal via
`n.children[#n.children+1] = {...}`); a grandchild's field edited 2 levels
deep round-tripping through the whole tree; a node built entirely as a Lua
table literal read back correctly; empty attributes/children round-trip
without error; a `std::vector<XmlNode>` of sibling top-level nodes; and a
recursive `expose_func` (a `std::function` capturing itself by reference)
that flattens an Lua-table-literal-built tree into a depth-first name list
entirely in C++. Explicitly does NOT model real mixed-content XML
(interleaved text/element order, comments, CDATA, processing
instructions) - documented as a known limitation, both in the test file
comment block and in the new README section. Full suite: 257/257 passing
(1234 assertions, up from 248/~1200).

Added a matching README section (in "Struct Binding", right after the
existing "Construction from Lua" struct example, before the `expose_func`
heading): explains the `XmlNode` shape, the vector<Node*>-to-vector<Node>
value-semantics point, the C++17 incomplete-type-in-vector justification,
the "data-centric subset only" caveat, and a short attribute-mutation
round-trip code sample, pointing to the `[xml]`-tagged tests for full
runnable examples.

Follow-up 3: user asked for a similar example using `expose_mutable_method`.
Added 2 more `[xml]` tests (now 12 total, 39 assertions): "xml:
expose_mutable_method mutates an XmlNode instance's attributes in place
from Lua" (`set_attribute(key, value)` writing into `n.attributes[key]` on
a `lua.assign()`-registered instance, then read back via
`shape_node.attributes.color`), and "xml: expose_mutable_method appends a
child to an XmlNode instance in place and returns the new count"
(`add_child(name)` pushes a new `XmlNode` onto `n.children` and returns
`n.children.size()` as an `int`, demonstrating the mutation+return-value
combo). Both operate on a Lua-assigned instance (`p:method(...)` style,
matching the existing Point mutable-method tests) rather than going
through `call<XmlNode>`. Added a matching README snippet (right after the
`expose_func` getter example in the Data-centric XML tree section) showing
`expose_mutable_method<XmlNode>("set_attribute", ...)`. Verified: full
rebuild, `[xml]` tests 12/12 (39 assertions), full suite 260/260 (1239
assertions, up from 258/1237).

Follow-up 2: user asked whether we have an example of "Lua retrieves XML
data from C++" - distinct from the existing tests, which were all either
push-style (C++ passes an XmlNode as an argument to a Lua function) or
round-trip (Lua mutates and returns it). Confirmed the gap (no
`expose_func<XmlNode>` existed) and, on request, added a 10th `[xml]` test:
"xml: expose_func exposes a C++-built tree that Lua fetches on demand" -
`lua.expose_func<XmlNode>("get_document", std::function<XmlNode()>([doc]()
{ return doc; }))`, then a Lua script calls `get_document()` itself and
reads nested attributes (`d.attributes.version`,
`d.children[1].attributes.value`). Also added a matching short README
snippet (same section, right after the attribute-mutation round-trip
example) titled "The reverse direction - Lua pulling data from C++ on
demand". Verified: full rebuild, `[xml]` tests now 10/10 (37 assertions),
full suite 258/258 (1237 assertions, up from 257/1234).

Follow-up: user asked to use `R"(...)"` raw string literals instead of
concatenated `"..." "..."` string literals for embedded Lua source, matching
the pre-existing style used elsewhere in `test.cpp` (e.g. "struct: nested
struct round-trip" at the time used `R"( ... )"` with tab-indented Lua
inside). Converted all 8 `lua.run_script(...)` calls across the new `[xml]`
tests to raw strings (tab-indented Lua body between `R"(` / `)"`), and fixed
a brace/`end`-matching mistake introduced during that conversion in the
"build tree + flatten via expose_func" test (dropped closing `}` and `end`
for the `build()` function). Also updated the matching README code sample
to raw-string form for consistency. Re-verified: full rebuild, `[xml]`
tests 9/9 (34 assertions), full suite 257/257 (1234 assertions).

## Prior update: expose_mutable_method extended with owner keep-alive overloads

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
- Full test suite is currently 269/269 passing (1267 assertions).

## Named recurring magic numbers in Lua.hpp

User asked to replace magic numbers with named constants. Added, near
`TOP_OF_STACK`/HookState: `SELF_STACK_INDEX = 1` + `FIRST_ARG_AFTER_SELF = 2`
(self/arg-start stack slots for expose_method/expose_mutable_method
wrappers), `FIRST_ARG_NO_SELF = 1` (expose_func's arg-start slot),
`WEAK_FUNC_UPVALUE_COUNT = 1` (the single upvalue every trampoline closure
has - used for both `lua_pushcclosure(..., N)` call sites and
`lua_upvalueindex(N)` in trampoline()), and
`DEFAULT_INSTRUCTION_COUNT_PERIOD = 1000` (previously duplicated as a
literal default arg in both `enable_instruction_counting` and
`set_instruction_limit`). Deliberately did NOT touch the Lua-idiomatic
stack-offset literals (-1/-2/-3 etc.) used throughout push/read/struct
field code - those are universal, already commented per-use-site Lua C API
conventions, not real "magic numbers". Verified: full rebuild, 248/248
tests passing, luacpp_example runs end-to-end (exit 0). Committed as
297c178.