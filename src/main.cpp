// LuaCpp feature showcase.
//
// Each function below demonstrates exactly one feature of the library.
// Run the executable to see all of them exercised in sequence with their
// output printed to stdout/stderr.
#include "Lua.hpp"
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// A struct made exchangeable with Lua (as a table) via LUA_REGISTER_STRUCT.
// Used throughout the rest of this file to demonstrate struct binding,
// nesting, methods, and mutation.
//
// Defined at global (not anonymous-namespace) scope because explicit
// specializations of LuaFields<T> (created by LUA_REGISTER_STRUCT) must be
// declared in the same namespace that LuaFields itself lives in.
struct Point
{
    int x = 0;
    int y = 0;
};
LUA_REGISTER_STRUCT(Point, lua_field("x", &Point::x), lua_field("y", &Point::y))

// A struct with a nested registered struct field and a container field, to
// demonstrate that nesting and containers work with zero extra registration.
struct Rect
{
    Point origin;
    int w = 0;
    int h = 0;
};
LUA_REGISTER_STRUCT(Rect, lua_field("origin", &Rect::origin), lua_field("w", &Rect::w), lua_field("h", &Rect::h))

namespace
{

    void print_header(const char* title)
    {
        std::cout << "\n=== " << title << " ===\n";
    }

    // ---------------------------------------------------------------------------
    // run_script: execute a Lua string in the Lua instance's global state.
    // ---------------------------------------------------------------------------
    void demo_run_script()
    {
        print_header("run_script");

        Lua lua;
        const auto [ok, err] = lua.run_script("print('Hello from Lua ' .. _VERSION)");
        if(!ok)
            std::cerr << "Lua error: " << err << '\n';

        // Globals set by one run_script call persist for later calls on the same
        // Lua instance.
        lua.run_script("greeting = 'set by a previous run_script call'");
        lua.run_script("print(greeting)");
    }

    // ---------------------------------------------------------------------------
    // assign: expose C++ values to Lua as named globals.
    // ---------------------------------------------------------------------------
    void demo_assign()
    {
        print_header("assign");

        Lua lua;
        lua.assign("float_value", 42.0);
        lua.assign("string_value", std::string("Hello from C++"));
        lua.assign("charp_value", "Hello from C++ char *");
        lua.assign("int_value", 666);
        lua.assign("vector_value", std::vector<int>{1, 2, 3});
        lua.assign("map_value", std::map<std::string, int>{{"a", 1}, {"b", 2}});
        lua.assign("point_value", Point{3, 4}); // registered struct

        lua.run_script(R"(
        print('Float: ' .. float_value)
        print('String: ' .. string_value)
        print('char *: ' .. charp_value)
        print('Integer: ' .. int_value)
        print('Vector[2]: ' .. vector_value[2])
        print('Map.a: ' .. map_value.a)
        print('Point: ' .. point_value.x .. ',' .. point_value.y)
    )");
    }

    // ---------------------------------------------------------------------------
    // call: invoke a Lua-side function from C++ and receive typed return values.
    // ---------------------------------------------------------------------------
    void demo_call()
    {
        print_header("call");

        Lua lua;
        lua.run_script(R"(
        function add(a, b) return 'Sum:', a + b end
        function minmax(a, b) return math.min(a, b), math.max(a, b) end
    )");

        // Multiple return values.
        const auto [ok, err, label, sum] = lua.call<std::string, int>("add", 3, 4);
        if(ok)
            std::cout << label << " " << sum << '\n';
        else
            std::cerr << "Lua error: " << err << '\n';

        const auto [ok2, err2, lo, hi] = lua.call<int, int>("minmax", 7, 3);
        if(ok2)
            std::cout << "min=" << lo << " max=" << hi << '\n';

        // Calling a function purely for its side effect (no ReturnTypes, no result
        // captured beyond ok/err).
        lua.run_script("function greet(name) print('Hello, ' .. name) end");
        lua.call("greet", "world");
    }

    // ---------------------------------------------------------------------------
    // Struct binding: registered structs round-trip as Lua tables, including
    // nested structs and container fields, with zero extra registration.
    // ---------------------------------------------------------------------------
    void demo_struct_binding()
    {
        print_header("Struct binding");

        Lua lua;
        lua.assign("r", Rect{Point{1, 2}, 10, 20});
        lua.run_script("print('Rect: origin=(' .. r.origin.x .. ',' .. r.origin.y .. ') ' .. r.w .. 'x' .. r.h)");

        // Lua can also construct a registered struct from a plain table literal.
        lua.run_script("function make_point() return {x = 5, y = 6} end");
        const auto [ok, err, p] = lua.call<Point>("make_point");
        if(ok)
            std::cout << "Point built from a Lua table literal: (" << p.x << ',' << p.y << ")\n";
    }

    // ---------------------------------------------------------------------------
    // expose_func: register a C++ callable as a Lua global function.
    // ---------------------------------------------------------------------------
    void demo_expose_func()
    {
        print_header("expose_func");

        Lua lua;

        // Scalar return.
        lua.expose_func<int>("add", std::function<int(int, int)>([](int a, int b) { return a + b; }));

        // Void return, side effect in C++.
        lua.expose_func("log", std::function<void(std::string)>([](const std::string& msg)
                                                                { std::cout << "[lua] " << msg << '\n'; }));

        // Multiple return values via std::tuple.
        lua.expose_func<int, int>("divmod", std::function<std::tuple<int, int>(int, int)>(
                                            [](int a, int b) { return std::make_tuple(a / b, a % b); }));

        // Owner-keep-alive overload: extends the lifetime of *sensor for as long
        // as this Lua instance keeps the registered closure - safe even though
        // the closure captures a raw pointer into *sensor. See LIFETIME.md.
        auto counter = std::make_shared<int>(0);
        lua.expose_func<int>("next_id", counter, std::function<int()>([raw = counter.get()]() { return ++(*raw); }));

        lua.run_script(R"(
        assert(add(3, 4) == 7)
        log('called from Lua via expose_func')
        local q, r = divmod(17, 5)
        print('17 / 5 = ' .. q .. ' remainder ' .. r)
        print('next_id() -> ' .. next_id())
        print('next_id() -> ' .. next_id())
    )");
    }

    // ---------------------------------------------------------------------------
    // expose_method: register a read-only (self-by-value) method on a struct.
    // ---------------------------------------------------------------------------
    void demo_expose_method()
    {
        print_header("expose_method");

        Lua lua;
        // Methods must be registered before any instance of Point is pushed to Lua.
        lua.expose_method<Point, int>("magnitude_sq",
                                      std::function<int(Point)>([](Point p) { return p.x * p.x + p.y * p.y; }));
        lua.expose_method<Point, Point>("translated",
                                        std::function<Point(Point, int, int)>([](Point p, int dx, int dy)
                                                                              { return Point{p.x + dx, p.y + dy}; }));

        lua.assign("p", Point{3, 4});
        lua.run_script(R"(
        print('p:magnitude_sq() = ' .. p:magnitude_sq())
        local q = p:translated(1, 1)
        print('p:translated(1,1) = (' .. q.x .. ',' .. q.y .. ')')
        -- self is passed by value: the original p is unchanged.
        print('p is still (' .. p.x .. ',' .. p.y .. ')')
    )");
    }

    // ---------------------------------------------------------------------------
    // expose_mutable_method: register a method that mutates self in place.
    // ---------------------------------------------------------------------------
    void demo_expose_mutable_method()
    {
        print_header("expose_mutable_method");

        Lua lua;
        lua.expose_mutable_method<Point>("translate", std::function<void(Point&, int, int)>(
                                                      [](Point& p, int dx, int dy)
                                                      {
                                                          p.x += dx;
                                                          p.y += dy;
                                                      }));

        lua.assign("p", Point{3, 4});
        lua.run_script(R"(
        p:translate(1, 1)
        assert(p.x == 4 and p.y == 5)
        print('p after p:translate(1,1) = (' .. p.x .. ',' .. p.y .. ')')
    )");
    }

    // ---------------------------------------------------------------------------
    // Exception handling: a C++ exception thrown inside a registered callable is
    // caught by LuaCpp's trampoline and surfaced as a normal Lua error, visible
    // through run_script's/call's {ok, err} result - it never crashes the process.
    // ---------------------------------------------------------------------------
    void demo_exception_handling()
    {
        print_header("Exception handling");

        Lua lua;
        lua.expose_func<int>("safe_div", std::function<int(int, int)>(
                                         [](int a, int b) -> int
                                         {
                                             if(b == 0)
                                                 throw std::runtime_error("division by zero");
                                             return a / b;
                                         }));

        const auto [ok, err] = lua.run_script("safe_div(1, 0)");
        std::cout << "safe_div(1, 0) succeeded? " << std::boolalpha << ok << " - error: " << err << '\n';

        // The Lua state remains fully usable after a thrown exception.
        const auto [ok2, err2, result] = lua.call<int>("safe_div", 10, 2);
        if(ok2)
            std::cout << "safe_div(10, 2) = " << result << '\n';
    }

    // ---------------------------------------------------------------------------
    // Error handling: run_script/call report failures as {false, message} instead
    // of throwing or crashing, for both syntax and runtime errors.
    // ---------------------------------------------------------------------------
    void demo_error_handling()
    {
        print_header("Error handling");

        Lua lua;

        const auto [ok1, err1] = lua.run_script("this is not valid Lua @@@");
        std::cout << "Syntax error caught: " << std::boolalpha << !ok1 << " - " << err1 << '\n';

        const auto [ok2, err2] = lua.run_script("error('something went wrong')");
        std::cout << "Runtime error caught: " << std::boolalpha << !ok2 << " - " << err2 << '\n';

        lua.expose_func<int>("add", std::function<int(int, int)>([](int a, int b) { return a + b; }));
        const auto [ok3, err3, val] = lua.call<int>("add", 1); // wrong argument count
        std::cout << "Argument-count error caught: " << std::boolalpha << !ok3 << " - " << err3 << '\n';
    }

} // namespace

int main()
{
    demo_run_script();
    demo_assign();
    demo_call();
    demo_struct_binding();
    demo_expose_func();
    demo_expose_method();
    demo_expose_mutable_method();
    demo_exception_handling();
    demo_error_handling();

    return 0;
}
