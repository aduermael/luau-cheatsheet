#include "luau_utils.hpp"
#include "Luau/Require.h"

namespace LuauUtils {

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


