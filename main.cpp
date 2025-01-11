#include <__config>
#include <cstddef>
#include <iostream>
#include <fstream>
#include <sstream>

#include "lua.h"
#include "lualib.h"

#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include "Luau/Ast.h"
#include "Luau/Parser.h"
#include "Luau/ParseOptions.h"
#include "Luau/Allocator.h"
#include "Luau/Module.h"
#include "Luau/Linter.h"
#include "Luau/TypeArena.h"
#include "Luau/Type.h"
#include "Luau/Frontend.h"
#include "Luau/TypeInfer.h"
#include "Luau/ToString.h"
#include "Luau/TypeChecker2.h"
#include "Luau/NotNull.h"

struct GlobalOptions {
	int optimizationLevel = 1;
	int debugLevel = 1;
} globalOptions;

static Luau::CompileOptions copts() {
	Luau::CompileOptions result = {};
	result.optimizationLevel = globalOptions.optimizationLevel;
	result.debugLevel = globalOptions.debugLevel;
	result.typeInfoLevel = 1;
	// result.coverageLevel = coverageActive() ? 2 : 0;
	result.coverageLevel = 0;
	return result;
}

static int lua_loadstring(lua_State* L) {
	size_t l = 0;
	const char* s = luaL_checklstring(L, 1, &l);
	const char* chunkname = luaL_optstring(L, 2, s);

	lua_setsafeenv(L, LUA_ENVIRONINDEX, false);

	std::string bytecode = Luau::compile(std::string(s, l), copts());
	if (luau_load(L, chunkname, bytecode.data(), bytecode.size(), 0) == 0) {
		return 1;
	}

	lua_pushnil(L);
	lua_insert(L, -2); // put before error message
	return 2;          // return nil plus error message
}

static int lua_collectgarbage(lua_State* L) {
	const char* option = luaL_optstring(L, 1, "collect");
	if (strcmp(option, "collect") == 0) {
		lua_gc(L, LUA_GCCOLLECT, 0);
		return 0;
	}
	if (strcmp(option, "count") == 0) {
		int c = lua_gc(L, LUA_GCCOUNT, 0);
		lua_pushnumber(L, c);
		return 1;
	}
	luaL_error(L, "collectgarbage must be called with 'count' or 'collect'");
}

void runLuau(const std::string& script) {
	std::cout << "Creating Lua state..." << std::endl;
	lua_State* L = luaL_newstate();
	if (!L) {
		std::cout << "Failed to create Lua state" << std::endl;
		return;
	}

	std::cout << "Opening libraries..." << std::endl;
	luaL_openlibs(L);

	std::cout << "Registering functions..." << std::endl;
	static const luaL_Reg funcs[] = {
		{"loadstring", lua_loadstring},
		{"collectgarbage", lua_collectgarbage},
		{NULL, NULL},
	};

	lua_pushvalue(L, LUA_GLOBALSINDEX);
	luaL_register(L, NULL, funcs);
	lua_pop(L, 1);

	std::cout << "Compiling script..." << std::endl;
	std::string bytecode = Luau::compile(script, copts());
	
	std::cout << "Loading bytecode..." << std::endl;
	if (luau_load(L, "=script", bytecode.data(), bytecode.size(), 0) != 0) {
		size_t len;
		const char* msg = lua_tolstring(L, -1, &len);
		std::string error(msg, len);
		std::cout << "LOAD SCRIPT ERROR: " << error << std::endl;
		lua_close(L);
		return;
	}

	std::cout << "Creating thread..." << std::endl;
	lua_State* T = lua_newthread(L);
	if (!T) {
		std::cout << "Failed to create thread" << std::endl;
		lua_close(L);
		return;
	}

	lua_pushvalue(L, -2);
	lua_remove(L, -3);
	lua_xmove(L, T, 1);

	std::cout << "Running script..." << std::endl;
	int status = lua_resume(T, NULL, 0);

	if (status != 0) {
		std::string error;

		if (status == LUA_YIELD) {
			error = "thread yielded unexpectedly";
		} else if (const char* str = lua_tostring(T, -1)) {
			error = str;
		}

		error += "\nstack backtrace:\n";
		error += lua_debugtrace(T);

		std::cout << "ERROR: " << error << std::endl;
		lua_pop(L, 1);
	}

	std::cout << "Cleaning up..." << std::endl;
	lua_close(L);
}

void analyzeLuau(const std::string& script) {}

int main(int argc, char* argv[]) {
	std::string script;

	if (argc < 2) {
		std::cout << "Usage: " << argv[0] << " <script_string> or " << argv[0] << " -f <script_file>" << std::endl;
		return 1;
	}

	try {
		if (std::string(argv[1]) == "-f") {
			if (argc < 3) {
				std::cout << "Error: No file specified after -f flag" << std::endl;
				return 1;
			}

			std::cout << "Reading file: " << argv[2] << std::endl;
			std::ifstream file(argv[2]);
			if (!file.is_open()) {
				std::cout << "Error: Could not open file " << argv[2] << std::endl;
				return 1;
			}

			std::stringstream buffer;
			buffer << file.rdbuf();
			script = buffer.str();

		} else {
			script = argv[1];
		}

		std::cout << "Running analysis..." << std::endl;
		analyzeLuau(script);
		
		std::cout << "Running script..." << std::endl;
		runLuau(script);

	} catch (const std::exception& e) {
		std::cout << "ERROR: " << e.what() << std::endl;
		return 1;
	} catch (...) {
		std::cout << "Unknown error occurred" << std::endl;
		return 1;
	}

	return 0;
}
