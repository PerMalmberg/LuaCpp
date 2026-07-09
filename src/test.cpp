#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Lua.hpp"

// ============================================================
// run_script
// ============================================================

TEST_CASE("run_script: valid script succeeds", "[run_script]")
{
	Lua lua;
	auto [ok, err] = lua.run_script("x = 1 + 1");
	REQUIRE(ok);
	REQUIRE(err.empty());
}

TEST_CASE("run_script: syntax error returns failure", "[run_script]")
{
	Lua lua;
	auto [ok, err] = lua.run_script("@@@ not valid lua");
	REQUIRE_FALSE(ok);
	REQUIRE_FALSE(err.empty());
}

TEST_CASE("run_script: runtime error returns failure and message", "[run_script]")
{
	Lua lua;
	auto [ok, err] = lua.run_script("error('boom')");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("boom"));
}

TEST_CASE("run_script: state is preserved across calls", "[run_script]")
{
	Lua lua;
	REQUIRE(std::get<0>(lua.run_script("counter = 0")));
	REQUIRE(std::get<0>(lua.run_script("counter = counter + 1")));
	auto [ok, err] = lua.run_script("assert(counter == 1)");
	REQUIRE(ok);
}

// ============================================================
// assign
// ============================================================

TEST_CASE("assign: int is visible in Lua", "[assign]")
{
	Lua lua;
	lua.assign("n", 42);
	auto [ok, err] = lua.run_script("assert(n == 42)");
	REQUIRE(ok);
}

TEST_CASE("assign: double is visible in Lua", "[assign]")
{
	Lua lua;
	lua.assign("x", 1.5);
	auto [ok, err] = lua.run_script("assert(x == 1.5)");
	REQUIRE(ok);
}

TEST_CASE("assign: std::string is visible in Lua", "[assign]")
{
	Lua lua;
	lua.assign("s", std::string("hello"));
	auto [ok, err] = lua.run_script("assert(s == 'hello')");
	REQUIRE(ok);
}

TEST_CASE("assign: const char* is visible in Lua", "[assign]")
{
	Lua lua;
	lua.assign("s", "world");
	auto [ok, err] = lua.run_script("assert(s == 'world')");
	REQUIRE(ok);
}

// ============================================================
// call - success cases
// ============================================================

TEST_CASE("call: no args, no return values", "[call]")
{
	Lua lua;
	lua.run_script("function noop() end");
	auto [ok, err] = lua.call("noop");
	REQUIRE(ok);
	REQUIRE(err.empty());
}

TEST_CASE("call: int arg and int return", "[call]")
{
	Lua lua;
	lua.run_script("function double_it(x) return x * 2 end");
	auto [ok, err, result] = lua.call<int>("double_it", 21);
	REQUIRE(ok);
	REQUIRE(result == 42);
}

TEST_CASE("call: long arg and long return", "[call]")
{
	Lua lua;
	lua.run_script("function id(x) return x end");
	auto [ok, err, result] = lua.call<long>("id", 123L);
	REQUIRE(ok);
	REQUIRE(result == 123L);
}

TEST_CASE("call: double arg and double return", "[call]")
{
	Lua lua;
	lua.run_script("function half(x) return x / 2.0 end");
	auto [ok, err, result] = lua.call<double>("half", 7.0);
	REQUIRE(ok);
	REQUIRE(result == Catch::Approx(3.5));
}

TEST_CASE("call: float arg and float return", "[call]")
{
	Lua lua;
	lua.run_script("function id(x) return x end");
	auto [ok, err, result] = lua.call<float>("id", 2.5f);
	REQUIRE(ok);
	REQUIRE(result == Catch::Approx(2.5f));
}

TEST_CASE("call: const char* arg and string return", "[call]")
{
	Lua lua;
	lua.run_script("function greet(name) return 'hello ' .. name end");
	auto [ok, err, result] = lua.call<std::string>("greet", "world");
	REQUIRE(ok);
	REQUIRE(result == "hello world");
}

TEST_CASE("call: std::string arg and string return", "[call]")
{
	Lua lua;
	lua.run_script("function greet(name) return 'hi ' .. name end");
	auto [ok, err, result] = lua.call<std::string>("greet", std::string("lua"));
	REQUIRE(ok);
	REQUIRE(result == "hi lua");
}

TEST_CASE("call: multiple return values", "[call]")
{
	Lua lua;
	lua.run_script("function swap(a, b) return b, a end");
	auto [ok, err, first, second] = lua.call<int, int>("swap", 1, 2);
	REQUIRE(ok);
	REQUIRE(first == 2);
	REQUIRE(second == 1);
}

TEST_CASE("call: mixed arg and return types", "[call]")
{
	Lua lua;
	lua.run_script("function info(n) return n, n * 0.5, tostring(n) end");
	auto [ok, err, i, d, s] = lua.call<int, double, std::string>("info", 10);
	REQUIRE(ok);
	REQUIRE(i == 10);
	REQUIRE(d == Catch::Approx(5.0));
	REQUIRE(s == "10");
}

TEST_CASE("call: Lua number is coerced to string by lua_isstring", "[call]")
{
	// lua_isstring returns true for numbers, so integer return values can be
	// read back as std::string via Lua's implicit coercion.
	Lua lua;
	lua.run_script("function ret_num() return 42 end");
	auto [ok, err, val] = lua.call<std::string>("ret_num");
	REQUIRE(ok);
	REQUIRE(val == "42");
}

TEST_CASE("call: Lua numeric string is coerced to double by lua_isnumber", "[call]")
{
	// lua_isnumber returns true for strings that look like numbers.
	Lua lua;
	lua.run_script("function ret_str() return '3.14' end");
	auto [ok, err, val] = lua.call<double>("ret_str");
	REQUIRE(ok);
	REQUIRE(val == Catch::Approx(3.14));
}

TEST_CASE("call: uses assign'd global", "[call][assign]")
{
	Lua lua;
	lua.assign("base", 10);
	lua.run_script("function add_base(x) return base + x end");
	auto [ok, err, result] = lua.call<int>("add_base", 5);
	REQUIRE(ok);
	REQUIRE(result == 15);
}

// ============================================================
// call - error cases
// ============================================================

TEST_CASE("call error: undefined global is not a function", "[call]")
{
	Lua lua;
	auto [ok, err] = lua.call("does_not_exist");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("does_not_exist"));
}

TEST_CASE("call error: non-function global returns error", "[call]")
{
	Lua lua;
	lua.run_script("x = 42");
	auto [ok, err] = lua.call("x");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("Not a function"));
}

TEST_CASE("call error: runtime error in function", "[call]")
{
	Lua lua;
	lua.run_script("function fail() error('something went wrong') end");
	auto [ok, err] = lua.call("fail");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("something went wrong"));
}

TEST_CASE("call error: expected integer, got float", "[call]")
{
	// In Lua 5.4+ integers and floats are distinct subtypes; lua_isinteger
	// returns false for floats, even whole-number ones.
	Lua lua;
	lua.run_script("function f() return 1.5 end");
	auto [ok, err, val] = lua.call<int>("f");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("number"));
}

TEST_CASE("call error: expected integer, got string", "[call]")
{
	Lua lua;
	lua.run_script("function f() return 'hello' end");
	auto [ok, err, val] = lua.call<int>("f");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("string"));
}

TEST_CASE("call error: expected number, got boolean", "[call]")
{
	Lua lua;
	lua.run_script("function f() return true end");
	auto [ok, err, val] = lua.call<double>("f");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected number"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("boolean"));
}

TEST_CASE("call error: expected string, got nil", "[call]")
{
	Lua lua;
	lua.run_script("function f() return nil end");
	auto [ok, err, val] = lua.call<std::string>("f");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected string"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("nil"));
}

TEST_CASE("call error: type error on second of multiple return values", "[call]")
{
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

TEST_CASE("stack hygiene: multiple successful calls", "[stack]")
{
	Lua lua;
	lua.run_script("function add(a, b) return a + b end");
	for(int i = 0; i < 5; ++i)
	{
		auto [ok, err, result] = lua.call<int>("add", i, i);
		REQUIRE(ok);
		REQUIRE(result == i * 2);
	}
}

TEST_CASE("stack hygiene: multiple failed calls", "[stack]")
{
	Lua lua;
	lua.run_script("function fail() error('oops') end");
	for(int i = 0; i < 3; ++i)
	{
		auto [ok, err] = lua.call("fail");
		REQUIRE_FALSE(ok);
	}
}

TEST_CASE("stack hygiene: successful call after type-check failure", "[stack]")
{
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
// ============================================================
// expose_func - success cases
// ============================================================

TEST_CASE("expose_func: void return, no args, side effect in C++", "[expose_func]")
{
	Lua lua;
	int count = 0;
	lua.expose_func("increment", std::function<void()>([&count]() { ++count; }));

	lua.run_script("increment()");
	REQUIRE(count == 1);
	lua.run_script("increment(); increment()");
	REQUIRE(count == 3);
}

TEST_CASE("expose_func: void return with args", "[expose_func]")
{
	Lua lua;
	int last = 0;
	lua.expose_func("store", std::function<void(int)>([&last](int n) { last = n; }));

	lua.run_script("store(42)");
	REQUIRE(last == 42);
}

TEST_CASE("expose_func: int return callable from Lua", "[expose_func]")
{
	Lua lua;
	lua.expose_func<int>("add", std::function<int(int, int)>([](int a, int b) { return a + b; }));

	auto [ok, err] = lua.run_script("assert(add(3, 4) == 7)");
	REQUIRE(ok);
}

TEST_CASE("expose_func: int return callable via call<>", "[expose_func]")
{
	Lua lua;
	lua.expose_func<int>("mul", std::function<int(int, int)>([](int a, int b) { return a * b; }));

	auto [ok, err, result] = lua.call<int>("mul", 6, 7);
	REQUIRE(ok);
	REQUIRE(result == 42);
}

TEST_CASE("expose_func: double return", "[expose_func]")
{
	Lua lua;
	lua.expose_func<double>("half", std::function<double(double)>([](double x) { return x / 2.0; }));

	auto [ok, err, result] = lua.call<double>("half", 3.14);
	REQUIRE(ok);
	REQUIRE(result == Catch::Approx(1.57));
}

TEST_CASE("expose_func: string return", "[expose_func]")
{
	Lua lua;
	lua.expose_func<std::string>("greet", std::function<std::string(std::string)>([](std::string name)
	                                                                              { return "hi " + name; }));

	auto [ok, err, result] = lua.call<std::string>("greet", std::string("lua"));
	REQUIRE(ok);
	REQUIRE(result == "hi lua");
}

TEST_CASE("expose_func: multiple return values", "[expose_func]")
{
	Lua lua;
	lua.expose_func<int, std::string>("pair", std::function<std::tuple<int, std::string>(int)>(
	                                          [](int n) { return std::make_tuple(n, std::to_string(n)); }));

	auto [ok, err, i, s] = lua.call<int, std::string>("pair", 99);
	REQUIRE(ok);
	REQUIRE(i == 99);
	REQUIRE(s == "99");
}

TEST_CASE("expose_func: captures value from registration site", "[expose_func]")
{
	Lua lua;
	constexpr int offset = 100;
	lua.expose_func<int>("add_offset", std::function<int(int)>([=](int x) { return x + offset; }));

	auto [ok, err, result] = lua.call<int>("add_offset", 5);
	REQUIRE(ok);
	REQUIRE(result == 105);
}

TEST_CASE("expose_func: multiple functions registered independently", "[expose_func]")
{
	Lua lua;
	lua.expose_func<int>("add", std::function<int(int, int)>([](int a, int b) { return a + b; }));
	lua.expose_func<int>("mul", std::function<int(int, int)>([](int a, int b) { return a * b; }));

	auto [ok1, err1] = lua.run_script("assert(add(2, 3) == 5)");
	auto [ok2, err2] = lua.run_script("assert(mul(2, 3) == 6)");
	REQUIRE(ok1);
	REQUIRE(ok2);
}

TEST_CASE("expose_func: usable inside a Lua function", "[expose_func]")
{
	Lua lua;
	lua.expose_func<int>("square", std::function<int(int)>([](int x) { return x * x; }));
	lua.run_script("function sum_of_squares(a, b) return square(a) + square(b) end");

	auto [ok, err, result] = lua.call<int>("sum_of_squares", 3, 4);
	REQUIRE(ok);
	REQUIRE(result == 25); // 9 + 16
}

// ============================================================
// expose_func - error cases
// ============================================================

TEST_CASE("expose_func error: too few arguments", "[expose_func]")
{
	Lua lua;
	lua.expose_func<int>("add", std::function<int(int, int)>([](int a, int b) { return a + b; }));

	auto [ok, err, val] = lua.call<int>("add", 1); // one arg instead of two
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected 2"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("got 1"));
}

TEST_CASE("expose_func error: too many arguments", "[expose_func]")
{
	Lua lua;
	lua.expose_func<int>("add", std::function<int(int, int)>([](int a, int b) { return a + b; }));

	auto [ok, err] = lua.run_script("add(1, 2, 3)"); // three args instead of two
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected 2"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("got 3"));
}

TEST_CASE("expose_func error: wrong argument type", "[expose_func]")
{
	Lua lua;
	lua.expose_func<int>("add", std::function<int(int, int)>([](int a, int b) { return a + b; }));

	auto [ok, err] = lua.run_script("add(1, 'two')"); // string instead of integer
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("string"));
}

// ============================================================
// expose_func - stack hygiene
// ============================================================

TEST_CASE("expose_func stack hygiene: callable multiple times without stack leak", "[expose_func][stack]")
{
	Lua lua;
	lua.expose_func<int>("inc", std::function<int(int)>([](int x) { return x + 1; }));

	for(int i = 0; i < 10; ++i)
	{
		auto [ok, err, result] = lua.call<int>("inc", i);
		REQUIRE(ok);
		REQUIRE(result == i + 1);
	}
}

TEST_CASE("expose_func stack hygiene: succeeds after an error", "[expose_func][stack]")
{
	Lua lua;
	lua.expose_func<int>("add", std::function<int(int, int)>([](int a, int b) { return a + b; }));

	// Trigger an argument count error
	auto [ok1, err1] = lua.run_script("add(1)");
	REQUIRE_FALSE(ok1);

	// Stack must be clean; next call must succeed
	auto [ok2, err2, result] = lua.call<int>("add", 10, 20);
	REQUIRE(ok2);
	REQUIRE(result == 30);
}

// ============================================================
// Struct support
// ============================================================

struct Point
{
	int x, y;
};
LUA_REGISTER_STRUCT(Point, lua_field("x", &Point::x), lua_field("y", &Point::y))

struct Rect
{
	Point origin;
	int w, h;
};
LUA_REGISTER_STRUCT(Rect, lua_field("origin", &Rect::origin), lua_field("w", &Rect::w), lua_field("h", &Rect::h))

TEST_CASE("struct: passed as argument to Lua function", "[struct]")
{
	Lua lua;
	lua.run_script("function sum(p) return p.x + p.y end");
	auto [ok, err, result] = lua.call<int>("sum", Point{3, 4});
	REQUIRE(ok);
	REQUIRE(result == 7);
}

TEST_CASE("struct: returned from Lua via call<>", "[struct]")
{
	Lua lua;
	lua.run_script("function make(x, y) return {x = x, y = y} end");
	auto [ok, err, p] = lua.call<Point>("make", 10, 20);
	REQUIRE(ok);
	REQUIRE(p.x == 10);
	REQUIRE(p.y == 20);
}

TEST_CASE("struct: round-trip through Lua", "[struct]")
{
	Lua lua;
	lua.run_script("function move(p, dx, dy) return {x = p.x + dx, y = p.y + dy} end");
	auto [ok, err, result] = lua.call<Point>("move", Point{1, 2}, 10, 20);
	REQUIRE(ok);
	REQUIRE(result.x == 11);
	REQUIRE(result.y == 22);
}

TEST_CASE("struct: as expose_func argument", "[struct]")
{
	Lua lua;
	lua.expose_func<int>("magnitude_sq", std::function<int(Point)>([](Point p) { return p.x * p.x + p.y * p.y; }));
	auto [ok, err] = lua.run_script("assert(magnitude_sq({x=3, y=4}) == 25)");
	REQUIRE(ok);
}

TEST_CASE("struct: as expose_func return type", "[struct]")
{
	Lua lua;
	lua.expose_func<Point>("make_point", std::function<Point(int, int)>([](int x, int y) { return Point{x, y}; }));
	auto [ok, err, p] = lua.call<Point>("make_point", 5, 6);
	REQUIRE(ok);
	REQUIRE(p.x == 5);
	REQUIRE(p.y == 6);
}

TEST_CASE("struct: nested struct round-trip", "[struct]")
{
	Lua lua;
	lua.run_script(R"(
		function scale(r, f)
			return {origin = {x = r.origin.x * f, y = r.origin.y * f}, w = r.w * f, h = r.h * f}
		end
	)");
	Rect r{Point{1, 2}, 3, 4};
	auto [ok, err, result] = lua.call<Rect>("scale", r, 2);
	REQUIRE(ok);
	REQUIRE(result.origin.x == 2);
	REQUIRE(result.origin.y == 4);
	REQUIRE(result.w == 6);
	REQUIRE(result.h == 8);
}

TEST_CASE("struct: two distinct instances have independent values in Lua", "[struct]")
{
	Lua lua;
	lua.run_script(R"(
		function check(a, b)
			return a.x + a.y, b.x + b.y
		end
	)");
	auto [ok, err, sum_a, sum_b] = lua.call<int, int>("check", Point{1, 2}, Point{10, 20});
	REQUIRE(ok);
	REQUIRE(sum_a == 3); // 1 + 2
	REQUIRE(sum_b == 30); // 10 + 20
}

// ============================================================
// Struct - error cases
// ============================================================

TEST_CASE("struct error: non-table returned where struct expected", "[struct]")
{
	Lua lua;
	lua.run_script("function f() return 42 end");
	auto [ok, err, p] = lua.call<Point>("f");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected table"));
}

TEST_CASE("struct error: wrong field type in table", "[struct]")
{
	Lua lua;
	lua.run_script("function f() return {x = 'oops', y = 2} end");
	auto [ok, err, p] = lua.call<Point>("f");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
}

TEST_CASE("struct error: missing field reads as nil", "[struct]")
{
	// lua_getfield returns nil for a missing key; read<int> then throws
	Lua lua;
	lua.run_script("function f() return {x = 1} end"); // y is missing
	auto [ok, err, p] = lua.call<Point>("f");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("nil"));
}

// ============================================================
// std::vector support
// ============================================================

TEST_CASE("vector: passed as argument to Lua function", "[vector]")
{
	Lua lua;
	lua.run_script("function sum(t) local s = 0; for _, v in ipairs(t) do s = s + v end; return s end");
	auto [ok, err, result] = lua.call<int>("sum", std::vector<int>{1, 2, 3, 4, 5});
	REQUIRE(ok);
	REQUIRE(result == 15);
}

TEST_CASE("vector: returned from Lua via call<>", "[vector]")
{
	Lua lua;
	lua.run_script("function make() return {10, 20, 30} end");
	auto [ok, err, result] = lua.call<std::vector<int>>("make");
	REQUIRE(ok);
	REQUIRE(result.size() == 3);
	REQUIRE(result[0] == 10);
	REQUIRE(result[1] == 20);
	REQUIRE(result[2] == 30);
}

TEST_CASE("vector: round-trip preserves order and values", "[vector]")
{
	Lua lua;
	lua.run_script("function identity(t) return t end");
	std::vector<int> input{7, 3, 9, 1};
	auto [ok, err, result] = lua.call<std::vector<int>>("identity", input);
	REQUIRE(ok);
	REQUIRE(result == input);
}

TEST_CASE("vector: of strings", "[vector]")
{
	Lua lua;
	lua.run_script("function first(t) return t[1] end");
	auto [ok, err, result] = lua.call<std::string>("first", std::vector<std::string>{"hello", "world"});
	REQUIRE(ok);
	REQUIRE(result == "hello");
}

TEST_CASE("vector: of registered structs", "[vector]")
{
	Lua lua;
	lua.run_script("function sum_xs(t) local s = 0; for _, p in ipairs(t) do s = s + p.x end; return s end");
	std::vector<Point> pts{{1, 0}, {2, 0}, {3, 0}};
	auto [ok, err, result] = lua.call<int>("sum_xs", pts);
	REQUIRE(ok);
	REQUIRE(result == 6);
}

TEST_CASE("vector: empty round-trip", "[vector]")
{
	Lua lua;
	lua.run_script("function identity(t) return t end");
	auto [ok, err, result] = lua.call<std::vector<int>>("identity", std::vector<int>{});
	REQUIRE(ok);
	REQUIRE(result.empty());
}

TEST_CASE("vector error: non-table returned where vector expected", "[vector]")
{
	Lua lua;
	lua.run_script("function f() return 42 end");
	auto [ok, err, result] = lua.call<std::vector<int>>("f");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected table"));
}

// ============================================================
// std::map / std::unordered_map support
// ============================================================

TEST_CASE("map: passed as argument to Lua function", "[map]")
{
	Lua lua;
	lua.run_script("function get(t, k) return t[k] end");
	std::map<std::string, int> m{{"a", 1}, {"b", 2}};
	auto [ok, err, result] = lua.call<int>("get", m, std::string("a"));
	REQUIRE(ok);
	REQUIRE(result == 1);
}

TEST_CASE("map: returned from Lua via call<>", "[map]")
{
	Lua lua;
	lua.run_script("function make() return {x = 10, y = 20} end");
	auto [ok, err, result] = lua.call<std::map<std::string, int>>("make");
	REQUIRE(ok);
	REQUIRE(result.at("x") == 10);
	REQUIRE(result.at("y") == 20);
}

TEST_CASE("map: unordered_map round-trip", "[map]")
{
	Lua lua;
	lua.run_script("function identity(t) return t end");
	std::unordered_map<std::string, int> input{{"one", 1}, {"two", 2}, {"three", 3}};
	auto [ok, err, result] = lua.call<std::unordered_map<std::string, int>>("identity", input);
	REQUIRE(ok);
	REQUIRE(result == input);
}

TEST_CASE("map error: non-table returned where map expected", "[map]")
{
	Lua lua;
	lua.run_script("function f() return 'oops' end");
	auto [ok, err, result] = lua.call<std::map<std::string, int>>("f");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected table"));
}

// ============================================================
// expose_method - struct methods
// ============================================================

TEST_CASE("expose_method: void return, no extra args", "[expose_method]")
{
	Lua lua;
	int call_count = 0;
	lua.expose_method<Point>("touch", std::function<void(Point)>([&call_count](Point) { ++call_count; }));
	lua.assign("p", Point{1, 2});
	lua.run_script("p:touch()");
	REQUIRE(call_count == 1);
	lua.run_script("p:touch(); p:touch()");
	REQUIRE(call_count == 3);
}

TEST_CASE("expose_method: scalar return, no extra args", "[expose_method]")
{
	Lua lua;
	lua.expose_method<Point, int>("magnitude_sq",
	                              std::function<int(Point)>([](Point p) { return p.x * p.x + p.y * p.y; }));
	lua.assign("p", Point{3, 4});
	auto [ok, err] = lua.run_script("assert(p:magnitude_sq() == 25)");
	REQUIRE(ok);
}

TEST_CASE("expose_method: scalar return, with extra args", "[expose_method]")
{
	Lua lua;
	lua.expose_method<Point, int>("dot", std::function<int(Point, Point)>([](Point a, Point b)
	                                                                      { return a.x * b.x + a.y * b.y; }));
	lua.assign("p", Point{3, 4});
	lua.assign("q", Point{1, 2});
	auto [ok, err] = lua.run_script("assert(p:dot(q) == 11)");
	REQUIRE(ok);
}

TEST_CASE("expose_method: struct return via Lua wrapper", "[expose_method]")
{
	Lua lua;
	lua.expose_method<Point, Point>("translate", std::function<Point(Point, int, int)>(
	                                             [](Point p, int dx, int dy) { return Point{p.x + dx, p.y + dy}; }));
	lua.assign("p", Point{1, 2});
	lua.run_script("function get_moved() return p:translate(10, 20) end");
	auto [ok, err, result] = lua.call<Point>("get_moved");
	REQUIRE(ok);
	REQUIRE(result.x == 11);
	REQUIRE(result.y == 22);
}

TEST_CASE("expose_method: multiple return values", "[expose_method]")
{
	Lua lua;
	lua.expose_method<Point, int, int>("components", std::function<std::tuple<int, int>(Point)>(
	                                                 [](Point p) { return std::make_tuple(p.x, p.y); }));
	lua.assign("p", Point{7, 13});
	lua.run_script("function get_comps() return p:components() end");
	auto [ok, err, x, y] = lua.call<int, int>("get_comps");
	REQUIRE(ok);
	REQUIRE(x == 7);
	REQUIRE(y == 13);
}

TEST_CASE("expose_method: two methods on same type work independently", "[expose_method]")
{
	Lua lua;
	lua.expose_method<Point, int>("sum_xy", std::function<int(Point)>([](Point p) { return p.x + p.y; }));
	lua.expose_method<Point, int>("diff_xy", std::function<int(Point)>([](Point p) { return p.x - p.y; }));
	lua.assign("p", Point{10, 3});
	auto [ok1, err1] = lua.run_script("assert(p:sum_xy() == 13)");
	auto [ok2, err2] = lua.run_script("assert(p:diff_xy() == 7)");
	REQUIRE(ok1);
	REQUIRE(ok2);
}

TEST_CASE("expose_method: method on nested struct field", "[expose_method]")
{
	Lua lua;
	lua.expose_method<Point, int>("magnitude_sq",
	                              std::function<int(Point)>([](Point p) { return p.x * p.x + p.y * p.y; }));
	lua.assign("r", Rect{Point{3, 4}, 10, 20});
	auto [ok, err] = lua.run_script("assert(r.origin:magnitude_sq() == 25)");
	REQUIRE(ok);
}

// expose_method - error cases

TEST_CASE("expose_method error: too many arguments", "[expose_method]")
{
	Lua lua;
	lua.expose_method<Point, int>("magnitude_sq",
	                              std::function<int(Point)>([](Point p) { return p.x * p.x + p.y * p.y; }));
	lua.assign("p", Point{3, 4});
	auto [ok, err] = lua.run_script("p:magnitude_sq(99)"); // one extra arg beyond self
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected 0"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("got 1"));
}

TEST_CASE("expose_method error: too few arguments", "[expose_method]")
{
	Lua lua;
	lua.expose_method<Point, int>("dot", std::function<int(Point, Point)>([](Point a, Point b)
	                                                                      { return a.x * b.x + a.y * b.y; }));
	lua.assign("p", Point{3, 4});
	auto [ok, err] = lua.run_script("p:dot()"); // missing the second Point arg
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected 1"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("got 0"));
}

TEST_CASE("expose_method error: wrong argument type", "[expose_method]")
{
	Lua lua;
	lua.expose_method<Point, int>("add_x", std::function<int(Point, int)>([](Point p, int n) { return p.x + n; }));
	lua.assign("p", Point{1, 2});
	auto [ok, err] = lua.run_script("p:add_x('bad')"); // string instead of integer
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
}

// expose_method - stack hygiene

TEST_CASE("expose_method stack hygiene: callable multiple times without leak", "[expose_method][stack]")
{
	Lua lua;
	lua.expose_method<Point, int>("magnitude_sq",
	                              std::function<int(Point)>([](Point p) { return p.x * p.x + p.y * p.y; }));
	lua.assign("p", Point{3, 4});
	for(int i = 0; i < 10; ++i)
	{
		auto [ok, err] = lua.run_script("assert(p:magnitude_sq() == 25)");
		REQUIRE(ok);
	}
}

TEST_CASE("expose_method: data modified alternately by C++ and Lua", "[expose_method]")
{
	Lua lua;

	lua.expose_method<Point, Point>("shift", std::function<Point(Point, int, int)>(
	                                         [](Point p, int dx, int dy) { return Point{p.x + dx, p.y + dy}; }));

	lua.expose_method<Point, Point>("scale", std::function<Point(Point, int)>(
	                                         [](Point p, int factor) { return Point{p.x * factor, p.y * factor}; }));

	// Lua orchestrates a chain of two method calls, each dispatching into C++.
	lua.run_script(R"(
		function lua_process(p)
			return p:shift(1, 1):scale(2)
		end
	)");

	// Step 1 - C++ owns the initial value.
	const Point origin{3, 4};

	// Step 2 - C++ hands it to Lua; Lua applies :shift then :scale via method calls.
	auto [ok1, err1, p1] = lua.call<Point>("lua_process", origin);
	REQUIRE(ok1);
	REQUIRE(p1.x == 8); // (3+1)*2
	REQUIRE(p1.y == 10); // (4+1)*2

	// Step 3 - C++ modifies the returned value (subtracts the original coordinates).
	const Point p2{p1.x - origin.x, p1.y - origin.y}; // {8-3, 10-4} = {5, 6}

	// Step 4 - C++ passes the modified value back to Lua for another round.
	auto [ok2, err2, p3] = lua.call<Point>("lua_process", p2);
	REQUIRE(ok2);
	REQUIRE(p3.x == 12); // (5+1)*2
	REQUIRE(p3.y == 14); // (6+1)*2
}
// ============================================================
// expose_mutable_method - struct mutable methods
// ============================================================

TEST_CASE("expose_mutable_method: void return, no extra args", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("negate", std::function<void(Point&)>(
	                                           [](Point& p)
	                                           {
		                                           p.x = -p.x;
		                                           p.y = -p.y;
	                                           }));
	lua.assign("p", Point{3, 4});
	lua.run_script("p:negate()");
	auto [ok, err] = lua.run_script("assert(p.x == -3 and p.y == -4)");
	REQUIRE(ok);
}

TEST_CASE("expose_mutable_method: void return, with extra args", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("translate", std::function<void(Point&, int, int)>(
	                                              [](Point& p, int dx, int dy)
	                                              {
		                                              p.x += dx;
		                                              p.y += dy;
	                                              }));
	lua.assign("p", Point{1, 2});
	lua.run_script("p:translate(10, 20)");
	auto [ok, err] = lua.run_script("assert(p.x == 11 and p.y == 22)");
	REQUIRE(ok);
}

TEST_CASE("expose_mutable_method: scalar return, also modifies self", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point, int>("scale_and_sum", std::function<int(Point&, int)>(
	                                                       [](Point& p, int f)
	                                                       {
		                                                       p.x *= f;
		                                                       p.y *= f;
		                                                       return p.x + p.y;
	                                                       }));
	lua.assign("p", Point{3, 4});
	// Return value is 14 (6 + 8), and the table must be updated
	auto [ok1, err1] = lua.run_script("assert(p:scale_and_sum(2) == 14)");
	REQUIRE(ok1);
	auto [ok2, err2] = lua.run_script("assert(p.x == 6 and p.y == 8)");
	REQUIRE(ok2);
}

TEST_CASE("expose_mutable_method: multiple return values, also modifies self", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point, int, int>("scale_and_components", std::function<std::tuple<int, int>(Point&, int)>(
	                                                                   [](Point& p, int f)
	                                                                   {
		                                                                   p.x *= f;
		                                                                   p.y *= f;
		                                                                   return std::make_tuple(p.x, p.y);
	                                                                   }));
	lua.assign("p", Point{3, 4});
	lua.run_script("function get() return p:scale_and_components(2) end");
	auto [ok, err, x, y] = lua.call<int, int>("get");
	REQUIRE(ok);
	REQUIRE(x == 6);
	REQUIRE(y == 8);
	// Write-back must also have updated the table
	auto [ok2, err2] = lua.run_script("assert(p.x == 6 and p.y == 8)");
	REQUIRE(ok2);
}

TEST_CASE("expose_mutable_method: sequential calls accumulate state", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("translate", std::function<void(Point&, int, int)>(
	                                              [](Point& p, int dx, int dy)
	                                              {
		                                              p.x += dx;
		                                              p.y += dy;
	                                              }));
	lua.assign("p", Point{0, 0});
	lua.run_script("p:translate(1, 2)");
	lua.run_script("p:translate(3, 4)");
	auto [ok, err] = lua.run_script("assert(p.x == 4 and p.y == 6)");
	REQUIRE(ok);
}

TEST_CASE("expose_mutable_method: mixed with immutable method on same type", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("scale", std::function<void(Point&, int)>(
	                                          [](Point& p, int f)
	                                          {
		                                          p.x *= f;
		                                          p.y *= f;
	                                          }));
	lua.expose_method<Point, int>("magnitude_sq",
	                              std::function<int(Point)>([](Point p) { return p.x * p.x + p.y * p.y; }));
	lua.assign("p", Point{3, 4});
	lua.run_script("p:scale(2)"); // p becomes {6, 8}
	auto [ok, err] = lua.run_script("assert(p:magnitude_sq() == 100)"); // 36 + 64
	REQUIRE(ok);
}

TEST_CASE("expose_mutable_method: modifies nested struct field in place", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("translate", std::function<void(Point&, int, int)>(
	                                              [](Point& p, int dx, int dy)
	                                              {
		                                              p.x += dx;
		                                              p.y += dy;
	                                              }));
	lua.assign("r", Rect{Point{1, 2}, 10, 20});
	// r.origin is a Lua table reference; the write-back must update it in place
	lua.run_script("r.origin:translate(5, 5)");
	auto [ok, err] = lua.run_script("assert(r.origin.x == 6 and r.origin.y == 7)");
	REQUIRE(ok);
}

// expose_mutable_method - error cases

TEST_CASE("expose_mutable_method error: too many arguments", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("negate", std::function<void(Point&)>(
	                                           [](Point& p)
	                                           {
		                                           p.x = -p.x;
		                                           p.y = -p.y;
	                                           }));
	lua.assign("p", Point{1, 2});
	auto [ok, err] = lua.run_script("p:negate(99)"); // one extra arg beyond self
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected 0"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("got 1"));
}

TEST_CASE("expose_mutable_method error: too few arguments", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("translate", std::function<void(Point&, int, int)>(
	                                              [](Point& p, int dx, int dy)
	                                              {
		                                              p.x += dx;
		                                              p.y += dy;
	                                              }));
	lua.assign("p", Point{1, 2});
	auto [ok, err] = lua.run_script("p:translate(5)"); // missing second arg
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected 2"));
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("got 1"));
}

TEST_CASE("expose_mutable_method error: wrong argument type", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("translate", std::function<void(Point&, int, int)>(
	                                              [](Point& p, int dx, int dy)
	                                              {
		                                              p.x += dx;
		                                              p.y += dy;
	                                              }));
	lua.assign("p", Point{1, 2});
	auto [ok, err] = lua.run_script("p:translate('bad', 1)"); // string instead of integer
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
}

// expose_mutable_method - stack hygiene

TEST_CASE("expose_mutable_method stack hygiene: callable multiple times without leak", "[expose_mutable_method][stack]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("increment", std::function<void(Point&)>(
	                                              [](Point& p)
	                                              {
		                                              ++p.x;
		                                              ++p.y;
	                                              }));
	lua.assign("p", Point{0, 0});
	for(int i = 0; i < 10; ++i)
	{
		auto [ok, err] = lua.run_script("p:increment()");
		REQUIRE(ok);
	}
	auto [ok, err] = lua.run_script("assert(p.x == 10 and p.y == 10)");
	REQUIRE(ok);
}

TEST_CASE("expose_mutable_method stack hygiene: succeeds after an error", "[expose_mutable_method][stack]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("translate", std::function<void(Point&, int, int)>(
	                                              [](Point& p, int dx, int dy)
	                                              {
		                                              p.x += dx;
		                                              p.y += dy;
	                                              }));
	lua.assign("p", Point{0, 0});

	// Trigger an argument count error
	auto [ok1, err1] = lua.run_script("p:translate(1)"); // missing second arg
	REQUIRE_FALSE(ok1);

	// Stack must be clean; next call must succeed and accumulate correctly
	auto [ok2, err2] = lua.run_script("p:translate(3, 4)");
	REQUIRE(ok2);
	auto [ok3, err3] = lua.run_script("assert(p.x == 3 and p.y == 4)");
	REQUIRE(ok3);
}

// ============================================================
// Duplicate method name detection
// ============================================================

TEST_CASE("duplicate method: expose_method then expose_mutable_method throws", "[expose_method][expose_mutable_method]")
{
	Lua lua;
	lua.expose_method<Point, int>("foo", std::function<int(Point)>([](Point p) { return p.x + p.y; }));
	REQUIRE_THROWS_AS(lua.expose_mutable_method<Point>("foo", std::function<void(Point&)>([](Point&) {})),
	                  std::runtime_error);
}

TEST_CASE("duplicate method: expose_mutable_method then expose_method throws", "[expose_method][expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("bar", std::function<void(Point&)>([](Point&) {}));
	REQUIRE_THROWS_AS((lua.expose_method<Point, int>("bar",
	                                                 std::function<int(Point)>([](Point p) { return p.x + p.y; }))),
	                  std::runtime_error);
}

TEST_CASE("duplicate method: expose_method twice with same name throws", "[expose_method]")
{
	Lua lua;
	lua.expose_method<Point, int>("baz", std::function<int(Point)>([](Point p) { return p.x + p.y; }));
	REQUIRE_THROWS_AS((lua.expose_method<Point, int>("baz",
	                                                 std::function<int(Point)>([](Point p) { return p.x - p.y; }))),
	                  std::runtime_error);
}

TEST_CASE("duplicate method: expose_mutable_method twice with same name throws", "[expose_mutable_method]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("qux", std::function<void(Point&)>([](Point& p) { p.x = 0; }));
	REQUIRE_THROWS_AS(lua.expose_mutable_method<Point>("qux", std::function<void(Point&)>([](Point& p) { p.y = 0; })),
	                  std::runtime_error);
}

TEST_CASE("duplicate method: same name on different types is allowed", "[expose_method][expose_mutable_method]")
{
	// 'value' registered on Point must not block 'value' being registered on Rect.
	Lua lua;
	REQUIRE_NOTHROW(
	(lua.expose_method<Point, int>("value", std::function<int(Point)>([](Point p) { return p.x + p.y; }))));
	REQUIRE_NOTHROW(
	(lua.expose_method<Rect, int>("value", std::function<int(Rect)>([](Rect r) { return r.w * r.h; }))));

	lua.assign("p", Point{3, 4});
	lua.assign("r", Rect{Point{0, 0}, 5, 6});
	auto [ok1, err1] = lua.run_script("assert(p:value() == 7)");
	auto [ok2, err2] = lua.run_script("assert(r:value() == 30)");
	REQUIRE(ok1);
	REQUIRE(ok2);
}

TEST_CASE("duplicate method: error message names the conflicting method", "[expose_method][expose_mutable_method]")
{
	Lua lua;
	lua.expose_method<Point, int>("my_method", std::function<int(Point)>([](Point p) { return p.x; }));
	try
	{
		lua.expose_mutable_method<Point>("my_method", std::function<void(Point&)>([](Point&) {}));
		FAIL("expected exception was not thrown");
	}
	catch(const std::runtime_error& e)
	{
		REQUIRE_THAT(std::string(e.what()), Catch::Matchers::ContainsSubstring("my_method"));
	}
}
