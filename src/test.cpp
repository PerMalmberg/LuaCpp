#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Lua.hpp"

// ============================================================
// run_script
// ============================================================

TEST_CASE("run_script: valid script succeeds", "[run_script]") {
  Lua lua;
  auto [ok, err] = lua.run_script("x = 1 + 1");
  REQUIRE(ok);
  REQUIRE(err.empty());
}

TEST_CASE("run_script: syntax error returns failure", "[run_script]") {
  Lua lua;
  auto [ok, err] = lua.run_script("@@@ not valid lua");
  REQUIRE_FALSE(ok);
  REQUIRE_FALSE(err.empty());
}

TEST_CASE("run_script: runtime error returns failure and message",
          "[run_script]") {
  Lua lua;
  auto [ok, err] = lua.run_script("error('boom')");
  REQUIRE_FALSE(ok);
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("boom"));
}

TEST_CASE("run_script: state is preserved across calls", "[run_script]") {
  Lua lua;
  REQUIRE(std::get<0>(lua.run_script("counter = 0")));
  REQUIRE(std::get<0>(lua.run_script("counter = counter + 1")));
  auto [ok, err] = lua.run_script("assert(counter == 1)");
  REQUIRE(ok);
}

// ============================================================
// assign
// ============================================================

TEST_CASE("assign: int is visible in Lua", "[assign]") {
  Lua lua;
  lua.assign("n", 42);
  auto [ok, err] = lua.run_script("assert(n == 42)");
  REQUIRE(ok);
}

TEST_CASE("assign: double is visible in Lua", "[assign]") {
  Lua lua;
  lua.assign("x", 1.5);
  auto [ok, err] = lua.run_script("assert(x == 1.5)");
  REQUIRE(ok);
}

TEST_CASE("assign: std::string is visible in Lua", "[assign]") {
  Lua lua;
  lua.assign("s", std::string("hello"));
  auto [ok, err] = lua.run_script("assert(s == 'hello')");
  REQUIRE(ok);
}

TEST_CASE("assign: const char* is visible in Lua", "[assign]") {
  Lua lua;
  lua.assign("s", "world");
  auto [ok, err] = lua.run_script("assert(s == 'world')");
  REQUIRE(ok);
}

// ============================================================
// call – success cases
// ============================================================

TEST_CASE("call: no args, no return values", "[call]") {
  Lua lua;
  lua.run_script("function noop() end");
  auto [ok, err] = lua.call("noop");
  REQUIRE(ok);
  REQUIRE(err.empty());
}

TEST_CASE("call: int arg and int return", "[call]") {
  Lua lua;
  lua.run_script("function double_it(x) return x * 2 end");
  auto [ok, err, result] = lua.call<int>("double_it", 21);
  REQUIRE(ok);
  REQUIRE(result == 42);
}

TEST_CASE("call: long arg and long return", "[call]") {
  Lua lua;
  lua.run_script("function id(x) return x end");
  auto [ok, err, result] = lua.call<long>("id", 123L);
  REQUIRE(ok);
  REQUIRE(result == 123L);
}

TEST_CASE("call: double arg and double return", "[call]") {
  Lua lua;
  lua.run_script("function half(x) return x / 2.0 end");
  auto [ok, err, result] = lua.call<double>("half", 7.0);
  REQUIRE(ok);
  REQUIRE(result == Catch::Approx(3.5));
}

TEST_CASE("call: float arg and float return", "[call]") {
  Lua lua;
  lua.run_script("function id(x) return x end");
  auto [ok, err, result] = lua.call<float>("id", 2.5f);
  REQUIRE(ok);
  REQUIRE(result == Catch::Approx(2.5f));
}

TEST_CASE("call: const char* arg and string return", "[call]") {
  Lua lua;
  lua.run_script("function greet(name) return 'hello ' .. name end");
  auto [ok, err, result] = lua.call<std::string>("greet", "world");
  REQUIRE(ok);
  REQUIRE(result == "hello world");
}

TEST_CASE("call: std::string arg and string return", "[call]") {
  Lua lua;
  lua.run_script("function greet(name) return 'hi ' .. name end");
  auto [ok, err, result] = lua.call<std::string>("greet", std::string("lua"));
  REQUIRE(ok);
  REQUIRE(result == "hi lua");
}

TEST_CASE("call: multiple return values", "[call]") {
  Lua lua;
  lua.run_script("function swap(a, b) return b, a end");
  auto [ok, err, first, second] = lua.call<int, int>("swap", 1, 2);
  REQUIRE(ok);
  REQUIRE(first == 2);
  REQUIRE(second == 1);
}

TEST_CASE("call: mixed arg and return types", "[call]") {
  Lua lua;
  lua.run_script(
      "function info(n) return n, n * 0.5, tostring(n) end");
  auto [ok, err, i, d, s] =
      lua.call<int, double, std::string>("info", 10);
  REQUIRE(ok);
  REQUIRE(i == 10);
  REQUIRE(d == Catch::Approx(5.0));
  REQUIRE(s == "10");
}

TEST_CASE("call: Lua number is coerced to string by lua_isstring", "[call]") {
  // lua_isstring returns true for numbers, so integer return values can be
  // read back as std::string via Lua's implicit coercion.
  Lua lua;
  lua.run_script("function ret_num() return 42 end");
  auto [ok, err, val] = lua.call<std::string>("ret_num");
  REQUIRE(ok);
  REQUIRE(val == "42");
}

TEST_CASE("call: Lua numeric string is coerced to double by lua_isnumber",
          "[call]") {
  // lua_isnumber returns true for strings that look like numbers.
  Lua lua;
  lua.run_script("function ret_str() return '3.14' end");
  auto [ok, err, val] = lua.call<double>("ret_str");
  REQUIRE(ok);
  REQUIRE(val == Catch::Approx(3.14));
}

TEST_CASE("call: uses assign'd global", "[call][assign]") {
  Lua lua;
  lua.assign("base", 10);
  lua.run_script("function add_base(x) return base + x end");
  auto [ok, err, result] = lua.call<int>("add_base", 5);
  REQUIRE(ok);
  REQUIRE(result == 15);
}

// ============================================================
// call – error cases
// ============================================================

TEST_CASE("call error: undefined global is not a function", "[call]") {
  Lua lua;
  auto [ok, err] = lua.call("does_not_exist");
  REQUIRE_FALSE(ok);
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("does_not_exist"));
}

TEST_CASE("call error: non-function global returns error", "[call]") {
  Lua lua;
  lua.run_script("x = 42");
  auto [ok, err] = lua.call("x");
  REQUIRE_FALSE(ok);
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("Not a function"));
}

TEST_CASE("call error: runtime error in function", "[call]") {
  Lua lua;
  lua.run_script("function fail() error('something went wrong') end");
  auto [ok, err] = lua.call("fail");
  REQUIRE_FALSE(ok);
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("something went wrong"));
}

TEST_CASE("call error: expected integer, got float", "[call]") {
  // In Lua 5.4+ integers and floats are distinct subtypes; lua_isinteger
  // returns false for floats, even whole-number ones.
  Lua lua;
  lua.run_script("function f() return 1.5 end");
  auto [ok, err, val] = lua.call<int>("f");
  REQUIRE_FALSE(ok);
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("number"));
}

TEST_CASE("call error: expected integer, got string", "[call]") {
  Lua lua;
  lua.run_script("function f() return 'hello' end");
  auto [ok, err, val] = lua.call<int>("f");
  REQUIRE_FALSE(ok);
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("string"));
}

TEST_CASE("call error: expected number, got boolean", "[call]") {
  Lua lua;
  lua.run_script("function f() return true end");
  auto [ok, err, val] = lua.call<double>("f");
  REQUIRE_FALSE(ok);
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected number"));
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("boolean"));
}

TEST_CASE("call error: expected string, got nil", "[call]") {
  Lua lua;
  lua.run_script("function f() return nil end");
  auto [ok, err, val] = lua.call<std::string>("f");
  REQUIRE_FALSE(ok);
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected string"));
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("nil"));
}

TEST_CASE("call error: type error on second of multiple return values",
          "[call]") {
  Lua lua;
  lua.run_script("function f() return 1, 'two' end");
  auto [ok, err, a, b] = lua.call<int, int>("f");
  REQUIRE_FALSE(ok);
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
  REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("string"));
}

// ============================================================
// Stack hygiene
// ============================================================

TEST_CASE("stack hygiene: multiple successful calls", "[stack]") {
  Lua lua;
  lua.run_script("function add(a, b) return a + b end");
  for (int i = 0; i < 5; ++i) {
    auto [ok, err, result] = lua.call<int>("add", i, i);
    REQUIRE(ok);
    REQUIRE(result == i * 2);
  }
}

TEST_CASE("stack hygiene: multiple failed calls", "[stack]") {
  Lua lua;
  lua.run_script("function fail() error('oops') end");
  for (int i = 0; i < 3; ++i) {
    auto [ok, err] = lua.call("fail");
    REQUIRE_FALSE(ok);
  }
}

TEST_CASE("stack hygiene: successful call after type-check failure", "[stack]") {
  Lua lua;
  lua.run_script("function ret_str() return 'hello' end");
  lua.run_script("function add(a, b) return a + b end");

  // Trigger a type-check failure (expected int, got string)
  auto [ok1, err1, val1] = lua.call<int>("ret_str");
  REQUIRE_FALSE(ok1);

  // Stack must be clean; next call must succeed
  auto [ok2, err2, result] = lua.call<int>("add", 3, 4);
  REQUIRE(ok2);
  REQUIRE(result == 7);
}
