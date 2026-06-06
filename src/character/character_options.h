#pragma once

#include "ui/editor_panel.h"

#include <filesystem>
#include <vector>

namespace phoenix::character
{
    std::vector<phoenix::ui::CharacterOption> scan_character_options(
        const std::filesystem::path& dataRoot);
}
