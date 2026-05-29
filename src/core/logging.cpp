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

        std::ofstream log(gEngineLogPath, std::ios::trunc);
        if (!log)
            return;

        log << "[" << timestamp() << "] [Core] Phoenix Engine logging initialized\n";
        log << "[" << timestamp() << "] [Core] Logs directory: " << gLogsDirectory.string() << "\n";
    }

    const std::filesystem::path& logs_directory()
    {
        return gLogsDirectory;
    }

    const std::filesystem::path& engine_log_path()
    {
        return gEngineLogPath;
    }

    std::ofstream open_engine_log(LogOpenMode mode)
    {
        const auto flags = mode == LogOpenMode::Truncate ? std::ios::trunc : std::ios::app;
        return std::ofstream(gEngineLogPath, flags);
    }

    void write_log_line(std::string_view category, std::string_view message)
    {
        std::lock_guard lock(gLogMutex);
        std::ofstream log(gEngineLogPath, std::ios::app);
        if (!log)
            return;

        log << "[" << timestamp() << "] [" << category << "] " << message << "\n";
    }
}
