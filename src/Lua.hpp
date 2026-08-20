extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Determines the C++ return type for a expose_func registration based on the number
// of declared Lua return values - mirrors the convention used by call<>:
//   0 types  -> void
//   1 type   -> T          (not wrapped in a tuple)
//   N types  -> std::tuple<Ts...>
template <typename... Ts>
struct lua_return_type
{
    using type = std::tuple<Ts...>;
};
template <typename T>
struct lua_return_type<T>
{
    using type = T;
};
template <>
struct lua_return_type<>
{
    using type = void;
};
template <typename... Ts>
using lua_return_t = typename lua_return_type<Ts...>::type;

// ---------------------------------------------------------------------------
// Struct <-> Lua-table binding
//
// Describe a single named member:  lua_field("x", &Point::x)
// Register all fields at namespace scope after the struct definition:
//
//   LUA_REGISTER_STRUCT(Point,
//       lua_field("x", &Point::x),
//       lua_field("y", &Point::y))
//
// The type can then be used as an argument or return type in call<>()
// and expose_func<>(); it maps to/from a Lua table.
// Nesting is supported: a field whose type is itself registered works
// automatically.
//
// Constraints:
//   - Must be placed at namespace scope, not inside a function or class body.
//   - The struct must be default-constructible; read<T> zero-initialises a
//     T result{} before filling registered fields one by one.
//   - Only fields listed in the macro are exchanged with Lua. Extra Lua table
//     keys are silently ignored on read. Fields absent from the Lua table are
//     read as nil, which causes a type error for most C++ member types.
//   - Structs are always exchanged by value; each push or read copies the
//     entire struct, including any container-typed fields.
//   - If the struct lives in a namespace, LUA_REGISTER_STRUCT must still be
//     invoked at GLOBAL scope, not inside that namespace's braces. The macro
//     expands to an explicit specialization of the global-namespace template
//     LuaFields<T>, and the C++ standard requires explicit specializations to
//     be declared in a namespace enclosing the primary template - here, that
//     means global scope, regardless of which namespace T itself is in. Refer
//     to the type by its fully-qualified name:
//
//       namespace myapp { struct Foo { int x; }; }
//       LUA_REGISTER_STRUCT(myapp::Foo, lua_field("x", &myapp::Foo::x)) // OK, at global scope
//
//       namespace myapp {
//           struct Foo { int x; };
//           LUA_REGISTER_STRUCT(Foo, lua_field("x", &Foo::x)) // ERROR: wrong namespace
//       }
// ---------------------------------------------------------------------------

template <typename Struct, typename Member>
struct LuaField
{
    const char* name;
    Member Struct::* ptr;
};

template <typename Struct, typename Member>
constexpr LuaField<Struct, Member> lua_field(const char* name, Member Struct::* ptr)
{
    return {name, ptr};
}

// Specialise this for every struct you want to exchange with Lua.
template <typename T>
struct LuaFields; // intentionally undefined - gives a clear error for unregistered types

// True when LuaFields<T> has been specialised.
template <typename T, typename = void>
struct has_lua_fields : std::false_type
{
};
template <typename T>
struct has_lua_fields<T, std::void_t<decltype(LuaFields<T>::value)>> : std::true_type
{
};

#define LUA_REGISTER_STRUCT(Type, ...)                                                                                 \
    template <>                                                                                                        \
    struct LuaFields<Type>                                                                                             \
    {                                                                                                                  \
        static constexpr auto value = std::make_tuple(__VA_ARGS__);                                                    \
    };

// ---------------------------------------------------------------------------
// Standard container traits
// Used by push()/read() to detect std::vector and std::map/unordered_map.
// T::value_type, T::key_type and T::mapped_type are used to recurse into
// element types, so nested containers and containers of registered structs
// work without any extra registration.
// ---------------------------------------------------------------------------

template <typename T>
struct is_std_vector : std::false_type
{
};
template <typename T, typename A>
struct is_std_vector<std::vector<T, A>> : std::true_type
{
};

template <typename T>
struct is_std_map : std::false_type
{
};
template <typename K, typename V, typename C, typename A>
struct is_std_map<std::map<K, V, C, A>> : std::true_type
{
};
template <typename K, typename V, typename H, typename E, typename A>
struct is_std_map<std::unordered_map<K, V, H, E, A>> : std::true_type
{
};

// Emitted for every Lua function call/return while call tracing is active -
// see Lua::enable_call_tracing.
struct LuaCallTraceEvent
{
    std::string name; // best-effort function name; "?" if Lua couldn't determine one
    bool is_call; // true = call/tailcall event, false = return event
    int depth; // call-stack depth at the moment of this event (1 = outermost call)
};

using LuaCallTraceCallback = std::function<void(const LuaCallTraceEvent&)>;
using LuaOutputCallback = std::function<void(std::string_view)>;
using LuaErrorLogCallback = std::function<void(std::string_view)>;

// Bitmask selecting which standard Lua libraries a Lua instance opens - see
// the Lua(LuaLib) constructor and sandbox_deny(). LuaLib::All (the default,
// used by the no-arg Lua() constructor) matches the previous unconditional
// luaL_openlibs() behavior exactly, so existing code is unaffected.
enum class LuaLib : unsigned
{
    None = 0,
    Base = 1u << 0, // _G: print, pcall, type, ipairs, error, ...
    Table = 1u << 1,
    String = 1u << 2,
    Math = 1u << 3,
    Os = 1u << 4,
    Io = 1u << 5,
    Package = 1u << 6, // also enables require()
    Debug = 1u << 7,
    Coroutine = 1u << 8,
    Utf8 = 1u << 9,
    All = Base | Table | String | Math | Os | Io | Package | Debug | Coroutine | Utf8,
};

constexpr LuaLib operator|(LuaLib lhs, LuaLib rhs)
{
    return static_cast<LuaLib>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
}

constexpr LuaLib operator&(LuaLib lhs, LuaLib rhs)
{
    return static_cast<LuaLib>(static_cast<unsigned>(lhs) & static_cast<unsigned>(rhs));
}

constexpr LuaLib operator~(LuaLib lib)
{
    return static_cast<LuaLib>(~static_cast<unsigned>(lib) & static_cast<unsigned>(LuaLib::All));
}

constexpr bool has_lib(LuaLib set, LuaLib lib)
{
    return (set & lib) == lib;
}

class Lua final
{
  public:
    // Opens every standard library
    Lua() : Lua(LuaLib::All)
    {
    }

    // Opens only the standard libraries selected by `libs`, instead of every
    // one of them. Use this to reduce a script's attack surface - e.g.
    // excluding LuaLib::Os/LuaLib::Io/LuaLib::Package/LuaLib::Debug removes
    // os.execute, io.*, require/package, and the debug library entirely,
    // rather than trying to individually nil out dangerous functions after
    // the fact. Combine with sandbox_deny() for finer-grained denial within
    // a library that is otherwise still opened (e.g. keeping `os.time` but
    // removing `os.execute`).
    //
    // print() is unaffected by this constructor either way - see
    // enable_output_capture: print() output is discarded by default
    // regardless of which libraries are opened.
    explicit Lua(LuaLib libs)
    {
        *static_cast<Lua**>(lua_getextraspace(static_cast<lua_State*>(*this))) = this;
        lua_atpanic(*this, &Lua::panic);
        open_selected_libs(libs);
        lua_pushcfunction(*this, &Lua::captured_print);
        lua_setglobal(*this, "print"); // default: print() output goes nowhere
        setup_global_protection_proxy();
    }

    ~Lua() = default;

    // Explicit early teardown - see LIFETIME.md item 3 for the full rationale.
    //
    // Call this as the FIRST statement in the destructor of any class that
    // embeds a Lua instance alongside other members that expose_func/
    // expose_method closures capture by raw pointer/reference (rather than
    // via the std::shared_ptr<Owner> overloads from item 1). A destructor's
    // BODY always runs before any of its member subobjects are destroyed,
    // regardless of declaration order, so calling close() there guarantees
    // every sibling member is still fully alive when close() forces a GC
    // cycle - unlike relying on ~Lua() running implicitly during automatic
    // member teardown, whose safety would otherwise depend on getting every
    // embedding class's member declaration order exactly right, forever.
    //
    // What it does:
    //   1. Nils every global name registered via expose_func (does NOT
    //      affect expose_method/expose_mutable_method entries, which live in
    //      per-type metatables in the Lua registry rather than as globals -
    //      those remain reachable for the lifetime of the Lua state).
    //   2. Forces a full GC cycle (LUA_GCCOLLECT) so any Lua-side objects
    //      with a script-set __gc metamethod that happens to reference a
    //      registered closure are finalized NOW, while C++ state the
    //      closures may reach into (via raw captures) is still alive.
    //   3. Marks the instance closed; safe to call multiple times (a second
    //      call is a cheap no-op).
    //
    // Calling close() does not prevent later use of this Lua instance - it
    // only removes the specific globals that were registered so far and
    // runs one GC pass. run_script/call/assign/expose_* remain usable
    // afterward if desired.
    void close()
    {
        if(closed)
        {
            return;
        }

        for(const auto& name : registered_global_names)
        {
            lua_pushnil(*this);
            write_real_global(name.c_str());
            protected_globals.erase(name);
        }

        // A single LUA_GCCOLLECT queues newly-unreachable objects with a
        // __gc metamethod for finalization and also runs any finalizers
        // already queued from a previous cycle. Running it twice ensures
        // objects that only became unreachable in *this* cycle are also
        // finalized before close() returns, rather than being deferred to
        // whatever future GC activity happens to run next (possibly after
        // the caller's other members have already started tearing down).
        lua_gc(*this, LUA_GCCOLLECT, 0);
        lua_gc(*this, LUA_GCCOLLECT, 0);

        closed = true;
    }

    Lua(const Lua&) = delete;
    Lua& operator=(const Lua&) = delete;
    Lua(Lua&&) = delete;
    Lua& operator=(Lua&&) = delete;

    // ---------------------------------------------------------------------------
    // Call tracing, output capture, error logging, and script-protection hooks
    //
    // All of these are backed by a single lua_sethook installation (Lua only
    // allows one hook callback + mask per lua_State), merged and re-installed
    // via refresh_hook_mask() whenever any one of them is (re)configured, so
    // enabling several of them together composes safely instead of one
    // overwriting another's registration.
    // ---------------------------------------------------------------------------

    // Installs `cb`, invoked synchronously on every Lua function call/return
    // while tracing is active. `cb` runs from inside the Lua hook - it must be
    // cheap and must not rely on exceptions escaping uncleanly: any
    // std::exception it throws is caught here and reported via the error-log
    // callback (see enable_error_logging), if one is registered; non-
    // std::exception throws are also caught and reported generically. Either
    // way the script itself is unaffected - tracing failures never abort
    // execution.
    //
    // Value-capture in `cb` is always safe; reference captures must not
    // outlive this Lua instance (same rule as expose_func/expose_method).
    void enable_call_tracing(LuaCallTraceCallback cb)
    {
        hooks.trace_callback = std::move(cb);
        refresh_hook_mask();
    }

    void disable_call_tracing()
    {
        hooks.trace_callback = nullptr;
        refresh_hook_mask();
    }

    // By default, before this is ever called, print() output is discarded
    // entirely - LuaCpp never writes to stdout. Call this to start receiving
    // print() output via `cb`: one call per invocation of print(), arguments
    // tab-separated (honouring any __tostring metamethod, via luaL_tolstring,
    // matching Lua's own print() formatting) and newline-terminated.
    // disable_output_capture() returns to discarding output - it does NOT
    // restore writing to stdout. Independent of error logging - see
    // enable_error_logging; enabling one does not affect the other.
    void enable_output_capture(LuaOutputCallback cb)
    {
        output_callback = std::move(cb);
    }

    void disable_output_capture()
    {
        output_callback = nullptr;
    }

    // By default, internal LuaCpp problems (currently: an exception thrown by
    // a call-trace callback registered via enable_call_tracing) are silently
    // dropped. Call this to receive them via `cb` as single, "[LuaCpp] "-
    // prefixed messages - NOT newline-terminated; the consumer's callback is
    // responsible for its own line formatting (e.g. appending '\n' when
    // writing to a stream, or none at all when appending to a structured log
    // record). Independent of enable_output_capture - does not receive
    // print() output, and does not require print() capture to be enabled.
    void enable_error_logging(LuaErrorLogCallback cb)
    {
        error_log_callback = std::move(cb);
    }

    void disable_error_logging()
    {
        error_log_callback = nullptr;
    }

    // Enables LUA_MASKCOUNT-based instruction counting: get_instruction_count()
    // increases by roughly `period` Lua VM instructions at a time (an
    // approximation - lua_sethook's count parameter fires the hook every
    // `period` instructions, and the counter is incremented by `period` on
    // each firing rather than tracking the exact instruction executed).
    // If instruction-limit protection (set_instruction_limit) is also active,
    // the smaller of the two requested periods is used for both, since Lua
    // only supports a single count-period per lua_State.
    void enable_instruction_counting(int period = 1000)
    {
        hooks.counting_enabled = true;
        hooks.instruction_count = 0;
        update_count_period(period);
        refresh_hook_mask();
    }

    void disable_instruction_counting()
    {
        hooks.counting_enabled = false;
        update_count_period(0);
        refresh_hook_mask();
    }

    std::uint64_t get_instruction_count() const
    {
        return hooks.instruction_count;
    }

    // Enables the instruction-limit protection: once the running instruction
    // count reaches `limit`, the next LUA_HOOKCOUNT firing raises a catchable
    // Lua error (visible via run_script's/call<>'s {false, msg} return),
    // aborting the offending script. See enable_instruction_counting for the
    // note on shared count periods when both features are active together.
    void set_instruction_limit(std::uint64_t limit, int period = 1000)
    {
        hooks.instruction_limit = limit;
        update_count_period(period);
        refresh_hook_mask();
    }

    void clear_instruction_limit()
    {
        hooks.instruction_limit = 0;
        update_count_period(0);
        refresh_hook_mask();
    }

    // Enables the recursion-depth-cap protection: once the Lua call stack
    // depth (tracked via LUA_MASKCALL/LUA_MASKRET) exceeds `max_depth`, the
    // next call raises a catchable Lua error instead of recursing further.
    void set_recursion_depth_cap(int max_depth)
    {
        hooks.max_depth = max_depth;
        refresh_hook_mask();
    }

    void clear_recursion_depth_cap()
    {
        hooks.max_depth = 0;
        refresh_hook_mask();
    }

    // Enables a memory cap: once total bytes allocated by this Lua instance
    // (tracked via a custom lua_Alloc - see limited_alloc) would exceed
    // `bytes`, the allocator refuses the offending allocation by returning
    // nullptr. Lua treats this exactly like a real out-of-memory condition and
    // raises a catchable "not enough memory" error (visible via run_script's/
    // call<>'s {false, msg} return) - it does NOT crash or corrupt the state.
    // Existing allocations already above the limit are left untouched; only
    // future allocations that would push usage over the limit are rejected.
    // Pass 0 to disable the cap (the default - unlimited).
    void set_memory_limit(std::size_t bytes)
    {
        memory.limit = bytes;
    }

    void clear_memory_limit()
    {
        memory.limit = 0;
    }

    // Total bytes currently allocated by this Lua instance's custom allocator
    // (i.e. everything Lua itself has allocated - state, strings, tables,
    // closures, etc. - not just script-visible data). Always tracked, whether
    // or not a memory limit is set.
    std::size_t get_memory_usage() const
    {
        return memory.bytes_used;
    }

    // Removes a single global, or a single nested field one level deep, so it
    // is no longer reachable from Lua code. `dotted_path` is either a bare
    // global name (e.g. "dofile") or exactly one dot-separated nesting level
    // (e.g. "os.execute"). Deeper paths are not supported.
    //
    // If the named global (or the parent of a nested path) does not exist, or
    // is not a table, this is a silent no-op rather than an error - so it is
    // safe to call sandbox_deny("os.execute") even when LuaLib::Os was never
    // opened at all; the two approaches compose without needing to be kept in
    // sync with each other.
    //
    // LuaCpp does not ship a built-in denylist - callers decide exactly which
    // names to remove for their own use case.
    void sandbox_deny(const std::string& dotted_path)
    {
        const auto dot = dotted_path.find('.');
        if(dot == std::string::npos)
        {
            lua_pushnil(*this);
            write_real_global(dotted_path.c_str()); // bypass protection - this is a trusted C++ call
            protected_globals.erase(dotted_path); // no longer meaningful once nil'd
            return;
        }

        const auto parent_name = dotted_path.substr(0, dot);
        const auto field_name = dotted_path.substr(dot + 1);

        lua_getglobal(*this, parent_name.c_str());
        if(!lua_istable(*this, TOP_OF_STACK))
        {
            lua_pop(*this, 1);
            return;
        }

        lua_pushnil(*this);
        lua_setfield(*this, -2, field_name.c_str());
        lua_pop(*this, 1); // pop parent table
    }

    // Marks an existing Lua global as protected: subsequent script-side
    // attempts to write to it raise a catchable Lua error ("attempt to
    // modify protected global '<name>'") instead of silently overwriting it.
    // Every global set via assign() or expose_func() is protected
    // automatically the moment it is registered - use this to extend the
    // same protection to a global instead set from Lua itself (e.g. via
    // run_script), or to re-protect a name after unprotect_global(). Reads
    // of a protected global are completely unaffected; only writes are
    // rejected. Has no effect on globals set via expose_method/
    // expose_mutable_method, which are not stored as globals at all.
    void protect_global(const std::string& name)
    {
        protected_globals.insert(name);
    }

    // Removes protection from `name`, allowing script code to overwrite it
    // again. No effect if `name` was not protected.
    void unprotect_global(const std::string& name)
    {
        protected_globals.erase(name);
    }

    std::tuple<bool, std::string> run_script(const char* script)
    {
        // Lua's own loader (luaL_loadstring/lua_load) transparently accepts
        // pre-compiled bytecode when the input starts with the bytecode
        // signature byte '\x1b' ('\27', LUA_SIGNATURE[0]) - it skips the
        // lexer/parser entirely, so a script string can never be "just text"
        // in that case. Precompiled chunks are not sandboxed the same way
        // source is (e.g. crafted bytecode can encode out-of-range constant/
        // register indices that the parser would never produce, historically
        // a source of crashes/memory corruption in Lua and other embedders),
        // so untrusted input must never be allowed to reach the bytecode
        // loader. Reject it up front, before doing anything else.
        if(script != nullptr && script[0] == '\x1b')
        {
            auto msg = std::string("bytecode not allowed");
            log_error(msg);
            return {false, std::move(msg)};
        }

        // Use luaL_loadstring + lua_pcall(L,0,0,0) to discard script return values and prevent stack
        // growth on repeated calls
        if(luaL_loadstring(*this, script) != LUA_OK)
        {
            auto msg = get_error_message();
            log_error(msg);
            return {false, std::move(msg)};
        }
        if(lua_pcall(*this, 0, 0, 0) != LUA_OK)
        {
            auto msg = get_error_message();
            log_error(msg);
            return {false, std::move(msg)};
        }
        return {true, {}};
    }

    // Assigns a C++ value to a named Lua global, replacing any previous value.
    //
    // bool: pushed as a native Lua boolean; Lua code compares with == true / == false.
    //
    // const char*: the string is copied by Lua immediately; the pointer does not
    //   need to remain valid after assign() returns.
    //
    // Structs and containers: copied in full by value into a new Lua table.
    //   Subsequent Lua-side mutations to the table do not affect the original
    //   C++ object.
    template <typename T>
    void assign(const char* name, const T& value)
    {
        push(state.get(), decay_for_push(value));
        write_real_global(name); // bypass protection - this is a trusted C++ call
        protected_globals.insert(name); // protect it from script-side overwrite by default
    }

    // Calls a named Lua global function and collects up to N typed return values.
    //
    // ReturnTypes must appear in the same left-to-right order as the Lua
    // function's return values. Specifying fewer types than the function returns
    // is safe; Lua discards the extras. Specifying MORE types causes missing
    // stack slots to be nil, which fails read<T> and returns
    // {false, "expected ..., got nil", ...}.
    //
    // bool ReturnType: expects a native Lua boolean. Integers (even 0 or 1)
    //   fail the lua_isboolean check and produce a type error.
    //
    // On any failure the first tuple element is false and the second is the
    // error message; all ReturnType slots hold their zero-initialised defaults.
    template <typename... ReturnTypes, typename... Args>
    std::tuple<bool, std::string, ReturnTypes...> call(const char* func, Args&&... args)
    {
        lua_getglobal(*this, func);
        if(!lua_isfunction(*this, TOP_OF_STACK))
        {
            lua_pop(*this, 1); // Remove the non-function value from the stack
            auto msg = "Not a function: " + std::string(func);
            log_error(msg);
            return {false, std::move(msg), ReturnTypes{}...};
        }

        (push(state.get(), decay_for_push(std::forward<Args>(args))),
         ...); // C++17 fold expression to push all arguments onto the Lua stack

        const auto arg_count = static_cast<int>(sizeof...(Args));
        const auto ret_count = static_cast<int>(sizeof...(ReturnTypes));

        if(lua_pcall(*this, arg_count, ret_count, 0) != LUA_OK)
        {
            auto msg = get_error_message();
            log_error(msg);
            return {false, std::move(msg), ReturnTypes{}...};
        }

        try
        {
            auto ret = collect<ReturnTypes...>(std::make_index_sequence<sizeof...(ReturnTypes)>{});
            lua_pop(*this, ret_count);
            return std::tuple_cat(std::make_tuple(true, std::string{}), std::move(ret));
        }
        catch(const std::runtime_error& e)
        {
            lua_pop(*this, ret_count);
            log_error(e.what());
            return {false, e.what(), ReturnTypes{}...};
        }
    }

    // Registers a C++ callable as a named Lua global.
    //
    // ReturnTypes - what the function returns to Lua, using the same convention
    //               as call<>: omit for void, one type for a scalar return,
    //               multiple types when the function returns std::tuple<Ts...>.
    // Args        - deduced from the std::function signature.
    //
    // Type mismatches in arguments or return values, and wrong argument counts,
    // are reported as Lua errors (visible to the caller via pcall / call<>).
    //
    // Example - scalar return:
    //   lua.expose_func<int>("add",
    //       std::function<int(int, int)>([](int a, int b){ return a + b; }));
    //
    // Example - multiple returns:
    //   lua.expose_func<int, std::string>("pair",
    //       std::function<std::tuple<int,std::string>(int)>(
    //           [](int n){ return std::make_tuple(n, std::to_string(n)); }));
    //
    // Example - void return:
    //   lua.expose_func("log",
    //       std::function<void(std::string)>(
    //           [](std::string msg){ std::cout << msg << '\n'; }));
    //
    // Pitfalls:
    //   - The callable must be wrapped in std::function; a raw lambda or
    //     function pointer is not accepted because Args... cannot be deduced.
    //   - const char* cannot be used as an Arg type: read<const char*> is
    //     disabled via static_assert to prevent dangling pointers into Lua
    //     memory. Use std::string instead.
    //   - Lambda captures that hold references must outlive the Lua instance.
    //     Value captures are always safe. If a reference capture is needed,
    //     use the overload below that takes a std::shared_ptr<Owner> so the
    //     referent's lifetime is tied to the registered closure instead.
    //   - Registering a name that already exists as a Lua global silently
    //     replaces it; no warning is produced.
    template <typename... ReturnTypes, typename... Args>
    void expose_func(const char* name, std::function<lua_return_t<ReturnTypes...>(Args...)> func)
    {
        register_global_func(name, make_func_wrapper<ReturnTypes...>(std::move(func), NoOwner{}));
    }

    // Same as expose_func above, but keeps `owner` alive for as long as the
    // registered Lua closure exists (i.e. for the lifetime of this Lua
    // instance, or until it is replaced/erased). Use this overload whenever
    // `func` captures a reference to *owner, or to something owned by it,
    // so the closure can never outlive the referent - the shared_ptr copy
    // captured inside the closure keeps the object alive even if all other
    // owners release it.
    //
    // Example:
    //   auto sensor = std::make_shared<Sensor>();
    //   lua.expose_func<int>("read_sensor", sensor,
    //       std::function<int()>([s = sensor.get()]{ return s->read(); }));
    template <typename... ReturnTypes, typename... Args, typename Owner>
    void expose_func(const char* name,
                     std::shared_ptr<Owner> owner,
                     std::function<lua_return_t<ReturnTypes...>(Args...)> func)
    {
        register_global_func(name, make_func_wrapper<ReturnTypes...>(std::move(func), std::move(owner)));
    }

    // Registers a C++ callable as a named method on all Lua instances of StructType.
    //
    // After registration every StructType value pushed to Lua (via assign, call<>,
    // or an expose_func return value) carries a shared metatable whose __index
    // table contains this method, enabling colon-call syntax from Lua:
    //   obj:name(extra_args...)
    //
    // StructType is always the first explicit template argument.
    // ReturnTypes follow the same convention as expose_func<>:
    //   omit for void, one type for a scalar return, multiple for a tuple.
    // The C++ function receives self (the struct) as its first argument by value;
    // any additional arguments follow in order.
    //
    // Example - no extra args:
    //   lua.expose_method<Point, int>("magnitude_sq",
    //       std::function<int(Point)>([](Point p){ return p.x*p.x + p.y*p.y; }));
    //
    // Example - extra args:
    //   lua.expose_method<Point, Point>("translate",
    //       std::function<Point(Point, int, int)>(
    //           [](Point p, int dx, int dy){ return Point{p.x+dx, p.y+dy}; }));
    //
    // Pitfalls:
    //   - Methods must be registered BEFORE any instance of StructType is pushed
    //     to Lua (via assign, call<>, or an expose_func return value). Tables
    //     already in Lua do not retroactively gain the metatable.
    //   - self is read from the Lua table by value on every call. Mutations
    //     inside the C++ function are NOT written back to Lua. Use
    //     expose_mutable_method when the struct must be updated in place.
    //   - self is a BY-VALUE COPY local to this call, living on the C++ call
    //     stack (or, for expose_mutable_method, additionally captured by value
    //     before being written back - see below). Storing its address (e.g.
    //     &self, or a pointer/reference into one of its fields) anywhere that
    //     outlives the call - a global registry, a singleton, a captured
    //     lambda, another container - leaves a dangling pointer the moment the
    //     method returns; the stack slot is reused or popped immediately
    //     afterward. Copy the value (or the specific field) instead of taking
    //     its address.
    //   - const char* cannot be used as an Arg or ReturnType (see expose_func).
    //   - Lambda captures that hold references must outlive the Lua instance.
    //     Value captures are always safe. Use the std::shared_ptr<Owner> overload
    //     below when a reference capture to a shorter-lived object is needed.
    template <typename StructType, typename... ReturnTypes, typename... Args>
    void expose_method(const char* name, std::function<lua_return_t<ReturnTypes...>(StructType, Args...)> func)
    {
        static_assert(has_lua_fields<StructType>::value,
                      "expose_method: StructType must be registered with LUA_REGISTER_STRUCT");

        auto wrapper = make_method_wrapper<StructType, ReturnTypes...>(std::move(func), NoOwner{});
        std::shared_ptr<LuaFunc> fn(std::move(wrapper));
        registered_funcs.push_back(fn);

        add_method_to_registry<StructType>(*this, name, fn);
    }

    // Same as expose_method above, but keeps `owner` alive for as long as the
    // registered Lua closure exists. Use this overload whenever `func`
    // captures a reference to *owner, or to something owned by it - see
    // expose_func(name, owner, func) for the full rationale.
    template <typename StructType, typename... ReturnTypes, typename... Args, typename Owner>
    void expose_method(const char* name,
                       std::shared_ptr<Owner> owner,
                       std::function<lua_return_t<ReturnTypes...>(StructType, Args...)> func)
    {
        static_assert(has_lua_fields<StructType>::value,
                      "expose_method: StructType must be registered with LUA_REGISTER_STRUCT");

        auto wrapper = make_method_wrapper<StructType, ReturnTypes...>(std::move(func), std::move(owner));
        std::shared_ptr<LuaFunc> fn(std::move(wrapper));
        registered_funcs.push_back(fn);

        add_method_to_registry<StructType>(*this, name, fn);
    }

    // Registers a C++ callable as a named mutating method on all Lua instances of StructType.
    //
    // Unlike expose_method, the C++ function receives self as a non-const reference
    // (StructType&). Any modifications made to self are written back into the caller's
    // Lua table before the closure returns, enabling true in-place mutation:
    //
    //   p:translate(1, 1)   -- p.x and p.y are modified in place in the Lua table
    //
    // StructType is always the first explicit template argument.
    // ReturnTypes follow the same convention as expose_method:
    //   omit for void, one type for a scalar return, multiple for a tuple.
    // The C++ function receives self by reference as its first argument;
    // any additional arguments follow in order.
    //
    // Example - void return, modifies self:
    //   lua.expose_mutable_method<Point>("translate",
    //       std::function<void(Point&, int, int)>(
    //           [](Point& p, int dx, int dy){ p.x += dx; p.y += dy; }));
    //
    // Example - scalar return, also modifies self:
    //   lua.expose_mutable_method<Point, int>("scale_and_sum",
    //       std::function<int(Point&, int)>(
    //           [](Point& p, int f){ p.x *= f; p.y *= f; return p.x + p.y; }));
    //
    // Pitfalls:
    //   - Same registration-timing rule as expose_method: register before
    //     pushing instances to Lua.
    //   - Write-back covers only fields declared in LUA_REGISTER_STRUCT for
    //     StructType. Extra keys added to the Lua table by Lua code are
    //     preserved but not managed.
    //   - self follows a copy-mutate-write-back cycle: copied from the Lua
    //     table into a C++ value, mutated by the function, then all registered
    //     fields are written back. There is no shared pointer into Lua memory;
    //     all exchanged fields must be copyable value types (or supported
    //     containers / registered structs thereof).
    //   - self is a BY-VALUE COPY local to this call (see expose_method's
    //     pitfall above for the full rationale). Even though it is passed as
    //     StructType&, that reference refers to a local variable on this call's
    //     C++ stack, NOT to any Lua-managed memory. Storing &self (or a
    //     pointer/reference into one of its fields) anywhere that outlives the
    //     call leaves a dangling pointer; the write-back to Lua happens
    //     separately, by value, after the function returns.
    template <typename StructType, typename... ReturnTypes, typename... Args>
    void expose_mutable_method(const char* name, std::function<lua_return_t<ReturnTypes...>(StructType&, Args...)> func)
    {
        static_assert(has_lua_fields<StructType>::value,
                      "expose_mutable_method: StructType must be registered with LUA_REGISTER_STRUCT");

        auto wrapper = std::make_unique<LuaFunc>(
        [func = std::move(func)](lua_State* L) -> int
        {
            constexpr int n_method_args = static_cast<int>(sizeof...(Args));
            constexpr int expected = n_method_args + 1; // +1 for implicit self
            if(const int got = lua_gettop(L); got != expected)
            {
                throw std::runtime_error("expected " + std::to_string(n_method_args) + " argument(s), got " +
                                         std::to_string(got - 1));
            }

            auto self = read<StructType>(L, 1);
            auto extra_args = collect_args_from<Args...>(L, 2, std::index_sequence_for<Args...>{});

            if constexpr(sizeof...(ReturnTypes) == 0)
            {
                std::apply([&](auto&&... a) { func(self, std::forward<decltype(a)>(a)...); }, std::move(extra_args));
                write_struct_back(L, 1, self);
                return 0;
            }
            else if constexpr(sizeof...(ReturnTypes) == 1)
            {
                auto result = std::apply([&](auto&&... a) { return func(self, std::forward<decltype(a)>(a)...); },
                                         std::move(extra_args));
                write_struct_back(L, 1, self);
                push(L, result);
                return 1;
            }
            else
            {
                auto results = std::apply([&](auto&&... a) { return func(self, std::forward<decltype(a)>(a)...); },
                                          std::move(extra_args));
                write_struct_back(L, 1, self);
                push_results(L, results, std::index_sequence_for<ReturnTypes...>{});
                return static_cast<int>(sizeof...(ReturnTypes));
            }
        });

        std::shared_ptr<LuaFunc> fn(std::move(wrapper));
        registered_funcs.push_back(fn);

        add_method_to_registry<StructType>(*this, name, fn);
    }

  private:
    using LuaCloser = std::function<void(lua_State*)>;
    using LuaStatePtr = std::unique_ptr<lua_State, LuaCloser>;
    using LuaFunc = std::function<int(lua_State*)>;

    // Default "keep-alive" object for the expose_func / expose_method overloads
    // that do not take an explicit owner. Capturing an empty struct by value in
    // the closure has no runtime cost.
    struct NoOwner
    {
    };

    // ---------------------------------------------------------------------------
    // Hook-based auditing/protection state (call tracing, instruction counting,
    // instruction limit, recursion depth cap) - see the public enable_*/set_*
    // methods above. All four share a single lua_sethook installation, since
    // Lua only allows one hook callback + mask per lua_State; refresh_hook_mask()
    // merges whichever of these are currently active into one mask.
    // ---------------------------------------------------------------------------
    struct HookState
    {
        // Call tracing - no natural "disabled" sentinel value, so an empty
        // std::function (rather than a separate bool) IS the disabled state.
        LuaCallTraceCallback trace_callback;

        // Instruction counting - needs its own flag since instruction_count == 0
        // does not imply "disabled" (e.g. right after being (re)enabled).
        bool counting_enabled = false;
        std::uint64_t instruction_count = 0;

        // Instruction limit (protection) - 0 unambiguously means "no limit set".
        std::uint64_t instruction_limit = 0;

        // Recursion depth cap (protection) - 0 unambiguously means "no cap set".
        int max_depth = 0;

        // Shared call-depth bookkeeping used by both call tracing and the
        // recursion depth cap; not itself a feature flag.
        int call_depth = 0;

        // Shared LUA_MASKCOUNT period between instruction counting and the
        // instruction limit, since lua_sethook only accepts a single count
        // period per lua_State. See update_count_period().
        int count_period = 0;
    };
    HookState hooks;

    // Sink for print() output - see enable_output_capture/disable_output_capture.
    // Empty by default: print() output is discarded, never written to stdout.
    LuaOutputCallback output_callback;

    // Sink for internal LuaCpp error reports (currently: exceptions thrown by a
    // call-trace callback) - see enable_error_logging/disable_error_logging.
    // Deliberately independent of output_callback: enabling one does not imply
    // or require the other.
    LuaErrorLogCallback error_log_callback;

    // Backs set_memory_limit/get_memory_usage - see limited_alloc. Declared
    // here (before `state`, further down) so it is already default-constructed
    // by the time state's own member initializer calls lua_newstate(), which
    // immediately makes allocator calls through limited_alloc with ud == this.
    struct MemoryState
    {
        std::size_t bytes_used = 0;
        std::size_t limit = 0; // 0 = unlimited (default)
    };
    MemoryState memory;

    // Opens exactly the standard libraries selected by `libs`, using
    // luaL_requiref (matching what luaL_openlibs does internally for each
    // one) instead of the unconditional luaL_openlibs() call. Used by the
    // Lua(LuaLib) constructor - see there for the rationale.
    void open_selected_libs(LuaLib libs)
    {
        struct Entry
        {
            LuaLib flag;
            const char* name;
            lua_CFunction open_fn;
        };
        static constexpr Entry entries[] = {
        {LuaLib::Base, LUA_GNAME, luaopen_base},
        {LuaLib::Table, LUA_TABLIBNAME, luaopen_table},
        {LuaLib::String, LUA_STRLIBNAME, luaopen_string},
        {LuaLib::Math, LUA_MATHLIBNAME, luaopen_math},
        {LuaLib::Os, LUA_OSLIBNAME, luaopen_os},
        {LuaLib::Io, LUA_IOLIBNAME, luaopen_io},
        {LuaLib::Package, LUA_LOADLIBNAME, luaopen_package},
        {LuaLib::Debug, LUA_DBLIBNAME, luaopen_debug},
        {LuaLib::Coroutine, LUA_COLIBNAME, luaopen_coroutine},
        {LuaLib::Utf8, LUA_UTF8LIBNAME, luaopen_utf8},
        };

        for(const auto& entry : entries)
        {
            if(has_lib(libs, entry.flag))
            {
                luaL_requiref(*this, entry.name, entry.open_fn, 1);
                lua_pop(*this, 1); // pop the library table luaL_requiref leaves on the stack
            }
        }
    }

    // Recomputes the requested LUA_MASKCOUNT period as the minimum of whatever
    // instruction-counting and instruction-limit each currently want, since
    // lua_sethook only supports one count period per lua_State. Passing 0
    // means "this feature no longer has an opinion"; if neither feature wants
    // a period any more, count_period collapses back to 0 (harmless - it's
    // only used when LUA_MASKCOUNT is actually included in the mask).
    void update_count_period(int requested)
    {
        int period = 0;
        if(hooks.counting_enabled)
        {
            period = period == 0 ? requested : std::min(period, requested);
        }
        if(hooks.instruction_limit > 0)
        {
            period = period == 0 ? requested : std::min(period, requested);
        }
        hooks.count_period = period > 0 ? period : requested;
    }

    // Merges whichever hook-based features are currently active into a single
    // lua_sethook mask and (re)installs it - or removes the hook entirely when
    // nothing needs it. Must be called after any change to `hooks`.
    void refresh_hook_mask()
    {
        int mask = 0;
        if(hooks.trace_callback || hooks.max_depth > 0)
        {
            mask |= LUA_MASKCALL;
            if(hooks.trace_callback)
            {
                mask |= LUA_MASKRET;
            }
        }
        if(hooks.counting_enabled || hooks.instruction_limit > 0)
        {
            mask |= LUA_MASKCOUNT;
        }

        if(mask == 0)
        {
            lua_sethook(*this, nullptr, 0, 0);
        }
        else
        {
            lua_sethook(*this, &Lua::master_hook, mask, hooks.count_period);
        }
    }

    // Reports an internal LuaCpp problem (currently only: an exception thrown
    // from a user-supplied call-trace callback) via error_log_callback, if one
    // is registered. Silently dropped otherwise. Never throws.
    //
    // The message is prefixed with "[LuaCpp] " but is NOT newline-terminated -
    // the consumer's callback is responsible for its own line formatting
    // (e.g. appending '\n' before writing to a stream, or none at all if
    // appending to a structured log record).
    void log_error(std::string_view what)
    {
        if(!error_log_callback)
        {
            return;
        }
        std::string line = "[LuaCpp] ";
        line.append(what);
        try
        {
            error_log_callback(line);
        }
        catch(...)
        {
            // Logging itself must never throw back into hook code.
        }
    }

    // Best-effort function name for a call/return hook event; "?" when Lua
    // couldn't determine one (e.g. anonymous functions, tail calls).
    static std::string get_call_name(lua_State* L, lua_Debug* ar)
    {
        lua_getinfo(L, "nS", ar);
        return ar->name ? std::string(ar->name) : std::string("?");
    }

    // Single lua_sethook callback shared by call tracing, instruction counting,
    // the instruction limit, and the recursion depth cap. Recovers the owning
    // Lua instance via lua_getextraspace (set once in the constructor) and
    // dispatches by event type.
    static void master_hook(lua_State* L, lua_Debug* ar)
    {
        auto* self = *static_cast<Lua**>(lua_getextraspace(L));
        if(self)
        {
            self->dispatch_hook(L, ar);
        }
    }

    void dispatch_hook(lua_State* L, lua_Debug* ar)
    {
        switch(ar->event)
        {
            case LUA_HOOKCALL:
            case LUA_HOOKTAILCALL:
            {
                ++hooks.call_depth;
                if(hooks.trace_callback)
                {
                    try
                    {
                        hooks.trace_callback(LuaCallTraceEvent{get_call_name(L, ar), true, hooks.call_depth});
                    }
                    catch(const std::exception& e)
                    {
                        log_error(e.what());
                    }
                    catch(...)
                    {
                        log_error("unknown non-std::exception thrown");
                    }
                }
                if(hooks.max_depth > 0 && hooks.call_depth > hooks.max_depth)
                {
                    luaL_error(L, "recursion depth limit exceeded (%d)", hooks.max_depth);
                }
                break;
            }
            case LUA_HOOKRET:
            {
                if(hooks.trace_callback)
                {
                    try
                    {
                        hooks.trace_callback(LuaCallTraceEvent{get_call_name(L, ar), false, hooks.call_depth});
                    }
                    catch(const std::exception& e)
                    {
                        log_error(e.what());
                    }
                    catch(...)
                    {
                        log_error("unknown non-std::exception thrown");
                    }
                }
                --hooks.call_depth;
                break;
            }
            case LUA_HOOKCOUNT:
            {
                // This branch only fires while LUA_MASKCOUNT is installed, which only
                // happens when counting_enabled and/or instruction_limit is active (see
                // refresh_hook_mask) - so the counter must advance unconditionally here,
                // regardless of whether get_instruction_count() reporting is enabled.
                // Otherwise set_instruction_limit() alone (without also calling
                // enable_instruction_counting()) would never observe progress and an
                // infinite loop would never be aborted.
                hooks.instruction_count += static_cast<std::uint64_t>(hooks.count_period);
                if(hooks.instruction_limit > 0 && hooks.instruction_count >= hooks.instruction_limit)
                {
                    // Lua's luaL_error/lua_pushvfstring only understands its own small
                    // format subset (%d, %s, %f, %p, %c, %U, %I, %%) - NOT the C/glibc
                    // %llu specifier, which would otherwise leak into the error message
                    // verbatim instead of being substituted. Format the number in C++
                    // first and pass it through as a plain %s.
                    luaL_error(L, "instruction limit exceeded (%s)", std::to_string(hooks.instruction_limit).c_str());
                }
                break;
            }
            default:
                break;
        }
    }

    // Replacement for the Lua global `print`, installed by the constructor.
    // Formats arguments exactly like Lua's own print() (tab-separated,
    // honouring __tostring via luaL_tolstring, newline-terminated) but sends
    // the result to output_callback instead of stdout. If output capture has
    // not been enabled (output_callback is empty), the call is a no-op and no
    // formatting work is done at all - print() output goes nowhere by default.
    static int captured_print(lua_State* L)
    {
        auto* self = *static_cast<Lua**>(lua_getextraspace(L));
        if(!self || !self->output_callback)
        {
            return 0;
        }

        std::string line;
        const int n = lua_gettop(L);
        for(int i = 1; i <= n; ++i)
        {
            std::size_t len = 0;
            const char* s = luaL_tolstring(L, i, &len);
            if(i > 1)
            {
                line += '\t';
            }
            line.append(s, len);
            lua_pop(L, 1); // pop the string luaL_tolstring pushed
        }
        line += '\n';

        try
        {
            self->output_callback(line);
        }
        catch(...)
        {
            // Never let a user callback's exception propagate into Lua's C call chain.
        }
        return 0;
    }

    // Custom lua_Alloc used in place of Lua's default allocator (l_alloc), so
    // every allocation/reallocation/free can be tracked and, optionally,
    // capped via set_memory_limit(). `ud` is the owning Lua* (passed to
    // lua_newstate as the userdata argument in the constructor).
    //
    // Per the lua_Alloc manual entry: when `ptr` is nullptr, `osize` is NOT a
    // real byte count - it's a type tag (e.g. LUA_TSTRING) identifying what
    // kind of object is being allocated for the first time. Only `osize` is
    // meaningful as a byte count when `ptr` is non-null (a real, previously-
    // tracked allocation is being resized or freed).
    static void* limited_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize)
    {
        auto* self = static_cast<Lua*>(ud);
        const std::size_t old_size = ptr ? osize : 0;

        if(nsize == 0)
        {
            std::free(ptr);
            self->memory.bytes_used -= old_size;
            return nullptr;
        }

        if(self->memory.limit > 0)
        {
            const std::size_t projected = self->memory.bytes_used - old_size + nsize;
            if(projected > self->memory.limit)
            {
                // Rejecting here makes Lua treat this exactly like a real
                // out-of-memory condition: it raises a catchable "not enough
                // memory" error that lua_pcall (run_script/call<>) reports via
                // the usual {false, msg} tuple - the existing allocation at
                // `ptr` (if any) is left untouched and still valid.
                self->log_error("memory limit exceeded: refused allocation of " + std::to_string(nsize) +
                                " byte(s) (limit " + std::to_string(self->memory.limit) + ", currently " +
                                std::to_string(self->memory.bytes_used) + " byte(s) in use)");
                return nullptr;
            }
        }

        void* new_ptr = std::realloc(ptr, nsize);
        if(!new_ptr)
        {
            // Genuine system OOM, not our own cap - still logged for visibility.
            self->log_error("memory allocation failed: system out of memory (requested " + std::to_string(nsize) +
                            " byte(s))");
            return nullptr;
        }

        self->memory.bytes_used = self->memory.bytes_used - old_size + nsize;
        return new_ptr;
    }

    // Replaces the panic handler luaL_newstate would otherwise install (we use
    // lua_newstate + our own limited_alloc instead of luaL_newstate, so that
    // default handler is never installed automatically). Mirrors Lua's own
    // default panic behaviour (report the unprotected error) but routes the
    // message through log_error instead of writing directly to stderr. Only
    // fires for errors raised with no lua_pcall protection anywhere on the
    // call stack - should not normally trigger, since run_script/call<> always
    // go through lua_pcall.
    static int panic(lua_State* L)
    {
        auto* self = *static_cast<Lua**>(lua_getextraspace(L));

        const char* msg = lua_tostring(L, -1);
        if(!msg)
        {
            msg = "error object is not a string";
        }

        if(self)
        {
            self->log_error(std::string("PANIC: unprotected error in call to Lua API (") + msg + ")");
        }
        return 0;
    }

    // Builds the typed trampoline closure shared by both expose_func overloads.
    // `keep_alive` is captured by value inside the closure purely to extend the
    // lifetime of an owning object that `func`'s captures may reference; it is
    // never otherwise accessed. Pass NoOwner{} when there is nothing to keep
    // alive - see the expose_func(name, owner, func) overload for the rationale.
    template <typename... ReturnTypes, typename... Args, typename KeepAlive>
    static std::unique_ptr<LuaFunc> make_func_wrapper(std::function<lua_return_t<ReturnTypes...>(Args...)> func,
                                                      KeepAlive keep_alive)
    {
        // Typed wrapper: validates argument count, reads typed arguments, invokes
        // the C++ function, and pushes return values onto the Lua stack.
        // Throws std::runtime_error on argument count or type mismatch.
        return std::make_unique<LuaFunc>(
        [func = std::move(func), keep_alive = std::move(keep_alive)](lua_State* L) -> int
        {
            (void)keep_alive; // captured only to extend the owner's lifetime
            constexpr int expected = static_cast<int>(sizeof...(Args));
            if(const int got = lua_gettop(L); got != expected)
            {
                throw std::runtime_error("expected " + std::to_string(expected) + " argument(s), got " +
                                         std::to_string(got));
            }

            auto args = collect_args_from<Args...>(L, 1, std::index_sequence_for<Args...>{});

            if constexpr(sizeof...(ReturnTypes) == 0)
            {
                std::apply(func, std::move(args));
                return 0;
            }
            else if constexpr(sizeof...(ReturnTypes) == 1)
            {
                push(L, std::apply(func, std::move(args)));
                return 1;
            }
            else
            {
                auto results = std::apply(func, std::move(args));
                push_results(L, results, std::index_sequence_for<ReturnTypes...>{});
                return static_cast<int>(sizeof...(ReturnTypes));
            }
        });
    }

    // Transfers ownership of `wrapper` into registered_funcs (as a shared_ptr)
    // and registers a trampoline closure holding a weak_ptr to it as a Lua
    // global named `name` - see push_weak_upvalue for the rationale.
    void register_global_func(const char* name, std::unique_ptr<LuaFunc> wrapper)
    {
        // Transfer ownership before touching the Lua stack so that an allocation
        // failure leaves Lua's state untouched.
        std::shared_ptr<LuaFunc> fn(std::move(wrapper));
        registered_funcs.push_back(fn);
        registered_global_names.emplace_back(name);

        push_weak_upvalue(*this, fn);
        lua_pushcclosure(*this, &Lua::trampoline, 1);
        write_real_global(name); // bypass protection - this is a trusted C++ call
        protected_globals.insert(name); // protect it from script-side overwrite by default
    }

    // Builds the typed trampoline closure shared by both expose_method overloads.
    // See make_func_wrapper for the meaning of `keep_alive`.
    template <typename StructType, typename... ReturnTypes, typename... Args, typename KeepAlive>
    static std::unique_ptr<LuaFunc> make_method_wrapper(
    std::function<lua_return_t<ReturnTypes...>(StructType, Args...)> func,
    KeepAlive keep_alive)
    {
        return std::make_unique<LuaFunc>(
        [func = std::move(func), keep_alive = std::move(keep_alive)](lua_State* L) -> int
        {
            (void)keep_alive; // captured only to extend the owner's lifetime
            constexpr int n_method_args = static_cast<int>(sizeof...(Args));
            constexpr int expected = n_method_args + 1; // +1 for implicit self
            if(const int got = lua_gettop(L); got != expected)
            {
                throw std::runtime_error("expected " + std::to_string(n_method_args) + " argument(s), got " +
                                         std::to_string(got - 1));
            }

            auto self = read<StructType>(L, 1);
            auto args = collect_args_from<Args...>(L, 2, std::index_sequence_for<Args...>{});
            auto all_args = std::tuple_cat(std::make_tuple(std::move(self)), std::move(args));

            if constexpr(sizeof...(ReturnTypes) == 0)
            {
                std::apply(func, std::move(all_args));
                return 0;
            }
            else if constexpr(sizeof...(ReturnTypes) == 1)
            {
                push(L, std::apply(func, std::move(all_args)));
                return 1;
            }
            else
            {
                auto results = std::apply(func, std::move(all_args));
                push_results(L, results, std::index_sequence_for<ReturnTypes...>{});
                return static_cast<int>(sizeof...(ReturnTypes));
            }
        });
    }

    // Positive indexes are from the bottom of the stack, negative indexes are
    // from the top of the stack. -1 = top, -2 = second from top, etc.
    //  1 = bottom, 2 = second from bottom, etc.
    static constexpr int TOP_OF_STACK = -1;

    const std::string get_error_message()
    {
        const std::string error = luaL_tolstring(*this, TOP_OF_STACK, nullptr);
        lua_pop(*this, 2); // pop original error object + string pushed by luaL_tolstring
        return error;
    }

    operator lua_State*() const
    {
        return state.get();
    }

    // Owns the shared references to registered LuaFunc trampolines. Declared
    // before state so it is destroyed after lua_close(). Stored as shared_ptr
    // (not unique_ptr) because the Lua closure itself only holds a
    // std::weak_ptr<LuaFunc> upvalue (see push_weak_upvalue / trampoline): if a
    // stale closure ever survives past this container's own destruction (e.g.
    // a coroutine resumed after teardown - see LIFETIME.md item 5), calling it
    // observes an expired weak_ptr and raises a catchable Lua error instead of
    // dereferencing freed memory.
    std::list<std::shared_ptr<LuaFunc>> registered_funcs;

    // Names registered as Lua globals via expose_func/register_global_func -
    // used by close() to nil them out. expose_method/expose_mutable_method
    // entries are intentionally NOT tracked here; they live in per-type
    // metatables in the Lua registry, not as globals - see close().
    std::vector<std::string> registered_global_names;

    // Names that script-side writes are currently rejected for - see
    // protect_global/unprotect_global and setup_global_protection_proxy().
    // Every assign()/expose_func() registration inserts its name here
    // automatically; entries are removed again by unprotect_global(),
    // close() (for expose_func names), and sandbox_deny() (for a bare name
    // it nils out).
    std::unordered_set<std::string> protected_globals;

    // Set by close(); guards against redundant nil-ing/GC work if close() is
    // called more than once. See close() for the full rationale.
    bool closed = false;

    // Uses lua_newstate (not luaL_newstate) with limited_alloc as the custom
    // allocator and `this` as its userdata, so every allocation is tracked and
    // optionally capped via set_memory_limit(). This is why the constructor
    // installs the panic handler itself (see Lua()) instead of relying on
    // luaL_newstate's automatic one.
    LuaStatePtr state{lua_newstate(&Lua::limited_alloc, this, luaL_makeseed(nullptr)),
                      [](lua_State* L) { lua_close(L); }};

    // ---------------------------------------------------------------------------
    // Read-only C++ globals (script-side write protection for assign()/
    // expose_func()-registered globals)
    //
    // Lua only consults a table's __newindex metamethod for keys that don't
    // already raw-exist in it (lua_load in lapi.c reads the globals table
    // from the registry - LUA_RIDX_GLOBALS - at chunk-load time to bind each
    // chunk's _ENV upvalue). Because assign()/expose_func() write directly
    // into the real globals table, a metamethod attached to THAT table would
    // never fire for a second write to an already-existing key - exactly the
    // case that needs to be caught. So instead, the real globals table is
    // renamed internally ("real_G", kept reachable via a private registry
    // key) and a brand-new, permanently-EMPTY proxy table is installed in
    // its place as LUA_RIDX_GLOBALS. Because the proxy never gains raw
    // entries of its own, its __index/__newindex metamethods fire for every
    // single global read/write from Lua code, without exception - __index
    // transparently forwards reads to real_G; __newindex
    // (protected_newindex) rejects writes to any name in protected_globals
    // and otherwise writes through to real_G. All C++-driven global writes
    // (assign/register_global_func/close()/sandbox_deny) go through
    // write_real_global() instead of lua_setglobal(), bypassing the proxy
    // entirely - protection only ever blocks *script*-side writes, never
    // LuaCpp's own.
    // ---------------------------------------------------------------------------

    // Stable registry key under which the real globals table is stashed once
    // the proxy is installed - see setup_global_protection_proxy().
    static const void* real_globals_key()
    {
        static const char sentinel = 0;
        return &sentinel;
    }

    // Writes the value currently on top of the stack directly into the real
    // (unproxied) globals table under `name`, consuming that value. Used for
    // every C++-driven global write so protection never blocks LuaCpp's own
    // writes, only script-side ones.
    void write_real_global(const char* name)
    {
        lua_rawgetp(*this, LUA_REGISTRYINDEX, real_globals_key()); // stack: [value, real_G]
        lua_insert(*this, -2); // stack: [real_G, value]
        lua_setfield(*this, -2, name); // real_G[name] = value; pops value; real_G has no
                                       // metatable of its own, so this is effectively raw
        lua_pop(*this, 1); // pop real_G
    }

    // Snapshots the current (real) globals table under a private registry
    // key, then installs a fresh, permanently-empty proxy table as the
    // "globals table" that lua_getglobal/lua_setglobal and every future
    // chunk's _ENV actually operate on - see the block comment above.
    // Called once, at the end of construction, after every library and
    // `print` have already been installed into the real table.
    void setup_global_protection_proxy()
    {
        lua_rawgeti(*this, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS); // push current (real) globals table
        lua_rawsetp(*this, LUA_REGISTRYINDEX, real_globals_key()); // registry[key] = real_G; pops it

        lua_newtable(*this); // proxy
        lua_newtable(*this); // metatable
        lua_rawgetp(*this, LUA_REGISTRYINDEX, real_globals_key());
        lua_setfield(*this, -2, "__index"); // metatable.__index = real_G
        lua_pushcfunction(*this, &Lua::protected_newindex);
        lua_setfield(*this, -2, "__newindex"); // metatable.__newindex = protected_newindex
        lua_setmetatable(*this, -2); // set metatable on proxy; pops metatable
        lua_rawseti(*this, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS); // proxy becomes THE globals table; pops it
    }

    // __newindex metamethod for the globals proxy installed by
    // setup_global_protection_proxy(). Standard __newindex signature:
    // argument 1 = table (the proxy), 2 = key, 3 = value. Raises a catchable
    // Lua error (also reported via log_error) for any string key present in
    // protected_globals; otherwise writes through to the real globals table.
    static int protected_newindex(lua_State* L)
    {
        auto* self = *static_cast<Lua**>(lua_getextraspace(L));

        if(self && lua_type(L, 2) == LUA_TSTRING)
        {
            std::size_t len = 0;
            const char* key = lua_tolstring(L, 2, &len);
            std::string name(key, len);
            if(self->protected_globals.count(name) > 0)
            {
                auto msg = "attempt to modify protected global '" + name + "'";
                self->log_error(msg);
                return luaL_error(L, "%s", msg.c_str());
            }
        }

        lua_rawgetp(L, LUA_REGISTRYINDEX, real_globals_key()); // stack: [table, key, value, real_G]
        lua_pushvalue(L, 2); // key
        lua_pushvalue(L, 3); // value
        lua_rawset(L, -3); // real_G[key] = value
        lua_pop(L, 1); // pop real_G
        return 0;
    }

    // ---------------------------------------------------------------------------
    // Stack I/O - static so they work both from instance methods and from inside
    // expose_func closures that only have a bare lua_State *.
    // ---------------------------------------------------------------------------

    template <typename T>
    static T read(lua_State* L, int index)
    {
        const auto actual = std::string(lua_typename(L, lua_type(L, index)));
        if constexpr(std::is_same_v<T, bool>)
        {
            if(!lua_isboolean(L, index))
            {
                throw std::runtime_error("expected boolean, got " + actual);
            }
            return lua_toboolean(L, index) != 0;
        }
        else if constexpr(std::is_integral_v<T>)
        {
            // lua_isinteger returns false for Lua floats, even whole-number ones
            // such as 1.0. Use a floating-point ReturnType if the Lua expression
            // may yield a float.
            if(!lua_isinteger(L, index))
            {
                throw std::runtime_error("expected integer, got " + actual);
            }
            return static_cast<T>(lua_tointeger(L, index));
        }
        else if constexpr(std::is_floating_point_v<T>)
        {
            // lua_isnumber returns true for Lua integers as well as floats, so
            // integer values are silently widened to the C++ floating-point type.
            if(!lua_isnumber(L, index))
            {
                throw std::runtime_error("expected number, got " + actual);
            }
            return static_cast<T>(lua_tonumber(L, index));
        }
        else if constexpr(std::is_same_v<T, std::string>)
        {
            // lua_isstring returns true for Lua numbers (implicit coercion), so a
            // Lua integer or float value can be read back as std::string.
            // lua_tolstring is used (not lua_tostring) so that the explicit byte
            // length is obtained; this preserves embedded null bytes that
            // std::string{lua_tostring(...)} would otherwise truncate.
            if(!lua_isstring(L, index))
            {
                throw std::runtime_error("expected string, got " + actual);
            }
            std::size_t len = 0;
            const char* ptr = lua_tolstring(L, index, &len);
            return {ptr, len};
        }
        else if constexpr(std::is_same_v<T, const char*>)
        {
            static_assert(!std::is_same_v<T, T>, "read: const char* is unsafe as a return type - "
                                                 "lua_tostring returns a pointer into Lua-managed memory "
                                                 "that becomes invalid after the value is popped from the "
                                                 "stack. Use std::string instead.");
        }
        else if constexpr(has_lua_fields<T>::value)
        {
            if(!lua_istable(L, index))
            {
                throw std::runtime_error("expected table, got " + actual);
            }
            // T result{} zero-initialises all fields before individual reads.
            // If a registered field is absent from the Lua table, lua_getfield
            // returns nil and read<Member> will throw a type error. All registered
            // fields must be present and correctly typed in the Lua table.
            T result{};
            std::apply([&](const auto&... fields) { (read_struct_field(L, index, result, fields), ...); },
                       LuaFields<T>::value);
            return result;
        }
        else if constexpr(is_std_vector<T>::value)
        {
            if(!lua_istable(L, index))
            {
                throw std::runtime_error("expected table, got " + actual);
            }

            T result;
            const lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, index));
            result.reserve(static_cast<std::size_t>(len));
            for(lua_Integer i = 1; i <= len; ++i)
            {
                lua_rawgeti(L, index, i);
                result.push_back(read<typename T::value_type>(L, -1));
                lua_pop(L, 1);
            }
            return result;
        }
        else if constexpr(is_std_map<T>::value)
        {
            if(!lua_istable(L, index))
            {
                throw std::runtime_error("expected table, got " + actual);
            }

            T result;
            const int abs_idx = lua_absindex(L, index);
            lua_pushnil(L); // first key for lua_next
            while(lua_next(L, abs_idx) != 0)
            {
                // key at -2, value at -1
                // push a copy of the key so lua_tostring cannot mutate the original
                // during traversal (lua manual: do not call lua_tolstring on a key directly)
                lua_pushvalue(L, -2);
                auto key = read<typename T::key_type>(L, -1);
                lua_pop(L, 1); // pop key copy
                auto val = read<typename T::mapped_type>(L, -1);
                lua_pop(L, 1); // pop value; leave original key for next iteration
                result.emplace(std::move(key), std::move(val));
            }
            return result;
        }
        else
        {
            static_assert(!std::is_same_v<T, T>, "read: unsupported type");
        }
    }

    template <typename T>
    static void push(lua_State* L, const T& value)
    {
        if constexpr(std::is_same_v<T, bool>)
        {
            lua_pushboolean(L, value ? 1 : 0);
        }
        else if constexpr(std::is_integral_v<T>)
        {
            lua_pushinteger(L, static_cast<lua_Integer>(value));
        }
        else if constexpr(std::is_floating_point_v<T>)
        {
            lua_pushnumber(L, static_cast<lua_Number>(value));
        }
        else if constexpr(std::is_same_v<T, std::string>)
        {
            lua_pushlstring(L, value.c_str(), value.size());
        }
        else if constexpr(std::is_same_v<T, const char*>)
        {
            lua_pushstring(L, value);
        }
        else if constexpr(has_lua_fields<T>::value)
        {
            lua_newtable(L);
            std::apply([&](const auto&... fields) { (push_struct_field(L, value, fields), ...); }, LuaFields<T>::value);
            attach_methods_if_any<T>(L);
        }
        else if constexpr(is_std_vector<T>::value)
        {
            lua_newtable(L);
            for(lua_Integer i = 0; i < static_cast<lua_Integer>(value.size()); ++i)
            {
                push(L, value[static_cast<std::size_t>(i)]);
                lua_rawseti(L, -2, i + 1); // 1-indexed; table is at -2 after element push
            }
        }
        else if constexpr(is_std_map<T>::value)
        {
            lua_newtable(L);
            for(const auto& [k, v] : value)
            {
                push(L, k);
                push(L, v);
                lua_rawset(L, -3); // table is at -3 after pushing key and value
            }
        }
        else
        {
            static_assert(!std::is_same_v<T, T>, "push: unsupported type");
        }
    }

    // Decays array types (e.g. const char[N] from string literals) to pointers
    // before they reach push(). Other types are forwarded unchanged.
    // Needed because const T& and T&& bind without array-to-pointer decay.
    template <typename T>
    static decltype(auto) decay_for_push(T&& value)
    {
        if constexpr(std::is_array_v<std::remove_reference_t<T>>)
        {
            return static_cast<std::decay_t<std::remove_reference_t<T>>>(std::forward<T>(value));
        }
        else
        {
            return std::forward<T>(value);
        }
    }

    // Reads N typed arguments from the Lua stack starting at position `base`.
    // expose_func passes base=1 (args at 1..N).
    // expose_method / expose_mutable_method pass base=2 (self at 1, args at 2..N+1).
    template <typename... Args, std::size_t... Is>
    static std::tuple<Args...> collect_args_from([[maybe_unused]] lua_State* L, int base, std::index_sequence<Is...>)
    {
        return {read<Args>(L, base + static_cast<int>(Is))...};
    }

    // Pushes each element of a tuple onto the Lua stack in order.
    // Used by expose_func to return multiple values.
    template <typename Tuple, std::size_t... Is>
    static void push_results(lua_State* L, const Tuple& t, std::index_sequence<Is...>)
    {
        (push(L, std::get<Is>(t)), ...);
    }

    // Reads N return values from the top of the Lua stack into a typed tuple.
    // Used by call() to collect results after lua_pcall.
    template <typename... ReturnTypes, std::size_t... Is>
    std::tuple<ReturnTypes...> collect(std::index_sequence<Is...>) const
    {
        constexpr int N = static_cast<int>(sizeof...(ReturnTypes));
        // Stack layout after lua_pcall (N=3): [-3]=first, [-2]=second, [-1]=last
        // index_sequence {0,1,2} maps to stack indices {0-3, 1-3, 2-3} = {-3,-2,-1}
        return {read<ReturnTypes>(state.get(), static_cast<int>(Is) - N)...};
    }

    // Pushes one field of a registered struct as a named Lua table entry.
    template <typename Struct, typename Member>
    static void push_struct_field(lua_State* L, const Struct& s, const LuaField<Struct, Member>& f)
    {
        push(L, s.*f.ptr);
        lua_setfield(L, -2, f.name); // table is at -2 after the field value was pushed
    }

    // Reads one named table entry into a field of a registered struct.
    template <typename Struct, typename Member>
    static void read_struct_field(lua_State* L, int idx, Struct& s, const LuaField<Struct, Member>& f)
    {
        lua_getfield(L, idx, f.name); // pushes field value; idx still refers to the table
        s.*f.ptr = read<Member>(L, -1);
        lua_pop(L, 1);
    }

    // Writes one field from a C++ struct back into an existing Lua table at idx.
    // The counterpart of push_struct_field; used by expose_mutable_method to
    // propagate self mutations back to the Lua-side table.
    template <typename Struct, typename Member>
    static void write_struct_field_back(lua_State* L, int idx, const Struct& s, const LuaField<Struct, Member>& f)
    {
        push(L, s.*f.ptr);
        lua_setfield(L, idx, f.name); // table[f.name] = value; pops the value
    }

    // Writes all registered fields of s back into the Lua table at idx.
    // Used by expose_mutable_method after calling the C++ handler so that any
    // modifications made to self are reflected in the caller's Lua table.
    // Only fields declared in LuaFields<T> are written; extra keys that Lua
    // code may have added to the table are left untouched.
    template <typename T>
    static void write_struct_back(lua_State* L, int idx, const T& s)
    {
        const int abs = lua_absindex(L, idx);
        std::apply([&](const auto&... fields) { (write_struct_field_back(L, abs, s, fields), ...); },
                   LuaFields<T>::value);
    }

    // ---------------------------------------------------------------------------
    // Struct method dispatch
    // ---------------------------------------------------------------------------

    // Non-capturing lua_CFunction used as the trampoline for both expose_func
    // and expose_method/expose_mutable_method closures.
    //
    // Reads a std::weak_ptr<LuaFunc> from upvalue slot 1 (a full userdata, not
    // light userdata - see push_weak_upvalue) and attempts to lock() it before
    // calling. If the referenced LuaFunc no longer exists the weak_ptr is
    // expired and a Lua error is raised instead of dereferencing freed memory.
    //
    // Calling lua_error()/luaL_error() directly from within this try/catch
    // frame is safe: Lua is compiled as C++ WITHOUT LUA_USE_LONGJMP, so
    // lua_error() propagates via a genuine C++ exception (`throw(lua_longjmp*)`
    // in ldo.c) rather than a raw longjmp. A real C++ throw correctly unwinds
    // this frame - including running fn's destructor - and is safe to
    // propagate through an unrelated try/catch(const std::exception&) block
    // regardless of the compiler's exception model (/EHsc or /EHa). This used
    // to be split into trampoline()/trampoline_impl()/call_impl() to keep
    // lua_error() out of any frame with a live try/catch, back when Lua used
    // setjmp/longjmp for errors and MSVC's default /EHsc made longjmp-across-
    // try/catch corrupt the SEH handler chain. Neither condition applies
    // anymore (see the /EHa fix and the removal of LUA_USE_LONGJMP elsewhere
    // in this file/CMakeLists.txt), so the split was removed.
    static int trampoline(lua_State* L)
    {
        auto* self = *static_cast<Lua**>(lua_getextraspace(L));
        auto* weak = static_cast<std::weak_ptr<LuaFunc>*>(lua_touserdata(L, lua_upvalueindex(1)));
        auto fn = weak->lock();
        if(!fn)
        {
            const char* msg = "stale function reference: the registered C++ callable no longer exists";
            if(self)
            {
                self->log_error(msg);
            }
            return luaL_error(L, "%s", msg);
        }

        try
        {
            return (*fn)(L);
        }
        catch(const std::exception& e)
        {
            if(self)
            {
                self->log_error(e.what());
            }
            return luaL_error(L, "%s", e.what());
        }
    }

    // GC metamethod for the weak upvalue userdata created by push_weak_upvalue.
    // Destroys the std::weak_ptr<LuaFunc> in place; the pointee LuaFunc itself
    // (if still alive) is owned independently by registered_funcs and is
    // unaffected by this.
    static int weak_upvalue_gc(lua_State* L)
    {
        auto* weak = static_cast<std::weak_ptr<LuaFunc>*>(lua_touserdata(L, 1));
        weak->~weak_ptr<LuaFunc>();
        return 0;
    }

    // Pushes a full-userdata upvalue holding a std::weak_ptr<LuaFunc> referring
    // to `fn`. Full userdata (with a __gc metamethod) is used instead of light
    // userdata so the upvalue never holds a raw pointer into registered_funcs:
    // if the LuaFunc is ever destroyed while a stale closure is still
    // reachable from Lua, trampoline() observes the expired weak_ptr and
    // raises a catchable Lua error instead of dereferencing freed memory.
    static void push_weak_upvalue(lua_State* L, const std::shared_ptr<LuaFunc>& fn)
    {
        void* mem = lua_newuserdatauv(L, sizeof(std::weak_ptr<LuaFunc>), 0);
        new(mem) std::weak_ptr<LuaFunc>(fn);

        if(luaL_newmetatable(L, "LuaCpp.WeakLuaFunc")) // created once per lua_State
        {
            lua_pushcfunction(L, &Lua::weak_upvalue_gc);
            lua_setfield(L, -2, "__gc");
        }
        lua_setmetatable(L, -2);
    }

    // Returns a unique void* identifying type T in the Lua registry.
    // The address of the function-local static is stable for the process lifetime
    // and unique per template instantiation.
    template <typename T>
    static const void* get_type_key()
    {
        static const char sentinel = 0;
        return &sentinel;
    }

    // Looks up (or creates) the per-type metatable for T in the Lua registry and
    // adds a trampoline closure holding a weak_ptr to `fn` to its __index
    // sub-table under `name` - see push_weak_upvalue for the rationale.
    // Leaves the Lua stack balanced.
    template <typename T>
    static void add_method_to_registry(lua_State* L, const char* name, const std::shared_ptr<LuaFunc>& fn)
    {
        lua_pushlightuserdata(L, const_cast<void*>(get_type_key<T>()));
        lua_rawget(L, LUA_REGISTRYINDEX); // push metatable or nil

        if(lua_isnil(L, -1))
        {
            lua_pop(L, 1);

            lua_newtable(L); // metatable
            lua_newtable(L); // __index table
            lua_setfield(L, -2, "__index"); // metatable.__index = {}

            // Setting __metatable makes this metatable opaque to Lua code:
            // getmetatable(instance) returns this sentinel value instead of the
            // real metatable, and setmetatable(instance, ...) raises "cannot
            // change a protected metatable" (built into Lua's own
            // luaB_setmetatable) - so a script can neither read nor replace an
            // instance's metatable, and therefore can never reach the shared
            // __index table to overwrite/add methods for the type. Note: the
            // debug library intentionally bypasses __metatable
            // (debug.getmetatable/debug.setmetatable) - exclude LuaLib::Debug
            // (see Sandboxing) if this protection must hold against untrusted
            // scripts.
            lua_pushstring(L, typeid(T).name());
            lua_setfield(L, -2, "__metatable");

            lua_pushlightuserdata(L, const_cast<void*>(get_type_key<T>()));
            lua_pushvalue(L, -2); // duplicate metatable
            lua_rawset(L, LUA_REGISTRYINDEX); // registry[type_key] = metatable
            // stack: [metatable]
        }
        // stack: [metatable]

        lua_getfield(L, -1, "__index"); // stack: [metatable, __index_table]

        // Guard against duplicate registration: if `name` already exists in the
        // __index table a second expose_method / expose_mutable_method call with
        // the same name would silently overwrite the first.  Throw instead so the
        // mistake is caught at registration time rather than producing mysterious
        // runtime behaviour.
        lua_getfield(L, -1, name); // stack: [metatable, __index_table, existing_or_nil]
        const bool duplicate = !lua_isnil(L, -1);
        lua_pop(L, 1); // stack: [metatable, __index_table]
        if(duplicate)
        {
            lua_pop(L, 2); // restore stack balance before throwing
            auto msg = std::string("method '") + name + "' is already registered on this type";
            auto* self = *static_cast<Lua**>(lua_getextraspace(L));
            if(self)
            {
                self->log_error(msg);
            }
            throw std::runtime_error(msg);
        }

        push_weak_upvalue(L, fn);
        lua_pushcclosure(L, &Lua::trampoline, 1); // stack: [metatable, __index_table, closure]
        lua_setfield(L, -2, name); // __index_table[name] = closure

        lua_pop(L, 2); // pop __index_table and metatable
    }

    // Attaches T's method metatable (if any) to the table currently on top of the
    // stack. Looks up the Lua registry; no-op when no methods have been registered
    // for T yet.
    // Instances pushed before any expose_method / expose_mutable_method call for T
    // will not have a metatable and cannot use colon-call syntax. Always register
    // all methods before pushing instances of the type.
    template <typename T>
    static void attach_methods_if_any(lua_State* L)
    {
        lua_pushlightuserdata(L, const_cast<void*>(get_type_key<T>()));
        lua_rawget(L, LUA_REGISTRYINDEX); // push metatable or nil
        if(lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            return;
        }
        // metatable at -1, struct table at -2
        lua_setmetatable(L, -2); // consumes the metatable
    }
};
