#include "core/logging.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace phoenix::core
{
    namespace
    {
        std::filesystem::path gLogsDirectory;
        std::filesystem::path gEngineLogPath;
        std::mutex gLogMutex;

        std::string timestamp()
        {
            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;

            std::tm localTime{};
#ifdef _WIN32
            localtime_s(&localTime, &time);
#else
            localtime_r(&time, &localTime);
#endif

            std::ostringstream out;
            out << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S")
                << '.' << std::setw(3) << std::setfill('0') << ms.count();
            return out.str();
        }

        std::filesystem::path environment_path(const char* name)
        {
#ifdef _WIN32
            char* rawValue{};
            std::size_t valueSize{};
            if (_dupenv_s(&rawValue, &valueSize, name) != 0 || !rawValue)
                return {};
            std::unique_ptr<char, decltype(&std::free)> value(rawValue, &std::free);
            if (valueSize <= 1 || value.get()[0] == '\0')
                return {};
            return std::filesystem::path(value.get());
#else
            const char* value = std::getenv(name);
            if (!value || !value[0])
                return {};
            return std::filesystem::path(value);
#endif
        }

        const char* category_filename(LogCategory cat)
        {
            switch (cat)
            {
            case LogCategory::Engine:   return "Engine";
            case LogCategory::Renderer: return "Renderer";
            case LogCategory::Assets:   return "Assets";
            case LogCategory::World:    return "World";
            case LogCategory::Audio:    return "Audio";
            case LogCategory::Session:  return "Session";
            case LogCategory::Errors:   return "Errors";
            }
            return "Engine";
        }

        const char* category_label(LogCategory cat)
        {
            switch (cat)
            {
            case LogCategory::Engine:   return "Engine";
            case LogCategory::Renderer: return "Renderer";
            case LogCategory::Assets:   return "Assets";
            case LogCategory::World:    return "World";
            case LogCategory::Audio:    return "Audio";
            case LogCategory::Session:  return "Session";
            case LogCategory::Errors:   return "ERROR";
            }
            return "Engine";
        }
    }

    void initialize_logging(const std::filesystem::path& executableDir)
    {
        std::lock_guard lock(gLogMutex);

        std::filesystem::path baseDirectory = executableDir;
        if (const auto envLogs = environment_path("PHOENIX_ENGINE_LOGS"); !envLogs.empty())
        {
            gLogsDirectory = envLogs;
        }
        else
        {
            const auto workingDirectory = std::filesystem::current_path();
            if (std::filesystem::exists(workingDirectory / "Data")
                || std::filesystem::exists(workingDirectory / "PhoenixEngine.sln"))
            {
                baseDirectory = workingDirectory;
            }
            gLogsDirectory = baseDirectory / "Logs";
        }

        std::error_code ec;
        std::filesystem::create_directories(gLogsDirectory, ec);
        gEngineLogPath = gLogsDirectory / "PhoenixEngine.log";

        // Truncate all category logs at startup.
        for (auto cat : { LogCategory::Engine, LogCategory::Renderer, LogCategory::Assets,
                          LogCategory::World, LogCategory::Audio, LogCategory::Session,
                          LogCategory::Errors })
        {
            const auto path = gLogsDirectory / (std::string(category_filename(cat)) + ".log");
            std::ofstream trunc(path, std::ios::trunc);
            if (trunc)
                trunc << "# Phoenix Engine — " << category_label(cat) << " Log\n"
                      << "# Started: " << timestamp() << "\n\n";
        }

        // Legacy engine log (kept for backward compat).
        std::ofstream legacyLog(gEngineLogPath, std::ios::trunc);
        if (legacyLog)
        {
            legacyLog << "[" << timestamp() << "] [Core] Phoenix Engine logging initialized\n";
            legacyLog << "[" << timestamp() << "] [Core] Logs directory: " << gLogsDirectory.string() << "\n";
        }
    }

    const std::filesystem::path& logs_directory()
    {
        return gLogsDirectory;
    }

    const std::filesystem::path& engine_log_path()
    {
        return gEngineLogPath;
    }

    std::filesystem::path specialized_log_path(std::string_view name)
    {
        return gLogsDirectory / (std::string(name) + ".log");
    }

    std::filesystem::path category_log_path(LogCategory category)
    {
        return gLogsDirectory / (std::string(category_filename(category)) + ".log");
    }

    std::ofstream open_engine_log(LogOpenMode mode)
    {
        const auto flags = mode == LogOpenMode::Truncate ? std::ios::trunc : std::ios::app;
        return std::ofstream(gEngineLogPath, flags);
    }

    std::ofstream open_log(LogCategory category, LogOpenMode mode)
    {
        const auto flags = mode == LogOpenMode::Truncate ? std::ios::trunc : std::ios::app;
        return std::ofstream(category_log_path(category), flags);
    }

    void write_log_line(std::string_view category, std::string_view message)
    {
        std::lock_guard lock(gLogMutex);
        std::ofstream legacyLog(gEngineLogPath, std::ios::app);
        if (legacyLog)
            legacyLog << "[" << timestamp() << "] [" << category << "] " << message << "\n";
    }

    void log(LogCategory category, std::string_view message)
    {
        std::lock_guard lock(gLogMutex);
        const auto ts = timestamp();
        const auto label = category_label(category);

        // Write to specialized log.
        std::ofstream catLog(category_log_path(category), std::ios::app);
        if (catLog)
            catLog << "[" << ts << "] " << message << "\n";

        // Mirror to legacy engine log.
        std::ofstream legacyLog(gEngineLogPath, std::ios::app);
        if (legacyLog)
            legacyLog << "[" << ts << "] [" << label << "] " << message << "\n";
    }

}
