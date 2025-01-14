#include "luau_utils.hpp"
#include "Luau/Require.h"

namespace LuauUtils {

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

}


