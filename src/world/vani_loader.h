#pragma once

#include "world/smod_loader.h"

#include <filesystem>

namespace phoenix::world
{
    SmodModel load_vani(const std::filesystem::path& path);
}
