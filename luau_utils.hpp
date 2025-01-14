#pragma once

#include <string>
#include <optional>
#include <unordered_map>
#include <vector>
#include <utility>
#include "Luau/FileResolver.h"
#include "Luau/Ast.h"
#include "Luau/FileUtils.h"
#include "Luau/Config.h"

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

    class ConfigResolver : public Luau::ConfigResolver
    {
    public:
        explicit ConfigResolver(Luau::Mode mode);
        const Luau::Config& getConfig(const Luau::ModuleName& name) const override;
        mutable std::vector<std::pair<std::string, std::string>> configErrors;

    private:
        const Luau::Config& readConfigRec(const std::string& path) const;

        Luau::Config defaultConfig;
        mutable std::unordered_map<std::string, Luau::Config> configCache;
    };
}