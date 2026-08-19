extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
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

class Lua final
{
  public:
	Lua()
	{
		luaL_openlibs(*this);
	}

	~Lua() = default;

	Lua(const Lua&) = delete;
	Lua& operator=(const Lua&) = delete;
	Lua(Lua&&) = delete;
	Lua& operator=(Lua&&) = delete;

	std::tuple<bool, std::string> run_script(const char* script)
	{
		// Use luaL_loadstring + lua_pcall(L,0,0,0) to discard script return values and prevent stack
		// growth on repeated calls
		if(luaL_loadstring(*this, script) != LUA_OK)
		{
			return {false, get_error_message()};
		}
		if(lua_pcall(*this, 0, 0, 0) != LUA_OK)
		{
			return {false, get_error_message()};
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
		lua_setglobal(*this, name);
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
			return {false, "Not a function: " + std::string(func), ReturnTypes{}...};
		}

		(push(state.get(), decay_for_push(std::forward<Args>(args))),
		 ...); // C++17 fold expression to push all arguments onto the Lua stack

		const auto arg_count = static_cast<int>(sizeof...(Args));
		const auto ret_count = static_cast<int>(sizeof...(ReturnTypes));

		if(lua_pcall(*this, arg_count, ret_count, 0) != LUA_OK)
		{
			return {false, get_error_message(), ReturnTypes{}...};
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
				throw std::runtime_error("expected " + std::to_string(n_method_args) + " argument(s), got " +
				                         std::to_string(got - 1));

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
				throw std::runtime_error("expected " + std::to_string(expected) + " argument(s), got " +
				                         std::to_string(got));

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

		push_weak_upvalue(*this, fn);
		lua_pushcclosure(*this, &Lua::trampoline, 1);
		lua_setglobal(*this, name);
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
				throw std::runtime_error("expected " + std::to_string(n_method_args) + " argument(s), got " +
				                         std::to_string(got - 1));

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

	LuaStatePtr state{luaL_newstate(), [](lua_State* L) { lua_close(L); }};

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
				throw std::runtime_error("expected boolean, got " + actual);
			return lua_toboolean(L, index) != 0;
		}
		else if constexpr(std::is_integral_v<T>)
		{
			// lua_isinteger returns false for Lua floats, even whole-number ones
			// such as 1.0. Use a floating-point ReturnType if the Lua expression
			// may yield a float.
			if(!lua_isinteger(L, index))
				throw std::runtime_error("expected integer, got " + actual);
			return static_cast<T>(lua_tointeger(L, index));
		}
		else if constexpr(std::is_floating_point_v<T>)
		{
			// lua_isnumber returns true for Lua integers as well as floats, so
			// integer values are silently widened to the C++ floating-point type.
			if(!lua_isnumber(L, index))
				throw std::runtime_error("expected number, got " + actual);
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
				throw std::runtime_error("expected string, got " + actual);
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
			return static_cast<std::decay_t<std::remove_reference_t<T>>>(std::forward<T>(value));
		else
			return std::forward<T>(value);
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

	// Does the actual call + exception handling. Contains a try/catch, so this
	// frame must NEVER call lua_error()/longjmp itself: on MSVC, C++ exception
	// handling is implemented via SEH, and a longjmp that unwinds past a frame
	// which registered a try/catch handler corrupts the thread's SEH handler
	// chain - even though the catch has already run to completion. This does
	// not crash immediately; it silently breaks stack/error handling for
	// later, unrelated Lua calls on the same thread (observed as later calls
	// failing on Windows CI only, since gcc/Linux's exception ABI tolerates
	// it). Returns normally in all cases; the caller (trampoline) is the one
	// that may call lua_error(), from a frame with no C++ EH handler of its
	// own.
	//
	// Reads a std::weak_ptr<LuaFunc> from upvalue slot 1 (a full userdata, not
	// light userdata - see push_weak_upvalue) and attempts to lock() it before
	// calling. If the referenced LuaFunc no longer exists the weak_ptr is
	// expired and an error is reported via err_msg instead of dereferencing
	// freed memory.
	static int trampoline_impl(lua_State* L, std::weak_ptr<LuaFunc>* weak, std::string& err_msg)
	{
		auto fn = weak->lock();
		if(!fn)
		{
			err_msg = "stale function reference: the registered C++ callable no longer exists";
			return -1;
		}
		try
		{
			return (*fn)(L);
		}
		catch(const std::exception& e)
		{
			err_msg = e.what();
			return -1;
		}
		// fn (a std::shared_ptr) is destroyed here via normal C++ return - no
		// longjmp involved in this frame, so this is always safe.
	}

	// Non-capturing lua_CFunction used as the trampoline for both expose_func
	// and expose_method/expose_mutable_method closures. Delegates to
	// trampoline_impl() for the actual call and exception handling, and only
	// calls lua_error() here, in a frame with no C++ try/catch of its own -
	// see trampoline_impl for why that split matters.
	static int trampoline(lua_State* L)
	{
		auto* weak = static_cast<std::weak_ptr<LuaFunc>*>(lua_touserdata(L, lua_upvalueindex(1)));
		// err_msg/nret are trivially destructible (int, and a string that is
		// fully consumed - pushed to Lua - before any possible longjmp), so
		// nothing of interest is skipped by lua_error()'s longjmp below.
		// err_msg itself is scoped inside call_impl() so its (non-trivial)
		// destructor always runs via normal return, never across a longjmp.
		int nret;
		const bool has_error = call_impl(L, weak, nret);
		if(has_error)
		{
			return lua_error(L); // error message already pushed onto the Lua stack
		}
		return nret;
	}

	// Pushes the error message (if any) onto the Lua stack itself, so the
	// std::string holding it is destroyed here - via normal return - before
	// trampoline() potentially calls lua_error()/longjmp. Returns true if an
	// error was pushed (message on top of stack), false if nret holds the
	// real return count.
	static bool call_impl(lua_State* L, std::weak_ptr<LuaFunc>* weak, int& nret)
	{
		std::string err_msg;
		nret = trampoline_impl(L, weak, err_msg);
		if(nret < 0)
		{
			lua_pushlstring(L, err_msg.data(), err_msg.size());
			return true;
		}
		return false;
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
			throw std::runtime_error(std::string("method '") + name + "' is already registered on this type");
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
