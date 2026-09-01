# LuaCpp project: "compile Lua as C++" saga

See `/memories/luacpp_build.md` (memory-tool file) for the full detailed
history of this project's build/feature work. This repo-tracked copy exists
so memory content can be included in commits; sync it from the memory-tool
file whenever asked to "include memory in the commit".

## Latest update: data-centric XML tree struct-binding tests + README section

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
- Full test suite is currently 260/260 passing (1239 assertions).

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