#include "app/discord_presence.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace phoenix::app
{
    namespace
    {
        // ---- User configuration --------------------------------------------
        // Your Discord Application ID (Developer Portal -> your app ->
        // "Application ID"). The app's NAME there is the line shown as the
        // title ("Phoenix Engine"). Until a numeric id is set, presence is a
        // no-op. The icon is an art asset you upload under Rich Presence ->
        // Art Assets; reference it by its key below.
        constexpr const char* kDiscordClientId  = "PASTE_YOUR_APPLICATION_ID_HERE";
        constexpr const char* kDiscordLargeImage = "phoenix";          // art asset key
        constexpr const char* kDiscordLargeText  = "Phoenix Engine";   // hover tooltip
        // --------------------------------------------------------------------

        // Discord IPC opcodes.
        enum : std::uint32_t { kOpHandshake = 0, kOpFrame = 1, kOpClose = 2, kOpPing = 3, kOpPong = 4 };

#ifdef _WIN32
        using IpcHandle = HANDLE;
        const IpcHandle kInvalid = INVALID_HANDLE_VALUE;   // not constexpr (a cast)
#else
        using IpcHandle = int;
        constexpr IpcHandle kInvalid = -1;
#endif

        bool client_id_is_valid()
        {
            const char* p = kDiscordClientId;
            if (!p || !*p)
                return false;
            for (; *p; ++p)
                if (*p < '0' || *p > '9')
                    return false;   // placeholder / non-numeric -> stay inert
            return true;
        }

#ifdef _WIN32
        IpcHandle ipc_connect()
        {
            for (int i = 0; i < 10; ++i)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
                HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                    return h;
            }
            return kInvalid;
        }

        bool ipc_write(IpcHandle h, const void* data, std::size_t size)
        {
            const char* p = static_cast<const char*>(data);
            while (size > 0)
            {
                DWORD wrote = 0;
                if (!WriteFile(h, p, static_cast<DWORD>(size), &wrote, nullptr) || wrote == 0)
                    return false;
                p += wrote;
                size -= wrote;
            }
            return true;
        }

        bool ipc_read(IpcHandle h, void* data, std::size_t size)
        {
            char* p = static_cast<char*>(data);
            while (size > 0)
            {
                DWORD got = 0;
                if (!ReadFile(h, p, static_cast<DWORD>(size), &got, nullptr) || got == 0)
                    return false;
                p += got;
                size -= got;
            }
            return true;
        }

        void ipc_close(IpcHandle h) { if (h != kInvalid) CloseHandle(h); }
        int current_pid() { return static_cast<int>(GetCurrentProcessId()); }
#else
        IpcHandle ipc_connect()
        {
            const char* envDirs[] = {
                std::getenv("XDG_RUNTIME_DIR"),
                std::getenv("TMPDIR"),
                std::getenv("TMP"),
                std::getenv("TEMP"),
                "/tmp",
            };
            // Discord may live under flatpak/snap sandbox subdirectories.
            const char* subDirs[] = { "", "/app/com.discordapp.Discord", "/snap.discord" };
            for (const char* base : envDirs)
            {
                if (!base || !*base)
                    continue;
                for (const char* sub : subDirs)
                {
                    for (int i = 0; i < 10; ++i)
                    {
                        char path[256];
                        std::snprintf(path, sizeof(path), "%s%s/discord-ipc-%d", base, sub, i);
                        if (std::strlen(path) >= sizeof(sockaddr_un::sun_path))
                            continue;
                        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                        if (fd < 0)
                            continue;
                        sockaddr_un addr{};
                        addr.sun_family = AF_UNIX;
                        std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
                        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
                            return fd;
                        close(fd);
                    }
                }
            }
            return kInvalid;
        }

        bool ipc_write(IpcHandle fd, const void* data, std::size_t size)
        {
            const char* p = static_cast<const char*>(data);
            while (size > 0)
            {
                const auto wrote = ::write(fd, p, size);
                if (wrote <= 0)
                    return false;
                p += wrote;
                size -= static_cast<std::size_t>(wrote);
            }
            return true;
        }

        bool ipc_read(IpcHandle fd, void* data, std::size_t size)
        {
            char* p = static_cast<char*>(data);
            while (size > 0)
            {
                const auto got = ::read(fd, p, size);
                if (got <= 0)
                    return false;
                p += got;
                size -= static_cast<std::size_t>(got);
            }
            return true;
        }

        void ipc_close(IpcHandle fd) { if (fd != kInvalid) close(fd); }
        int current_pid() { return static_cast<int>(getpid()); }
#endif

        // A Discord IPC frame: [opcode:u32 LE][length:u32 LE][payload bytes].
        bool send_frame(IpcHandle h, std::uint32_t opcode, const std::string& payload)
        {
            std::uint32_t header[2]{ opcode, static_cast<std::uint32_t>(payload.size()) };
            return ipc_write(h, header, sizeof(header))
                && (payload.empty() || ipc_write(h, payload.data(), payload.size()));
        }

        void worker()
        {
            IpcHandle h = ipc_connect();
            if (h == kInvalid)
                return;   // Discord not running — silently give up.

            // Handshake, then publish the activity once.
            const std::string handshake =
                std::string("{\"v\":1,\"client_id\":\"") + kDiscordClientId + "\"}";
            std::string activity =
                std::string("{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"1\",\"args\":{\"pid\":")
                + std::to_string(current_pid())
                + ",\"activity\":{\"assets\":{\"large_image\":\"" + kDiscordLargeImage
                + "\",\"large_text\":\"" + kDiscordLargeText + "\"}}}}";

            if (!send_frame(h, kOpHandshake, handshake) || !send_frame(h, kOpFrame, activity))
            {
                ipc_close(h);
                return;
            }

            // Keep the connection alive: Discord periodically PINGs; reply PONG
            // so the presence persists for the whole session. Any read failure
            // means Discord closed (or quit) — just stop. The process-exit path
            // (std::_Exit) reclaims this detached thread and the socket.
            for (;;)
            {
                std::uint32_t header[2]{};
                if (!ipc_read(h, header, sizeof(header)))
                    break;
                std::string payload;
                if (header[1] > 0)
                {
                    if (header[1] > (1u << 20))
                        break;   // absurd length — bail
                    payload.resize(header[1]);
                    if (!ipc_read(h, payload.data(), payload.size()))
                        break;
                }
                if (header[0] == kOpPing && !send_frame(h, kOpPong, payload))
                    break;
                if (header[0] == kOpClose)
                    break;
            }
            ipc_close(h);
        }
    }

    void start_discord_presence()
    {
        if (!client_id_is_valid())
            return;   // not configured yet — no-op
        std::thread(worker).detach();
    }
}
