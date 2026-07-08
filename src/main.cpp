#include "Lua.hpp"
#include <iostream>

int main(void)
{
	const char* script = R"(
      print('Hello from Lua ' .. _VERSION)
	  print('Float: ' .. float) 
	  print('String: ' .. str)
	  print('char *: ' .. charp)
	  print('Integer: ' .. int)

	  function call_me(a, b, c)
		print("call_me: " .. tostring(a+b+int))
	  end

	  function add(a, b)
		return "Sum:", a+b
	  end
  )";

	Lua lua{};
	lua.assign("float", 42.0);
	lua.assign("str", std::string("Hello from C++"));
	lua.assign("charp", "Hello from C++ char *");
	lua.assign("int", 666);
	const auto [res, err] = lua.run_script(script);
	if(!res)
	{
		std::cerr << "Lua error: " << err;
	}

	lua.call("print", "Hello from C++ to Lua function call");
	lua.call("call_me", 1, 2);
	const auto [add_res, add_err, header, sum] = lua.call<std::string, int>("add", 3, 4);
	if(!add_res)
	{
		std::cerr << "Lua error: " << add_err;
	}
	else
	{
		std::cout << header << " " << sum << std::endl;
	}

	return 0;
}
