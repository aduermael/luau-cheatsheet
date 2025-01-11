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
#include "Luau/BuiltinDefinitions.h"
#include "Luau/FileUtils.h"

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

enum class ReportFormat
{
    Default,
    Luacheck,
    Gnu,
};

struct CliFileResolver : Luau::FileResolver
{
    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override
    {
        Luau::SourceCode::Type sourceType;
        std::optional<std::string> source = std::nullopt;

        // If the module name is "-", then read source from stdin
        if (name == "-")
        {
            source = readStdin();
            sourceType = Luau::SourceCode::Script;
        }
        else
        {
            source = readFile(name);
            sourceType = Luau::SourceCode::Module;
        }

        if (!source)
            return std::nullopt;

        return Luau::SourceCode{*source, sourceType};
    }

    std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* node) override
    {
        if (Luau::AstExprConstantString* expr = node->as<Luau::AstExprConstantString>())
        {
            std::string path{expr->value.data, expr->value.size};

            AnalysisRequireContext requireContext{context->name};
            AnalysisCacheManager cacheManager;
            AnalysisErrorHandler errorHandler;

            RequireResolver resolver(path, requireContext, cacheManager, errorHandler);
            RequireResolver::ResolvedRequire resolvedRequire = resolver.resolveRequire();

            if (resolvedRequire.status == RequireResolver::ModuleStatus::FileRead)
                return {{resolvedRequire.identifier}};
        }

        return std::nullopt;
    }

    std::string getHumanReadableModuleName(const Luau::ModuleName& name) const override
    {
        if (name == "-")
            return "stdin";
        return name;
    }

private:
    struct AnalysisRequireContext : RequireResolver::RequireContext
    {
        explicit AnalysisRequireContext(std::string path)
            : path(std::move(path))
        {
        }

        std::string getPath() override
        {
            return path;
        }

        bool isRequireAllowed() override
        {
            return true;
        }

        bool isStdin() override
        {
            return path == "-";
        }

        std::string createNewIdentifer(const std::string& path) override
        {
            return path;
        }

    private:
        std::string path;
    };

    struct AnalysisCacheManager : public RequireResolver::CacheManager
    {
        AnalysisCacheManager() = default;
    };

    struct AnalysisErrorHandler : RequireResolver::ErrorHandler
    {
        AnalysisErrorHandler() = default;
    };
};

struct CliConfigResolver : Luau::ConfigResolver
{
    Luau::Config defaultConfig;

    mutable std::unordered_map<std::string, Luau::Config> configCache;
    mutable std::vector<std::pair<std::string, std::string>> configErrors;

    CliConfigResolver(Luau::Mode mode)
    {
        defaultConfig.mode = mode;
    }

    const Luau::Config& getConfig(const Luau::ModuleName& name) const override
    {
        std::optional<std::string> path = getParentPath(name);
        if (!path)
            return defaultConfig;

        return readConfigRec(*path);
    }

    const Luau::Config& readConfigRec(const std::string& path) const
    {
        auto it = configCache.find(path);
        if (it != configCache.end())
            return it->second;

        std::optional<std::string> parent = getParentPath(path);
        Luau::Config result = parent ? readConfigRec(*parent) : defaultConfig;

        std::string configPath = joinPaths(path, Luau::kConfigName);

        if (std::optional<std::string> contents = readFile(configPath))
        {
            Luau::ConfigOptions::AliasOptions aliasOpts;
            aliasOpts.configLocation = configPath;
            aliasOpts.overwriteAliases = true;

            Luau::ConfigOptions opts;
            opts.aliasOptions = std::move(aliasOpts);

            std::optional<std::string> error = Luau::parseConfig(*contents, result, opts);
            if (error)
                configErrors.push_back({configPath, *error});
        }

        return configCache[path] = result;
    }
};

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
		{"collectgarbage", lua_collectgarbage},
		{NULL, NULL},
	};

	lua_pushvalue(L, LUA_GLOBALSINDEX);
	luaL_register(L, NULL, funcs);
	lua_pop(L, 1);

	DEBUG_LOG("Compiling script...");
	std::string bytecode = Luau::compile(script, copts());
	
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

		error += "\nstack backtrace:\n";
		error += lua_debugtrace(T);

		std::cout << "ERROR: " << error << std::endl;
		lua_pop(L, 1);
	}

	DEBUG_LOG("Cleaning up...");
	lua_close(L);
}

static int assertionHandler(const char* expr, const char* file, int line, const char* function)
{
    printf("%s(%d): ASSERTION FAILED: %s\n", file, line, expr);
    fflush(stdout);
    return 1;
}

void analyzeLuau(const std::string& script) {

	Luau::assertHandler() = assertionHandler;

    // setLuauFlagsDefault();

	ReportFormat format = ReportFormat::Default;
    Luau::Mode mode = Luau::Mode::Nonstrict;
    bool annotate = false;
    int threadCount = 0;
    std::string basePath = "";

	// TODO: expose options in parameters

	Luau::FrontendOptions frontendOptions;
    frontendOptions.retainFullTypeGraphs = annotate;
    frontendOptions.runLintChecks = true;

	// CliFileResolver fileResolver;
    // CliConfigResolver configResolver(mode);
    // Luau::Frontend frontend(&fileResolver, &configResolver, frontendOptions);

	// Luau::registerBuiltinGlobals(frontend, frontend.globals);
    // Luau::freeze(frontend.globals.globalTypes);


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

			DEBUG_LOG("Reading file: " << argv[2]);
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

		DEBUG_LOG("Running analysis...");
		analyzeLuau(script);
		
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
