#pragma once

#include "world/wld_loader.h"

#include <filesystem>

namespace phoenix::world
{
    // Check if a Phoenix Worlds directory exists for the given map ID.
    bool phoenix_world_exists(const std::filesystem::path& worldsRoot, const std::string& mapId);

    // Load a map from the Phoenix Worlds open format (CSVs + raw files).
    // Produces the same WldAnalysis struct that analyze_wld() returns.
    WldAnalysis load_phoenix_world(const std::filesystem::path& worldDir);
}
