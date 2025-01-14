#pragma once

#include <string>
#include <optional>
#include <unordered_map>
#include <vector>
#include <utility>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include "Luau/FileResolver.h"
#include "Luau/Ast.h"
#include "Luau/FileUtils.h"
#include "Luau/Config.h"
#include "Luau/Frontend.h"
#include "Luau/Linter.h"

namespace LuauUtils
{
    enum class ReportFormat
    {
        Default,
        Luacheck,
        Gnu,
    };

    void report(ReportFormat format, const char* name, const Luau::Location& loc, const char* type, const char* message);
    void reportError(const Luau::Frontend& frontend, ReportFormat format, const Luau::TypeError& error);
    void reportWarning(ReportFormat format, const char* name, const Luau::LintWarning& warning);
    bool reportModuleResult(Luau::Frontend& frontend, const Luau::ModuleName& name, ReportFormat format, bool annotate);
    int assertionHandler(const char* expr, const char* file, int line, const char* function);

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

    class TaskScheduler
    {
    public:
        explicit TaskScheduler(unsigned threadCount);
        ~TaskScheduler();

        std::function<void()> pop();
        void push(std::function<void()> task);
        static unsigned getThreadCount();

    private:
        void workerFunction();

        unsigned threadCount = 1;
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<std::thread> workers;
        std::queue<std::function<void()>> tasks;
    };
}