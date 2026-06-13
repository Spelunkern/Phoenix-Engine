#include "character/monster_manager.h"

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
        constexpr float kMonsterBaseScale = 0.95f;
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

    bool MonsterManager::load_catalog(const std::filesystem::path& dataRoot)
    {
        const auto root = resolve_ci(dataRoot / "monster");
        if (root.empty())
        {
            status_ = "monster data folder not found.";
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
            auto file = phoenix::assets::open_ifstream(root / "monster.csv");
            if (!file)
            {
                status_ = "monster.csv not found.";
                return false;
            }
            std::string line;
            std::getline(file, line);
            while (std::getline(file, line))
            {
                if (line.empty())
                    continue;
                const auto row = split_csv_line(line);
                if (row.size() < 16)
                    continue;
                VisualPartRow part{};
                part.modelIndex = parse_u32(row[0]);
                part.objectIndex = parse_u32(row[2]);
                part.meshName = row[4];
                part.textureName = row[5];
                part.walkAni = row[7];
                part.runAni = row[8];
                part.attack1Ani = row[9];
                part.attack2Ani = row[10];
                part.attack3Ani = row[11];
                part.deathAni = row[12];
                part.breathAni = row[13];
                part.damageAni = row[14];
                part.idleAni = row[15];
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
            auto file = phoenix::assets::open_ifstream(root / "monsterdata.csv");
            if (!file)
            {
                status_ = "monsterdata.csv not found.";
                return false;
            }
            std::string line;
            std::getline(file, line);
            while (std::getline(file, line))
            {
                if (line.empty())
                    continue;
                const auto row = split_csv_line(line);
                if (row.size() < 4)
                    continue;
                MonsterCatalogEntry entry{};
                entry.catalogIndex = catalog_.size();
                entry.monsterId = parse_u32(row[0]);
                entry.name = row[1];
                entry.modelIndex = parse_u32(row[2]);
                entry.size = parse_u32(row[3]);
                entry.label = entry.name + "  [id " + std::to_string(entry.monsterId)
                    + " / model " + std::to_string(entry.modelIndex)
                    + " / size " + std::to_string(entry.size) + "]";
                if (visualRows_.contains(entry.modelIndex))
                    catalog_.push_back(std::move(entry));
            }
        }

        status_ = catalog_.empty()
            ? "No monster entries with visual rows."
            : std::format("monster catalog: {} entries", catalog_.size());
        return !catalog_.empty();
    }

    bool MonsterManager::spawn(
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

        ActiveMonster monster{};
        monster.modelIndex = entry.modelIndex;
        // CSV `size` is a percentage anchored at 100 = default (x1.0); every
        // +100 adds 10% of the default size. So 200 -> x1.1, 800 -> x1.7,
        // 1000 -> x1.9. Floored so tiny/zero sizes never invert or vanish.
        monster.scale = std::max(0.1f, 1.0f + (static_cast<float>(entry.size) - 100.0f) * 0.001f);
        monster.x = x;
        monster.y = y - visual->localGroundY * monster.scale;
        monster.z = z;
        monster.yaw = yaw;
        monster.activeAnimation = visual->breathAnimation;
        monster.previousAnimation = visual->breathAnimation;
        monster.idleGesturePick = static_cast<std::uint32_t>(active_.size() * 13u + entry.modelIndex);
        active_.push_back(monster);
        activeLabel_ = entry.label;
        // The shared mesh only changes when a model appears for the first time;
        // spawning more of an existing model just adds an instance at draw time.
        if (!modelIndexOffset_.contains(entry.modelIndex))
            rebuild_render_mesh(renderer);
        status_ = std::format("monster active: {} ({} visible)", active_.size(), visibleCount_);
        return true;
    }

    void MonsterManager::clear(phoenix::renderer::VulkanRenderer& renderer)
    {
        active_.clear();
        activeLabel_.clear();
        renderVertices_.clear();
        renderIndices_.clear();
        instances_.clear();
        instanceBatches_.clear();
        visibleCount_ = 0;
        renderer.update_monster_character_instances(instances_, instanceBatches_);
        renderer.set_monster_character_visible(false);
    }

    MonsterManager::Visual* MonsterManager::load_visual(
        const std::filesystem::path& dataRoot,
        const MonsterCatalogEntry& entry,
        std::uint32_t textureBaseSlot,
        std::uint32_t textureSlotReserve,
        phoenix::renderer::VulkanRenderer& renderer)
    {
        if (auto it = visuals_.find(entry.modelIndex); it != visuals_.end())
            return &it->second;

        const auto rowsIt = visualRows_.find(entry.modelIndex);
        if (rowsIt == visualRows_.end() || rowsIt->second.empty())
        {
            status_ = "monster visual model not found.";
            return nullptr;
        }

        Visual next{};
        next.breathAnimation = kInvalidAnimation;
        next.idleAnimation = kInvalidAnimation;
        const auto root = resolve_ci(dataRoot / "monster");
        const auto meshRoot = resolve_ci(root / "3dc");
        const auto textureRoot = resolve_ci(root / "dds");
        const auto animationRoot = resolve_ci(root / "ani");
        if (meshRoot.empty() || textureRoot.empty() || animationRoot.empty())
        {
            status_ = "monster asset folders are incomplete.";
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
                    status_ = "monster texture reserve exhausted.";
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
                gv.position[0] = sv.position[0] * kMonsterBaseScale;
                gv.position[1] = sv.position[1] * kMonsterBaseScale;
                gv.position[2] = sv.position[2] * kMonsterBaseScale;
                gv.color[0] = gv.color[1] = gv.color[2] = 1.0f;
                gv.normal[0] = sv.normal[0];
                gv.normal[1] = sv.normal[1];
                gv.normal[2] = sv.normal[2];
                gv.uv[0] = sv.uv[0];
                gv.uv[1] = sv.uv[1];
                gv.textureLayer = textureBaseSlot + textureSlot + (alphaCutout ? 2048u : 0u);
                next.bindVertices.push_back(gv);

                // GPU-skinning bind vertex: UNSCALED bind pose (kMonsterBaseScale
                // is folded into the per-instance basis) + global bone indices.
                phoenix::renderer::SkinnedVertex skv{};
                skv.position[0] = sv.position[0];
                skv.position[1] = sv.position[1];
                skv.position[2] = sv.position[2];
                skv.color[0] = skv.color[1] = skv.color[2] = 1.0f;
                skv.normal[0] = sv.normal[0];
                skv.normal[1] = sv.normal[1];
                skv.normal[2] = sv.normal[2];
                skv.uv[0] = sv.uv[0];
                skv.uv[1] = sv.uv[1];
                skv.textureLayer = gv.textureLayer;
                for (int b = 0; b < 3; ++b)
                {
                    skv.bones[b] = meshBoneBase + static_cast<std::uint32_t>(sv.bones[b]);
                    skv.weights[b] = sv.weights[b];
                }
                next.skinnedBind.push_back(skv);

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
            status_ = "monster visual load failed: no mesh parts or animation.";
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
        status_ = std::format("monster visual cached: model {}, {} parts, {} textures used",
            entry.modelIndex, parsedParts, textureSlotByPath_.size());
        return &it->second;
    }

    void MonsterManager::rebuild_render_mesh(phoenix::renderer::VulkanRenderer& renderer)
    {
        // ONE shared bind mesh per unique model (not per entity). Every monster
        // of a model draws this geometry, instanced — so memory and draw calls
        // scale with the number of models, not the number of monsters.
        renderVertices_.clear();
        renderIndices_.clear();
        modelIndexOffset_.clear();
        instances_.clear();
        instanceBatches_.clear();
        visibleCount_ = 0;

        for (const auto& monster : active_)
        {
            if (modelIndexOffset_.contains(monster.modelIndex))
                continue;
            auto it = visuals_.find(monster.modelIndex);
            if (it == visuals_.end() || !it->second.ready)
                continue;
            const auto& visual = it->second;
            const auto vertexOffset = static_cast<std::uint32_t>(renderVertices_.size());
            const auto indexOffset = static_cast<std::uint32_t>(renderIndices_.size());
            renderVertices_.insert(renderVertices_.end(), visual.skinnedBind.begin(), visual.skinnedBind.end());
            for (const auto index : visual.indices)
                renderIndices_.push_back(vertexOffset + index);
            modelIndexOffset_[monster.modelIndex] = indexOffset;
        }

        if (renderVertices_.empty() || renderIndices_.empty())
        {
            renderer.update_monster_character_instances(instances_, instanceBatches_);
            renderer.set_monster_character_visible(false);
            return;
        }

        // Static bind mesh — uploaded once here; per frame only the palette +
        // instances change (the GPU skins the bind pose).
        if (!renderer.set_monster_skinned_mesh(renderVertices_, renderIndices_))
            status_ = "monster skinned mesh upload failed.";
    }

    void MonsterManager::update_animation(float deltaSeconds, ActiveMonster& monster, const Visual& visual)
    {
        if (visual.animations.empty())
            return;

        const bool hasSeparateIdle = visual.idleAnimation != kInvalidAnimation
            && visual.idleAnimation != visual.breathAnimation
            && visual.idleAnimation < visual.animations.size();
        if (!monster.playingIdle && hasSeparateIdle)
        {
            monster.idleGestureTimer += deltaSeconds;
            const float interval = 8.0f + static_cast<float>((monster.idleGesturePick * 37u) % 60u) * 0.1f;
            if (monster.idleGestureTimer >= interval)
            {
                monster.playingIdle = true;
                monster.idleGestureTimer = 0.0f;
                ++monster.idleGesturePick;
            }
        }

        const auto desiredAnimation = monster.playingIdle ? visual.idleAnimation : visual.breathAnimation;
        if (desiredAnimation != monster.activeAnimation && desiredAnimation < visual.animations.size())
        {
            const bool canBlend = monster.activeAnimation < visual.animations.size()
                && visual.animations[monster.activeAnimation].animation.parsed
                && visual.animations[desiredAnimation].animation.parsed;
            monster.previousAnimation = monster.activeAnimation;
            monster.previousAnimationSeconds = monster.animationSeconds;
            monster.previousAnimationHoldEnd = monster.pendingHoldPreviousAtEnd;
            monster.animationBlendSeconds = 0.0f;
            monster.animationBlendDuration = canBlend
                ? (monster.previousAnimationHoldEnd ? kOneShotBlendDuration : kAnimationBlendDuration)
                : 0.0f;
            monster.activeAnimation = desiredAnimation;
            monster.animationSeconds = 0.0f;
            monster.pendingHoldPreviousAtEnd = false;
        }

        monster.animationSeconds += deltaSeconds;
        if (monster.animationBlendDuration > 0.0f)
        {
            monster.previousAnimationSeconds += deltaSeconds;
            monster.animationBlendSeconds += deltaSeconds;
            if (monster.animationBlendSeconds >= monster.animationBlendDuration)
            {
                monster.animationBlendSeconds = 0.0f;
                monster.animationBlendDuration = 0.0f;
                monster.previousAnimation = monster.activeAnimation;
                monster.previousAnimationSeconds = monster.animationSeconds;
                monster.previousAnimationHoldEnd = false;
            }
        }

        if (monster.playingIdle && monster.activeAnimation == visual.idleAnimation
            && visual.idleAnimation < visual.animations.size())
        {
            const auto& anim = visual.animations[visual.idleAnimation].animation;
            if (monster.animationSeconds >= animation_duration(anim))
            {
                monster.playingIdle = false;
                monster.idleGestureTimer = 0.0f;
                monster.pendingHoldPreviousAtEnd = true;
            }
        }
    }

    void MonsterManager::append_palette(const ActiveMonster& monster, const Visual& visual)
    {
        // Append this entity's model-space bone palette (16 floats / bone, the 4
        // rows of meshBind * animatedFinal) to paletteFloats_. The GPU applies it
        // to the static bind mesh. Bones not referenced by any vertex stay
        // identity (harmless — no vertex indexes them).
        const std::size_t boneCount = visual.meshBones.size();
        const std::size_t baseFloat = paletteFloats_.size();
        paletteFloats_.resize(baseFloat + boneCount * 16);
        for (std::size_t b = 0; b < boneCount; ++b)
        {
            float* dst = &paletteFloats_[baseFloat + b * 16];
            for (int i = 0; i < 16; ++i) dst[i] = 0.0f;
            dst[0] = dst[5] = dst[10] = dst[15] = 1.0f;
        }
        if (!visual.ready || visual.animations.empty()
            || monster.activeAnimation >= visual.animations.size() || boneCount == 0)
            return;

        const auto& activeChoice = visual.animations[monster.activeAnimation];
        static thread_local std::vector<Mat4> clientFinals;
        compute_client_finals(
            activeChoice.animation,
            activeChoice.bindLocals,
            sample_animation_frame(activeChoice.animation, monster.animationSeconds),
            clientFinals);

        if (monster.animationBlendDuration > 0.0f
            && monster.previousAnimation < visual.animations.size()
            && visual.animations[monster.previousAnimation].animation.parsed
            && visual.animations[monster.previousAnimation].animation.bones.size() == clientFinals.size())
        {
            const auto& previousChoice = visual.animations[monster.previousAnimation];
            const auto& previousAnim = previousChoice.animation;
            const float previousFrame = monster.previousAnimationHoldEnd
                ? static_cast<float>(previousAnim.endKeyframe)
                : sample_animation_frame(previousAnim, monster.previousAnimationSeconds);
            static thread_local std::vector<Mat4> previousFinals;
            compute_client_finals(previousAnim, previousChoice.bindLocals, previousFrame, previousFinals);
            const float rawT = std::clamp(monster.animationBlendSeconds / monster.animationBlendDuration, 0.0f, 1.0f);
            const float t = rawT * rawT * (3.0f - 2.0f * rawT);
            for (std::size_t i = 0; i < clientFinals.size(); ++i)
                clientFinals[i] = blend_mat4(previousFinals[i], clientFinals[i], t);
        }

        // Fill each referenced mesh bone once (a vertex carries the local bone +
        // its part's meshBoneBase; the global index is the palette slot).
        static thread_local std::vector<std::uint32_t> stampTbl;
        static thread_local std::uint32_t stampCtr = 0;
        const std::uint32_t stamp = ++stampCtr;
        if (stampTbl.size() < boneCount)
            stampTbl.resize(boneCount, 0u);
        for (const auto& source : visual.sourceVertices)
        {
            for (int influence = 0; influence < 3; ++influence)
            {
                const auto boneIndex = static_cast<std::size_t>(source.bones[influence]);
                if (boneIndex >= source.meshBoneCount || boneIndex >= clientFinals.size())
                    continue;
                if (source.weights[influence] <= 0.0001f)
                    continue;
                const auto meshBoneIdx = static_cast<std::size_t>(source.meshBoneBase) + boneIndex;
                if (meshBoneIdx >= boneCount || stampTbl[meshBoneIdx] == stamp)
                    continue;
                stampTbl[meshBoneIdx] = stamp;
                const auto meshBone = mat4_from_shaiya_transposed(visual.meshBones[meshBoneIdx].matrix);
                const auto m = mat4_multiply(meshBone, clientFinals[boneIndex]);
                float* dst = &paletteFloats_[baseFloat + meshBoneIdx * 16];
                for (int r = 0; r < 4; ++r)
                {
                    dst[r * 4 + 0] = m.m[r][0];
                    dst[r * 4 + 1] = m.m[r][1];
                    dst[r * 4 + 2] = m.m[r][2];
                    dst[r * 4 + 3] = m.m[r][3];
                }
            }
        }
    }

    void MonsterManager::update(
        float deltaSeconds,
        const phoenix::renderer::CameraView& view,
        phoenix::renderer::VulkanRenderer& renderer,
        phoenix::app::LoadingScheduler* workerPool)
    {
        if (!active())
        {
            renderer.set_monster_character_visible(false);
            return;
        }

        (void)workerPool;
        ++frameCounter_;
        instances_.clear();
        instanceBatches_.clear();
        paletteFloats_.clear();
        visibleCount_ = 0;

        // GPU skinning: one pass — cull, advance animation, compute each unique
        // pose's bone palette (deduped: entities sharing model+animation+phase
        // reuse one palette), build the instance (placement transform; the
        // palette base bone index is bit-packed into right.w) + draw batches.
        // The bind mesh is static on the GPU; only the palette + instances are
        // uploaded per frame, and the GPU does all per-vertex skinning.
        struct PoseKey { std::uint32_t model; std::size_t anim; std::int32_t bucket; std::uint32_t baseBone; };
        static std::vector<PoseKey> poseKeys;
        poseKeys.clear();
        // Visible entities with their built instance, kept per-entity so they can
        // be grouped by model into contiguous instanced draws below.
        struct VisInst { std::uint32_t model; phoenix::renderer::ObjectInstance inst; };
        static std::vector<VisInst> vis;
        vis.clear();

        for (auto& monster : active_)
        {
            monster.visible = false;
            const auto it = visuals_.find(monster.modelIndex);
            if (it == visuals_.end() || !it->second.ready)
                continue;
            const auto& visual = it->second;
            if (!phoenix::renderer::sphere_visible(
                view,
                monster.x,
                monster.y + visual.boundsCenterY * monster.scale,
                monster.z,
                visual.boundsRadius * monster.scale))
                continue;

            update_animation(deltaSeconds, monster, visual);
            monster.visible = true;
            ++visibleCount_;

            // Palette dedup: reuse an identical pose's palette base; otherwise
            // append this entity's palette. Skipped while blending (transient).
            std::uint32_t baseBone = static_cast<std::uint32_t>(paletteFloats_.size() / 16u);
            bool reused = false;
            std::int32_t bucket = 0;
            const bool dedup = monster.animationBlendDuration <= 0.0f;
            if (dedup)
            {
                bucket = static_cast<std::int32_t>(std::lround(monster.animationSeconds * 60.0f));
                for (const auto& k : poseKeys)
                    if (k.model == monster.modelIndex && k.anim == monster.activeAnimation && k.bucket == bucket)
                    { baseBone = k.baseBone; reused = true; break; }
            }
            if (!reused)
            {
                append_palette(monster, visual);
                if (dedup)
                    poseKeys.push_back({ monster.modelIndex, monster.activeAnimation, bucket, baseBone });
            }

            // Placement transform; kMonsterBaseScale folded into the basis (the
            // GPU skins the UNSCALED bind pose). Palette base packed in right.w.
            const float S = monster.scale * kMonsterBaseScale;
            const float sn = std::sin(monster.yaw);
            const float cs = std::cos(monster.yaw);
            phoenix::renderer::ObjectInstance inst{};
            inst.right[0] = cs * S;
            inst.right[2] = -sn * S;
            inst.up[1] = S;
            inst.forward[0] = sn * S;
            inst.forward[2] = cs * S;
            inst.position[0] = monster.x;
            inst.position[1] = monster.y;
            inst.position[2] = monster.z;
            inst.right[3] = static_cast<float>(baseBone);  // palette base (exact int in float)
            vis.push_back({ monster.modelIndex, inst });
        }

        // Group visible entities by model into contiguous instance blocks, and
        // emit one instanced batch per (model, part): a single draw covers all
        // entities of a model, each reading its own transform + palette base.
        static std::vector<std::uint32_t> modelOrder;
        modelOrder.clear();
        for (const auto& v : vis)
            if (std::find(modelOrder.begin(), modelOrder.end(), v.model) == modelOrder.end())
                modelOrder.push_back(v.model);
        for (const std::uint32_t model : modelOrder)
        {
            const auto geomIt = modelIndexOffset_.find(model);
            const auto visualIt = visuals_.find(model);
            if (geomIt == modelIndexOffset_.end() || visualIt == visuals_.end())
                continue;
            const std::uint32_t indexOffset = geomIt->second;
            const auto firstInstance = static_cast<std::uint32_t>(instances_.size());
            for (const auto& v : vis)
                if (v.model == model)
                    instances_.push_back(v.inst);
            const auto count = static_cast<std::uint32_t>(instances_.size()) - firstInstance;
            for (auto batch : visualIt->second.batches)
            {
                batch.firstIndex += indexOffset;
                batch.firstInstance = firstInstance;
                batch.instanceCount = count;
                instanceBatches_.push_back(batch);
            }
        }

        if (!paletteFloats_.empty())
            renderer.update_monster_bone_palette(paletteFloats_.data(), paletteFloats_.size());
        renderer.update_monster_character_instances(instances_, instanceBatches_);
        if (visibleCount_ != lastStatusVisible_)
        {
            status_ = std::format("monster active: {} ({} visible)", active_.size(), visibleCount_);
            lastStatusVisible_ = visibleCount_;
        }
        renderer.set_monster_character_visible(visibleCount_ > 0);
    }
}
