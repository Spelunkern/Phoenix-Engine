#include "character/npc_manager.h"

#include "app/loading_scheduler.h"
#include "assets/data_index.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace phoenix::character
{
    namespace
    {
        constexpr float kNpcScale = 0.95f;
        constexpr float kAniFramesPerSecond = 30.0f;
        constexpr float kAnimationBlendDuration = 0.12f;
        constexpr float kOneShotBlendDuration = 0.25f;
        constexpr std::size_t kInvalidAnimation = std::numeric_limits<std::size_t>::max();

        std::string trim(std::string value)
        {
            return phoenix::assets::trim_ascii(std::move(value));
        }

        std::filesystem::path resolve_ci(const std::filesystem::path& path)
        {
            return phoenix::assets::resolve_existing_path_case_insensitive(path);
        }

        std::vector<std::string> split_csv_line(const std::string& line)
        {
            std::vector<std::string> cells;
            std::string cell;
            bool quoted = false;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                const char c = line[i];
                if (c == '"')
                {
                    if (quoted && i + 1 < line.size() && line[i + 1] == '"')
                    {
                        cell.push_back('"');
                        ++i;
                    }
                    else
                    {
                        quoted = !quoted;
                    }
                }
                else if (c == ',' && !quoted)
                {
                    cells.push_back(trim(cell));
                    cell.clear();
                }
                else
                {
                    cell.push_back(c);
                }
            }
            cells.push_back(trim(cell));
            return cells;
        }

        std::uint32_t parse_u32(const std::string& value)
        {
            return static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        }

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
                return {};
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
                return normalize_quat({
                    a.x + (b.x - a.x) * t,
                    a.y + (b.y - a.y) * t,
                    a.z + (b.z - a.z) * t,
                    a.w + (b.w - a.w) * t,
                });
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
            const float xx = q.x * q.x;
            const float yy = q.y * q.y;
            const float zz = q.z * q.z;
            const float xy = q.x * q.y;
            const float xz = q.x * q.z;
            const float yz = q.y * q.z;
            const float wx = q.w * q.x;
            const float wy = q.w * q.y;
            const float wz = q.w * q.z;
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

        Mat4 blend_mat4(const Mat4& a, const Mat4& b, float t)
        {
            Mat4 r{};
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    r.m[i][j] = a.m[i][j] + (b.m[i][j] - a.m[i][j]) * t;
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

        Quat sample_rotation(const world::CharacterAnimationBone& bone, float frame)
        {
            if (bone.rotationFrames.empty())
                return {};
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

        // Bind-relative local matrices (one per bone, flattened to 16 floats).
        // These depend only on the static bind pose, so they are computed once
        // per animation at load and reused every frame — moving the per-bone
        // parent inverse (the old per-frame hot path) entirely off the render
        // loop.
        std::vector<float> compute_bind_locals_flat(const world::CharacterAnimation& animation)
        {
            const auto boneCount = animation.bones.size();
            std::vector<Mat4> rawMatrices(boneCount);
            for (std::size_t i = 0; i < boneCount; ++i)
                rawMatrices[i] = mat4_from_shaiya_transposed(animation.bones[i].matrix);

            std::vector<float> flat(boneCount * 16);
            for (std::size_t i = 0; i < boneCount; ++i)
            {
                Mat4 local = rawMatrices[i];
                const auto parent = animation.bones[i].parentBoneIndex;
                if (parent >= 0 && static_cast<std::size_t>(parent) < boneCount)
                    local = mat4_multiply(rawMatrices[i], mat4_inverse(rawMatrices[static_cast<std::size_t>(parent)]));
                std::memcpy(&flat[i * 16], &local.m[0][0], 16 * sizeof(float));
            }
            return flat;
        }

        // Fills `finals` (caller-owned, reused across calls to avoid per-frame
        // heap churn). The bone-local scratch is a reused thread-local too.
        void compute_client_finals(const world::CharacterAnimation& animation,
            const std::vector<float>& bindLocals, float frame, std::vector<Mat4>& finals)
        {
            const auto boneCount = animation.bones.size();
            static thread_local std::vector<Mat4> locals;
            locals.resize(boneCount);
            for (std::size_t i = 0; i < boneCount; ++i)
            {
                const auto& bone = animation.bones[i];
                Mat4 local = Mat4::identity();
                if ((i + 1) * 16 <= bindLocals.size())
                    std::memcpy(&local.m[0][0], &bindLocals[i * 16], 16 * sizeof(float));

                if (!bone.rotationFrames.empty() || !bone.translationFrames.empty())
                {
                    Vec3 translation = mat4_get_translation(local);
                    if (!bone.rotationFrames.empty())
                        local = mat4_from_rotation_translation(sample_rotation(bone, frame), {});
                    if (!bone.translationFrames.empty())
                        translation = sample_translation(bone, frame);
                    local.m[3][0] = translation.x;
                    local.m[3][1] = translation.y;
                    local.m[3][2] = translation.z;
                    local.m[3][3] = 1.0f;
                }
                locals[i] = local;
            }

            finals.resize(boneCount);
            for (std::size_t i = 0; i < boneCount; ++i)
            {
                auto matrix = locals[i];
                const auto parent = animation.bones[i].parentBoneIndex;
                if (parent >= 0 && static_cast<std::size_t>(parent) < i)
                    matrix = mat4_multiply(matrix, finals[static_cast<std::size_t>(parent)]);
                finals[i] = matrix;
            }
        }

        float sample_animation_frame(const world::CharacterAnimation& animation, float seconds)
        {
            const float startFrame = static_cast<float>(animation.startKeyframe);
            const float endFrame = static_cast<float>(animation.endKeyframe);
            const float frameCount = std::max(1.0f, endFrame - startFrame);
            return startFrame + std::fmod(seconds * kAniFramesPerSecond, frameCount);
        }

        float animation_duration(const world::CharacterAnimation& animation)
        {
            return std::max(0.033f,
                static_cast<float>(animation.endKeyframe - animation.startKeyframe) / kAniFramesPerSecond);
        }

        bool is_load_placeholder(const std::string& value)
        {
            const auto lower = phoenix::assets::lower_ascii(trim(value));
            return lower.empty() || lower == "load";
        }
    }

    bool NpcManager::load_catalog(const std::filesystem::path& dataRoot)
    {
        const auto root = resolve_ci(dataRoot / "npc");
        if (root.empty())
        {
            status_ = "NPC data folder not found.";
            return false;
        }
        if (catalogRoot_ == root && !catalog_.empty() && !visualRows_.empty())
            return true;

        catalog_.clear();
        visualRows_.clear();
        visuals_.clear();
        textureSlotByPath_.clear();
        catalogRoot_ = root;
        nextTextureSlot_ = 0;

        {
            auto file = phoenix::assets::open_ifstream(root / "npc.csv");
            if (!file)
            {
                status_ = "npc.csv not found.";
                return false;
            }
            std::string line;
            std::getline(file, line);
            while (std::getline(file, line))
            {
                if (line.empty())
                    continue;
                const auto row = split_csv_line(line);
                if (row.size() < 14)
                    continue;
                VisualPartRow part{};
                part.modelIndex = parse_u32(row[0]);
                part.objectIndex = parse_u32(row[2]);
                part.meshName = row[3];
                part.textureName = row[4];
                part.walkAni = row[5];
                part.runAni = row[6];
                part.attack1Ani = row[7];
                part.attack2Ani = row[8];
                part.attack3Ani = row[9];
                part.deathAni = row[10];
                part.breathAni = row[11];
                part.damageAni = row[12];
                part.idleAni = row[13];
                visualRows_[part.modelIndex].push_back(std::move(part));
            }
            for (auto& [_, rows] : visualRows_)
            {
                std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                    return a.objectIndex < b.objectIndex;
                });
            }
        }

        {
            auto file = phoenix::assets::open_ifstream(root / "npcdata.csv");
            if (!file)
            {
                status_ = "npcdata.csv not found.";
                return false;
            }
            std::string line;
            std::getline(file, line);
            while (std::getline(file, line))
            {
                if (line.empty())
                    continue;
                const auto row = split_csv_line(line);
                if (row.size() < 7)
                    continue;
                NpcCatalogEntry entry{};
                entry.catalogIndex = catalog_.size();
                entry.npcIndex = parse_u32(row[0]);
                entry.npcId = row[1];
                entry.npcType = parse_u32(row[2]);
                entry.npcTypeName = row[3];
                entry.modelIndex = parse_u32(row[5]);
                entry.name = row[6];
                entry.label = entry.name + "  [" + entry.npcId + " / model " + std::to_string(entry.modelIndex) + "]";
                if (visualRows_.contains(entry.modelIndex))
                    catalog_.push_back(std::move(entry));
            }
        }

        status_ = catalog_.empty()
            ? "No NPC entries with visual rows."
            : std::format("NPC catalog: {} entries", catalog_.size());
        return !catalog_.empty();
    }

    bool NpcManager::spawn(
        const std::filesystem::path& dataRoot,
        std::size_t catalogIndex,
        float x,
        float y,
        float z,
        float yaw,
        std::uint32_t textureBaseSlot,
        std::uint32_t textureSlotReserve,
        phoenix::renderer::VulkanRenderer& renderer)
    {
        if (!load_catalog(dataRoot) || catalogIndex >= catalog_.size())
            return false;

        const auto& entry = catalog_[catalogIndex];
        auto* visual = load_visual(dataRoot, entry, textureBaseSlot, textureSlotReserve, renderer);
        if (!visual)
            return false;

        ActiveNpc npc{};
        npc.modelIndex = entry.modelIndex;
        npc.x = x;
        npc.y = y - visual->localGroundY;
        npc.z = z;
        npc.yaw = yaw;
        npc.activeAnimation = visual->breathAnimation;
        npc.previousAnimation = visual->breathAnimation;
        npc.idleGesturePick = static_cast<std::uint32_t>(active_.size() * 13u + entry.modelIndex);
        active_.push_back(npc);
        activeLabel_ = entry.label;
        rebuild_render_mesh(renderer);
        status_ = std::format("NPC active: {} ({} visible)", active_.size(), visibleCount_);
        return true;
    }

    void NpcManager::clear(phoenix::renderer::VulkanRenderer& renderer)
    {
        active_.clear();
        activeLabel_.clear();
        renderVertices_.clear();
        renderIndices_.clear();
        instances_.clear();
        instanceBatches_.clear();
        visibleCount_ = 0;
        renderer.update_bot_character_instances(instances_, instanceBatches_);
        renderer.set_bot_character_visible(false);
    }

    NpcManager::Visual* NpcManager::load_visual(
        const std::filesystem::path& dataRoot,
        const NpcCatalogEntry& entry,
        std::uint32_t textureBaseSlot,
        std::uint32_t textureSlotReserve,
        phoenix::renderer::VulkanRenderer& renderer)
    {
        if (auto it = visuals_.find(entry.modelIndex); it != visuals_.end())
            return &it->second;

        const auto rowsIt = visualRows_.find(entry.modelIndex);
        if (rowsIt == visualRows_.end() || rowsIt->second.empty())
        {
            status_ = "NPC visual model not found.";
            return nullptr;
        }

        Visual next{};
        next.breathAnimation = kInvalidAnimation;
        next.idleAnimation = kInvalidAnimation;
        const auto root = resolve_ci(dataRoot / "npc");
        const auto meshRoot = resolve_ci(root / "3dc");
        const auto textureRoot = resolve_ci(root / "dds");
        const auto animationRoot = resolve_ci(root / "ani");
        if (meshRoot.empty() || textureRoot.empty() || animationRoot.empty())
        {
            status_ = "NPC asset folders are incomplete.";
            return nullptr;
        }

        std::vector<std::pair<std::uint32_t, phoenix::renderer::DdsTexture>> newTextures;
        std::size_t parsedParts = 0;
        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float minZ = std::numeric_limits<float>::max();
        float maxX = -std::numeric_limits<float>::max();
        float maxY = -std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();
        for (const auto& part : rowsIt->second)
        {
            const auto meshPath = resolve_ci(meshRoot / part.meshName);
            auto model = phoenix::world::load_character_3dc(meshPath);
            if (!model.parsed || model.vertices.empty())
                continue;

            const auto texturePath = resolve_ci(textureRoot / part.textureName);
            if (texturePath.empty())
                continue;

            const auto textureKey = phoenix::assets::lower_ascii(texturePath.generic_string());
            std::uint32_t textureSlot = 0;
            if (const auto it = textureSlotByPath_.find(textureKey); it != textureSlotByPath_.end())
            {
                textureSlot = it->second;
            }
            else
            {
                if (nextTextureSlot_ >= textureSlotReserve)
                {
                    status_ = "NPC texture reserve exhausted.";
                    return nullptr;
                }
                textureSlot = nextTextureSlot_++;
                textureSlotByPath_.emplace(textureKey, textureSlot);
                newTextures.push_back({ textureSlot, phoenix::renderer::load_dds(texturePath) });
            }
            next.texturePaths.push_back(texturePath);

            const auto baseVertex = static_cast<std::uint32_t>(next.bindVertices.size());
            const auto meshBoneBase = static_cast<std::uint32_t>(next.meshBones.size());
            const auto meshBoneCount = static_cast<std::uint32_t>(model.bones.size());
            next.meshBones.insert(next.meshBones.end(), model.bones.begin(), model.bones.end());
            const bool alphaCutout = phoenix::renderer::dds_file_has_alpha_cutout(texturePath);

            for (const auto& src : model.vertices)
            {
                SourceVertex sv{};
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
                sv.gpuIndex = static_cast<std::uint32_t>(next.bindVertices.size());
                next.sourceVertices.push_back(sv);

                phoenix::renderer::TerrainVertex gv{};
                gv.position[0] = sv.position[0] * kNpcScale;
                gv.position[1] = sv.position[1] * kNpcScale;
                gv.position[2] = sv.position[2] * kNpcScale;
                gv.color[0] = gv.color[1] = gv.color[2] = 1.0f;
                gv.normal[0] = sv.normal[0];
                gv.normal[1] = sv.normal[1];
                gv.normal[2] = sv.normal[2];
                gv.uv[0] = sv.uv[0];
                gv.uv[1] = sv.uv[1];
                gv.textureLayer = textureBaseSlot + textureSlot + (alphaCutout ? 2048u : 0u);
                next.bindVertices.push_back(gv);

                minX = std::min(minX, gv.position[0]);
                minY = std::min(minY, gv.position[1]);
                minZ = std::min(minZ, gv.position[2]);
                maxX = std::max(maxX, gv.position[0]);
                maxY = std::max(maxY, gv.position[1]);
                maxZ = std::max(maxZ, gv.position[2]);
            }

            phoenix::renderer::ObjectBatch batch{};
            batch.firstIndex = static_cast<std::uint32_t>(next.indices.size());
            batch.firstInstance = 0;
            batch.instanceCount = 1;
            for (const auto& face : model.faces)
            {
                next.indices.push_back(baseVertex + face.indices[0]);
                next.indices.push_back(baseVertex + face.indices[1]);
                next.indices.push_back(baseVertex + face.indices[2]);
            }
            batch.indexCount = static_cast<std::uint32_t>(next.indices.size()) - batch.firstIndex;
            if (batch.indexCount > 0)
                next.batches.push_back(batch);
            ++parsedParts;
        }

        const auto loadAnimation = [&](const std::string& name) -> std::size_t {
            if (is_load_placeholder(name))
                return kInvalidAnimation;
            const auto aniPath = resolve_ci(animationRoot / name);
            if (aniPath.empty())
                return kInvalidAnimation;
            const auto key = phoenix::assets::lower_ascii(aniPath.generic_string());
            for (std::size_t i = 0; i < next.animations.size(); ++i)
            {
                if (phoenix::assets::lower_ascii(next.animations[i].path.generic_string()) == key)
                    return i;
            }
            auto animation = phoenix::world::load_character_ani(aniPath);
            if (!animation.parsed)
                return kInvalidAnimation;
            AnimationChoice choice{};
            choice.name = name;
            choice.path = aniPath;
            choice.animation = std::move(animation);
            choice.bindLocals = compute_bind_locals_flat(choice.animation);
            next.animations.push_back(std::move(choice));
            return next.animations.size() - 1;
        };

        const auto& firstPart = rowsIt->second.front();
        next.breathAnimation = loadAnimation(firstPart.breathAni);
        next.idleAnimation = loadAnimation(firstPart.idleAni);
        if (next.breathAnimation == kInvalidAnimation)
            next.breathAnimation = next.idleAnimation;
        if (next.idleAnimation == kInvalidAnimation)
            next.idleAnimation = next.breathAnimation;
        if (next.breathAnimation == kInvalidAnimation)
        {
            const std::string fallbackCandidates[] = {
                firstPart.walkAni,
                firstPart.runAni,
                firstPart.attack1Ani,
            };
            for (const auto& name : fallbackCandidates)
            {
                next.breathAnimation = loadAnimation(name);
                if (next.breathAnimation != kInvalidAnimation)
                {
                    next.idleAnimation = next.breathAnimation;
                    break;
                }
            }
        }

        if (parsedParts == 0 || next.animations.empty() || next.breathAnimation == kInvalidAnimation)
        {
            status_ = "NPC visual load failed: no mesh parts or animation.";
            return nullptr;
        }

        for (const auto& [slot, texture] : newTextures)
        {
            std::vector<phoenix::renderer::DdsTexture> single{ texture };
            renderer.upload_terrain_texture_layers(textureBaseSlot + slot, single);
        }

        next.localGroundY = std::isfinite(minY) ? minY : 0.0f;
        if (std::isfinite(minY) && std::isfinite(maxY))
        {
            const float cx = (minX + maxX) * 0.5f;
            const float cy = (minY + maxY) * 0.5f;
            const float cz = (minZ + maxZ) * 0.5f;
            next.boundsCenterY = cy;
            const float ex = maxX - cx;
            const float ey = maxY - cy;
            const float ez = maxZ - cz;
            next.boundsRadius = std::max(0.5f, std::sqrt(ex * ex + ey * ey + ez * ez));
        }
        next.ready = true;

        auto [it, inserted] = visuals_.emplace(entry.modelIndex, std::move(next));
        status_ = std::format("NPC visual cached: model {}, {} parts, {} textures used",
            entry.modelIndex, parsedParts, textureSlotByPath_.size());
        return &it->second;
    }

    void NpcManager::rebuild_render_mesh(phoenix::renderer::VulkanRenderer& renderer)
    {
        renderVertices_.clear();
        renderIndices_.clear();
        instances_.clear();
        instanceBatches_.clear();
        visibleCount_ = 0;

        for (auto& npc : active_)
        {
            auto it = visuals_.find(npc.modelIndex);
            if (it == visuals_.end() || !it->second.ready)
                continue;
            const auto& visual = it->second;
            npc.vertexOffset = static_cast<std::uint32_t>(renderVertices_.size());
            npc.vertexCount = static_cast<std::uint32_t>(visual.bindVertices.size());
            npc.indexOffset = static_cast<std::uint32_t>(renderIndices_.size());
            renderVertices_.insert(renderVertices_.end(), visual.bindVertices.begin(), visual.bindVertices.end());
            for (const auto index : visual.indices)
                renderIndices_.push_back(npc.vertexOffset + index);
        }

        if (renderVertices_.empty() || renderIndices_.empty())
        {
            renderer.update_bot_character_instances(instances_, instanceBatches_);
            renderer.set_bot_character_visible(false);
            return;
        }

        if (!renderer.set_bot_character_mesh(renderVertices_, renderIndices_))
            status_ = "NPC renderer mesh upload failed.";
    }

    void NpcManager::update_animation(float deltaSeconds, ActiveNpc& npc, const Visual& visual)
    {
        if (visual.animations.empty())
            return;

        const bool hasSeparateIdle = visual.idleAnimation != kInvalidAnimation
            && visual.idleAnimation != visual.breathAnimation
            && visual.idleAnimation < visual.animations.size();
        if (!npc.playingIdle && hasSeparateIdle)
        {
            npc.idleGestureTimer += deltaSeconds;
            const float interval = 8.0f + static_cast<float>((npc.idleGesturePick * 37u) % 60u) * 0.1f;
            if (npc.idleGestureTimer >= interval)
            {
                npc.playingIdle = true;
                npc.idleGestureTimer = 0.0f;
                ++npc.idleGesturePick;
            }
        }

        const auto desiredAnimation = npc.playingIdle ? visual.idleAnimation : visual.breathAnimation;
        if (desiredAnimation != npc.activeAnimation && desiredAnimation < visual.animations.size())
        {
            const bool canBlend = npc.activeAnimation < visual.animations.size()
                && visual.animations[npc.activeAnimation].animation.parsed
                && visual.animations[desiredAnimation].animation.parsed;
            npc.previousAnimation = npc.activeAnimation;
            npc.previousAnimationSeconds = npc.animationSeconds;
            npc.previousAnimationHoldEnd = npc.pendingHoldPreviousAtEnd;
            npc.animationBlendSeconds = 0.0f;
            npc.animationBlendDuration = canBlend
                ? (npc.previousAnimationHoldEnd ? kOneShotBlendDuration : kAnimationBlendDuration)
                : 0.0f;
            npc.activeAnimation = desiredAnimation;
            npc.animationSeconds = 0.0f;
            npc.pendingHoldPreviousAtEnd = false;
        }

        npc.animationSeconds += deltaSeconds;
        if (npc.animationBlendDuration > 0.0f)
        {
            npc.previousAnimationSeconds += deltaSeconds;
            npc.animationBlendSeconds += deltaSeconds;
            if (npc.animationBlendSeconds >= npc.animationBlendDuration)
            {
                npc.animationBlendSeconds = 0.0f;
                npc.animationBlendDuration = 0.0f;
                npc.previousAnimation = npc.activeAnimation;
                npc.previousAnimationSeconds = npc.animationSeconds;
                npc.previousAnimationHoldEnd = false;
            }
        }

        if (npc.playingIdle && npc.activeAnimation == visual.idleAnimation
            && visual.idleAnimation < visual.animations.size())
        {
            const auto& anim = visual.animations[visual.idleAnimation].animation;
            if (npc.animationSeconds >= animation_duration(anim))
            {
                npc.playingIdle = false;
                npc.idleGestureTimer = 0.0f;
                npc.pendingHoldPreviousAtEnd = true;
            }
        }
    }

    void NpcManager::skin(const ActiveNpc& npc, const Visual& visual)
    {
        if (!visual.ready || visual.animations.empty()
            || npc.activeAnimation >= visual.animations.size()
            || npc.vertexCount == 0)
            return;

        const auto& activeChoice = visual.animations[npc.activeAnimation];
        static thread_local std::vector<Mat4> clientFinals;
        compute_client_finals(
            activeChoice.animation,
            activeChoice.bindLocals,
            sample_animation_frame(activeChoice.animation, npc.animationSeconds),
            clientFinals);

        if (npc.animationBlendDuration > 0.0f
            && npc.previousAnimation < visual.animations.size()
            && visual.animations[npc.previousAnimation].animation.parsed
            && visual.animations[npc.previousAnimation].animation.bones.size() == clientFinals.size())
        {
            const auto& previousChoice = visual.animations[npc.previousAnimation];
            const auto& previousAnim = previousChoice.animation;
            const float previousFrame = npc.previousAnimationHoldEnd
                ? static_cast<float>(previousAnim.endKeyframe)
                : sample_animation_frame(previousAnim, npc.previousAnimationSeconds);
            static thread_local std::vector<Mat4> previousFinals;
            compute_client_finals(previousAnim, previousChoice.bindLocals, previousFrame, previousFinals);
            const float rawT = std::clamp(npc.animationBlendSeconds / npc.animationBlendDuration, 0.0f, 1.0f);
            const float t = rawT * rawT * (3.0f - 2.0f * rawT);
            for (std::size_t i = 0; i < clientFinals.size(); ++i)
                clientFinals[i] = blend_mat4(previousFinals[i], clientFinals[i], t);
        }

        // Precompute each mesh bone's skin matrix (bind * animated final) once
        // per frame. The inner loop used to rebuild it for every vertex
        // influence — thousands of redundant matrix multiplies per entity when
        // only ~30 bones exist. A monotonic stamp marks which are valid this
        // call without clearing the whole table.
        static thread_local std::vector<Mat4> skinMatrices;
        static thread_local std::vector<std::uint32_t> skinStamp;
        static thread_local std::uint32_t skinStampCounter = 0;
        const std::uint32_t stamp = ++skinStampCounter;
        if (skinMatrices.size() < visual.meshBones.size())
            skinMatrices.resize(visual.meshBones.size());
        if (skinStamp.size() < visual.meshBones.size())
            skinStamp.resize(visual.meshBones.size(), 0u);

        for (const auto& source : visual.sourceVertices)
        {
            const auto localIndex = static_cast<std::size_t>(source.gpuIndex);
            const auto vi = static_cast<std::size_t>(npc.vertexOffset) + localIndex;
            if (localIndex >= visual.bindVertices.size() || vi >= renderVertices_.size())
                continue;
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
                if (meshBoneIdx >= visual.meshBones.size())
                    continue;
                if (skinStamp[meshBoneIdx] != stamp)
                {
                    const auto meshBone = mat4_from_shaiya_transposed(visual.meshBones[meshBoneIdx].matrix);
                    skinMatrices[meshBoneIdx] = mat4_multiply(meshBone, clientFinals[boneIndex]);
                    skinStamp[meshBoneIdx] = stamp;
                }
                const auto& skinMatrix = skinMatrices[meshBoneIdx];
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
            const float invWeight = 1.0f / totalWeight;
            position.x *= invWeight;
            position.y *= invWeight;
            position.z *= invWeight;
            const auto n = normalize_vec3(normal);
            auto out = visual.bindVertices[localIndex];
            out.position[0] = position.x * kNpcScale;
            out.position[1] = position.y * kNpcScale;
            out.position[2] = position.z * kNpcScale;
            out.normal[0] = n.x;
            out.normal[1] = n.y;
            out.normal[2] = n.z;
            renderVertices_[vi] = out;
        }
    }

    void NpcManager::update(
        float deltaSeconds,
        const phoenix::renderer::CameraView& view,
        phoenix::renderer::VulkanRenderer& renderer,
        phoenix::app::LoadingScheduler* workerPool)
    {
        if (!active())
        {
            renderer.set_bot_character_visible(false);
            return;
        }

        ++frameCounter_;
        instances_.clear();
        instanceBatches_.clear();
        visibleCount_ = 0;

        // Single pass: cull, advance animation, build the instance + draw
        // batches, and dedup poses. Entities sharing model+animation+phase
        // produce identical MODEL-SPACE skinned vertices (per-entity scale/yaw/
        // position come from the instance transform, not baked into the mesh),
        // so a sharer's batches point straight at the first ("donor") entity's
        // geometry — no skin, no copy, no upload for it. This is the bots'
        // pose-sharing model: 50 identical NPCs cost ~1 skinning pass.
        struct SkinItem { ActiveNpc* npc; const Visual* visual; };
        static std::vector<SkinItem> donorSkins;
        donorSkins.clear();
        struct PoseKey { std::uint32_t model; std::size_t anim; std::int32_t bucket; std::uint32_t geomIndexOffset; };
        static std::vector<PoseKey> poseKeys;
        poseKeys.clear();

        std::uint32_t slot = 0;
        for (auto& npc : active_)
        {
            const std::uint32_t s = slot++;
            npc.visible = false;
            const auto it = visuals_.find(npc.modelIndex);
            if (it == visuals_.end() || !it->second.ready)
                continue;
            const auto& visual = it->second;
            // sphere_visible culls by both view.distance (UI "Cull dist", clamped
            // to fog) and the frustum.
            if (!phoenix::renderer::sphere_visible(
                view,
                npc.x,
                npc.y + visual.boundsCenterY,
                npc.z,
                visual.boundsRadius))
                continue;

            update_animation(deltaSeconds, npc, visual);
            npc.visible = true;
            ++visibleCount_;

            // Which index range's vertices this entity draws — its own by
            // default; a matching donor overrides it. Skipped while blending.
            std::uint32_t geomIndexOffset = npc.indexOffset;
            bool ownsGeometry = true;
            if (npc.animationBlendDuration <= 0.0f)
            {
                const std::int32_t bucket =
                    static_cast<std::int32_t>(std::lround(npc.animationSeconds * 60.0f));
                for (const auto& k : poseKeys)
                    if (k.model == npc.modelIndex && k.anim == npc.activeAnimation && k.bucket == bucket)
                    { geomIndexOffset = k.geomIndexOffset; ownsGeometry = false; break; }
                if (ownsGeometry)
                    poseKeys.push_back({ npc.modelIndex, npc.activeAnimation, bucket, npc.indexOffset });
            }

            const auto instanceIndex = static_cast<std::uint32_t>(instances_.size());
            const float sn = std::sin(npc.yaw);
            const float cs = std::cos(npc.yaw);
            phoenix::renderer::ObjectInstance inst{};
            inst.right[0] = cs;
            inst.right[2] = -sn;
            inst.up[1] = 1.0f;
            inst.forward[0] = sn;
            inst.forward[2] = cs;
            inst.position[0] = npc.x;
            inst.position[1] = npc.y;
            inst.position[2] = npc.z;
            instances_.push_back(inst);

            for (auto batch : visual.batches)
            {
                batch.firstIndex += geomIndexOffset;
                batch.firstInstance = instanceIndex;
                batch.instanceCount = 1;
                instanceBatches_.push_back(batch);
            }

            // Donors (unique pose) skin their own geometry; distant donors
            // re-skin every 2nd/3rd frame, but the first skin is never skipped.
            if (ownsGeometry)
            {
                std::uint32_t stride = 1;
                if (npc.everSkinned)
                {
                    const float ddx = npc.x - view.x;
                    const float ddz = npc.z - view.z;
                    const float distSq = ddx * ddx + ddz * ddz;
                    if (distSq > 55.0f * 55.0f) stride = 3;
                    else if (distSq > 25.0f * 25.0f) stride = 2;
                }
                if (!npc.everSkinned || ((frameCounter_ + s) % stride) == 0)
                {
                    npc.everSkinned = true;
                    donorSkins.push_back({ &npc, &visual });
                }
            }
        }

        // Skin donor poses in parallel (disjoint ranges + thread_local scratch).
        if (workerPool && donorSkins.size() >= 4)
        {
            phoenix::app::parallel_for_loading(*workerPool, donorSkins.size(), [&](std::size_t i) {
                skin(*donorSkins[i].npc, *donorSkins[i].visual);
            });
        }
        else
        {
            for (auto& d : donorSkins)
                skin(*d.npc, *d.visual);
        }

        // Upload only the donor ranges (sharers draw a donor's geometry).
        for (auto& d : donorSkins)
            renderer.update_bot_character_vertices_range(
                renderVertices_.data(),
                d.npc->vertexOffset,
                d.npc->vertexCount);

        renderer.update_bot_character_instances(instances_, instanceBatches_);
        if (visibleCount_ != lastStatusVisible_)
        {
            status_ = std::format("NPC active: {} ({} visible)", active_.size(), visibleCount_);
            lastStatusVisible_ = visibleCount_;
        }
        renderer.set_bot_character_visible(visibleCount_ > 0);
    }
}
