#include <__config>
#include <cstddef>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>

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
#include "Luau/BuiltinDefinitions.h"
#include "Luau/FileUtils.h"
#include "Luau/Require.h"
#include "Luau/TypeAttach.h"
#include "Luau/Transpiler.h"
#include "luau_utils.hpp"

#ifndef DEBUG
#define DEBUG 0
#endif

#define DEBUG_LOG(msg) do { if (DEBUG) std::cout << "[DEBUG] " << msg << std::endl; } while (0)

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

static int finishrequire(lua_State* L)
{
    if (lua_isstring(L, -1))
        lua_error(L);

    return 1;
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

static int lua_require(lua_State* L)
{
    std::string name = luaL_checkstring(L, 1);

    RequireResolver::ResolvedRequire resolvedRequire;
    {
        lua_Debug ar;
        lua_getinfo(L, 1, "s", &ar);

        LuauUtils::RuntimeRequireContext requireContext{ar.source};
        LuauUtils::RuntimeCacheManager cacheManager{L};
        LuauUtils::RuntimeErrorHandler errorHandler{L};

        RequireResolver resolver(std::move(name), requireContext, cacheManager, errorHandler);

        resolvedRequire = resolver.resolveRequire(
            [L, &cacheKey = cacheManager.cacheKey](const RequireResolver::ModuleStatus status)
            {
                lua_getfield(L, LUA_REGISTRYINDEX, "_MODULES");
                if (status == RequireResolver::ModuleStatus::Cached)
                    lua_getfield(L, -1, cacheKey.c_str());
            }
        );
    }

    if (resolvedRequire.status == RequireResolver::ModuleStatus::Cached) {
        return finishrequire(L);
    }

    // module needs to run in a new thread, isolated from the rest
    // note: we create ML on main thread so that it doesn't inherit environment of L
    lua_State* GL = lua_mainthread(L);
    lua_State* ML = lua_newthread(GL);
    lua_xmove(GL, L, 1);

    // new thread needs to have the globals sandboxed
    luaL_sandboxthread(ML);

    // now we can compile & run module on the new thread
    std::string bytecode = Luau::compile(resolvedRequire.sourceCode, copts());
    if (luau_load(ML, resolvedRequire.identifier.c_str(), bytecode.data(), bytecode.size(), 0) == 0)
    {
        // if (codegen)
        // {
        //     Luau::CodeGen::CompilationOptions nativeOptions;
        //     Luau::CodeGen::compile(ML, -1, nativeOptions);
        // }

        // if (coverageActive())
        //     coverageTrack(ML, -1);

        int status = lua_resume(ML, L, 0);

        if (status == 0)
        {
            if (lua_gettop(ML) == 0)
                lua_pushstring(ML, "module must return a value");
            else if (!lua_istable(ML, -1) && !lua_isfunction(ML, -1))
                lua_pushstring(ML, "module must return a table or function");
        }
        else if (status == LUA_YIELD)
        {
            lua_pushstring(ML, "module can not yield");
        }
        else if (!lua_isstring(ML, -1))
        {
            lua_pushstring(ML, "unknown error while running module");
        }
    }

    // there's now a return value on top of ML; L stack: _MODULES ML
    lua_xmove(ML, L, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, -4, resolvedRequire.absolutePath.c_str());

    // // L stack: _MODULES ML result
	return finishrequire(L);
}

void runLuau(const std::string& script) {
	DEBUG_LOG("Creating Lua state...");
	lua_State* L = luaL_newstate();
	if (!L) {
		std::cout << "Failed to create Lua state" << std::endl;
		return;
	}

	DEBUG_LOG("Opening libraries...");
	luaL_openlibs(L);

	DEBUG_LOG("Registering functions...");
	static const luaL_Reg funcs[] = {
		{"loadstring", lua_loadstring},
		{"require", lua_require},
		{"collectgarbage", lua_collectgarbage},
		{NULL, NULL},
	};

	lua_pushvalue(L, LUA_GLOBALSINDEX);
	luaL_register(L, NULL, funcs);
	lua_pop(L, 1);

	DEBUG_LOG("Compiling script...");
	std::string bytecode = Luau::compile(script, copts());

    // printf("BYTE CODE: ");
	// for (unsigned char c : bytecode) {
	// 	printf("%02X ", c);
	// }
	// printf("\n");

	// std::ofstream bytecodeFile("last-run.bytecode", std::ios::binary);
	// if (bytecodeFile.is_open()) {
	// 	bytecodeFile.write(bytecode.data(), bytecode.size());
	// 	bytecodeFile.close();
	// } else {
	// 	std::cerr << "Failed to open last-run.bytecode for writing" << std::endl;
	// }
	
	DEBUG_LOG("Loading bytecode...");
	if (luau_load(L, "=script", bytecode.data(), bytecode.size(), 0) != 0) {
		size_t len;
		const char* msg = lua_tolstring(L, -1, &len);
		std::string error(msg, len);
		std::cout << "LOAD SCRIPT ERROR: " << error << std::endl;
		lua_close(L);
		return;
	}

	DEBUG_LOG("Creating thread...");
	lua_State* T = lua_newthread(L);
	if (!T) {
		std::cout << "Failed to create thread" << std::endl;
			lua_close(L);
			return;
	}

	lua_pushvalue(L, -2);
	lua_remove(L, -3);
	lua_xmove(L, T, 1);

	DEBUG_LOG("Running script...");
	int status = lua_resume(T, NULL, 0);

	if (status != 0) {
		std::string error;

		if (status == LUA_YIELD) {
			error = "thread yielded unexpectedly";
		} else if (const char* str = lua_tostring(T, -1)) {
			error = str;
		}

		// error += "\nstack backtrace:\n";
        error += "\n";
		error += lua_debugtrace(T);

		std::cout << "❌ " << error << std::endl;
		lua_pop(L, 1);
	}

	DEBUG_LOG("Cleaning up...");
	lua_close(L);
}

bool analyzeLuau(const std::string& scriptFilePath) {
    Luau::assertHandler() = LuauUtils::assertionHandler;

    LuauUtils::ReportFormat format = LuauUtils::ReportFormat::Default;
    Luau::Mode mode = Luau::Mode::Strict;
    bool annotate = false;
    int threadCount = 0;
    std::string basePath = "";

    Luau::FrontendOptions frontendOptions;
    frontendOptions.retainFullTypeGraphs = annotate;
    frontendOptions.runLintChecks = true;

    LuauUtils::FileResolver fileResolver;
    LuauUtils::ConfigResolver configResolver(mode);
    Luau::Frontend frontend(&fileResolver, &configResolver, frontendOptions);

    Luau::registerBuiltinGlobals(frontend, frontend.globals);
    Luau::freeze(frontend.globals.globalTypes);

    std::vector<std::string> files = {
        scriptFilePath,
    };

    for (const std::string& path : files)
        frontend.queueModuleCheck(path);

    std::vector<Luau::ModuleName> checkedModules;

    if (threadCount <= 0)
        threadCount = std::min(LuauUtils::TaskScheduler::getThreadCount(), 8u);

    try
    {
        LuauUtils::TaskScheduler scheduler(threadCount);

        checkedModules = frontend.checkQueuedModules(
            std::nullopt,
            [&](std::function<void()> f)
            {
                scheduler.push(std::move(f));
            }
        );
    }
    catch (const Luau::InternalCompilerError& ice)
    {
        Luau::Location location = ice.location ? *ice.location : Luau::Location();

        std::string moduleName = ice.moduleName ? *ice.moduleName : "<unknown module>";
        std::string humanReadableName = frontend.fileResolver->getHumanReadableModuleName(moduleName);

        Luau::TypeError error(location, moduleName, Luau::InternalError{ice.message});

        LuauUtils::reportError(frontend, format, error);
        return false;
    }

    int failed = 0;

    for (const Luau::ModuleName& name : checkedModules)
        failed += !LuauUtils::reportModuleResult(frontend, name, format, annotate);

    if (!configResolver.configErrors.empty())
    {
        failed += int(configResolver.configErrors.size());

        for (const auto& pair : configResolver.configErrors)
            fprintf(stderr, "%s: %s\n", pair.first.c_str(), pair.second.c_str());
    }

    // if (format == ReportFormat::Luacheck) {
	// 	// return 0;
	// } else {
    //     // return failed ? 1 : 0;
	// }

    // std::cout << "DONE ANALYZING - FAILED: " << failed << std::endl;
	// std::cout << "DONE ANALYZING" << std::endl;

    return failed == 0;
}

int main(int argc, char* argv[]) {
	std::string script;
	std::string scriptFilePath = "";
	bool runAnalyzer = true;

	if (argc < 2) {
		std::cout << "Usage: " << argv[0] << " <script_string> or " << argv[0] << " -f <script_file> [--analyzer=0|1]" << std::endl;
		return 1;
	}

	try {
		// Parse command line arguments
		for (int i = 1; i < argc; i++) {
			std::string arg = argv[i];
			if (arg.substr(0, 11) == "--analyzer=") {
				runAnalyzer = (arg.substr(11) == "1");
			} else if (arg == "-f") {
				if (i + 1 >= argc) {
					std::cout << "Error: No file specified after -f flag" << std::endl;
					return 1;
				}
				scriptFilePath = argv[++i];

				DEBUG_LOG("Reading file: " << scriptFilePath);
				std::ifstream file(scriptFilePath);
				if (!file.is_open()) {
					std::cout << "Error: Could not open file " << scriptFilePath << std::endl;
					return 1;
				}

				std::stringstream buffer;
				buffer << file.rdbuf();
				script = buffer.str();
			} else if (script.empty()) {
				script = arg;
			}
		}

		if (scriptFilePath != "" && runAnalyzer) {
			DEBUG_LOG("Running analysis...");
			bool success = analyzeLuau(scriptFilePath);
			if (success == false) {
				return 1;
			}
		}
		
		DEBUG_LOG("Running script...");
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
