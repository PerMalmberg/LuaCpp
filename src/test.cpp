#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>

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
// expose_func / expose_method - shared_ptr<Owner> keep-alive overload
// ============================================================

TEST_CASE("expose_func: shared_ptr owner keeps referenced object alive", "[expose_func][lifetime]")
{
	Lua lua;
	auto counter = std::make_shared<int>(0);

	// Lambda captures a raw reference into *counter; the shared_ptr overload
	// keeps `counter` alive inside the closure for as long as it is registered,
	// even though the local `counter` variable here still also owns it.
	lua.expose_func("bump", counter, std::function<void()>([raw = counter.get()]() { ++(*raw); }));

	lua.run_script("bump(); bump(); bump()");
	REQUIRE(*counter == 3);
}

TEST_CASE("expose_func: shared_ptr owner survives after local shared_ptr is reset", "[expose_func][lifetime]")
{
	Lua lua;
	{
		auto counter = std::make_shared<int>(0);
		lua.expose_func("bump", counter, std::function<void()>([raw = counter.get()]() { ++(*raw); }));
		// `counter` goes out of scope here; the closure's own shared_ptr copy
		// keeps the int alive.
	}
	auto [ok, err] = lua.run_script("bump(); bump()");
	REQUIRE(ok);
	auto [ok2, err2, result] = lua.call<int>("bump"); // bump has no return value; call<int> should fail cleanly
	REQUIRE_FALSE(ok2);
}

// ============================================================
// close() - explicit early teardown (LIFETIME.md item 3)
// ============================================================

TEST_CASE("close(): nils registered globals", "[lifetime]")
{
	Lua lua;
	lua.expose_func<int>("add", std::function<int(int, int)>([](int a, int b) { return a + b; }));

	auto [ok1, err1, r1] = lua.call<int>("add", 1, 2);
	REQUIRE(ok1);
	REQUIRE(r1 == 3);

	lua.close();

	auto [ok2, err2] = lua.run_script("assert(add == nil)");
	REQUIRE(ok2);
}

TEST_CASE("close(): is idempotent", "[lifetime]")
{
	Lua lua;
	lua.expose_func("noop", std::function<void()>([]() {}));
	lua.close();
	lua.close(); // must not throw / crash / double-free
	SUCCEED();
}

TEST_CASE("close(): forces a GC cycle that runs script-set __gc metamethods while C++ state is still alive",
          "[lifetime]")
{
	// Simulates the scenario from LIFETIME.md item 3: a registered closure
	// captures a raw reference to a C++ object (`value` here plays the role
	// of a sibling member in an embedding class). A Lua script attaches that
	// closure as a __gc metamethod on a table it then drops. Calling close()
	// forces the GC to finalize that table - and therefore invoke the
	// closure - immediately, while `value` is still fully alive, rather than
	// leaving the timing to whatever GC activity happens to run later.
	int value = 42;
	bool called = false;
	int observed = -1;

	Lua lua;
	lua.expose_func("on_finalize", std::function<void()>(
	                               [&]()
	                               {
		                               called = true;
		                               observed = value;
	                               }));

	auto [ok, err] = lua.run_script(R"(
		local fn = on_finalize -- capture the closure value itself as an upvalue,
		                       -- not by looking up the global name again later -
		                       -- close() nils the global before running the GC.
		local t = {}
		setmetatable(t, {__gc = function() fn() end})
		t = nil
	)");
	REQUIRE(ok);

	REQUIRE_FALSE(called); // nothing guarantees the GC has run yet

	lua.close();

	REQUIRE(called);
	REQUIRE(observed == 42);
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
// expose_func - C++ exception handling
// ============================================================
// The trampoline that dispatches into a registered C++ callable wraps the
// call in try/catch(const std::exception&) and turns any thrown exception
// into a Lua error (via luaL_error) carrying the exception's what() message.
// These tests exercise that path directly, as opposed to the "call error"
// tests above which cover Lua's own error()/runtime-error mechanism.

TEST_CASE("expose_func: std::exception thrown in C++ callable surfaces as Lua error", "[expose_func][exceptions]")
{
	Lua lua;
	lua.expose_func<int>("boom", std::function<int(int)>(
	                             [](int x) -> int
	                             {
		                             if(x < 0)
			                             throw std::runtime_error("negative input not allowed");
		                             return x;
	                             }));

	auto [ok, err] = lua.run_script("boom(-1)");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("negative input not allowed"));
}

TEST_CASE("expose_func: exception thrown in C++ callable surfaces via call<>", "[expose_func][exceptions]")
{
	Lua lua;
	lua.expose_func<int>("boom", std::function<int(int)>(
	                             [](int x) -> int
	                             {
		                             if(x < 0)
			                             throw std::logic_error("logic_error path");
		                             return x;
	                             }));

	auto [ok, err, val] = lua.call<int>("boom", -5);
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("logic_error path"));
}

TEST_CASE("expose_func: exception does not prevent a subsequent successful call", "[expose_func][exceptions][stack]")
{
	// Exercises the same setjmp/longjmp-vs-C++-exception stack-hygiene concern
	// as the "succeeds after an error" tests above, but triggered by a C++
	// exception unwinding through the trampoline rather than luaL_error being
	// called directly for an argument-count/type mismatch.
	Lua lua;
	lua.expose_func<int>("boom", std::function<int(int)>(
	                             [](int x) -> int
	                             {
		                             if(x < 0)
			                             throw std::runtime_error("boom");
		                             return x;
	                             }));

	auto [ok1, err1] = lua.run_script("boom(-1)");
	REQUIRE_FALSE(ok1);

	// Stack must be clean; next call must succeed.
	auto [ok2, err2, result] = lua.call<int>("boom", 42);
	REQUIRE(ok2);
	REQUIRE(result == 42);
}

TEST_CASE("expose_func: exception thrown mid-call destroys captured shared_ptr owner correctly",
          "[expose_func][exceptions][lifetime]")
{
	// Confirms that when an exception unwinds through the trampoline, any
	// local C++ objects on that stack frame (here, the locked shared_ptr to
	// the registered LuaFunc itself) are destroyed via normal C++ stack
	// unwinding rather than being skipped, as a raw longjmp would do.
	auto counter = std::make_shared<int>(0);
	std::weak_ptr<int> weak_counter = counter;

	{
		Lua lua;
		lua.expose_func("boom", counter, std::function<void()>([]() { throw std::runtime_error("boom"); }));

		auto [ok, err] = lua.run_script("boom()");
		REQUIRE_FALSE(ok);
		REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("boom"));
		REQUIRE_FALSE(weak_counter.expired()); // lua + our local counter still keep it alive
	}

	counter.reset();
	REQUIRE(weak_counter.expired()); // no leaked reference kept by the trampoline
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

TEST_CASE("expose_method: shared_ptr owner keeps referenced object alive", "[expose_method][lifetime]")
{
	Lua lua;
	auto log = std::make_shared<std::vector<int>>();

	lua.expose_method<Point>("log_x", log,
	                         std::function<void(Point)>([raw = log.get()](Point p) { raw->push_back(p.x); }));
	lua.assign("p", Point{5, 6});
	lua.run_script("p:log_x(); p:log_x()");

	REQUIRE(log->size() == 2);
	REQUIRE((*log)[0] == 5);
	REQUIRE((*log)[1] == 5);
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

// expose_method - C++ exception handling

TEST_CASE("expose_method: std::exception thrown in C++ callable surfaces as Lua error", "[expose_method][exceptions]")
{
	Lua lua;
	lua.expose_method<Point, int>("safe_div", std::function<int(Point, int)>(
	                                          [](Point p, int divisor) -> int
	                                          {
		                                          if(divisor == 0)
			                                          throw std::runtime_error("division by zero");
		                                          return p.x / divisor;
	                                          }));
	lua.assign("p", Point{10, 20});
	auto [ok, err] = lua.run_script("p:safe_div(0)");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("division by zero"));
}

TEST_CASE("expose_method: exception does not prevent a subsequent successful call",
          "[expose_method][exceptions][stack]")
{
	Lua lua;
	lua.expose_method<Point, int>("safe_div", std::function<int(Point, int)>(
	                                          [](Point p, int divisor) -> int
	                                          {
		                                          if(divisor == 0)
			                                          throw std::runtime_error("division by zero");
		                                          return p.x / divisor;
	                                          }));
	lua.assign("p", Point{10, 20});

	auto [ok1, err1] = lua.run_script("p:safe_div(0)");
	REQUIRE_FALSE(ok1);

	// Stack must be clean; next call must succeed.
	auto [ok2, err2] = lua.run_script("assert(p:safe_div(2) == 5)");
	REQUIRE(ok2);
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

// expose_mutable_method - C++ exception handling

TEST_CASE("expose_mutable_method: std::exception thrown in C++ callable surfaces as Lua error",
          "[expose_mutable_method][exceptions]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("safe_scale", std::function<void(Point&, int)>(
	                                               [](Point& p, int factor)
	                                               {
		                                               if(factor == 0)
			                                               throw std::runtime_error("scale factor cannot be zero");
		                                               p.x *= factor;
		                                               p.y *= factor;
	                                               }));
	lua.assign("p", Point{2, 3});
	auto [ok, err] = lua.run_script("p:safe_scale(0)");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("scale factor cannot be zero"));
}

TEST_CASE("expose_mutable_method: self is left unmodified in Lua when the callable throws before mutating",
          "[expose_mutable_method][exceptions]")
{
	// The callable throws before touching p, so no field writeback should
	// have happened - guards against the exception path accidentally
	// running write_struct_back with partially/un-mutated data in a way that
	// corrupts the Lua-side table.
	Lua lua;
	lua.expose_mutable_method<Point>("safe_scale", std::function<void(Point&, int)>(
	                                               [](Point& p, int factor)
	                                               {
		                                               if(factor == 0)
			                                               throw std::runtime_error("scale factor cannot be zero");
		                                               p.x *= factor;
		                                               p.y *= factor;
	                                               }));
	lua.assign("p", Point{2, 3});
	auto [ok, err] = lua.run_script("p:safe_scale(0)");
	REQUIRE_FALSE(ok);

	auto [ok2, err2] = lua.run_script("assert(p.x == 2 and p.y == 3)");
	REQUIRE(ok2);
}

TEST_CASE("expose_mutable_method: exception does not prevent a subsequent successful call",
          "[expose_mutable_method][exceptions][stack]")
{
	Lua lua;
	lua.expose_mutable_method<Point>("safe_scale", std::function<void(Point&, int)>(
	                                               [](Point& p, int factor)
	                                               {
		                                               if(factor == 0)
			                                               throw std::runtime_error("scale factor cannot be zero");
		                                               p.x *= factor;
		                                               p.y *= factor;
	                                               }));
	lua.assign("p", Point{2, 3});

	auto [ok1, err1] = lua.run_script("p:safe_scale(0)");
	REQUIRE_FALSE(ok1);

	// Stack must be clean; next call must succeed and actually mutate self.
	auto [ok2, err2] = lua.run_script("p:safe_scale(2); assert(p.x == 4 and p.y == 6)");
	REQUIRE(ok2);
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

// ============================================================
// New struct types: container-valued fields
// ============================================================

struct Bag
{
	std::vector<int> items;
};
LUA_REGISTER_STRUCT(Bag, lua_field("items", &Bag::items))

struct Registry
{
	std::map<std::string, int> entries;
};
LUA_REGISTER_STRUCT(Registry, lua_field("entries", &Registry::entries))

// ============================================================
// assign - remaining supported types
// ============================================================

TEST_CASE("assign: bool visible in Lua as boolean", "[assign]")
{
	Lua lua;
	lua.assign("t", true);
	lua.assign("f", false);
	auto [ok, err] = lua.run_script("assert(t == true and f == false)");
	REQUIRE(ok);
}

TEST_CASE("assign: long visible in Lua", "[assign]")
{
	Lua lua;
	lua.assign("n", 1000000L);
	auto [ok, err] = lua.run_script("assert(n == 1000000)");
	REQUIRE(ok);
}

TEST_CASE("assign: float visible in Lua", "[assign]")
{
	Lua lua;
	lua.assign("x", 3.5f);
	auto [ok, err] = lua.run_script("assert(x == 3.5)");
	REQUIRE(ok);
}

TEST_CASE("assign: struct (Point) visible in Lua as table", "[assign][struct]")
{
	Lua lua;
	lua.assign("p", Point{7, 8});
	auto [ok, err] = lua.run_script("assert(p.x == 7 and p.y == 8)");
	REQUIRE(ok);
}

TEST_CASE("assign: nested struct (Rect) visible in Lua as nested table", "[assign][struct]")
{
	Lua lua;
	lua.assign("r", Rect{Point{1, 2}, 10, 20});
	auto [ok, err] = lua.run_script("assert(r.origin.x == 1 and r.w == 10)");
	REQUIRE(ok);
}

TEST_CASE("assign: vector<int> visible in Lua as table", "[assign][vector]")
{
	Lua lua;
	lua.assign("v", std::vector<int>{10, 20, 30});
	auto [ok, err] = lua.run_script("assert(v[1] == 10 and v[2] == 20 and v[3] == 30)");
	REQUIRE(ok);
}

TEST_CASE("assign: vector<double> visible in Lua as table", "[assign][vector]")
{
	Lua lua;
	lua.assign("v", std::vector<double>{1.5, 2.5});
	auto [ok, err] = lua.run_script("assert(v[1] == 1.5 and v[2] == 2.5)");
	REQUIRE(ok);
}

TEST_CASE("assign: vector<string> visible in Lua as table", "[assign][vector]")
{
	Lua lua;
	lua.assign("v", std::vector<std::string>{"a", "b", "c"});
	auto [ok, err] = lua.run_script("assert(v[1] == 'a' and v[3] == 'c')");
	REQUIRE(ok);
}

TEST_CASE("assign: vector<Point> visible in Lua as table of tables", "[assign][vector][struct]")
{
	Lua lua;
	lua.assign("v", std::vector<Point>{{1, 2}, {3, 4}});
	auto [ok, err] = lua.run_script("assert(v[1].x == 1 and v[2].y == 4)");
	REQUIRE(ok);
}

TEST_CASE("assign: map<string,int> visible in Lua as table", "[assign][map]")
{
	Lua lua;
	lua.assign("m", std::map<std::string, int>{{"x", 10}, {"y", 20}});
	auto [ok, err] = lua.run_script("assert(m.x == 10 and m.y == 20)");
	REQUIRE(ok);
}

TEST_CASE("assign: unordered_map<string,int> visible in Lua as table", "[assign][map]")
{
	Lua lua;
	lua.assign("m", std::unordered_map<std::string, int>{{"a", 1}, {"b", 2}});
	auto [ok, err] = lua.run_script("assert(m.a == 1 and m.b == 2)");
	REQUIRE(ok);
}

TEST_CASE("assign: struct with vector field (Bag) visible in Lua", "[assign][struct][vector]")
{
	Lua lua;
	lua.assign("b", Bag{{1, 2, 3}});
	auto [ok, err] = lua.run_script("assert(b.items[1] == 1 and b.items[3] == 3)");
	REQUIRE(ok);
}

TEST_CASE("assign: struct with map field (Registry) visible in Lua", "[assign][struct][map]")
{
	Lua lua;
	lua.assign("reg", Registry{{{"score", 100}}});
	auto [ok, err] = lua.run_script("assert(reg.entries.score == 100)");
	REQUIRE(ok);
}

// ============================================================
// call<> - remaining type combinations
// ============================================================

TEST_CASE("call: bool arg and bool return", "[call]")
{
	// bool maps to a native Lua boolean; true/false round-trip correctly.
	Lua lua;
	lua.run_script("function id(x) return x end");
	auto [ok1, err1, r1] = lua.call<bool>("id", true);
	REQUIRE(ok1);
	REQUIRE(r1 == true);
	auto [ok2, err2, r2] = lua.call<bool>("id", false);
	REQUIRE(ok2);
	REQUIRE(r2 == false);
}

TEST_CASE("call: vector<double> round-trip", "[call][vector]")
{
	Lua lua;
	lua.run_script("function identity(t) return t end");
	std::vector<double> input{1.1, 2.2, 3.3};
	auto [ok, err, result] = lua.call<std::vector<double>>("identity", input);
	REQUIRE(ok);
	REQUIRE(result.size() == 3);
	REQUIRE(result[0] == Catch::Approx(1.1));
	REQUIRE(result[2] == Catch::Approx(3.3));
}

TEST_CASE("call: vector<Point> returned from Lua", "[call][vector][struct]")
{
	Lua lua;
	lua.run_script("function make() return {{x=1,y=2},{x=3,y=4}} end");
	auto [ok, err, result] = lua.call<std::vector<Point>>("make");
	REQUIRE(ok);
	REQUIRE(result.size() == 2);
	REQUIRE(result[0].x == 1);
	REQUIRE(result[1].y == 4);
}

TEST_CASE("call: map<int,string> arg and return", "[call][map]")
{
	Lua lua;
	lua.run_script("function identity(t) return t end");
	std::map<int, std::string> input{{1, "one"}, {2, "two"}};
	auto [ok, err, result] = lua.call<std::map<int, std::string>>("identity", input);
	REQUIRE(ok);
	REQUIRE(result.at(1) == "one");
	REQUIRE(result.at(2) == "two");
}

TEST_CASE("call: nested vector<vector<int>> round-trip", "[call][vector]")
{
	Lua lua;
	lua.run_script("function identity(t) return t end");
	std::vector<std::vector<int>> input{{1, 2}, {3, 4}, {5}};
	auto [ok, err, result] = lua.call<std::vector<std::vector<int>>>("identity", input);
	REQUIRE(ok);
	REQUIRE(result.size() == 3);
	REQUIRE(result[0] == std::vector<int>{1, 2});
	REQUIRE(result[2] == std::vector<int>{5});
}

TEST_CASE("call: map<string,vector<int>> round-trip", "[call][map][vector]")
{
	Lua lua;
	lua.run_script("function identity(t) return t end");
	std::map<std::string, std::vector<int>> input{{"a", {1, 2}}, {"b", {3}}};
	auto [ok, err, result] = lua.call<std::map<std::string, std::vector<int>>>("identity", input);
	REQUIRE(ok);
	REQUIRE(result.at("a") == std::vector<int>{1, 2});
	REQUIRE(result.at("b") == std::vector<int>{3});
}

TEST_CASE("call: Bag (struct with vector field) round-trip", "[call][struct][vector]")
{
	Lua lua;
	lua.run_script("function identity(b) return b end");
	Bag bag{{10, 20, 30}};
	auto [ok, err, result] = lua.call<Bag>("identity", bag);
	REQUIRE(ok);
	REQUIRE(result.items == std::vector<int>{10, 20, 30});
}

TEST_CASE("call: Registry (struct with map field) round-trip", "[call][struct][map]")
{
	Lua lua;
	lua.run_script("function identity(r) return r end");
	Registry reg{{{"x", 1}, {"y", 2}}};
	auto [ok, err, result] = lua.call<Registry>("identity", reg);
	REQUIRE(ok);
	REQUIRE(result.entries.at("x") == 1);
	REQUIRE(result.entries.at("y") == 2);
}

// ============================================================
// expose_func - remaining types
// ============================================================

TEST_CASE("expose_func: bool arg and bool return", "[expose_func]")
{
	// bool maps to a native Lua boolean.
	Lua lua;
	lua.expose_func<bool>("logic_or", std::function<bool(bool, bool)>([](bool a, bool b) { return a || b; }));
	auto [ok, err, result] = lua.call<bool>("logic_or", false, true);
	REQUIRE(ok);
	REQUIRE(result == true);
	auto [ok2, err2] = lua.run_script("assert(logic_or(false, false) == false)");
	REQUIRE(ok2);
}

TEST_CASE("expose_func: long arg and long return", "[expose_func]")
{
	Lua lua;
	lua.expose_func<long>("double_long", std::function<long(long)>([](long x) { return x * 2L; }));
	auto [ok, err, result] = lua.call<long>("double_long", 1000000L);
	REQUIRE(ok);
	REQUIRE(result == 2000000L);
}

TEST_CASE("expose_func: float arg and float return", "[expose_func]")
{
	Lua lua;
	lua.expose_func<float>("half_float", std::function<float(float)>([](float x) { return x / 2.0f; }));
	auto [ok, err, result] = lua.call<float>("half_float", 7.0f);
	REQUIRE(ok);
	REQUIRE(result == Catch::Approx(3.5f));
}

TEST_CASE("expose_func: vector<int> arg", "[expose_func][vector]")
{
	Lua lua;
	lua.expose_func<int>("vec_sum", std::function<int(std::vector<int>)>(
	                                [](std::vector<int> v)
	                                {
		                                int s = 0;
		                                for(auto x : v)
			                                s += x;
		                                return s;
	                                }));
	auto [ok, err, result] = lua.call<int>("vec_sum", std::vector<int>{1, 2, 3, 4});
	REQUIRE(ok);
	REQUIRE(result == 10);
}

TEST_CASE("expose_func: vector<int> return", "[expose_func][vector]")
{
	Lua lua;
	lua.expose_func<std::vector<int>>("range", std::function<std::vector<int>(int)>(
	                                           [](int n)
	                                           {
		                                           std::vector<int> v;
		                                           for(int i = 1; i <= n; ++i)
			                                           v.push_back(i);
		                                           return v;
	                                           }));
	auto [ok, err, result] = lua.call<std::vector<int>>("range", 4);
	REQUIRE(ok);
	REQUIRE(result == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("expose_func: vector<double> arg and return", "[expose_func][vector]")
{
	Lua lua;
	lua.expose_func<std::vector<double>>("scale_vec", std::function<std::vector<double>(std::vector<double>, double)>(
	                                                  [](std::vector<double> v, double f)
	                                                  {
		                                                  for(auto& x : v)
			                                                  x *= f;
		                                                  return v;
	                                                  }));
	auto [ok, err, result] = lua.call<std::vector<double>>("scale_vec", std::vector<double>{1.0, 2.0, 4.0}, 2.5);
	REQUIRE(ok);
	REQUIRE(result[0] == Catch::Approx(2.5));
	REQUIRE(result[1] == Catch::Approx(5.0));
	REQUIRE(result[2] == Catch::Approx(10.0));
}

TEST_CASE("expose_func: vector<string> arg and return", "[expose_func][vector]")
{
	Lua lua;
	lua.expose_func<std::vector<std::string>>("exclaim",
	                                          std::function<std::vector<std::string>(std::vector<std::string>)>(
	                                          [](std::vector<std::string> v)
	                                          {
		                                          for(auto& s : v)
			                                          s += "!";
		                                          return v;
	                                          }));
	auto [ok, err, result] = lua.call<std::vector<std::string>>("exclaim", std::vector<std::string>{"hi", "bye"});
	REQUIRE(ok);
	REQUIRE(result[0] == "hi!");
	REQUIRE(result[1] == "bye!");
}

TEST_CASE("expose_func: vector<Point> arg and return", "[expose_func][vector][struct]")
{
	Lua lua;
	lua.expose_func<std::vector<Point>>("shift_all", std::function<std::vector<Point>(std::vector<Point>, int, int)>(
	                                                 [](std::vector<Point> pts, int dx, int dy)
	                                                 {
		                                                 for(auto& p : pts)
		                                                 {
			                                                 p.x += dx;
			                                                 p.y += dy;
		                                                 }
		                                                 return pts;
	                                                 }));
	auto [ok, err, result] = lua.call<std::vector<Point>>("shift_all", std::vector<Point>{{1, 2}, {3, 4}}, 10, 10);
	REQUIRE(ok);
	REQUIRE(result[0].x == 11);
	REQUIRE(result[1].y == 14);
}

TEST_CASE("expose_func: map<string,int> arg", "[expose_func][map]")
{
	Lua lua;
	lua.expose_func<int>("map_sum", std::function<int(std::map<std::string, int>)>(
	                                [](std::map<std::string, int> m)
	                                {
		                                int s = 0;
		                                for(auto& [k, v] : m)
			                                s += v;
		                                return s;
	                                }));
	auto [ok, err, result] = lua.call<int>("map_sum", std::map<std::string, int>{{"a", 1}, {"b", 2}, {"c", 3}});
	REQUIRE(ok);
	REQUIRE(result == 6);
}

TEST_CASE("expose_func: map<string,int> return", "[expose_func][map]")
{
	Lua lua;
	lua.expose_func<std::map<std::string, int>>("make_counters",
	                                            std::function<std::map<std::string, int>(int)>(
	                                            [](int n)
	                                            { return std::map<std::string, int>{{"n", n}, {"n2", n * n}}; }));
	auto [ok, err, result] = lua.call<std::map<std::string, int>>("make_counters", 3);
	REQUIRE(ok);
	REQUIRE(result.at("n") == 3);
	REQUIRE(result.at("n2") == 9);
}

TEST_CASE("expose_func: unordered_map<string,int> arg and return", "[expose_func][map]")
{
	Lua lua;
	lua.expose_func<std::unordered_map<std::string, int>>("negate_all",
	                                                      std::function<std::unordered_map<std::string, int>(
	                                                      std::unordered_map<std::string, int>)>(
	                                                      [](std::unordered_map<std::string, int> m)
	                                                      {
		                                                      for(auto& [k, v] : m)
			                                                      v = -v;
		                                                      return m;
	                                                      }));
	std::unordered_map<std::string, int> input{{"x", 5}, {"y", 10}};
	auto [ok, err, result] = lua.call<std::unordered_map<std::string, int>>("negate_all", input);
	REQUIRE(ok);
	REQUIRE(result.at("x") == -5);
	REQUIRE(result.at("y") == -10);
}

TEST_CASE("expose_func: nested struct (Rect) arg and return", "[expose_func][struct]")
{
	Lua lua;
	lua.expose_func<Rect>("scale_rect",
	                      std::function<Rect(Rect, int)>(
	                      [](Rect r, int f) { return Rect{Point{r.origin.x * f, r.origin.y * f}, r.w * f, r.h * f}; }));
	auto [ok, err, result] = lua.call<Rect>("scale_rect", Rect{Point{1, 2}, 3, 4}, 2);
	REQUIRE(ok);
	REQUIRE(result.origin.x == 2);
	REQUIRE(result.origin.y == 4);
	REQUIRE(result.w == 6);
	REQUIRE(result.h == 8);
}

TEST_CASE("expose_func: Bag (struct with vector field) arg and return", "[expose_func][struct][vector]")
{
	Lua lua;
	lua.expose_func<Bag>("bag_append", std::function<Bag(Bag, int)>(
	                                   [](Bag b, int x)
	                                   {
		                                   b.items.push_back(x);
		                                   return b;
	                                   }));
	auto [ok, err, result] = lua.call<Bag>("bag_append", Bag{{1, 2}}, 3);
	REQUIRE(ok);
	REQUIRE(result.items == std::vector<int>{1, 2, 3});
}

TEST_CASE("expose_func: Registry (struct with map field) arg and return", "[expose_func][struct][map]")
{
	Lua lua;
	lua.expose_func<Registry>("reg_insert", std::function<Registry(Registry, std::string, int)>(
	                                        [](Registry r, std::string k, int v)
	                                        {
		                                        r.entries[k] = v;
		                                        return r;
	                                        }));
	auto [ok, err, result] = lua.call<Registry>("reg_insert", Registry{{}}, std::string("score"), 99);
	REQUIRE(ok);
	REQUIRE(result.entries.at("score") == 99);
}

// ============================================================
// expose_method - container arg / return types
// ============================================================

TEST_CASE("expose_method: vector<int> return", "[expose_method][vector]")
{
	Lua lua;
	lua.expose_method<Point, std::vector<int>>("as_vec", std::function<std::vector<int>(Point)>(
	                                                     [](Point p) { return std::vector<int>{p.x, p.y}; }));
	lua.assign("p", Point{5, 6});
	lua.run_script("function get_vec() return p:as_vec() end");
	auto [ok, err, result] = lua.call<std::vector<int>>("get_vec");
	REQUIRE(ok);
	REQUIRE(result == std::vector<int>{5, 6});
}

TEST_CASE("expose_method: vector<Point> arg", "[expose_method][vector][struct]")
{
	Lua lua;
	lua.expose_method<Point, int>("dot_sum", std::function<int(Point, std::vector<Point>)>(
	                                         [](Point p, std::vector<Point> others)
	                                         {
		                                         int s = 0;
		                                         for(auto& o : others)
			                                         s += p.x * o.x + p.y * o.y;
		                                         return s;
	                                         }));
	lua.assign("p", Point{1, 2});
	// (1*3 + 2*4) + (1*1 + 2*1) = 11 + 3 = 14
	lua.run_script("function calc() return p:dot_sum({{x=3,y=4},{x=1,y=1}}) end");
	auto [ok, err, result] = lua.call<int>("calc");
	REQUIRE(ok);
	REQUIRE(result == 14);
}

TEST_CASE("expose_method: map<string,int> return", "[expose_method][map]")
{
	Lua lua;
	lua.expose_method<Point, std::map<std::string, int>>("as_map", std::function<std::map<std::string, int>(Point)>(
	                                                               [](Point p)
	                                                               {
		                                                               return std::map<std::string, int>{{"x", p.x},
		                                                                                                 {"y", p.y}};
	                                                               }));
	lua.assign("p", Point{7, 8});
	lua.run_script("function get_map() return p:as_map() end");
	auto [ok, err, result] = lua.call<std::map<std::string, int>>("get_map");
	REQUIRE(ok);
	REQUIRE(result.at("x") == 7);
	REQUIRE(result.at("y") == 8);
}

TEST_CASE("expose_method: Bag::sum - method on struct with vector field", "[expose_method][struct][vector]")
{
	Lua lua;
	lua.expose_method<Bag, int>("sum", std::function<int(Bag)>(
	                                   [](Bag b)
	                                   {
		                                   int s = 0;
		                                   for(auto x : b.items)
			                                   s += x;
		                                   return s;
	                                   }));
	lua.assign("b", Bag{{1, 2, 3, 4}});
	auto [ok, err] = lua.run_script("assert(b:sum() == 10)");
	REQUIRE(ok);
}

TEST_CASE("expose_method: Registry::lookup - method with map field and string arg", "[expose_method][struct][map]")
{
	Lua lua;
	lua.expose_method<Registry, int>("lookup", std::function<int(Registry, std::string)>(
	                                           [](Registry r, std::string k)
	                                           { return r.entries.count(k) ? r.entries.at(k) : -1; }));
	lua.assign("reg", Registry{{{"x", 42}}});
	auto [ok1, err1] = lua.run_script("assert(reg:lookup('x') == 42)");
	REQUIRE(ok1);
	auto [ok2, err2] = lua.run_script("assert(reg:lookup('missing') == -1)");
	REQUIRE(ok2);
}

// ============================================================
// expose_mutable_method - container-field mutations
// ============================================================

TEST_CASE("expose_mutable_method: Bag::push_item appends to vector field in place",
          "[expose_mutable_method][struct][vector]")
{
	Lua lua;
	lua.expose_mutable_method<Bag>("push_item",
	                               std::function<void(Bag&, int)>([](Bag& b, int x) { b.items.push_back(x); }));
	lua.assign("b", Bag{{1, 2}});
	lua.run_script("b:push_item(3)");
	lua.run_script("b:push_item(4)");
	auto [ok, err] = lua.run_script("assert(b.items[3] == 3 and b.items[4] == 4)");
	REQUIRE(ok);
}

TEST_CASE("expose_mutable_method: Bag::clear empties vector field in place", "[expose_mutable_method][struct][vector]")
{
	Lua lua;
	lua.expose_mutable_method<Bag>("clear", std::function<void(Bag&)>([](Bag& b) { b.items.clear(); }));
	lua.assign("b", Bag{{1, 2, 3}});
	lua.run_script("b:clear()");
	auto [ok, err] = lua.run_script("assert(#b.items == 0)");
	REQUIRE(ok);
}

TEST_CASE("expose_mutable_method: Bag::pop - scalar return plus vector mutation",
          "[expose_mutable_method][struct][vector]")
{
	Lua lua;
	lua.expose_mutable_method<Bag, int>("pop", std::function<int(Bag&)>(
	                                           [](Bag& b)
	                                           {
		                                           if(b.items.empty())
			                                           return -1;
		                                           int v = b.items.back();
		                                           b.items.pop_back();
		                                           return v;
	                                           }));
	lua.assign("b", Bag{{10, 20, 30}});
	lua.run_script("function do_pop() return b:pop() end");
	auto [ok1, err1, v1] = lua.call<int>("do_pop");
	REQUIRE(ok1);
	REQUIRE(v1 == 30);
	auto [ok2, err2] = lua.run_script("assert(#b.items == 2)");
	REQUIRE(ok2);
}

TEST_CASE("expose_mutable_method: Registry::set inserts into map field in place",
          "[expose_mutable_method][struct][map]")
{
	Lua lua;
	lua.expose_mutable_method<Registry>("set", std::function<void(Registry&, std::string, int)>(
	                                           [](Registry& r, std::string k, int v) { r.entries[k] = v; }));
	lua.assign("reg", Registry{{}});
	lua.run_script("reg:set('score', 42)");
	lua.run_script("reg:set('level', 7)");
	auto [ok, err] = lua.run_script("assert(reg.entries.score == 42 and reg.entries.level == 7)");
	REQUIRE(ok);
}

TEST_CASE("expose_mutable_method: Registry::erase removes from map field in place",
          "[expose_mutable_method][struct][map]")
{
	Lua lua;
	lua.expose_mutable_method<Registry>("erase", std::function<void(Registry&, std::string)>(
	                                             [](Registry& r, std::string k) { r.entries.erase(k); }));
	lua.assign("reg", Registry{{{"x", 1}, {"y", 2}}});
	lua.run_script("reg:erase('x')");
	auto [ok, err] = lua.run_script("assert(reg.entries.x == nil and reg.entries.y == 2)");
	REQUIRE(ok);
}

// ============================================================
// Complex type construction from Lua
//
// Config has four field types: std::string, int,
// std::vector<std::string>, and std::map<std::string,int>.
// These tests show three ways Lua can create and return a
// fully-populated instance to C++.
// ============================================================

struct Config
{
	std::string name;
	int port;
	std::vector<std::string> tags;
	std::map<std::string, int> settings;
};
LUA_REGISTER_STRUCT(Config,
                    lua_field("name", &Config::name),
                    lua_field("port", &Config::port),
                    lua_field("tags", &Config::tags),
                    lua_field("settings", &Config::settings))

// ------------------------------------------------------------
// 1. Pure table literal - no factory, no exposed methods.
//    Lua constructs the entire instance from Lua-side literals.
// ------------------------------------------------------------
TEST_CASE("Lua constructs Config from pure table literal", "[construction][struct]")
{
	Lua lua;

	lua.run_script(R"(
		function make_config()
			return {
				name     = "server",
				port     = 8080,
				tags     = {"http", "v2"},
				settings = {timeout = 30, retries = 3}
			}
		end
	)");

	auto [ok, err, cfg] = lua.call<Config>("make_config");
	REQUIRE(ok);
	REQUIRE(cfg.name == "server");
	REQUIRE(cfg.port == 8080);
	REQUIRE(cfg.tags == std::vector<std::string>{"http", "v2"});
	REQUIRE(cfg.settings.at("timeout") == 30);
	REQUIRE(cfg.settings.at("retries") == 3);
}

// ------------------------------------------------------------
// 2. Factory + mutable builder.
//    C++ exposes a constructor and two mutating methods.
//    Lua calls the factory, calls the methods to populate
//    the instance, then returns the completed object.
// ------------------------------------------------------------
TEST_CASE("Lua constructs Config via factory and mutable builder",
          "[construction][expose_func][expose_mutable_method][struct]")
{
	Lua lua;

	// Factory: create a Config with name + port; tags and settings empty.
	lua.expose_func<Config>("new_config",
	                        std::function<Config(std::string, int)>([](std::string name, int port)
	                                                                { return Config{std::move(name), port, {}, {}}; }));

	// Builder: append one tag.
	lua.expose_mutable_method<Config>("add_tag", std::function<void(Config&, std::string)>(
	                                             [](Config& c, std::string tag) { c.tags.push_back(std::move(tag)); }));

	// Builder: insert / overwrite one settings entry.
	lua.expose_mutable_method<Config>("set", std::function<void(Config&, std::string, int)>(
	                                         [](Config& c, std::string k, int v) { c.settings[std::move(k)] = v; }));

	// Lua creates the instance entirely on its own side.
	lua.run_script(R"(
		function build_config()
			local cfg = new_config("server", 8080)
			cfg:add_tag("http")
			cfg:add_tag("v2")
			cfg:set("timeout", 30)
			cfg:set("retries", 3)
			return cfg
		end
	)");

	auto [ok, err, cfg] = lua.call<Config>("build_config");
	REQUIRE(ok);
	REQUIRE(cfg.name == "server");
	REQUIRE(cfg.port == 8080);
	REQUIRE(cfg.tags == std::vector<std::string>{"http", "v2"});
	REQUIRE(cfg.settings.at("timeout") == 30);
	REQUIRE(cfg.settings.at("retries") == 3);
}

// ------------------------------------------------------------
// 3. Factory-built collection.
//    Lua calls the factory in a loop and returns a
//    vector<Config> - each element fully created in Lua.
// ------------------------------------------------------------
TEST_CASE("Lua builds vector<Config> via factory", "[construction][expose_func][struct][vector]")
{
	Lua lua;

	lua.expose_func<Config>("new_config",
	                        std::function<Config(std::string, int)>([](std::string name, int port)
	                                                                { return Config{std::move(name), port, {}, {}}; }));

	lua.expose_mutable_method<Config>("add_tag", std::function<void(Config&, std::string)>(
	                                             [](Config& c, std::string tag) { c.tags.push_back(std::move(tag)); }));

	lua.run_script(R"(
		function make_services()
			local api = new_config("api",  9000)
			api:add_tag("rest")

			local web = new_config("web",  8080)
			web:add_tag("http")
			web:add_tag("https")

			local rpc = new_config("grpc", 50051)
			rpc:add_tag("grpc")

			return {api, web, rpc}
		end
	)");

	auto [ok, err, cfgs] = lua.call<std::vector<Config>>("make_services");
	REQUIRE(ok);
	REQUIRE(cfgs.size() == 3);

	REQUIRE(cfgs[0].name == "api");
	REQUIRE(cfgs[0].port == 9000);
	REQUIRE(cfgs[0].tags == std::vector<std::string>{"rest"});

	REQUIRE(cfgs[1].name == "web");
	REQUIRE(cfgs[1].port == 8080);
	REQUIRE(cfgs[1].tags == std::vector<std::string>{"http", "https"});

	REQUIRE(cfgs[2].name == "grpc");
	REQUIRE(cfgs[2].port == 50051);
	REQUIRE(cfgs[2].tags == std::vector<std::string>{"grpc"});
}

TEST_CASE("run_script: non-string Lua error produces non-empty message", "[run_script]")
{
	Lua lua;
	auto [ok, err] = lua.run_script("error({})");
	REQUIRE(!ok);
	REQUIRE(!err.empty());
}

TEST_CASE("call: non-string Lua error produces non-empty message", "[call]")
{
	Lua lua;
	lua.run_script("function bad() error({}) end");
	auto [ok, err] = lua.call<>("bad");
	REQUIRE(!ok);
	REQUIRE(!err.empty());
}

TEST_CASE("run_script: scripts that return values run successfully", "[run_script]")
{
	Lua lua;
	lua.run_script("function add(a, b) return a + b end");

	for(int i = 0; i < 500; ++i)
	{
		auto [ok, err] = lua.run_script("return 1, 2, 3, 4, 5");
		REQUIRE(ok);
	}

	auto [ok, err, result] = lua.call<int>("add", 11, 22);
	REQUIRE(ok);
	REQUIRE(result == 33);
}

TEST_CASE("assign: string literal is accepted", "[assign]")
{
	Lua lua;
	lua.assign("s", "literal");
	auto [ok, err] = lua.run_script("assert(s == 'literal')");
	REQUIRE(ok);
}

TEST_CASE("call: string literal passed as arg", "[call]")
{
	Lua lua;
	lua.run_script("function echo(s) return s end");
	auto [ok, err, result] = lua.call<std::string>("echo", "hello");
	REQUIRE(ok);
	REQUIRE(result == "hello");
}

TEST_CASE("assign: std::string preserves embedded null bytes", "[assign][string]")
{
	Lua lua;
	const std::string s("hello\0world", 11);
	lua.assign("s", s);
	auto [ok, err] = lua.run_script("assert(#s == 11)");
	REQUIRE(ok);
}

TEST_CASE("call: std::string with embedded null bytes round-trips", "[call][string]")
{
	Lua lua;
	lua.run_script("function id(x) return x end");
	const std::string s("ab\0cd", 5);
	auto [ok, err, result] = lua.call<std::string>("id", s);
	REQUIRE(ok);
	REQUIRE(result.size() == 5);
	REQUIRE(result == s);
}

// ============================================================
// Mixed-type struct
//
// Device combines every supported field kind in a single struct:
//   int, float, bool, std::string, nested struct (Point),
//   std::vector<std::string>, std::map<std::string, int>.
// ============================================================

struct Device
{
	std::string id;
	int channel;
	float threshold;
	bool active;
	Point position;
	std::vector<std::string> labels;
	std::map<std::string, int> counters;
};
LUA_REGISTER_STRUCT(Device,
                    lua_field("id", &Device::id),
                    lua_field("channel", &Device::channel),
                    lua_field("threshold", &Device::threshold),
                    lua_field("active", &Device::active),
                    lua_field("position", &Device::position),
                    lua_field("labels", &Device::labels),
                    lua_field("counters", &Device::counters))

TEST_CASE("mixed-type struct: round-trip through Lua preserves all fields", "[struct][mixed]")
{
	Lua lua;

	// Lua receives the struct as a table, reads two fields, returns it unchanged.
	lua.run_script(R"(
		function identity(d)
			assert(d.id        == "sensor-1")
			assert(d.channel   == 3)
			assert(d.active    == true)
			return d
		end
	)");

	Device
	input{"sensor-1", 3, 0.75f, true, Point{10, 20}, {"temperature", "humidity"}, {{"reads", 42}, {"errors", 1}}};

	auto [ok, err, d] = lua.call<Device>("identity", input);
	REQUIRE(ok);
	REQUIRE(d.id == "sensor-1");
	REQUIRE(d.channel == 3);
	REQUIRE(d.threshold == Catch::Approx(0.75f));
	REQUIRE(d.active == true);
	REQUIRE(d.position.x == 10);
	REQUIRE(d.position.y == 20);
	REQUIRE(d.labels == std::vector<std::string>{"temperature", "humidity"});
	REQUIRE(d.counters.at("reads") == 42);
	REQUIRE(d.counters.at("errors") == 1);
}

TEST_CASE("mixed-type struct: constructed entirely in Lua", "[struct][mixed][construction]")
{
	Lua lua;

	lua.run_script(R"(
		function make_device()
			return {
				id        = "actuator-7",
				channel   = 12,
				threshold = 1.5,
				active    = false,
				position  = {x = 5, y = -3},
				labels    = {"motor", "pwm", "output"},
				counters  = {writes = 100, overflows = 0}
			}
		end
	)");

	auto [ok, err, d] = lua.call<Device>("make_device");
	REQUIRE(ok);
	REQUIRE(d.id == "actuator-7");
	REQUIRE(d.channel == 12);
	REQUIRE(d.threshold == Catch::Approx(1.5f));
	REQUIRE(d.active == false);
	REQUIRE(d.position.x == 5);
	REQUIRE(d.position.y == -3);
	REQUIRE(d.labels == std::vector<std::string>{"motor", "pwm", "output"});
	REQUIRE(d.counters.at("writes") == 100);
	REQUIRE(d.counters.at("overflows") == 0);
}

// ============================================================
// Deeply nested tables and arrays (5 levels)
// ============================================================

// --- nested structs (5 levels deep) ---

struct L5
{
	int value;
};
LUA_REGISTER_STRUCT(L5, lua_field("value", &L5::value))

struct L4
{
	L5 inner;
};
LUA_REGISTER_STRUCT(L4, lua_field("inner", &L4::inner))

struct L3
{
	L4 inner;
};
LUA_REGISTER_STRUCT(L3, lua_field("inner", &L3::inner))

struct L2
{
	L3 inner;
};
LUA_REGISTER_STRUCT(L2, lua_field("inner", &L2::inner))

struct L1
{
	L2 inner;
};
LUA_REGISTER_STRUCT(L1, lua_field("inner", &L1::inner))

TEST_CASE("Lua returns 5-deep nested struct", "[nested][struct]")
{
	Lua lua;

	lua.run_script(R"(
		function make_deep()
			return {
				inner = {
					inner = {
						inner = {
							inner = {
								value = 42
							}
						}
					}
				}
			}
		end
	)");

	auto [ok, err, result] = lua.call<L1>("make_deep");
	REQUIRE(ok);
	REQUIRE(result.inner.inner.inner.inner.value == 42);
}

// --- 5-deep nested vector (vector<vector<vector<vector<vector<int>>>>>) ---

TEST_CASE("Lua returns 5-deep nested array", "[nested][vector]")
{
	Lua lua;

	lua.run_script(R"(
		function make_deep_array()
			return { { { { { 1, 2, 3 } } } } }
		end
	)");

	using V5 = std::vector<int>;
	using V4 = std::vector<V5>;
	using V3 = std::vector<V4>;
	using V2 = std::vector<V3>;
	using V1 = std::vector<V2>;

	auto [ok, err, result] = lua.call<V1>("make_deep_array");
	REQUIRE(ok);
	REQUIRE(result.size() == 1);
	REQUIRE(result[0].size() == 1);
	REQUIRE(result[0][0].size() == 1);
	REQUIRE(result[0][0][0].size() == 1);
	REQUIRE(result[0][0][0][0] == std::vector<int>{1, 2, 3});
}

// --- 5-deep nested map (map<string, map<string, map<string, map<string, map<string, int>>>>>) ---

TEST_CASE("Lua returns 5-deep nested map", "[nested][map]")
{
	Lua lua;

	lua.run_script(R"(
		function make_deep_map()
			return { a = { b = { c = { d = { e = 99 } } } } }
		end
	)");

	using M5 = std::map<std::string, int>;
	using M4 = std::map<std::string, M5>;
	using M3 = std::map<std::string, M4>;
	using M2 = std::map<std::string, M3>;
	using M1 = std::map<std::string, M2>;

	auto [ok, err, result] = lua.call<M1>("make_deep_map");
	REQUIRE(ok);
	REQUIRE(result.at("a").at("b").at("c").at("d").at("e") == 99);
}

// ============================================================
// Type mismatch errors: vectors, maps, scalars
// ============================================================

TEST_CASE("call: vector with wrong element type returns error", "[call][vector][type]")
{
	Lua lua;
	lua.run_script("function get() return {1, 'two', 3} end");
	auto [ok, err, _1] = lua.call<std::vector<int>>("get");
	REQUIRE(!ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
}

TEST_CASE("call: vector when table not returned returns error", "[call][vector][type]")
{
	Lua lua;
	lua.run_script("function get() return 42 end");
	auto [ok, err, _2] = lua.call<std::vector<int>>("get");
	REQUIRE(!ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected table"));
}

TEST_CASE("call: map with wrong value type returns error", "[call][map][type]")
{
	Lua lua;
	lua.run_script("function get() return {a = 1, b = 'two'} end");
	auto [ok, err, _3] = lua.call<std::map<std::string, int>>("get");
	REQUIRE(!ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
}

TEST_CASE("call: map with wrong key type returns error", "[call][map][type]")
{
	Lua lua;
	lua.run_script("function get() return {[true]=1, [false]=2} end");
	auto [ok, err, _4] = lua.call<std::map<std::string, int>>("get");
	REQUIRE(!ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected string"));
}

TEST_CASE("call: map when table not returned returns error", "[call][map][type]")
{
	Lua lua;
	lua.run_script("function get() return 'not a table' end");
	auto [ok, err, _5] = lua.call<std::map<std::string, int>>("get");
	REQUIRE(!ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected table"));
}

TEST_CASE("call: wrong scalar type returns error", "[call][type]")
{
	Lua lua;
	lua.run_script("function get() return 'hello' end");
	auto [ok, err, _6] = lua.call<int>("get");
	REQUIRE(!ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
}

TEST_CASE("call: nested vector with wrong inner element type returns error", "[call][vector][type]")
{
	Lua lua;
	lua.run_script("function get() return {{1, 2}, {3, 'four'}} end");
	auto [ok, err, _7] = lua.call<std::vector<std::vector<int>>>("get");
	REQUIRE(!ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
}

TEST_CASE("call: nested map with wrong inner value type returns error", "[call][map][type]")
{
	Lua lua;
	lua.run_script("function get() return {x = {a = 1}, y = {b = 'bad'}} end");
	auto [ok, err, _8] = lua.call<std::map<std::string, std::map<std::string, int>>>("get");
	REQUIRE(!ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("expected integer"));
}

// ============================================================
// Coroutines
//
// LuaCpp has no dedicated coroutine API - Lua's `coroutine` library is a
// standard part of luaL_openlibs and is simply Lua code exercised through
// run_script()/call(), the same as any other script. These tests show the
// usual coroutine.create/resume/yield/status/wrap patterns, plus how a
// coroutine can call back into a C++ function registered with expose_func.
//
// See LIFETIME.md, item 5, for the lifetime caveat that applies to any
// coroutine suspended mid-execution: resuming it after the C++ objects its
// captured closures depend on have been destroyed is undefined behaviour,
// exactly like calling those closures directly would be.
// ============================================================

TEST_CASE("coroutine: create, resume and yield via a pure Lua script", "[coroutine]")
{
	Lua lua;

	// A coroutine body that yields twice before returning, collected entirely
	// on the Lua side into a table of {status, values...} snapshots so the
	// whole sequence can be inspected with a single call<>().
	lua.run_script(R"(
		function drive_coroutine()
			local co = coroutine.create(function(a, b)
				local sum = a + b
				local more = coroutine.yield(sum)      -- 1st suspension point
				local doubled = more * 2
				coroutine.yield(doubled)               -- 2nd suspension point
				return "done"
			end)

			local results = {}
			local ok1, v1 = coroutine.resume(co, 3, 4)     -- runs to 1st yield, sum = 7
			table.insert(results, tostring(ok1) .. ":" .. tostring(v1))
			local ok2, v2 = coroutine.resume(co, 10)       -- runs to 2nd yield, doubled = 20
			table.insert(results, tostring(ok2) .. ":" .. tostring(v2))
			local ok3, v3 = coroutine.resume(co)           -- runs to completion, returns "done"
			table.insert(results, tostring(ok3) .. ":" .. tostring(v3))
			table.insert(results, coroutine.status(co))    -- "dead" once the body has returned
			return results
		end
	)");

	auto [ok, err, results] = lua.call<std::vector<std::string>>("drive_coroutine");
	REQUIRE(ok);
	REQUIRE(results == std::vector<std::string>{"true:7", "true:20", "true:done", "dead"});
}

TEST_CASE("coroutine: coroutine.wrap propagates an error as a Lua error", "[coroutine]")
{
	Lua lua;

	// coroutine.wrap returns a plain function (rather than resume's
	// ok/value pair); an error inside the coroutine body re-raises as a
	// normal Lua error at the call site, which run_script() surfaces the
	// same way it would for any other runtime error.
	auto [ok, err] = lua.run_script(R"(
		local step = coroutine.wrap(function()
			coroutine.yield(1)
			error("boom inside coroutine")
		end)
		assert(step() == 1)
		step() -- raises
	)");

	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("boom inside coroutine"));
}

TEST_CASE("coroutine: body calls a C++ function registered with expose_func", "[coroutine][expose_func]")
{
	Lua lua;

	// A coroutine's body runs in its own Lua thread, but expose_func-registered
	// closures are just regular Lua C functions - they can be called from
	// inside a coroutine exactly as from the main thread.
	lua.expose_func<int>("square", std::function<int(int)>([](int x) { return x * x; }));

	lua.run_script(R"(
		function drive()
			local co = coroutine.create(function(n)
				local squared = square(n)          -- call into C++ from inside the coroutine
				local resumed_with = coroutine.yield(squared)
				return square(resumed_with)
			end)
			local _, first = coroutine.resume(co, 5)   -- square(5) == 25
			local _, second = coroutine.resume(co, 6)  -- square(6) == 36
			return first, second
		end
	)");

	auto [ok, err, first, second] = lua.call<int, int>("drive");
	REQUIRE(ok);
	REQUIRE(first == 25);
	REQUIRE(second == 36);
}

// ============================================================
// call tracing
// ============================================================

TEST_CASE("call tracing: records nested call/return events with correct depth", "[call-tracing]")
{
	Lua lua;
	std::vector<LuaCallTraceEvent> events;
	lua.enable_call_tracing([&events](const LuaCallTraceEvent& e) { events.push_back(e); });

	auto [ok, err] = lua.run_script("function inner() return 1 end\n"
	                                "function outer() return inner() end\n"
	                                "outer()\n");
	REQUIRE(ok);

	bool saw_call = false;
	bool saw_return = false;
	for(const auto& e : events)
	{
		if(e.is_call)
		{
			saw_call = true;
		}
		else
		{
			saw_return = true;
		}
		REQUIRE(e.depth >= 1);
	}
	REQUIRE(saw_call);
	REQUIRE(saw_return);
	const auto call_count =
	std::count_if(events.begin(), events.end(), [](const LuaCallTraceEvent& e) { return e.is_call; });
	REQUIRE(call_count >= 2);
}

TEST_CASE("call tracing: disable_call_tracing stops further events", "[call-tracing]")
{
	Lua lua;
	std::vector<LuaCallTraceEvent> events;
	lua.enable_call_tracing([&events](const LuaCallTraceEvent& e) { events.push_back(e); });

	REQUIRE(std::get<0>(lua.run_script("function f() end f()")));
	const auto count_after_first = events.size();
	REQUIRE(count_after_first > 0);

	lua.disable_call_tracing();
	REQUIRE(std::get<0>(lua.run_script("function g() end g()")));
	REQUIRE(events.size() == count_after_first);
}

TEST_CASE("call tracing: tail calls are tracked without corrupting call depth", "[call-tracing]")
{
	Lua lua;
	std::vector<LuaCallTraceEvent> events;
	lua.enable_call_tracing([&events](const LuaCallTraceEvent& e) { events.push_back(e); });

	auto [ok, err] = lua.run_script("function countdown(n)\n"
	                                "  if n <= 0 then return 0 end\n"
	                                "  return countdown(n - 1)\n"
	                                "end\n"
	                                "countdown(20)\n");
	REQUIRE(ok);

	for(const auto& e : events)
	{
		REQUIRE(e.depth >= 1);
		REQUIRE(e.depth <= 25);
	}
}

TEST_CASE("call tracing: std::exception thrown by trace callback is logged, not fatal", "[call-tracing]")
{
	Lua lua;
	std::vector<std::string> logged;
	lua.enable_error_logging([&logged](std::string_view msg) { logged.emplace_back(msg); });
	lua.enable_call_tracing([](const LuaCallTraceEvent&) { throw std::runtime_error("boom"); });

	auto [ok, err] = lua.run_script("function f() end f()");
	REQUIRE(ok);

	REQUIRE(logged.size() > 0);
	bool found = false;
	for(const auto& line : logged)
	{
		if(line.find("boom") != std::string::npos)
		{
			found = true;
		}
	}
	REQUIRE(found);
}

TEST_CASE("call tracing: non-std::exception thrown by trace callback is logged generically", "[call-tracing]")
{
	Lua lua;
	std::vector<std::string> logged;
	lua.enable_error_logging([&logged](std::string_view msg) { logged.emplace_back(msg); });
	lua.enable_call_tracing([](const LuaCallTraceEvent&) -> void { throw 42; });

	auto [ok, err] = lua.run_script("function f() end f()");
	REQUIRE(ok);

	bool found = false;
	for(const auto& line : logged)
	{
		if(line.find("unknown non-std::exception") != std::string::npos)
		{
			found = true;
		}
	}
	REQUIRE(found);
}

TEST_CASE("call tracing: throwing trace callback with error logging disabled does not break script", "[call-tracing]")
{
	Lua lua;
	lua.enable_call_tracing([](const LuaCallTraceEvent&) { throw std::runtime_error("boom"); });

	auto [ok, err] = lua.run_script("function f() end f()");
	REQUIRE(ok);
}

// ============================================================
// output capture
// ============================================================

TEST_CASE("output capture: print() is discarded by default", "[output-capture]")
{
	Lua lua;
	auto [ok, err] = lua.run_script("print('hello')");
	REQUIRE(ok);
}

TEST_CASE("output capture: print() arguments are tab-separated and newline-terminated", "[output-capture]")
{
	Lua lua;
	std::vector<std::string> lines;
	lua.enable_output_capture([&lines](std::string_view s) { lines.emplace_back(s); });

	auto [ok, err] = lua.run_script("print('a', 1, true)");
	REQUIRE(ok);
	REQUIRE(lines.size() == 1);
	REQUIRE(lines[0] == "a\t1\ttrue\n");
}

TEST_CASE("output capture: multiple print() calls accumulate as separate lines", "[output-capture]")
{
	Lua lua;
	std::vector<std::string> lines;
	lua.enable_output_capture([&lines](std::string_view s) { lines.emplace_back(s); });

	auto [ok, err] = lua.run_script("print('first') print('second')");
	REQUIRE(ok);
	REQUIRE(lines.size() == 2);
	REQUIRE(lines[0] == "first\n");
	REQUIRE(lines[1] == "second\n");
}

TEST_CASE("output capture: disable_output_capture returns to discarding output", "[output-capture]")
{
	Lua lua;
	std::vector<std::string> lines;
	lua.enable_output_capture([&lines](std::string_view s) { lines.emplace_back(s); });
	REQUIRE(std::get<0>(lua.run_script("print('captured')")));
	REQUIRE(lines.size() == 1);

	lua.disable_output_capture();
	REQUIRE(std::get<0>(lua.run_script("print('not captured')")));
	REQUIRE(lines.size() == 1);
}

TEST_CASE("output capture and error logging are independent", "[output-capture]")
{
	Lua lua;
	std::vector<std::string> output_lines;
	std::vector<std::string> error_lines;
	lua.enable_output_capture([&output_lines](std::string_view s) { output_lines.emplace_back(s); });
	lua.enable_call_tracing([](const LuaCallTraceEvent&) { throw std::runtime_error("boom"); });

	REQUIRE(std::get<0>(lua.run_script("function f() end f()")));
	for(const auto& line : output_lines)
	{
		REQUIRE(line.find("boom") == std::string::npos);
	}
	REQUIRE(error_lines.empty());
}

// ============================================================
// error logging
// ============================================================

TEST_CASE("error logging: messages are prefixed and NOT newline-terminated", "[error-logging]")
{
	Lua lua;
	std::vector<std::string> logged;
	lua.enable_error_logging([&logged](std::string_view s) { logged.emplace_back(s); });
	lua.enable_call_tracing([](const LuaCallTraceEvent&) { throw std::runtime_error("oops"); });

	REQUIRE(std::get<0>(lua.run_script("function f() end f()")));
	REQUIRE(logged.size() > 0);
	REQUIRE(logged[0].rfind("[LuaCpp] ", 0) == 0);
	REQUIRE(logged[0] == "[LuaCpp] oops"); // no trailing newline - consumer formats its own message
}

TEST_CASE("error logging: disable_error_logging stops delivery", "[error-logging]")
{
	Lua lua;
	std::vector<std::string> logged;
	lua.enable_error_logging([&logged](std::string_view s) { logged.emplace_back(s); });
	lua.enable_call_tracing([](const LuaCallTraceEvent&) { throw std::runtime_error("oops"); });

	REQUIRE(std::get<0>(lua.run_script("function f() end f()")));
	REQUIRE(logged.size() > 0);

	lua.disable_error_logging();
	const auto count_before = logged.size();
	REQUIRE(std::get<0>(lua.run_script("function g() end g()")));
	REQUIRE(logged.size() == count_before);
}

TEST_CASE("error logging: run_script syntax error is logged", "[error-logging]")
{
	Lua lua;
	std::vector<std::string> logged;
	lua.enable_error_logging([&logged](std::string_view s) { logged.emplace_back(s); });

	auto [ok, err] = lua.run_script("@@@ not valid lua");
	REQUIRE_FALSE(ok);
	REQUIRE(logged.size() == 1);
	REQUIRE(logged[0].find(err) != std::string::npos);
}

TEST_CASE("error logging: run_script runtime error is logged", "[error-logging]")
{
	Lua lua;
	std::vector<std::string> logged;
	lua.enable_error_logging([&logged](std::string_view s) { logged.emplace_back(s); });

	auto [ok, err] = lua.run_script("error('boom')");
	REQUIRE_FALSE(ok);
	REQUIRE(logged.size() == 1);
	REQUIRE(logged[0].find("boom") != std::string::npos);
}

TEST_CASE("error logging: call<> failure paths are logged", "[error-logging]")
{
	// "Not a function" path
	{
		Lua lua;
		std::vector<std::string> logged;
		lua.enable_error_logging([&logged](std::string_view s) { logged.emplace_back(s); });
		auto [ok, err] = lua.call<>("does_not_exist");
		REQUIRE_FALSE(ok);
		REQUIRE(logged.size() == 1);
		REQUIRE(logged[0].find("Not a function") != std::string::npos);
	}

	// lua_pcall failure path
	{
		Lua lua;
		std::vector<std::string> logged;
		lua.enable_error_logging([&logged](std::string_view s) { logged.emplace_back(s); });
		REQUIRE(std::get<0>(lua.run_script("function boom() error('kaboom') end")));
		auto [ok, err] = lua.call<>("boom");
		REQUIRE_FALSE(ok);
		REQUIRE(logged.size() == 1);
		REQUIRE(logged[0].find("kaboom") != std::string::npos);
	}

	// collect<>() type-mismatch path
	{
		Lua lua;
		std::vector<std::string> logged;
		lua.enable_error_logging([&logged](std::string_view s) { logged.emplace_back(s); });
		REQUIRE(std::get<0>(lua.run_script("function give_string() return 'not an int' end")));
		auto [ok, err, value] = lua.call<int>("give_string");
		REQUIRE_FALSE(ok);
		REQUIRE(logged.size() == 1);
	}
}

TEST_CASE("error logging: exception thrown by an expose_func callable is logged", "[error-logging]")
{
	Lua lua;
	std::vector<std::string> logged;
	lua.enable_error_logging([&logged](std::string_view s) { logged.emplace_back(s); });
	lua.expose_func("boom", std::function<void()>([]() { throw std::runtime_error("exposed boom"); }));

	auto [ok, err] = lua.run_script("boom()");
	REQUIRE_FALSE(ok);
	// Both the trampoline's catch(std::exception) AND run_script's outer
	// lua_pcall-failure path log independently - this is expected: Tier 1
	// (trampoline) logs the raw C++ exception as soon as it's caught, and
	// Tier 2 (run_script) logs the resulting {false, msg} Lua error
	// separately, so the same underlying failure is intentionally reported
	// twice via two different code paths.
	REQUIRE(logged.size() == 2);
	bool found = false;
	for(const auto& line : logged)
	{
		if(line.find("exposed boom") != std::string::npos)
		{
			found = true;
		}
	}
	REQUIRE(found);
}

TEST_CASE("error logging: duplicate method registration is logged before throwing", "[error-logging]")
{
	struct LogDupPoint
	{
		int x = 0;
	};

	Lua lua;
	std::vector<std::string> logged;
	lua.enable_error_logging([&logged](std::string_view s) { logged.emplace_back(s); });

	// Registered inline via a locally-defined struct would require LUA_REGISTER_STRUCT
	// at namespace scope, which isn't possible inside a test body; instead exercise the
	// duplicate-registration path using a struct already registered elsewhere in this
	// file (Point), guarding against collisions with existing test method names.
	lua.expose_method<Point, int>("log_dup_test_method", std::function<int(Point)>([](Point p) { return p.x; }));
	bool threw = false;
	try
	{
		lua.expose_method<Point, int>("log_dup_test_method", std::function<int(Point)>([](Point p) { return p.x; }));
	}
	catch(const std::runtime_error&)
	{
		threw = true;
	}
	REQUIRE(threw);
	REQUIRE(logged.size() == 1);
	REQUIRE(logged[0].find("already registered") != std::string::npos);
}

// ============================================================
// instruction counting
// ============================================================

TEST_CASE("instruction counting: count increases across a loop-heavy script", "[instruction-counting]")
{
	Lua lua;
	lua.enable_instruction_counting(100);
	REQUIRE(lua.get_instruction_count() == 0);

	auto [ok, err] = lua.run_script("local sum = 0\nfor i = 1, 100000 do sum = sum + i end\n");
	REQUIRE(ok);
	REQUIRE(lua.get_instruction_count() > 0);
}

TEST_CASE("instruction counting: disable_instruction_counting stops accumulation", "[instruction-counting]")
{
	Lua lua;
	lua.enable_instruction_counting(100);
	REQUIRE(std::get<0>(lua.run_script("for i = 1, 100000 do end")));
	const auto count_after_first = lua.get_instruction_count();
	REQUIRE(count_after_first > 0);

	lua.disable_instruction_counting();
	REQUIRE(std::get<0>(lua.run_script("for i = 1, 100000 do end")));
	REQUIRE(lua.get_instruction_count() == count_after_first);
}

// ============================================================
// instruction limit (protection)
// ============================================================

TEST_CASE("instruction limit: infinite loop is aborted with a catchable error", "[instruction-limit]")
{
	Lua lua;
	lua.set_instruction_limit(10000, 100);

	auto [ok, err] = lua.run_script("while true do end");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("instruction limit"));
}

TEST_CASE("instruction limit: script under the limit succeeds", "[instruction-limit]")
{
	Lua lua;
	lua.set_instruction_limit(10000000, 1000);

	auto [ok, err] = lua.run_script("x = 1 + 1");
	REQUIRE(ok);
}

TEST_CASE("instruction limit: clear_instruction_limit removes the cap", "[instruction-limit]")
{
	Lua lua;
	lua.set_instruction_limit(1000, 100);
	lua.clear_instruction_limit();

	auto [ok, err] = lua.run_script("local sum = 0 for i = 1, 200000 do sum = sum + i end");
	REQUIRE(ok);
}

// ============================================================
// recursion depth cap (protection)
// ============================================================

TEST_CASE("recursion depth cap: deep recursion is aborted with a catchable error", "[recursion-depth-cap]")
{
	Lua lua;
	lua.set_recursion_depth_cap(50);

	auto [ok, err] = lua.run_script("function recurse(n)\n"
	                                "  if n <= 0 then return 0 end\n"
	                                "  local r = recurse(n - 1)\n"
	                                "  return r + 1\n"
	                                "end\n"
	                                "recurse(1000)\n");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("recursion depth limit"));
}

TEST_CASE("recursion depth cap: scripts under the cap are unaffected", "[recursion-depth-cap]")
{
	Lua lua;
	lua.set_recursion_depth_cap(50);

	auto [ok, err] = lua.run_script("function recurse(n)\n"
	                                "  if n <= 0 then return 0 end\n"
	                                "  return recurse(n - 1) + 1\n"
	                                "end\n"
	                                "assert(recurse(10) == 10)\n");
	REQUIRE(ok);
}

TEST_CASE("recursion depth cap: clear_recursion_depth_cap removes the cap", "[recursion-depth-cap]")
{
	Lua lua;
	lua.set_recursion_depth_cap(10);
	lua.clear_recursion_depth_cap();

	auto [ok, err] = lua.run_script("function recurse(n)\n"
	                                "  if n <= 0 then return 0 end\n"
	                                "  return recurse(n - 1) + 1\n"
	                                "end\n"
	                                "assert(recurse(200) == 200)\n");
	REQUIRE(ok);
}

// ============================================================
// hooks integration - merged mask correctness
// ============================================================

TEST_CASE("hooks integration: call tracing, instruction limit, and depth cap coexist", "[hooks-integration]")
{
	Lua lua;
	std::vector<LuaCallTraceEvent> events;
	lua.enable_call_tracing([&events](const LuaCallTraceEvent& e) { events.push_back(e); });
	lua.set_instruction_limit(10000000, 1000);
	lua.set_recursion_depth_cap(100);

	auto [ok, err] = lua.run_script("function recurse(n)\n"
	                                "  if n <= 0 then return 0 end\n"
	                                "  return recurse(n - 1) + 1\n"
	                                "end\n"
	                                "assert(recurse(10) == 10)\n");
	REQUIRE(ok);
	REQUIRE(events.size() > 0);

	events.clear();
	auto [ok2, err2] = lua.run_script("function recurse2(n)\n"
	                                  "  if n <= 0 then return 0 end\n"
	                                  "  return recurse2(n - 1) + 1\n"
	                                  "end\n"
	                                  "recurse2(1000)\n");
	REQUIRE_FALSE(ok2);
	REQUIRE_THAT(err2, Catch::Matchers::ContainsSubstring("recursion depth limit"));
	REQUIRE(events.size() > 0);
}

// ============================================================
// memory tracking / limiting
// ============================================================

TEST_CASE("memory tracking: get_memory_usage reports nonzero usage after construction", "[memory-limit]")
{
	Lua lua;
	// Just constructing a Lua state (luaL_openlibs, print installation, etc.)
	// already allocates plenty - confirms the custom allocator is actually
	// wired in and tracking from the very first allocation.
	REQUIRE(lua.get_memory_usage() > 0);
}

TEST_CASE("memory tracking: usage increases as script allocates tables/strings", "[memory-limit]")
{
	Lua lua;
	const auto before = lua.get_memory_usage();

	auto [ok, err] = lua.run_script("t = {}\n"
	                                "for i = 1, 1000 do t[i] = 'some string value ' .. i end\n");
	REQUIRE(ok);
	REQUIRE(lua.get_memory_usage() > before);
}

TEST_CASE("memory limit: allocations beyond the cap raise a catchable error", "[memory-limit]")
{
	Lua lua;
	// Set the cap just above whatever baseline construction already used, so
	// there's no room left for the script's own table/string allocations.
	lua.set_memory_limit(lua.get_memory_usage() + 64);

	auto [ok, err] = lua.run_script("t = {}\n"
	                                "for i = 1, 100000 do t[i] = 'padding string number ' .. i end\n");
	REQUIRE_FALSE(ok);
	REQUIRE_THAT(err, Catch::Matchers::ContainsSubstring("memory"));
}

TEST_CASE("memory limit: exceeding the cap is reported via error logging", "[memory-limit]")
{
	Lua lua;
	std::vector<std::string> logged;
	lua.enable_error_logging([&logged](std::string_view s) { logged.emplace_back(s); });
	lua.set_memory_limit(lua.get_memory_usage() + 64);

	auto [ok, err] = lua.run_script("t = {}\n"
	                                "for i = 1, 100000 do t[i] = 'padding string number ' .. i end\n");
	REQUIRE_FALSE(ok);
	bool found = false;
	for(const auto& line : logged)
	{
		if(line.find("memory limit exceeded") != std::string::npos)
		{
			found = true;
		}
	}
	REQUIRE(found);
}

TEST_CASE("memory limit: clear_memory_limit removes the cap", "[memory-limit]")
{
	Lua lua;
	lua.set_memory_limit(lua.get_memory_usage() + 64);
	lua.clear_memory_limit();

	auto [ok, err] = lua.run_script("t = {}\n"
	                                "for i = 1, 100000 do t[i] = 'padding string number ' .. i end\n");
	REQUIRE(ok);
}

TEST_CASE("memory limit: script under the limit succeeds normally", "[memory-limit]")
{
	Lua lua;
	lua.set_memory_limit(64 * 1024 * 1024); // generous cap, well above normal usage

	auto [ok, err] = lua.run_script("x = 1 + 1");
	REQUIRE(ok);
	REQUIRE(lua.get_memory_usage() > 0);
	REQUIRE(lua.get_memory_usage() <= 64 * 1024 * 1024);
}

TEST_CASE("memory limit: Lua instance remains usable after a memory-limit error", "[memory-limit]")
{
	Lua lua;
	lua.set_memory_limit(lua.get_memory_usage() + 64);

	auto [ok1, err1] = lua.run_script("t = {}\n"
	                                  "for i = 1, 100000 do t[i] = 'padding string number ' .. i end\n");
	REQUIRE_FALSE(ok1);

	lua.clear_memory_limit();
	auto [ok2, err2] = lua.run_script("y = 2 + 2");
	REQUIRE(ok2);
}
