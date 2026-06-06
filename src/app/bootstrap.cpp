#include "app/bootstrap.h"


#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#ifdef PHOENIX_USE_MIMALLOC
#include <mimalloc.h>
#elif __has_include(<malloc.h>)
#include <malloc.h>
#define PHOENIX_HAS_MALLOC_H 1
#endif
#endif

namespace phoenix::app
{
    std::filesystem::path executable_directory()
    {
#ifdef _WIN32
        std::wstring path(MAX_PATH, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return std::filesystem::current_path();
        path.resize(length);
        return std::filesystem::path(path).parent_path();
#else
        std::error_code ec;
        auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec && !path.empty())
            return path.parent_path();
        return std::filesystem::current_path();
#endif
    }

    void release_memory_to_os()
    {
#ifdef _WIN32
        SetProcessWorkingSetSize(GetCurrentProcess(),
            static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
#elif defined(PHOENIX_USE_MIMALLOC)
        mi_collect(true);
#elif defined(__GLIBC__) && defined(PHOENIX_HAS_MALLOC_H)
        malloc_trim(0);
#endif
    }

}
