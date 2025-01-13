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

struct TaskScheduler
{
    TaskScheduler(unsigned threadCount)
        : threadCount(threadCount)
    {
        for (unsigned i = 0; i < threadCount; i++)
        {
            workers.emplace_back(
                [this]
                {
                    workerFunction();
                }
            );
        }
    }

    ~TaskScheduler()
    {
        for (unsigned i = 0; i < threadCount; i++)
            push({});

        for (std::thread& worker : workers)
            worker.join();
    }

    std::function<void()> pop()
    {
        std::unique_lock guard(mtx);

        cv.wait(
            guard,
            [this]
            {
                return !tasks.empty();
            }
        );

        std::function<void()> task = tasks.front();
        tasks.pop();
        return task;
    }

    void push(std::function<void()> task)
    {
        {
            std::unique_lock guard(mtx);
            tasks.push(std::move(task));
        }

        cv.notify_one();
    }

    static unsigned getThreadCount()
    {
        return std::max(std::thread::hardware_concurrency(), 1u);
    }

private:
    void workerFunction()
    {
        while (std::function<void()> task = pop())
            task();
    }

    unsigned threadCount = 1;
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
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

static void report(ReportFormat format, const char* name, const Luau::Location& loc, const char* type, const char* message)
{
    switch (format)
    {
    case ReportFormat::Default:
        fprintf(stderr, "%s(%d,%d): %s: %s\n", name, loc.begin.line + 1, loc.begin.column + 1, type, message);
        break;

    case ReportFormat::Luacheck:
    {
        // Note: luacheck's end column is inclusive but our end column is exclusive
        // In addition, luacheck doesn't support multi-line messages, so if the error is multiline we'll fake end column as 100 and hope for the best
        int columnEnd = (loc.begin.line == loc.end.line) ? loc.end.column : 100;

        // Use stdout to match luacheck behavior
        fprintf(stdout, "%s:%d:%d-%d: (W0) %s: %s\n", name, loc.begin.line + 1, loc.begin.column + 1, columnEnd, type, message);
        break;
    }

    case ReportFormat::Gnu:
        // Note: GNU end column is inclusive but our end column is exclusive
        fprintf(stderr, "%s:%d.%d-%d.%d: %s: %s\n", name, loc.begin.line + 1, loc.begin.column + 1, loc.end.line + 1, loc.end.column, type, message);
        break;
    }
}

static void reportError(const Luau::Frontend& frontend, ReportFormat format, const Luau::TypeError& error)
{
    std::string humanReadableName = frontend.fileResolver->getHumanReadableModuleName(error.moduleName);

    if (const Luau::SyntaxError* syntaxError = Luau::get_if<Luau::SyntaxError>(&error.data))
        report(format, humanReadableName.c_str(), error.location, "SyntaxError", syntaxError->message.c_str());
    else
        report(
            format,
            humanReadableName.c_str(),
            error.location,
            "TypeError",
            Luau::toString(error, Luau::TypeErrorToStringOptions{frontend.fileResolver}).c_str()
        );
}

static void reportWarning(ReportFormat format, const char* name, const Luau::LintWarning& warning)
{
    report(format, name, warning.location, Luau::LintWarning::getName(warning.code), warning.text.c_str());
}

static bool reportModuleResult(Luau::Frontend& frontend, const Luau::ModuleName& name, ReportFormat format, bool annotate)
{
    std::optional<Luau::CheckResult> cr = frontend.getCheckResult(name, false);

    if (!cr)
    {
        fprintf(stderr, "Failed to find result for %s\n", name.c_str());
        return false;
    }

    if (!frontend.getSourceModule(name))
    {
        fprintf(stderr, "Error opening %s\n", name.c_str());
        return false;
    }

    for (auto& error : cr->errors)
        reportError(frontend, format, error);

    std::string humanReadableName = frontend.fileResolver->getHumanReadableModuleName(name);
    for (auto& error : cr->lintResult.errors)
        reportWarning(format, humanReadableName.c_str(), error);
    for (auto& warning : cr->lintResult.warnings)
        reportWarning(format, humanReadableName.c_str(), warning);

    if (annotate)
    {
        Luau::SourceModule* sm = frontend.getSourceModule(name);
        Luau::ModulePtr m = frontend.moduleResolver.getModule(name);

        Luau::attachTypeData(*sm, *m);

        std::string annotated = Luau::transpileWithTypes(*sm->root);

        printf("%s", annotated.c_str());
    }

    return cr->errors.empty() && cr->lintResult.errors.empty();
}

static int lua_require(lua_State* L)
{
    std::string name = luaL_checkstring(L, 1);

    // RequireResolver::ResolvedRequire resolvedRequire;
    // {
    //     lua_Debug ar;
    //     lua_getinfo(L, 1, "s", &ar);

    //     RuntimeRequireContext requireContext{ar.source};
    //     RuntimeCacheManager cacheManager{L};
    //     RuntimeErrorHandler errorHandler{L};

    //     RequireResolver resolver(std::move(name), requireContext, cacheManager, errorHandler);

    //     resolvedRequire = resolver.resolveRequire(
    //         [L, &cacheKey = cacheManager.cacheKey](const RequireResolver::ModuleStatus status)
    //         {
    //             lua_getfield(L, LUA_REGISTRYINDEX, "_MODULES");
    //             if (status == RequireResolver::ModuleStatus::Cached)
    //                 lua_getfield(L, -1, cacheKey.c_str());
    //         }
    //     );
    // }

    // if (resolvedRequire.status == RequireResolver::ModuleStatus::Cached)
    //     return finishrequire(L);

    // module needs to run in a new thread, isolated from the rest
    // note: we create ML on main thread so that it doesn't inherit environment of L
    lua_State* GL = lua_mainthread(L);
    lua_State* ML = lua_newthread(GL);
    lua_xmove(GL, L, 1);

    // new thread needs to have the globals sandboxed
    luaL_sandboxthread(ML);

    // // now we can compile & run module on the new thread
    // std::string bytecode = Luau::compile(resolvedRequire.sourceCode, copts());
    // if (luau_load(ML, resolvedRequire.identifier.c_str(), bytecode.data(), bytecode.size(), 0) == 0)
    // {
    //     if (codegen)
    //     {
    //         Luau::CodeGen::CompilationOptions nativeOptions;
    //         Luau::CodeGen::compile(ML, -1, nativeOptions);
    //     }

    //     if (coverageActive())
    //         coverageTrack(ML, -1);

    //     int status = lua_resume(ML, L, 0);

    //     if (status == 0)
    //     {
    //         if (lua_gettop(ML) == 0)
    //             lua_pushstring(ML, "module must return a value");
    //         else if (!lua_istable(ML, -1) && !lua_isfunction(ML, -1))
    //             lua_pushstring(ML, "module must return a table or function");
    //     }
    //     else if (status == LUA_YIELD)
    //     {
    //         lua_pushstring(ML, "module can not yield");
    //     }
    //     else if (!lua_isstring(ML, -1))
    //     {
    //         lua_pushstring(ML, "unknown error while running module");
    //     }
    // }

    // // there's now a return value on top of ML; L stack: _MODULES ML
    // lua_xmove(ML, L, 1);
    // lua_pushvalue(L, -1);
    // lua_setfield(L, -4, resolvedRequire.absolutePath.c_str());

    // // L stack: _MODULES ML result
    // return finishrequire(L);

	std::cout << "REQUIRE CALLED" << std::endl;

	return 0;
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

// Returns true if analysis is successful, false otherwise
bool analyzeLuau(const std::string& scriptFilePath) {

	Luau::assertHandler() = assertionHandler;

    // setLuauFlagsDefault();

	ReportFormat format = ReportFormat::Default;
    Luau::Mode mode = Luau::Mode::Strict;
    bool annotate = false;
    int threadCount = 0;
    std::string basePath = "";

	// TODO: expose options in parameters

	Luau::FrontendOptions frontendOptions;
    frontendOptions.retainFullTypeGraphs = annotate;
    frontendOptions.runLintChecks = true;

	CliFileResolver fileResolver;
    CliConfigResolver configResolver(mode);
    Luau::Frontend frontend(&fileResolver, &configResolver, frontendOptions);

	Luau::registerBuiltinGlobals(frontend, frontend.globals);
    Luau::freeze(frontend.globals.globalTypes);

	std::vector<std::string> files = {
		scriptFilePath,
	};

	for (const std::string& path : files)
        frontend.queueModuleCheck(path);

	std::vector<Luau::ModuleName> checkedModules;

		// If thread count is not set, try to use HW thread count, but with an upper limit
    // When we improve scalability of typechecking, upper limit can be adjusted/removed
    if (threadCount <= 0)
        threadCount = std::min(TaskScheduler::getThreadCount(), 8u);

    try
    {
        TaskScheduler scheduler(threadCount);

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

		std::cout << "ERROR" << std::endl;
        report(
            format,
            humanReadableName.c_str(),
            location,
            "InternalCompilerError",
            Luau::toString(error, Luau::TypeErrorToStringOptions{frontend.fileResolver}).c_str()
        );
        // return 1;
        return false;
    }

	int failed = 0;

    for (const Luau::ModuleName& name : checkedModules) {
        failed += !reportModuleResult(frontend, name, format, annotate);
	}

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

			scriptFilePath = argv[2];

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

		if (scriptFilePath != "") {
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
