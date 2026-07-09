extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <functional>
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

class Lua
{
  public:
	Lua()
	{
		luaL_openlibs(*this);
	}

	virtual ~Lua()
	{
	}

	std::tuple<bool, std::string> run_script(const char* script)
	{
		if(luaL_dostring(*this, script) != LUA_OK)
		{
			return {false, get_error_message()};
		}
		return {true, {}};
	}

	template <typename T>
	void assign(const char* name, const T value)
	{
		push(state.get(), value);
		lua_setglobal(*this, name);
	}

	template <typename... ReturnTypes, typename... Args>
	std::tuple<bool, std::string, ReturnTypes...> call(const char* func, Args... args)
	{
		lua_getglobal(*this, func);
		if(!lua_isfunction(*this, TOP_OF_STACK))
		{
			lua_pop(*this, 1); // Remove the non-function value from the stack
			return {false, "Not a function: " + std::string(func), ReturnTypes{}...};
		}

		(push(state.get(), args), ...); // C++17 fold expression to push all arguments onto the Lua stack

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
	template <typename... ReturnTypes, typename... Args>
	void expose_func(const char* name, std::function<lua_return_t<ReturnTypes...>(Args...)> func)
	{
		// Typed wrapper: validates argument count, reads typed arguments, invokes
		// the C++ function, and pushes return values onto the Lua stack.
		// Throws std::runtime_error on argument count or type mismatch.
		auto wrapper = std::make_unique<LuaFunc>(
		[func = std::move(func)](lua_State* L) -> int
		{
			constexpr int expected = static_cast<int>(sizeof...(Args));
			if(const int got = lua_gettop(L); got != expected)
				throw std::runtime_error("expected " + std::to_string(expected) + " argument(s), got " +
				                         std::to_string(got));

			auto args = collect_args<Args...>(L, std::index_sequence_for<Args...>{});

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

		// Transfer ownership before touching the Lua stack so that an allocation
		// failure leaves Lua's state untouched.
		registered_funcs_.push_back(std::move(wrapper));
		LuaFunc* fn_ptr = registered_funcs_.back().get();

		// Trampoline: the actual lua_CFunction stored with Lua. Non-capturing so
		// it converts to a plain function pointer. Catches C++ exceptions and
		// re-raises them as Lua errors; lua_error (longjmp) is called only after
		// all C++ objects on this frame are destroyed, avoiding UB.
		lua_pushlightuserdata(*this, fn_ptr);
		lua_pushcclosure(
		*this,
		[](lua_State* L) -> int
		{
			auto* fn = static_cast<LuaFunc*>(lua_touserdata(L, lua_upvalueindex(1)));
			bool has_error = false;
			int nret = 0;
			try
			{
				nret = (*fn)(L);
			}
			catch(const std::exception& e)
			{
				lua_pushstring(L,
				               e.what()); // copy message into Lua before unwinding
				has_error = true;
			}
			// All C++ objects are destroyed here; safe to longjmp.
			if(has_error)
			{
				return lua_error(L);
			}
			return nret;
		},
		1);
		lua_setglobal(*this, name);
	}

  private:
	using LuaCloser = std::function<void(lua_State*)>;
	using LuaStatePtr = std::unique_ptr<lua_State, LuaCloser>;
	using LuaFunc = std::function<int(lua_State*)>;

	// Positive indexes are from the bottom of the stack, negative indexes are
	// from the top of the stack. -1 = top, -2 = second from top, etc.
	//  1 = bottom, 2 = second from bottom, etc.
	static constexpr int TOP_OF_STACK = -1;

	const std::string get_error_message() const
	{
		const std::string error = lua_tostring(*this, TOP_OF_STACK);
		lua_pop(*this, 1);
		return error;
	}

	operator lua_State*() const
	{
		return state.get();
	}

	// Owns the typed wrapper objects whose raw pointers are stored as Lua
	// upvalues. Declared before state so it is destroyed after lua_close(),
	// ensuring no dangling LuaFunc* pointers exist while the state is live.
	std::vector<std::unique_ptr<LuaFunc>> registered_funcs_;

	LuaStatePtr state{luaL_newstate(), [](lua_State* L) { lua_close(L); }};

	// ---------------------------------------------------------------------------
	// Stack I/O - static so they work both from instance methods and from inside
	// expose_func closures that only have a bare lua_State *.
	// ---------------------------------------------------------------------------

	template <typename T>
	static T read(lua_State* L, int index)
	{
		const auto actual = std::string(lua_typename(L, lua_type(L, index)));
		if constexpr(std::is_integral_v<T>)
		{
			if(!lua_isinteger(L, index))
				throw std::runtime_error("expected integer, got " + actual);
			return static_cast<T>(lua_tointeger(L, index));
		}
		else if constexpr(std::is_floating_point_v<T>)
		{
			if(!lua_isnumber(L, index))
				throw std::runtime_error("expected number, got " + actual);
			return static_cast<T>(lua_tonumber(L, index));
		}
		else if constexpr(std::is_same_v<T, std::string>)
		{
			if(!lua_isstring(L, index))
				throw std::runtime_error("expected string, got " + actual);
			return {lua_tostring(L, index)};
		}
		else if constexpr(std::is_same_v<T, const char*>)
		{
			static_assert(!std::is_same_v<T, T>, "read: const char* is unsafe as a return type — "
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
		if constexpr(std::is_integral_v<T>)
		{
			lua_pushinteger(L, static_cast<lua_Integer>(value));
		}
		else if constexpr(std::is_floating_point_v<T>)
		{
			lua_pushnumber(L, static_cast<lua_Number>(value));
		}
		else if constexpr(std::is_same_v<T, std::string>)
		{
			lua_pushstring(L, value.c_str());
		}
		else if constexpr(std::is_same_v<T, const char*>)
		{
			lua_pushstring(L, value);
		}
		else if constexpr(has_lua_fields<T>::value)
		{
			lua_newtable(L);
			std::apply([&](const auto&... fields) { (push_struct_field(L, value, fields), ...); }, LuaFields<T>::value);
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
				lua_settable(L, -3); // table is at -3 after pushing key and value
			}
		}
		else
		{
			static_assert(!std::is_same_v<T, T>, "push: unsupported type");
		}
	}

	// Reads N arguments from Lua stack positions 1..N into a typed tuple.
	// Used by expose_func to populate arguments before calling the wrapped function.
	template <typename... Args, std::size_t... Is>
	static std::tuple<Args...> collect_args([[maybe_unused]] lua_State* L, std::index_sequence<Is...>)
	{
		return {read<Args>(L, static_cast<int>(Is) + 1)...};
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
};
