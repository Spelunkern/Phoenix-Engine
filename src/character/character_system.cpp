#include "character/character_system.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <format>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace phoenix::character
{
    namespace
    {
        constexpr float kCharacterScale = 0.95f;
        constexpr float kGroundClearance = 0.06f;
        constexpr float kRunSpeed = 4.6f;
        constexpr float kFastRunSpeed = 7.4f;
        constexpr float kSwimSpeed = 2.8f;
        constexpr float kWaterSurface = 0.0f;
        constexpr float kWaterEnterDepth = 2.0f;
        constexpr float kFloatFeetDepth = 1.20f;
        constexpr float kBuoyancySpeed = 1.05f;
        constexpr float kJumpImpulse = 2.7f;
        constexpr float kGravity = 7.5f;
        constexpr float kAniFramesPerSecond = 22.0f;
        constexpr float kPi = 3.1415926535f;

        // ---- Minimal math library ----

        struct Vec3
        {
            float x{};
            float y{};
            float z{};
        };

        struct Quat
        {
            float x{};
            float y{};
            float z{};
            float w{ 1.0f };
        };

        struct Mat4
        {
            float m[4][4]{};
            static Mat4 identity()
            {
                Mat4 r{};
                r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
                return r;
            }
        };

        Quat normalize_quat(Quat q)
        {
            const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (len <= 0.000001f)
                return { 0, 0, 0, 1 };
            return { q.x / len, q.y / len, q.z / len, q.w / len };
        }

        Quat slerp_quat(Quat a, Quat b, float t)
        {
            a = normalize_quat(a);
            b = normalize_quat(b);
            float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
            if (dot < 0.0f)
            {
                dot = -dot;
                b = { -b.x, -b.y, -b.z, -b.w };
            }
            if (dot > 0.9995f)
                return normalize_quat({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
            const float theta0 = std::acos(std::clamp(dot, -1.0f, 1.0f));
            const float theta = theta0 * t;
            const float sinTheta = std::sin(theta);
            const float sinTheta0 = std::sin(theta0);
            if (std::abs(sinTheta0) <= 0.000001f)
                return a;
            const float s0 = std::cos(theta) - dot * sinTheta / sinTheta0;
            const float s1 = sinTheta / sinTheta0;
            return normalize_quat({ a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1, a.w * s0 + b.w * s1 });
        }

        Mat4 mat4_from_rotation_translation(Quat q, Vec3 t)
        {
            q = normalize_quat(q);
            const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
            const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
            const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
            Mat4 r{};
            r.m[0][0] = 1.0f - 2.0f * (yy + zz);
            r.m[0][1] = 2.0f * (xy + wz);
            r.m[0][2] = 2.0f * (xz - wy);
            r.m[1][0] = 2.0f * (xy - wz);
            r.m[1][1] = 1.0f - 2.0f * (xx + zz);
            r.m[1][2] = 2.0f * (yz + wx);
            r.m[2][0] = 2.0f * (xz + wy);
            r.m[2][1] = 2.0f * (yz - wx);
            r.m[2][2] = 1.0f - 2.0f * (xx + yy);
            r.m[3][0] = t.x;
            r.m[3][1] = t.y;
            r.m[3][2] = t.z;
            r.m[3][3] = 1.0f;
            return r;
        }

        Mat4 mat4_multiply(const Mat4& a, const Mat4& b)
        {
            Mat4 r{};
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    for (int k = 0; k < 4; ++k)
                        r.m[i][j] += a.m[i][k] * b.m[k][j];
            return r;
        }

        Mat4 mat4_inverse(const Mat4& m)
        {
            Mat4 inv{};
            const float* s = &m.m[0][0];
            float* d = &inv.m[0][0];
            d[0] = s[5]*s[10]*s[15] - s[5]*s[11]*s[14] - s[9]*s[6]*s[15] + s[9]*s[7]*s[14] + s[13]*s[6]*s[11] - s[13]*s[7]*s[10];
            d[4] = -s[4]*s[10]*s[15] + s[4]*s[11]*s[14] + s[8]*s[6]*s[15] - s[8]*s[7]*s[14] - s[12]*s[6]*s[11] + s[12]*s[7]*s[10];
            d[8] = s[4]*s[9]*s[15] - s[4]*s[11]*s[13] - s[8]*s[5]*s[15] + s[8]*s[7]*s[13] + s[12]*s[5]*s[11] - s[12]*s[7]*s[9];
            d[12] = -s[4]*s[9]*s[14] + s[4]*s[10]*s[13] + s[8]*s[5]*s[14] - s[8]*s[6]*s[13] - s[12]*s[5]*s[10] + s[12]*s[6]*s[9];
            d[1] = -s[1]*s[10]*s[15] + s[1]*s[11]*s[14] + s[9]*s[2]*s[15] - s[9]*s[3]*s[14] - s[13]*s[2]*s[11] + s[13]*s[3]*s[10];
            d[5] = s[0]*s[10]*s[15] - s[0]*s[11]*s[14] - s[8]*s[2]*s[15] + s[8]*s[3]*s[14] + s[12]*s[2]*s[11] - s[12]*s[3]*s[10];
            d[9] = -s[0]*s[9]*s[15] + s[0]*s[11]*s[13] + s[8]*s[1]*s[15] - s[8]*s[3]*s[13] - s[12]*s[1]*s[11] + s[12]*s[3]*s[9];
            d[13] = s[0]*s[9]*s[14] - s[0]*s[10]*s[13] - s[8]*s[1]*s[14] + s[8]*s[2]*s[13] + s[12]*s[1]*s[10] - s[12]*s[2]*s[9];
            d[2] = s[1]*s[6]*s[15] - s[1]*s[7]*s[14] - s[5]*s[2]*s[15] + s[5]*s[3]*s[14] + s[13]*s[2]*s[7] - s[13]*s[3]*s[6];
            d[6] = -s[0]*s[6]*s[15] + s[0]*s[7]*s[14] + s[4]*s[2]*s[15] - s[4]*s[3]*s[14] - s[12]*s[2]*s[7] + s[12]*s[3]*s[6];
            d[10] = s[0]*s[5]*s[15] - s[0]*s[7]*s[13] - s[4]*s[1]*s[15] + s[4]*s[3]*s[13] + s[12]*s[1]*s[7] - s[12]*s[3]*s[5];
            d[14] = -s[0]*s[5]*s[14] + s[0]*s[6]*s[13] + s[4]*s[1]*s[14] - s[4]*s[2]*s[13] - s[12]*s[1]*s[6] + s[12]*s[2]*s[5];
            d[3] = -s[1]*s[6]*s[11] + s[1]*s[7]*s[10] + s[5]*s[2]*s[11] - s[5]*s[3]*s[10] - s[9]*s[2]*s[7] + s[9]*s[3]*s[6];
            d[7] = s[0]*s[6]*s[11] - s[0]*s[7]*s[10] - s[4]*s[2]*s[11] + s[4]*s[3]*s[10] + s[8]*s[2]*s[7] - s[8]*s[3]*s[6];
            d[11] = -s[0]*s[5]*s[11] + s[0]*s[7]*s[9] + s[4]*s[1]*s[11] - s[4]*s[3]*s[9] - s[8]*s[1]*s[7] + s[8]*s[3]*s[5];
            d[15] = s[0]*s[5]*s[10] - s[0]*s[6]*s[9] - s[4]*s[1]*s[10] + s[4]*s[2]*s[9] + s[8]*s[1]*s[6] - s[8]*s[2]*s[5];
            float det = s[0]*d[0] + s[1]*d[4] + s[2]*d[8] + s[3]*d[12];
            if (std::abs(det) < 1e-10f)
                return Mat4::identity();
            det = 1.0f / det;
            for (int i = 0; i < 16; ++i)
                d[i] *= det;
            return inv;
        }

        Mat4 mat4_from_shaiya_transposed(const float (&raw)[16])
        {
            Mat4 m{};
            m.m[0][0] = raw[0]; m.m[0][1] = raw[4]; m.m[0][2] = raw[8]; m.m[0][3] = raw[12];
            m.m[1][0] = raw[1]; m.m[1][1] = raw[5]; m.m[1][2] = raw[9]; m.m[1][3] = raw[13];
            m.m[2][0] = raw[2]; m.m[2][1] = raw[6]; m.m[2][2] = raw[10]; m.m[2][3] = raw[14];
            m.m[3][0] = raw[3]; m.m[3][1] = raw[7]; m.m[3][2] = raw[11]; m.m[3][3] = raw[15];
            return m;
        }

        Vec3 mat4_get_translation(const Mat4& m)
        {
            return { m.m[3][0], m.m[3][1], m.m[3][2] };
        }

        Vec3 transform_point(const Mat4& m, Vec3 v)
        {
            return {
                m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z + m.m[3][0],
                m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z + m.m[3][1],
                m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z + m.m[3][2],
            };
        }

        Vec3 transform_normal(const Mat4& m, Vec3 v)
        {
            return {
                m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z,
                m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z,
                m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z,
            };
        }

        Vec3 normalize_vec3(Vec3 v)
        {
            const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (len <= 0.0001f)
                return {};
            return { v.x / len, v.y / len, v.z / len };
        }

        // ---- Animation sampling ----

        Quat sample_rotation(const world::CharacterAnimationBone& bone, float frame)
        {
            if (bone.rotationFrames.empty())
                return { 0, 0, 0, 1 };
            auto previous = bone.rotationFrames.front();
            for (const auto& next : bone.rotationFrames)
            {
                if (static_cast<float>(next.frame) >= frame)
                {
                    const float span = std::max(1.0f, static_cast<float>(next.frame - previous.frame));
                    const float t = std::clamp((frame - static_cast<float>(previous.frame)) / span, 0.0f, 1.0f);
                    return slerp_quat(
                        { previous.quaternion[0], previous.quaternion[1], previous.quaternion[2], previous.quaternion[3] },
                        { next.quaternion[0], next.quaternion[1], next.quaternion[2], next.quaternion[3] }, t);
                }
                previous = next;
            }
            const auto& last = bone.rotationFrames.back();
            return normalize_quat({ last.quaternion[0], last.quaternion[1], last.quaternion[2], last.quaternion[3] });
        }

        Vec3 sample_translation(const world::CharacterAnimationBone& bone, float frame)
        {
            if (bone.translationFrames.empty())
                return {};
            auto previous = bone.translationFrames.front();
            for (const auto& next : bone.translationFrames)
            {
                if (static_cast<float>(next.frame) >= frame)
                {
                    const float span = std::max(1.0f, static_cast<float>(next.frame - previous.frame));
                    const float t = std::clamp((frame - static_cast<float>(previous.frame)) / span, 0.0f, 1.0f);
                    return {
                        previous.translation[0] + (next.translation[0] - previous.translation[0]) * t,
                        previous.translation[1] + (next.translation[1] - previous.translation[1]) * t,
                        previous.translation[2] + (next.translation[2] - previous.translation[2]) * t,
                    };
                }
                previous = next;
            }
            const auto& last = bone.translationFrames.back();
            return { last.translation[0], last.translation[1], last.translation[2] };
        }

        std::vector<Mat4> compute_client_finals(const world::CharacterAnimation& animation, float frame)
        {
            const auto boneCount = animation.bones.size();
            std::vector<Mat4> rawMatrices(boneCount);
            for (std::size_t i = 0; i < boneCount; ++i)
                rawMatrices[i] = mat4_from_shaiya_transposed(animation.bones[i].matrix);

            std::vector<Mat4> locals(boneCount);
            for (std::size_t i = 0; i < boneCount; ++i)
            {
                const auto& bone = animation.bones[i];
                Mat4 local = rawMatrices[i];
                const auto parent = bone.parentBoneIndex;
                if (parent >= 0 && static_cast<std::size_t>(parent) < boneCount)
                    local = mat4_multiply(rawMatrices[i], mat4_inverse(rawMatrices[static_cast<std::size_t>(parent)]));

                if (!bone.rotationFrames.empty() || !bone.translationFrames.empty())
                {
                    Vec3 translation = mat4_get_translation(local);
                    if (!bone.rotationFrames.empty())
                    {
                        const auto rotation = sample_rotation(bone, frame);
                        local = mat4_from_rotation_translation(rotation, {});
                    }
                    if (!bone.translationFrames.empty())
                        translation = sample_translation(bone, frame);
                    local.m[3][0] = translation.x;
                    local.m[3][1] = translation.y;
                    local.m[3][2] = translation.z;
                    local.m[3][3] = 1.0f;
                }
                locals[i] = local;
            }

            std::vector<Mat4> finals(boneCount, Mat4::identity());
            for (std::size_t i = 0; i < boneCount; ++i)
            {
                auto matrix = locals[i];
                const auto parent = animation.bones[i].parentBoneIndex;
                if (parent >= 0 && static_cast<std::size_t>(parent) < i)
                    matrix = mat4_multiply(matrix, finals[static_cast<std::size_t>(parent)]);
                finals[i] = matrix;
            }
            return finals;
        }

        // ---- Character loading helpers ----

        std::vector<CharacterAnimationChoice> load_animation_choices_from_csv(
            const std::filesystem::path& csvPath,
            const std::filesystem::path& animationRoot)
        {
            std::vector<CharacterAnimationChoice> animations;
            std::ifstream file(csvPath);
            if (!file)
                return animations;
            std::string line;
            std::getline(file, line); // skip header
            while (std::getline(file, line))
            {
                if (line.empty())
                    continue;
                // RecordIndex,Header,Name,Mode,Unknown,Float1,Float2,Float3,Float4
                std::size_t pos = 0;
                auto next = [&]() -> std::string {
                    auto comma = line.find(',', pos);
                    if (comma == std::string::npos) comma = line.size();
                    auto token = line.substr(pos, comma - pos);
                    pos = comma < line.size() ? comma + 1 : line.size();
                    return token;
                };
                next(); // RecordIndex (unused — vector index matches)
                next(); // Header (AL3)
                auto aniFileName = next();
                if (aniFileName.empty())
                    continue;

                auto aniPath = animationRoot / aniFileName;
                if (!std::filesystem::exists(aniPath))
                    continue;

                CharacterAnimationChoice choice{};
                choice.path = aniPath;
                auto stem = aniPath.stem().string();
                std::transform(stem.begin(), stem.end(), stem.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                choice.name = std::move(stem);
                choice.animation = world::load_character_ani(choice.path);
                if (choice.animation.parsed)
                    animations.push_back(std::move(choice));
            }
            return animations;
        }

        std::vector<CharacterAnimationChoice> load_animation_choices(const std::filesystem::path& animationRoot, const std::string& prefix)
        {
            const auto csvName = prefix.substr(0, prefix.size() - (prefix.ends_with("_") ? 1 : 0)) + "_action.csv";
            const auto csvPath = animationRoot.parent_path() / csvName;
            return load_animation_choices_from_csv(csvPath, animationRoot);
        }

        std::size_t find_animation(const std::vector<CharacterAnimationChoice>& animations, const std::string& token, std::size_t fallback)
        {
            const auto it = std::find_if(animations.begin(), animations.end(), [&](const auto& choice) {
                return choice.name.find(token) != std::string::npos;
            });
            if (it != animations.end())
                return static_cast<std::size_t>(std::distance(animations.begin(), it));
            return animations.empty() ? 0u : std::min(fallback, animations.size() - 1u);
        }

        std::string lower_ascii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::string path_key(const std::filesystem::path& path)
        {
            return lower_ascii(path.lexically_normal().string());
        }

        std::string animation_cache_key(const std::filesystem::path& animationRoot, std::string_view prefix)
        {
            return path_key(animationRoot) + "|" + lower_ascii(std::string(prefix));
        }

        bool is_display_character_texture(std::string stem)
        {
            stem = lower_ascii(std::move(stem));
            const auto startsWithDigits = [&](std::size_t pos) {
                return pos + 3 <= stem.size()
                    && std::isdigit(static_cast<unsigned char>(stem[pos]))
                    && std::isdigit(static_cast<unsigned char>(stem[pos + 1]))
                    && std::isdigit(static_cast<unsigned char>(stem[pos + 2]));
            };

            const auto checkEquipment = [&](std::string_view marker) {
                const auto pos = stem.find(marker);
                if (pos == std::string::npos || pos < 4)
                    return false;
                const auto indexPos = pos + marker.size();
                if (!startsWithDigits(indexPos))
                    return false;
                return stem.size() == indexPos + 3
                    || (stem.size() == indexPos + 4
                        && (stem[indexPos + 3] == '1' || stem[indexPos + 3] == '2'));
            };

            if (stem.starts_with("co_"))
                return checkEquipment("_torso")
                    || checkEquipment("_lower")
                    || checkEquipment("_hand")
                    || checkEquipment("_boots");

            if (checkEquipment("_torso")
                || checkEquipment("_lower")
                || checkEquipment("_hand")
                || checkEquipment("_boots"))
                return true;

            const auto checkBody = [&](std::string_view marker) {
                const auto pos = stem.find(marker);
                if (pos == std::string::npos || pos < 3)
                    return false;
                const auto indexPos = pos + marker.size();
                if (!startsWithDigits(indexPos))
                    return false;
                return stem.size() == indexPos + 3
                    || (stem.size() == indexPos + 5
                        && stem[indexPos + 3] == '_'
                        && std::isdigit(static_cast<unsigned char>(stem[indexPos + 4])));
            };
            return checkBody("_face") || checkBody("_hair");
        }

        struct PartTableEntry
        {
            std::string meshName;
            std::string textureName;
            bool alphaCutout{};
        };

        using PartTable = std::unordered_map<int, PartTableEntry>;

        PartTable load_part_table_csv(const std::filesystem::path& csvPath)
        {
            PartTable table;
            std::ifstream file(csvPath);
            if (!file)
                return table;
            std::string line;
            std::getline(file, line); // skip header
            while (std::getline(file, line))
            {
                if (line.empty())
                    continue;
                // Strip quotes and parse: RecordIndex,MeshIndex,TextureIndex,AlphaBlendingMode,AlphaBlendingModeValue,MeshName,TextureName
                std::string clean;
                clean.reserve(line.size());
                for (char c : line)
                    if (c != '"') clean += c;

                std::size_t pos = 0;
                auto next = [&]() -> std::string {
                    auto comma = clean.find(',', pos);
                    if (comma == std::string::npos) comma = clean.size();
                    auto token = clean.substr(pos, comma - pos);
                    pos = comma < clean.size() ? comma + 1 : clean.size();
                    return token;
                };
                const auto recordIndex = std::atoi(next().c_str());
                next(); // MeshIndex
                next(); // TextureIndex
                const auto alphaMode = next();
                next(); // AlphaBlendingModeValue
                auto meshName = next();
                auto textureName = next();
                if (meshName.empty())
                    continue;
                PartTableEntry entry{};
                entry.meshName = std::move(meshName);
                entry.textureName = std::move(textureName);
                entry.alphaCutout = (alphaMode == "Alpha" || alphaMode == "Visibility");
                table[recordIndex] = std::move(entry);
            }
            return table;
        }

        struct ResolvedPart
        {
            std::string mesh;
            std::filesystem::path texture;
            bool alphaCutout;
        };

        std::optional<ResolvedPart> resolve_part_from_table(
            const std::filesystem::path& textureRoot,
            const std::filesystem::path& raceRoot,
            std::string_view prefix,
            std::string_view partFile,
            int index)
        {
            auto csvPath = raceRoot / std::format("{}_{}.csv", prefix, partFile);
            if (!std::filesystem::exists(csvPath))
                return std::nullopt;
            auto table = load_part_table_csv(csvPath);
            auto it = table.find(index);
            if (it == table.end())
                return std::nullopt;
            const auto& entry = it->second;
            ResolvedPart part{};
            part.mesh = entry.meshName;
            part.alphaCutout = entry.alphaCutout;
            if (!entry.textureName.empty())
                part.texture = textureRoot / entry.textureName;
            return part;
        }

        void append_loaded_part(
            CharacterData& data,
            const world::CharacterModel& model,
            std::uint32_t textureIndex,
            bool alphaCutout)
        {
            const auto baseVertex = static_cast<std::uint32_t>(data.bindVertices.size());
            const auto meshBoneBase = static_cast<std::uint32_t>(data.meshBones.size());
            const auto meshBoneCount = static_cast<std::uint32_t>(model.bones.size());
            data.meshBones.insert(data.meshBones.end(), model.bones.begin(), model.bones.end());

            for (const auto& src : model.vertices)
            {
                CharacterData::SourceVertex sv{};
                sv.position[0] = src.position[0];
                sv.position[1] = src.position[1];
                sv.position[2] = src.position[2];
                sv.normal[0] = src.normal[0];
                sv.normal[1] = src.normal[1];
                sv.normal[2] = src.normal[2];
                sv.uv[0] = src.uv[0];
                sv.uv[1] = src.uv[1];
                sv.weights[0] = src.boneWeights[0];
                sv.weights[1] = src.boneWeights[1];
                sv.weights[2] = src.boneWeights[2];
                sv.bones[0] = src.boneIndices[0];
                sv.bones[1] = src.boneIndices[1];
                sv.bones[2] = src.boneIndices[2];
                sv.meshBoneBase = meshBoneBase;
                sv.meshBoneCount = meshBoneCount;
                data.sourceVertices.push_back(sv);

                CharacterGpuVertex gv{};
                gv.position[0] = sv.position[0] * kCharacterScale;
                gv.position[1] = sv.position[1] * kCharacterScale;
                gv.position[2] = sv.position[2] * kCharacterScale;
                gv.color[0] = 1.0f; gv.color[1] = 1.0f; gv.color[2] = 1.0f;
                gv.normal[0] = sv.normal[0];
                gv.normal[1] = sv.normal[1];
                gv.normal[2] = sv.normal[2];
                gv.uv[0] = sv.uv[0];
                gv.uv[1] = sv.uv[1];
                gv.textureLayer = textureIndex + (alphaCutout ? 2048u : 0u);
                data.bindVertices.push_back(gv);
            }

            CharacterBatch batch{};
            batch.textureIndex = textureIndex;
            batch.startIndex = static_cast<std::uint32_t>(data.indices.size());
            batch.alphaCutout = alphaCutout;
            for (const auto& face : model.faces)
            {
                data.indices.push_back(baseVertex + face.indices[0]);
                data.indices.push_back(baseVertex + face.indices[1]);
                data.indices.push_back(baseVertex + face.indices[2]);
            }
            batch.indexCount = static_cast<std::uint32_t>(data.indices.size()) - batch.startIndex;
            data.batches.push_back(batch);
        }
    }

    bool CharacterSystem::preload(const std::filesystem::path& dataRoot)
    {
        if (cacheReady_ && cachedDataRoot_ == dataRoot)
            return true;

        cachedDataRoot_ = dataRoot;
        cachedModels_.clear();
        cachedAnimations_.clear();
        cachedTextureSlotByPath_.clear();
        cachedTexturePaths_.clear();

        const auto characterRoot = dataRoot / "Character";
        if (!std::filesystem::exists(characterRoot))
            return false;

        std::map<std::string, std::vector<std::filesystem::path>> animationFilesByPrefix;
        for (const auto& raceEntry : std::filesystem::directory_iterator(characterRoot))
        {
            if (!raceEntry.is_directory())
                continue;

            const auto meshRoot = raceEntry.path() / "3DC";
            if (std::filesystem::exists(meshRoot))
            {
                for (const auto& entry : std::filesystem::directory_iterator(meshRoot))
                {
                    if (!entry.is_regular_file() || lower_ascii(entry.path().extension().string()) != ".3dc")
                        continue;
                    auto model = world::load_character_3dc(entry.path());
                    if (model.parsed)
                        cachedModels_.emplace(path_key(entry.path()), std::move(model));
                }
            }

            const auto textureRoot = raceEntry.path() / "DDS";
            if (std::filesystem::exists(textureRoot))
            {
                for (const auto& entry : std::filesystem::directory_iterator(textureRoot))
                {
                    if (!entry.is_regular_file()
                        || lower_ascii(entry.path().extension().string()) != ".dds"
                        || !is_display_character_texture(entry.path().stem().string()))
                    {
                        continue;
                    }
                    const auto key = path_key(entry.path());
                    if (cachedTextureSlotByPath_.contains(key))
                        continue;
                    const auto slot = static_cast<std::uint32_t>(cachedTexturePaths_.size());
                    cachedTextureSlotByPath_.emplace(key, slot);
                    cachedTexturePaths_.push_back(entry.path());
                }
            }

            const auto animationRoot = raceEntry.path() / "ANI";
            if (std::filesystem::exists(animationRoot))
            {
                for (const auto& entry : std::filesystem::directory_iterator(animationRoot))
                {
                    if (!entry.is_regular_file() || lower_ascii(entry.path().extension().string()) != ".ani")
                        continue;
                    const auto stem = lower_ascii(entry.path().stem().string());
                    const auto underscore = stem.find('_');
                    if (underscore == std::string::npos)
                        continue;
                    const auto prefix = stem.substr(0, underscore + 1);
                    animationFilesByPrefix[animation_cache_key(animationRoot, prefix)].push_back(entry.path());
                }
            }
        }

        for (auto& [key, paths] : animationFilesByPrefix)
        {
            std::vector<CharacterAnimationChoice> choices;
            choices.reserve(paths.size());
            for (const auto& path : paths)
            {
                CharacterAnimationChoice choice{};
                choice.path = path;
                choice.name = lower_ascii(path.stem().string());
                choice.animation = world::load_character_ani(path);
                if (choice.animation.parsed)
                    choices.push_back(std::move(choice));
            }
            std::ranges::sort(choices, [](const auto& lhs, const auto& rhs) {
                return lhs.name < rhs.name;
            });
            if (!choices.empty())
                cachedAnimations_.emplace(key, std::move(choices));
        }

        cacheReady_ = true;
        {
            std::size_t modelBytes = 0;
            for (const auto& [k, m] : cachedModels_)
                modelBytes += m.vertices.capacity() * sizeof(world::CharacterVertex)
                    + m.faces.capacity() * sizeof(world::CharacterFace)
                    + m.bones.capacity() * sizeof(world::CharacterBone);
            std::size_t animBytes = 0;
            std::size_t totalAnims = 0;
            for (const auto& [k, choices] : cachedAnimations_)
            {
                totalAnims += choices.size();
                for (const auto& c : choices)
                    for (const auto& bone : c.animation.bones)
                        animBytes += bone.rotationFrames.capacity() * sizeof(world::CharacterAnimationRotationFrame)
                            + bone.translationFrames.capacity() * sizeof(world::CharacterAnimationTranslationFrame)
                            + sizeof(world::CharacterAnimationBone);
            }
            std::ofstream log("PhoenixEngine.log", std::ios::app);
            log << "Character cache: models=" << cachedModels_.size()
                << " textures=" << cachedTexturePaths_.size()
                << " animationSets=" << cachedAnimations_.size()
                << " totalAnims=" << totalAnims
                << " modelMB=" << (modelBytes / (1024 * 1024))
                << " animMB=" << (animBytes / (1024 * 1024))
                << "\n";
        }
        return true;
    }

    bool CharacterSystem::load(const std::filesystem::path& dataRoot)
    {
        return load(dataRoot, CharacterAppearance{});
    }

    bool CharacterSystem::load(const std::filesystem::path& dataRoot, const CharacterAppearance& appearance)
    {
        preload(dataRoot);

        const auto root = dataRoot / "Character" / appearance.raceFolder;
        const auto meshRoot = root / "3DC";
        const auto textureRoot = root / "DDS";
        const auto animationRoot = root / "ANI";

        data_ = {};
        worldVertices_.clear();
        animationSeconds_ = 0.0f;
        activeAnimation_ = 0;

        struct Part
        {
            std::string mesh;
            std::filesystem::path texture;
            bool alphaCutout;
        };

        // Resolve parts from CSV tables (MLT converted to CSV).
        // Helmet and hair are mutually exclusive: if helmet is visible, hair is hidden.
        struct PartSpec { std::string_view partFile; int index; };
        std::vector<PartSpec> partSpecs{
            { "upper",   appearance.upperIndex },
            { "lower",   appearance.lowerIndex },
            { "hand",    appearance.handIndex },
            { "foot",    appearance.footIndex },
            { "face",    appearance.faceIndex },
        };
        if (appearance.helmetVisible)
            partSpecs.push_back({ "helmet", appearance.helmetIndex });
        else
            partSpecs.push_back({ "hair", appearance.hairIndex });

        std::vector<Part> parts;
        parts.reserve(6);
        for (const auto& spec : partSpecs)
        {
            auto resolved = resolve_part_from_table(textureRoot, root, appearance.prefix, spec.partFile, spec.index);
            if (resolved)
                parts.push_back({ std::move(resolved->mesh), std::move(resolved->texture), resolved->alphaCutout });
        }

        // Load animations.
        const auto prefix = appearance.prefix + "_";
        if (const auto it = cachedAnimations_.find(animation_cache_key(animationRoot, prefix)); it != cachedAnimations_.end())
            data_.animations = it->second;
        else
            data_.animations = load_animation_choices(animationRoot, prefix);
        data_.idleAnimation = find_animation(data_.animations, prefix + "000_normal", 0);
        data_.walkAnimation = find_animation(data_.animations, prefix + "001_walk", 1);
        data_.runAnimation = find_animation(data_.animations, prefix + "002_run", 2);
        data_.backAnimation = find_animation(data_.animations, prefix + "003_bstep", 3);
        data_.leftAnimation = find_animation(data_.animations, prefix + "004_lstep", 4);
        data_.rightAnimation = find_animation(data_.animations, prefix + "005_rstep", 5);
        data_.jumpAnimation = find_animation(data_.animations, prefix + "008_jump", 8);
        data_.swimIdleAnimation = find_animation(data_.animations, prefix + "006_swnormal", 6);
        data_.swimAnimation = find_animation(data_.animations, prefix + "007_swim", 7);
        activeAnimation_ = data_.idleAnimation;

        // Load mesh parts.
        std::unordered_map<std::string, std::uint32_t> selectedTextureSlotByPath;
        std::uint32_t parsedParts = 0;

        for (const auto& part : parts)
        {
            const auto meshPath = meshRoot / part.mesh;
            const world::CharacterModel* model = nullptr;
            if (const auto it = cachedModels_.find(path_key(meshPath)); it != cachedModels_.end())
                model = &it->second;
            else
            {
                auto loadedModel = world::load_character_3dc(meshPath);
                if (loadedModel.parsed)
                {
                    auto [insertedIt, _] = cachedModels_.emplace(path_key(meshPath), std::move(loadedModel));
                    model = &insertedIt->second;
                }
            }
            if (!model || !model->parsed)
                continue;

            // Resolve texture.
            if (part.texture.empty())
                continue;

            const auto textureKey = path_key(part.texture);
            std::uint32_t textureIndex = 0;
            if (const auto localIt = selectedTextureSlotByPath.find(textureKey); localIt != selectedTextureSlotByPath.end())
            {
                textureIndex = localIt->second;
            }
            else
            {
                textureIndex = static_cast<std::uint32_t>(data_.texturePaths.size());
                selectedTextureSlotByPath.emplace(textureKey, textureIndex);
                data_.texturePaths.push_back(part.texture);
            }

            append_loaded_part(data_, *model, textureIndex, part.alphaCutout);
            ++parsedParts;
        }

        data_.loaded = parsedParts > 0 && !data_.animations.empty();
        worldVertices_ = data_.bindVertices;

        {
            std::ofstream log("PhoenixEngine.log", std::ios::app);
            log << "Character loaded: race=" << appearance.raceFolder
                << " prefix=" << appearance.prefix
                << " upper=" << appearance.upperIndex
                << " lower=" << appearance.lowerIndex
                << " hand=" << appearance.handIndex
                << " foot=" << appearance.footIndex
                << " helmet=" << appearance.helmetIndex
                << " face=" << appearance.faceIndex
                << " hair=" << appearance.hairIndex
                << " parts=" << parsedParts
                << " vertices=" << data_.bindVertices.size()
                << " indices=" << data_.indices.size()
                << " textures=" << data_.texturePaths.size()
                << " animations=" << data_.animations.size()
                << " ready=" << data_.loaded << "\n";
        }
        return data_.loaded;
    }

    void CharacterSystem::update(float deltaSeconds, const PlayableInput& input)
    {
        if (!data_.loaded)
            return;

        const float clampedDelta = std::clamp(deltaSeconds, 0.0f, 0.05f);

        // ---- Camera orbit ----
        if (input.cameraDrag)
        {
            cameraYaw_ += input.mouseDx * 0.0065f;
            cameraPitch_ += input.mouseDy * 0.0045f;
        }
        if (input.mouseWheel != 0.0f)
        {
            const float wheelSteps = input.mouseWheel / 120.0f;
            cameraDistance_ = std::clamp(cameraDistance_ - wheelSteps * 1.15f, 2.6f, 18.0f);
        }
        if (input.yawLeft)
            cameraYaw_ -= 1.8f * clampedDelta;
        if (input.yawRight)
            cameraYaw_ += 1.8f * clampedDelta;
        if (input.pitchUp)
            cameraPitch_ += 1.4f * clampedDelta;
        if (input.pitchDown)
            cameraPitch_ -= 1.4f * clampedDelta;
        cameraPitch_ = std::clamp(cameraPitch_, -0.54f, 0.08f);

        // ---- Movement ----
        const float forwardX = std::sin(cameraYaw_);
        const float forwardZ = std::cos(cameraYaw_);
        const float swimForwardY = std::sin(cameraPitch_);
        const float rightX = std::cos(cameraYaw_);
        const float rightZ = -std::sin(cameraYaw_);
        float moveX = 0.0f;
        float moveY = 0.0f;
        float moveZ = 0.0f;
        const bool forwardPressed = input.forward && !input.backward;
        const bool backwardPressed = input.backward && !input.forward;
        const bool leftPressed = input.left && !input.right;
        const bool rightPressed = input.right && !input.left;

        if (input.forward)
        {
            moveX += forwardX;
            moveZ += forwardZ;
            if (inWater_)
                moveY += swimForwardY;
        }
        if (input.backward)
        {
            moveX -= forwardX;
            moveZ -= forwardZ;
            if (inWater_)
                moveY -= swimForwardY;
        }
        if (input.right) { moveX += rightX; moveZ += rightZ; }
        if (input.left) { moveX -= rightX; moveZ -= rightZ; }

        const float moveLength = inWater_
            ? std::sqrt(moveX * moveX + moveY * moveY + moveZ * moveZ)
            : std::sqrt(moveX * moveX + moveZ * moveZ);
        const bool moving = moveLength > 0.001f;

        // ---- Terrain + water ----
        float groundY = 0.0f;
        if (heightFn_)
            groundY = heightFn_(characterX_, characterZ_, heightUserData_);

        const bool waterAtPosition = groundY < kWaterSurface - 0.15f;
        if (waterAtPosition)
        {
            if (!inWater_)
                inWater_ = characterY_ <= kWaterSurface - kWaterEnterDepth;
        }
        else
        {
            inWater_ = false;
        }

        if (moving)
        {
            moveX /= moveLength;
            moveY = inWater_ ? moveY / moveLength : 0.0f;
            moveZ /= moveLength;
            const float speed = inWater_ ? kSwimSpeed
                : (input.fast ? kFastRunSpeed : kRunSpeed);
            characterX_ += moveX * speed * clampedDelta;
            if (inWater_)
                characterY_ += moveY * speed * clampedDelta;
            characterZ_ += moveZ * speed * clampedDelta;
            const bool strafingOrBacking = backwardPressed || ((leftPressed || rightPressed) && !forwardPressed);
            characterYaw_ = strafingOrBacking ? cameraYaw_ : std::atan2(moveX, moveZ);
        }

        // ---- Collision with world objects ----
        if (collisionFn_ && moving)
        {
            float adjustedX = characterX_;
            float adjustedZ = characterZ_;
            const float speed = inWater_ ? kSwimSpeed : (input.fast ? kFastRunSpeed : kRunSpeed);
            if (collisionFn_(characterX_, characterZ_,
                characterX_ - moveX * speed * clampedDelta,
                characterZ_ - moveZ * speed * clampedDelta,
                characterY_,
                adjustedX, adjustedZ, collisionUserData_))
            {
                characterX_ = adjustedX;
                characterZ_ = adjustedZ;
            }
        }

        // Resample terrain after collision adjustment.
        if (heightFn_)
            groundY = heightFn_(characterX_, characterZ_, heightUserData_);

        if (inWater_)
        {
            const float floorY = groundY + kGroundClearance;
            const float floatY = kWaterSurface - kFloatFeetDepth;
            const bool swimmingDown = moving && moveY < -0.08f;
            if (input.jump)
                characterY_ += (kBuoyancySpeed + 0.75f) * clampedDelta;
            else if (!swimmingDown)
                characterY_ += kBuoyancySpeed * clampedDelta;

            characterY_ = std::clamp(characterY_, floorY, floatY);
            verticalVelocity_ = 0.0f;
            grounded_ = true;
            groundInitialized_ = true;
        }
        else
        {
            const bool jumpPressed = input.jump && !jumpWasDown_;
            jumpWasDown_ = input.jump;
            if (jumpPressed && grounded_)
            {
                verticalVelocity_ = kJumpImpulse;
                grounded_ = false;
            }

            if (!grounded_)
            {
                verticalVelocity_ -= kGravity * clampedDelta;
                characterY_ += verticalVelocity_ * clampedDelta;
                if (characterY_ <= groundY)
                {
                    characterY_ = groundY;
                    verticalVelocity_ = 0.0f;
                    grounded_ = true;
                }
            }
            else
            {
                characterY_ = groundY;
                groundInitialized_ = true;
            }
        }

        // ---- Animation selection ----
        std::size_t desiredAnimation = data_.idleAnimation;
        if (inWater_)
        {
            desiredAnimation = moving ? data_.swimAnimation : data_.swimIdleAnimation;
        }
        else if (!grounded_)
            desiredAnimation = data_.jumpAnimation;
        else if (moving)
        {
            if (backwardPressed)
                desiredAnimation = data_.backAnimation;
            else if (leftPressed && !forwardPressed)
                desiredAnimation = data_.leftAnimation;
            else if (rightPressed && !forwardPressed)
                desiredAnimation = data_.rightAnimation;
            else
                desiredAnimation = input.fast ? data_.runAnimation : data_.walkAnimation;
        }
        if (desiredAnimation != activeAnimation_)
        {
            activeAnimation_ = desiredAnimation;
            animationSeconds_ = 0.0f;
        }
        float animationRate = 1.0f;
        if (inWater_)
            animationRate = moving ? 1.25f : 1.0f;
        else if (moving && grounded_)
            animationRate = input.fast ? 1.65f : 1.25f;
        animationSeconds_ += clampedDelta * animationRate;

        // ---- Skinning + world transform ----
        skin_and_transform();
    }

    void CharacterSystem::skin_and_transform()
    {
        const auto& anim = data_.animations[activeAnimation_].animation;
        if (!anim.parsed || anim.endKeyframe <= anim.startKeyframe)
        {
            worldVertices_ = data_.bindVertices;
            return;
        }

        const float startFrame = static_cast<float>(anim.startKeyframe);
        const float endFrame = static_cast<float>(anim.endKeyframe);
        const float frameCount = std::max(1.0f, endFrame - startFrame);
        const float frame = startFrame + std::fmod(animationSeconds_ * kAniFramesPerSecond, frameCount);
        const auto clientFinals = compute_client_finals(anim, frame);

        // Skin into local-space animated vertices.
        std::vector<CharacterGpuVertex> animated = data_.bindVertices;
        float minLocalY = std::numeric_limits<float>::max();

        for (std::size_t i = 0; i < data_.sourceVertices.size(); ++i)
        {
            const auto& source = data_.sourceVertices[i];
            Vec3 position{};
            Vec3 normal{};
            float totalWeight = 0.0f;

            for (std::size_t influence = 0; influence < 3; ++influence)
            {
                const auto boneIndex = static_cast<std::size_t>(source.bones[influence]);
                if (boneIndex >= source.meshBoneCount || boneIndex >= clientFinals.size())
                    continue;
                const float weight = std::max(0.0f, source.weights[influence]);
                if (weight <= 0.0001f)
                    continue;
                const auto meshBoneIdx = static_cast<std::size_t>(source.meshBoneBase) + boneIndex;
                if (meshBoneIdx >= data_.meshBones.size())
                    continue;

                const auto meshBone = mat4_from_shaiya_transposed(data_.meshBones[meshBoneIdx].matrix);
                const auto skinMatrix = mat4_multiply(meshBone, clientFinals[boneIndex]);
                const Vec3 srcPos{ source.position[0], source.position[1], source.position[2] };
                const Vec3 srcNrm{ source.normal[0], source.normal[1], source.normal[2] };
                const auto p = transform_point(skinMatrix, srcPos);
                const auto n = transform_normal(skinMatrix, srcNrm);
                position.x += p.x * weight;
                position.y += p.y * weight;
                position.z += p.z * weight;
                normal.x += n.x * weight;
                normal.y += n.y * weight;
                normal.z += n.z * weight;
                totalWeight += weight;
            }

            if (totalWeight <= 0.0001f)
                continue;

            position.x /= totalWeight;
            position.y /= totalWeight;
            position.z /= totalWeight;
            normal = normalize_vec3({ normal.x / totalWeight, normal.y / totalWeight, normal.z / totalWeight });

            animated[i].position[0] = position.x * kCharacterScale;
            animated[i].position[1] = position.y * kCharacterScale;
            animated[i].position[2] = position.z * kCharacterScale;
            animated[i].normal[0] = normal.x;
            animated[i].normal[1] = normal.y;
            animated[i].normal[2] = normal.z;

            minLocalY = std::min(minLocalY, animated[i].position[1]);
        }

        if (!std::isfinite(minLocalY))
            minLocalY = 0.0f;

        // World transform: rotate by yaw, offset Y to ground, translate to world position.
        const float yaw = characterYaw_ + kPi;
        const float cosYaw = std::cos(yaw);
        const float sinYaw = std::sin(yaw);

        for (std::size_t i = 0; i < animated.size(); ++i)
        {
            const float lx = animated[i].position[0];
            const float ly = animated[i].position[1];
            const float lz = animated[i].position[2];
            animated[i].position[0] = lx * cosYaw + lz * sinYaw + characterX_;
            animated[i].position[1] = ly - minLocalY + characterY_ + kGroundClearance;
            animated[i].position[2] = -lx * sinYaw + lz * cosYaw + characterZ_;

            const float nx = animated[i].normal[0];
            const float nz = animated[i].normal[2];
            animated[i].normal[0] = nx * cosYaw + nz * sinYaw;
            animated[i].normal[2] = -nx * sinYaw + nz * cosYaw;

            // Mark as character vertex (color=0 signals Shaiya lighting in shader).
            animated[i].color[0] = 0.0f;
            animated[i].color[1] = 0.0f;
            animated[i].color[2] = 0.0f;

            // Apply texture layer base offset.
            const auto rawLayer = data_.bindVertices[i].textureLayer;
            const bool isCutout = rawLayer >= 2048u;
            const auto baseLayer = isCutout ? (rawLayer - 2048u) : rawLayer;
            animated[i].textureLayer = (baseLayer + textureLayerBase_) + (isCutout ? 2048u : 0u);
        }

        worldVertices_ = std::move(animated);
    }

    void CharacterSystem::set_world_position(float x, float y, float z, float yaw)
    {
        characterX_ = x;
        characterY_ = y;
        characterZ_ = z;
        characterYaw_ = yaw;
        cameraYaw_ = yaw;
        verticalVelocity_ = 0.0f;
        grounded_ = true;
        groundInitialized_ = true;
        inWater_ = false;
        animationSeconds_ = 0.0f;
        jumpWasDown_ = false;
        if (data_.loaded)
        {
            activeAnimation_ = data_.idleAnimation;
            skin_and_transform();
        }
    }

    void CharacterSystem::camera_state(float& x, float& y, float& z, float& yaw, float& pitch) const
    {
        // Third-person orbit camera behind the character.
        const float lookTargetY = characterY_ + 1.25f;
        const float dirX = std::cos(cameraPitch_) * std::sin(cameraYaw_);
        const float dirY = std::sin(cameraPitch_);
        const float dirZ = std::cos(cameraPitch_) * std::cos(cameraYaw_);

        // Desired camera position.
        x = characterX_ - dirX * cameraDistance_;
        y = lookTargetY - dirY * cameraDistance_;
        z = characterZ_ - dirZ * cameraDistance_;

        yaw = cameraYaw_;
        pitch = cameraPitch_;

        // Hard clamp: camera must always be above terrain.
        if (heightFn_)
        {
            constexpr float kCameraClearance = 2.0f;
            const float terrainAtCam = heightFn_(x, z, heightUserData_);
            const float minY = terrainAtCam + kCameraClearance;
            if (y < minY)
                y = minY;

            // Also check midpoint between character and camera to catch ridges.
            const float midX = (characterX_ + x) * 0.5f;
            const float midZ = (characterZ_ + z) * 0.5f;
            const float midTerrainY = heightFn_(midX, midZ, heightUserData_) + kCameraClearance;
            const float midCamY = (lookTargetY + y) * 0.5f;
            if (midCamY < midTerrainY)
            {
                // Ridge between character and camera — push camera up more.
                const float lift = midTerrainY - midCamY;
                y += lift * 2.0f;
            }
        }
    }
}
