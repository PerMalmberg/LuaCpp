extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

class Lua {
public:
  Lua() { luaL_openlibs(*this); }

  virtual ~Lua() {}

  std::tuple<bool, std::string> run_script(const char *script) {
    if (luaL_dostring(*this, script) != LUA_OK) {
      return {false, get_error_message()};
    }
    return {true, {}};
  }

  template <typename T> void assign(const char *name, const T value) {
    push(value);
    lua_setglobal(*this, name);
  }

  template <typename... ReturnTypes, typename... Args>
  std::tuple<bool, std::string, ReturnTypes...> call(const char *func,
                                                     Args... args) {
    lua_getglobal(*this, func);
    if (!lua_isfunction(*this, TOP_OF_STACK)) {
      lua_pop(*this, 1); // Remove the non-function value from the stack
      return {false, "Not a function: " + std::string(func), ReturnTypes{}...};
    }

    (push(args),
     ...); // C++ 17 fold expression to push all arguments onto the Lua stack

    const auto arg_count = static_cast<int>(sizeof...(Args));
    const auto ret_count = static_cast<int>(sizeof...(ReturnTypes));

    if (lua_pcall(*this, arg_count, ret_count, 0) != LUA_OK) {
      return {false, get_error_message(), ReturnTypes{}...};
    }

    auto ret = collect<ReturnTypes...>(
        std::make_index_sequence<sizeof...(ReturnTypes)>{});
    lua_pop(*this, ret_count);
    return std::tuple_cat(std::make_tuple(true, std::string{}), std::move(ret));
  }

private:
  using LuaCloser = std::function<void(lua_State *)>;
  using LuaStatePtr = std::unique_ptr<lua_State, LuaCloser>;

  // Positive indexes are from the bottom of the stack, negative indexes are
  // from the top of the stack. -1 = top, -2 = second from top, etc.
  //  1 = bottom, 2 = second from bottom, etc.
  static constexpr int TOP_OF_STACK = -1;

  const std::string get_error_message() const {
    const std::string error = lua_tostring(*this, TOP_OF_STACK);
    lua_pop(*this, 1);
    return error;
  }

  operator lua_State *() const { return state.get(); }

  LuaStatePtr state{luaL_newstate(), [](lua_State *L) { lua_close(L); }};

  template <typename T> T read(int index) const {
    if constexpr (std::is_integral_v<T>) {
      return static_cast<T>(lua_tointeger(*this, index));
    } else if constexpr (std::is_floating_point_v<T>) {
      return static_cast<T>(lua_tonumber(*this, index));
    } else if constexpr (std::is_same_v<T, std::string>) {
      return {lua_tostring(*this, index)};
    } else if constexpr (std::is_same_v<T, const char *>) {
      return lua_tostring(*this, index);
    } else {
      static_assert(!std::is_same_v<T, T>, "read: unsupported type");
    }
  }

  template <typename... ReturnTypes, std::size_t... Is>
  std::tuple<ReturnTypes...> collect(std::index_sequence<Is...>) const {
    constexpr int N = static_cast<int>(sizeof...(ReturnTypes));
    // Stack layout after lua_pcall (N=3): [-3]=first, [-2]=second, [-1]=last
    // index_sequence {0,1,2} maps to stack indices {0-3, 1-3, 2-3} = {-3,-2,-1}
    return {read<ReturnTypes>(static_cast<int>(Is) - N)...};
  }

  template <typename T> void push(const T value) {
    // bool, char, int, long, ...
    if constexpr (std::is_integral_v<T>) {
      // integers, bool, char, long, etc.
      lua_pushinteger(*this, value);
    } else if constexpr (std::is_floating_point_v<T>) {
      // float, double, long double
      lua_pushnumber(*this, value);
    } else if constexpr (std::is_same_v<T, std::string>) {
      // std::string
      lua_pushstring(*this, value.c_str());
    } else if constexpr (std::is_same_v<T, const char *>) {
      // const char*
      lua_pushstring(*this, value);
    } else {
      static_assert(!std::is_same_v<T, T>, "push_value: unsupported type");
    }
  }
};
