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

void analyzeLuau(const std::string& script) {
	std::cout << "Starting analysis..." << std::endl;
	
	try {
		std::cout << "Creating allocator..." << std::endl;
		Luau::Allocator allocator;
		
		std::cout << "Creating name table..." << std::endl;
		Luau::AstNameTable names(allocator);
		
		std::cout << "Setting up parser options..." << std::endl;
		Luau::ParseOptions parseOptions;
		
		std::cout << "Parsing script..." << std::endl;
		Luau::ParseResult parseResult = Luau::Parser::parse(script.c_str(), script.length(), names, allocator);
		
		std::cout << "Checking for parse errors..." << std::endl;
		if (!parseResult.errors.empty()) {
			std::cout << "Parse errors:" << std::endl;
			for (const auto& error : parseResult.errors) {
				std::cout << "Line " << error.getLocation().begin.line + 1 
						 << ": " << error.getMessage() << std::endl;
			}
			return;
		}

		if (!parseResult.root) {
			std::cout << "Error: No AST root after parsing" << std::endl;
			return;
		}

		std::cout << "Creating source module..." << std::endl;
		Luau::SourceModule sourceModule;
		sourceModule.root = parseResult.root;
		
		std::cout << "Creating module scope..." << std::endl;
		Luau::ModulePtr moduleScope = std::make_shared<Luau::Module>();
		if (!moduleScope) {
			std::cout << "Error: Failed to create module scope" << std::endl;
			return;
		}
		moduleScope->name = "MainModule";
		
		std::cout << "Running linter..." << std::endl;
		try {
			// Print some debug info about the AST
			if (parseResult.root) {
				std::cout << "AST root is a block statement" << std::endl;
				
				const auto& statements = parseResult.root->body;
				std::cout << "Block contains " << statements.size << " statements" << std::endl;
				
				for (size_t i = 0; i < statements.size; ++i) {
					if (statements.data[i]) {
						std::cout << "Statement " << i << " is present" << std::endl;
						// Try to get more info about the statement type
						const auto* stmt = statements.data[i];
						if (const Luau::AstStatLocal* local = stmt->as<Luau::AstStatLocal>())
							std::cout << "  Type: Local variable declaration" << std::endl;
						else if (const Luau::AstStatExpr* expr = stmt->as<Luau::AstStatExpr>())
							std::cout << "  Type: Expression statement" << std::endl;
					}
				}
			}

			// Create root scope
			std::cout << "Creating root scope..." << std::endl;
			Luau::TypeArena arena;
			Luau::TypePackId emptyPack = arena.addTypePack({});
			
			Luau::ScopePtr rootScope = std::make_shared<Luau::Scope>(emptyPack);
			if (!rootScope) {
				std::cout << "Failed to create root scope" << std::endl;
				return;
			}

			std::cout << "Starting lint call..." << std::endl;
			
			// Try linting with root scope
			auto lintResult = Luau::lint(
				parseResult.root,  // AST root
				names,            // Name table
				rootScope,        // Root scope
				nullptr,          // No module needed
				{},              // Empty hot comments
				{}               // Default lint options
			);
			
			std::cout << "Lint call completed" << std::endl;
			
			if (!lintResult.empty()) {
				std::cout << "Found " << lintResult.size() << " lint warnings" << std::endl;
				for (const auto& warning : lintResult) {
					std::cout << "Line " << warning.location.begin.line + 1 
							 << ": " << warning.text << std::endl;
				}
			} else {
				std::cout << "No lint warnings found" << std::endl;
			}
			
		} catch (const std::exception& e) {
			std::cout << "Exception during linting: " << e.what() << std::endl;
		} catch (...) {
			std::cout << "Unknown exception during linting" << std::endl;
		}
		
		std::cout << "Analysis complete." << std::endl;
	} catch (const std::exception& e) {
		std::cout << "Exception during analysis: " << e.what() << std::endl;
	} catch (...) {
		std::cout << "Unknown exception during analysis" << std::endl;
	}
}

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
