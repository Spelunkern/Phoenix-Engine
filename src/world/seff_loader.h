#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace phoenix::world
{
    struct SeffTimeStamp
    {
        std::int16_t year{};
        std::int16_t month{};
        std::int16_t day{};
        std::int16_t hour{};
        std::int16_t minute{};
        std::int16_t second{};
    };

    // One particle-emitter component of a .SEFF effect — much simpler than
    // .EFT's EftEffect (fixed color/size/velocity, no keyframe curves).
    // Field names/order/format-conditional fields follow the reference
    // parser (github.com/matigramirez/Parsec, Shaiya.Seff.SeffEffect),
    // which leaves several fields unidentified (UnknownN) same as it does
    // for .EFT.
    struct SeffEffect
    {
        std::uint32_t particleCount{};
        float particleVelocity{};
        // D3D-style blend mode index, values 0-3 seen in practice (0 = Normal).
        std::uint32_t textureBlendMode{};
        // Values 0-9 seen in practice; scales where along the emitter's
        // spread newly spawned particles start.
        std::uint32_t startPositionMultiplier{};
        std::uint32_t particleLifetimeMs{};
        float unknown6{};
        std::string textureFileName; // UTF-16 in the file, decoded to ASCII here
        std::uint8_t red{};
        std::uint8_t green{};
        std::uint8_t blue{};
        float particleStartPosition[3]{};
        float unknown10{};
        float particleStartSize{};
        bool isVisible{ true };
        float unknown12{};
        // Present only when the containing Seff's format is > 2 / > 3 / > 5
        // respectively — older files simply don't have these bytes.
        float rotateWithStretchMultiplier{};
        float velocityMultiplier{};
        std::uint32_t unknown15{};
    };

    // A named group of effect components — SeffEffect entries sharing one id,
    // analogous to (but much flatter than) an .EFT sequence.
    struct SeffRecord
    {
        std::int32_t id{};
        std::vector<SeffEffect> effects;
    };

    // A parsed .SEFF file (weather, weapon glow/trail, login-screen, or
    // one-off event effects — see docs/SEFF_FORMAT.md).
    struct SeffLibrary
    {
        std::int32_t format{};
        SeffTimeStamp timeStamp{};
        std::vector<SeffRecord> records;
        bool parsed{};
    };

    SeffLibrary load_seff(const std::filesystem::path& path);
}
