#pragma once

#include <string>
#include <optional>
#include "Luau/FileResolver.h"
#include "Luau/Ast.h"
#include "Luau/FileUtils.h"

namespace LuauUtils
{
    class FileResolver : public Luau::FileResolver
    {
    public:
        std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override;
        std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* node) override;
        std::string getHumanReadableModuleName(const Luau::ModuleName& name) const override;

    private:
        struct AnalysisRequireContext;
        struct AnalysisCacheManager;
        struct AnalysisErrorHandler;
    };
}