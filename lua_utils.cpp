#include "luau_utils.hpp"
#include "Luau/Require.h"
#include "Luau/TypeAttach.h"
#include "Luau/ToString.h"
#include "Luau/Transpiler.h"

#include <iostream>



namespace LuauUtils {

void report(ReportFormat format, const char* name, const Luau::Location& loc, const char* type, const char* message)
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

void reportError(const Luau::Frontend& frontend, ReportFormat format, const Luau::TypeError& error)
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

void reportWarning(ReportFormat format, const char* name, const Luau::LintWarning& warning)
{
    report(format, name, warning.location, Luau::LintWarning::getName(warning.code), warning.text.c_str());
}

bool reportModuleResult(Luau::Frontend& frontend, const Luau::ModuleName& name, ReportFormat format, bool annotate)
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

    for (Luau::TypeError& error : cr->errors) {
        // printf("ERROR: %d\n", error.code());
        reportError(frontend, format, error);
    }

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

int assertionHandler(const char* expr, const char* file, int line, const char* function)
{
    printf("%s(%d): ASSERTION FAILED: %s\n", file, line, expr);
    fflush(stdout);
    return 1;
}

TaskScheduler::TaskScheduler(unsigned threadCount)
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

TaskScheduler::~TaskScheduler()
{
    for (unsigned i = 0; i < threadCount; i++)
        push({});

    for (std::thread& worker : workers)
        worker.join();
}

std::function<void()> TaskScheduler::pop()
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

void TaskScheduler::push(std::function<void()> task)
{
    {
        std::unique_lock guard(mtx);
        tasks.push(std::move(task));
    }

    cv.notify_one();
}

unsigned TaskScheduler::getThreadCount()
{
    return std::max(std::thread::hardware_concurrency(), 1u);
}

void TaskScheduler::workerFunction()
{
    while (std::function<void()> task = pop())
        task();
}

ConfigResolver::ConfigResolver(Luau::Mode mode)
{
    defaultConfig.mode = mode;
}

const Luau::Config& ConfigResolver::getConfig(const Luau::ModuleName& name) const
{
    std::optional<std::string> path = getParentPath(name);
    if (!path)
        return defaultConfig;

    return readConfigRec(*path);
}

const Luau::Config& ConfigResolver::readConfigRec(const std::string& path) const
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

struct FileResolver::AnalysisRequireContext : RequireResolver::RequireContext
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

struct FileResolver::AnalysisCacheManager : public RequireResolver::CacheManager
{
    AnalysisCacheManager() = default;
};

struct FileResolver::AnalysisErrorHandler : RequireResolver::ErrorHandler
{
    AnalysisErrorHandler() = default;
};

std::optional<Luau::SourceCode> FileResolver::readSource(const Luau::ModuleName& name)
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

std::optional<Luau::ModuleInfo> FileResolver::resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* node)
{
    if (Luau::AstExprConstantString* expr = node->as<Luau::AstExprConstantString>())
    {
        std::string path{expr->value.data, expr->value.size};

        // here, we'll need to handle standard library modules,
        // building path to where they are in the bundle. (platform specific)
        if (path.find('/') == std::string::npos) {
            path = "./" + path;
        }
        // std::cout << "RESOLVING MODULE: " << path << std::endl;

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

std::string FileResolver::getHumanReadableModuleName(const Luau::ModuleName& name) const
{
    if (name == "-")
        return "stdin";
    return name;
}


RuntimeRequireContext::RuntimeRequireContext(std::string source)
    : source(std::move(source))
{
}

std::string RuntimeRequireContext::getPath()
{
    return source.substr(1);
}

bool RuntimeRequireContext::isRequireAllowed()
{
    return true;
    // return isStdin() || (!source.empty() && source[0] == '@');
}

bool RuntimeRequireContext::isStdin()
{
    return source == "=stdin";
}

std::string RuntimeRequireContext::createNewIdentifer(const std::string& path)
{
    return "@" + path;
}

RuntimeCacheManager::RuntimeCacheManager(lua_State* L)
    : L(L)
{
}

bool RuntimeCacheManager::isCached(const std::string& path)
{
    luaL_findtable(L, LUA_REGISTRYINDEX, "_MODULES", 1);
    lua_getfield(L, -1, path.c_str());
    bool cached = !lua_isnil(L, -1);
    lua_pop(L, 2);

    if (cached)
        cacheKey = path;

    return cached;
}

RuntimeErrorHandler::RuntimeErrorHandler(lua_State* L)
    : L(L) {}

void RuntimeErrorHandler::reportError(const std::string message)
{
    luaL_errorL(L, "%s", message.c_str());
}

}


