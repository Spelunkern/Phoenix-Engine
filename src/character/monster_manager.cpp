#include "character/monster_manager.h"

#include "app/loading_scheduler.h"
#include "assets/data_index.h"
#include "world/svmap_loader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <random>
#include <unordered_map>

namespace phoenix::character
{
    namespace
    {
        constexpr float kMonsterBaseScale = 0.95f;
        // Animation-LOD distance tiers (squared, world units): near -> 60/s pose
        // rate; mid -> 30/s; far -> 15/s. Coarser rates let distant mobs share a
        // skinning job (imperceptible at distance).
        constexpr float kAnimLodNearSq = 25.0f * 25.0f;
        constexpr float kAnimLodMidSq = 55.0f * 55.0f;
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
        catalogByMobId_.clear();
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
                {
                    // svmap monster areas reference mobs by id; key the catalog by
                    // it so areas resolve. Keep the first entry per id.
                    catalogByMobId_.emplace(entry.monsterId, catalog_.size());
                    catalog_.push_back(std::move(entry));
                }
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
        phoenix::renderer::OpenGLRenderer& renderer)
    {
        if (!load_catalog(dataRoot) || catalogIndex >= catalog_.size())
            return false;

        const auto& entry = catalog_[catalogIndex];
        auto* visual = load_visual(dataRoot, entry, textureBaseSlot, textureSlotReserve, renderer);
        if (!visual)
            return false;

        ActiveMonster monster{};
        monster.modelIndex = entry.modelIndex;
        monster.catalogIndex = catalogIndex;
        // CSV `size` is a percentage anchored at 100 = default (x1.0), but the
        // curve is asymmetric:
        //   >= 100: gentle — every +100 adds 10% of default (800 -> x1.7, 1000 -> x1.9)
        //   <  100: steep  — scale = size/100 (90 -> x0.9, 50 -> x0.5, 10 -> x0.1)
        // Continuous at 100 (both give x1.0). Floored so tiny sizes don't vanish.
        const float sizePct = static_cast<float>(entry.size);
        const float sizeScale = (sizePct >= 100.0f)
            ? 1.0f + (sizePct - 100.0f) * 0.001f
            : sizePct / 100.0f;
        monster.scale = std::max(0.1f, sizeScale);
        monster.x = x;
        monster.y = y - visual->localGroundY * monster.scale;
        monster.z = z;
        monster.yaw = yaw;
        monster.activeAnimation = visual->breathAnimation;
        monster.previousAnimation = visual->breathAnimation;
        monster.idleGesturePick = static_cast<std::uint32_t>(active_.size() * 13u + entry.modelIndex);
        active_.push_back(monster);
        // The shared mesh only changes when a model appears for the first time;
        // spawning more of an existing model just adds an instance at draw time.
        if (!modelIndexOffset_.contains(entry.modelIndex))
            rebuild_render_mesh(renderer);
        status_ = std::format("monster active: {} ({} visible)", active_.size(), visibleCount_);
        return true;
    }

    void MonsterManager::clear(phoenix::renderer::OpenGLRenderer& renderer)
    {
        placements_.clear();
        placements_.shrink_to_fit();
        streamedPlacements_ = 0;
        visualLoads_.clear();
        failedModels_.clear();
        active_.clear();
        active_.shrink_to_fit();
        labels_.clear();
        labels_.shrink_to_fit();
        renderVertices_.clear();
        renderVertices_.shrink_to_fit();
        renderIndices_.clear();
        renderIndices_.shrink_to_fit();
        instances_.clear();
        instances_.shrink_to_fit();
        instanceBatches_.clear();
        instanceBatches_.shrink_to_fit();
        visibleCount_ = 0;
        renderer.update_monster_character_instances(instances_, instanceBatches_);
        renderer.set_monster_character_visible(false);
    }

    bool MonsterManager::plan_visual(
        const std::filesystem::path& dataRoot,
        const MonsterCatalogEntry& entry,
        std::uint32_t textureBaseSlot,
        std::uint32_t textureSlotReserve,
        VisualLoadPlan& out)
    {
        const auto rowsIt = visualRows_.find(entry.modelIndex);
        if (rowsIt == visualRows_.end() || rowsIt->second.empty())
        {
            status_ = "monster visual model not found.";
            return false;
        }
        const auto root = resolve_ci(dataRoot / "monster");
        const auto meshRoot = resolve_ci(root / "3dc");
        const auto textureRoot = resolve_ci(root / "dds");
        const auto animationRoot = resolve_ci(root / "ani");
        if (meshRoot.empty() || textureRoot.empty() || animationRoot.empty())
        {
            status_ = "monster asset folders are incomplete.";
            return false;
        }

        out = {};
        out.modelIndex = entry.modelIndex;
        out.textureBaseSlot = textureBaseSlot;
        out.animationRoot = animationRoot;
        // Slot assignment (shared maps) stays on the main thread; the worker parse
        // only reads the resulting plan.
        for (const auto& part : rowsIt->second)
        {
            VisualPartLoad load{};
            load.meshPath = resolve_ci(meshRoot / part.meshName);
            load.texturePath = resolve_ci(textureRoot / part.textureName);
            if (load.meshPath.empty() || load.texturePath.empty())
                continue;
            const auto textureKey = phoenix::assets::lower_ascii(load.texturePath.generic_string());
            if (const auto it = textureSlotByPath_.find(textureKey); it != textureSlotByPath_.end())
            {
                load.textureSlot = it->second;
                load.decodeTexture = false;
            }
            else
            {
                // Reuse a slot freed by eviction before growing the watermark.
                if (!freeTextureSlots_.empty())
                {
                    load.textureSlot = freeTextureSlots_.back();
                    freeTextureSlots_.pop_back();
                }
                else if (nextTextureSlot_ < textureSlotReserve)
                {
                    load.textureSlot = nextTextureSlot_++;
                }
                else
                {
                    status_ = "monster texture reserve exhausted.";
                    return false;
                }
                textureSlotByPath_.emplace(textureKey, load.textureSlot);
                textureKeyBySlot_[load.textureSlot] = textureKey;
                load.decodeTexture = true;
            }
            out.parts.push_back(std::move(load));
        }
        if (out.parts.empty())
        {
            status_ = "monster visual model not found.";
            return false;
        }
        const auto& firstPart = rowsIt->second.front();
        out.walkAni = firstPart.walkAni;
        out.runAni = firstPart.runAni;
        out.breathAni = firstPart.breathAni;
        out.idleAni = firstPart.idleAni;
        out.attack1Ani = firstPart.attack1Ani;
        return true;
    }

    MonsterManager::Visual* MonsterManager::finalize_visual(
        std::uint32_t modelIndex,
        std::shared_ptr<Visual> visual,
        std::uint32_t textureBaseSlot,
        phoenix::renderer::OpenGLRenderer& renderer)
    {
        for (auto& [slot, texture] : visual->pendingTextureUploads)
        {
            std::vector<phoenix::renderer::DdsTexture> single{ std::move(texture) };
            renderer.upload_terrain_texture_layers(textureBaseSlot + slot, single);
        }
        visual->pendingTextureUploads.clear();
        visual->ready = true;
        for (const auto slot : visual->textureSlots)
            ++slotRefcount_[slot];
        auto [it, inserted] = visuals_.emplace(modelIndex, std::move(*visual));
        return &it->second;
    }

    void MonsterManager::evict_visual(std::uint32_t modelIndex)
    {
        const auto it = visuals_.find(modelIndex);
        if (it == visuals_.end())
            return;
        for (const auto slot : it->second.textureSlots)
        {
            const auto refIt = slotRefcount_.find(slot);
            if (refIt == slotRefcount_.end())
                continue;
            if (--refIt->second == 0)
            {
                slotRefcount_.erase(refIt);
                freeTextureSlots_.push_back(slot);
                if (const auto keyIt = textureKeyBySlot_.find(slot); keyIt != textureKeyBySlot_.end())
                {
                    textureSlotByPath_.erase(keyIt->second);
                    textureKeyBySlot_.erase(keyIt);
                }
            }
        }
        visuals_.erase(it);
        modelIndexOffset_.erase(modelIndex);
        modelLastUsedFrame_.erase(modelIndex);
    }

    void MonsterManager::evict_visuals_if_needed(phoenix::renderer::OpenGLRenderer& renderer)
    {
        if (mapTextureReserve_ == 0 || visuals_.empty())
            return;
        const auto usedSlots = [&]() -> std::uint32_t {
            return nextTextureSlot_ - static_cast<std::uint32_t>(freeTextureSlots_.size());
        };
        const std::uint32_t highWater = mapTextureReserve_ * 3u / 4u;
        if (usedSlots() <= highWater)
            return;
        const std::uint32_t lowWater = mapTextureReserve_ / 2u;

        std::unordered_set<std::uint32_t> activeModels;
        activeModels.reserve(active_.size());
        for (const auto& monster : active_)
            activeModels.insert(monster.modelIndex);

        std::vector<std::pair<std::uint32_t, std::uint32_t>> candidates;   // (lastUsedFrame, model)
        for (const auto& [model, visual] : visuals_)
            if (!activeModels.contains(model))
            {
                const auto fit = modelLastUsedFrame_.find(model);
                candidates.emplace_back(fit != modelLastUsedFrame_.end() ? fit->second : 0u, model);
            }
        std::sort(candidates.begin(), candidates.end());

        bool evicted = false;
        for (const auto& [lastUsed, model] : candidates)
        {
            if (usedSlots() <= lowWater)
                break;
            (void)lastUsed;
            evict_visual(model);
            evicted = true;
        }
        if (evicted)
            rebuild_render_mesh(renderer);
    }

    MonsterManager::Visual* MonsterManager::load_visual(
        const std::filesystem::path& dataRoot,
        const MonsterCatalogEntry& entry,
        std::uint32_t textureBaseSlot,
        std::uint32_t textureSlotReserve,
        phoenix::renderer::OpenGLRenderer& renderer)
    {
        if (auto it = visuals_.find(entry.modelIndex); it != visuals_.end())
            return &it->second;
        VisualLoadPlan plan;
        if (!plan_visual(dataRoot, entry, textureBaseSlot, textureSlotReserve, plan))
            return nullptr;
        auto parsed = parse_visual(plan);
        if (!parsed)
        {
            status_ = "monster visual load failed: no mesh parts or animation.";
            return nullptr;
        }
        return finalize_visual(entry.modelIndex, std::move(parsed), textureBaseSlot, renderer);
    }

    std::shared_ptr<MonsterManager::Visual> MonsterManager::parse_visual(const VisualLoadPlan& plan)
    {
        auto visualPtr = std::make_shared<Visual>();
        Visual& next = *visualPtr;
        next.breathAnimation = kInvalidAnimation;
        next.idleAnimation = kInvalidAnimation;
        next.walkAnimation = kInvalidAnimation;
        next.runAnimation = kInvalidAnimation;

        std::size_t parsedParts = 0;
        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float minZ = std::numeric_limits<float>::max();
        float maxX = -std::numeric_limits<float>::max();
        float maxY = -std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();
        // Per-vertex skinning inputs, kept only long enough to build the static
        // skinning plan below — not stored in the cached Visual.
        std::vector<SourceVertex> sourceVertices;
        for (const auto& part : plan.parts)
        {
            auto model = phoenix::world::load_character_3dc(part.meshPath);
            if (!model.parsed || model.vertices.empty())
                continue;

            const std::uint32_t textureSlot = part.textureSlot;
            const bool alphaCutout = phoenix::renderer::dds_file_has_alpha_cutout(part.texturePath);
            if (part.decodeTexture)
                next.pendingTextureUploads.push_back(
                    { textureSlot, phoenix::renderer::load_dds(part.texturePath) });
            next.textureSlots.push_back(textureSlot);

            const auto baseVertex = static_cast<std::uint32_t>(next.skinnedBind.size());
            const auto meshBoneBase = static_cast<std::uint32_t>(next.meshBones.size());
            const auto meshBoneCount = static_cast<std::uint32_t>(model.bones.size());
            next.meshBones.insert(next.meshBones.end(), model.bones.begin(), model.bones.end());
            const std::uint32_t textureLayer = plan.textureBaseSlot + textureSlot + (alphaCutout ? 2048u : 0u);

            for (const auto& src : model.vertices)
            {
                SourceVertex sv{};
                sv.weights[0] = src.boneWeights[0];
                sv.weights[1] = src.boneWeights[1];
                sv.weights[2] = src.boneWeights[2];
                sv.bones[0] = src.boneIndices[0];
                sv.bones[1] = src.boneIndices[1];
                sv.bones[2] = src.boneIndices[2];
                sv.meshBoneBase = meshBoneBase;
                sv.meshBoneCount = meshBoneCount;
                sourceVertices.push_back(sv);

                // GPU-skinning bind vertex: UNSCALED bind pose (kMonsterBaseScale
                // is folded into the per-instance basis) + global bone indices.
                phoenix::renderer::SkinnedVertex skv{};
                skv.position[0] = src.position[0];
                skv.position[1] = src.position[1];
                skv.position[2] = src.position[2];
                skv.color[0] = skv.color[1] = skv.color[2] = 1.0f;
                skv.normal[0] = src.normal[0];
                skv.normal[1] = src.normal[1];
                skv.normal[2] = src.normal[2];
                skv.uv[0] = src.uv[0];
                skv.uv[1] = src.uv[1];
                skv.textureLayer = textureLayer;
                for (int b = 0; b < 3; ++b)
                {
                    skv.bones[b] = meshBoneBase + static_cast<std::uint32_t>(sv.bones[b]);
                    skv.weights[b] = sv.weights[b];
                }
                next.skinnedBind.push_back(skv);

                // Bounds use the scaled bind pose (kMonsterBaseScale in the basis).
                const float sx = src.position[0] * kMonsterBaseScale;
                const float sy = src.position[1] * kMonsterBaseScale;
                const float sz = src.position[2] * kMonsterBaseScale;
                minX = std::min(minX, sx);
                minY = std::min(minY, sy);
                minZ = std::min(minZ, sz);
                maxX = std::max(maxX, sx);
                maxY = std::max(maxY, sy);
                maxZ = std::max(maxZ, sz);
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
            const auto aniPath = resolve_ci(plan.animationRoot / name);
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

        next.breathAnimation = loadAnimation(plan.breathAni);
        next.idleAnimation = loadAnimation(plan.idleAni);
        // Walk + run drive wandering mobs.
        next.walkAnimation = loadAnimation(plan.walkAni);
        next.runAnimation = loadAnimation(plan.runAni);
        if (next.breathAnimation == kInvalidAnimation)
            next.breathAnimation = next.idleAnimation;
        if (next.idleAnimation == kInvalidAnimation)
            next.idleAnimation = next.breathAnimation;
        if (next.breathAnimation == kInvalidAnimation)
        {
            const std::string fallbackCandidates[] = {
                plan.walkAni,
                plan.runAni,
                plan.attack1Ani,
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
            return nullptr;

        // Precompute the static skinning plan (referenced bones + transposed bind
        // matrices) so per-frame skinning walks bones, not vertices.
        {
            const std::size_t boneCount = next.meshBones.size();
            std::vector<std::uint8_t> seen(boneCount, 0u);
            for (const auto& src : sourceVertices)
            {
                for (int influence = 0; influence < 3; ++influence)
                {
                    if (src.weights[influence] <= 0.0001f)
                        continue;
                    const auto local = static_cast<std::uint32_t>(src.bones[influence]);
                    if (local >= src.meshBoneCount)
                        continue;
                    const std::uint32_t global = src.meshBoneBase + local;
                    if (global >= boneCount || seen[global])
                        continue;
                    seen[global] = 1u;
                    next.paletteGlobalBone.push_back(global);
                    next.paletteLocalBone.push_back(local);
                    const auto bind = mat4_from_shaiya_transposed(next.meshBones[global].matrix);
                    const auto base = next.paletteMeshBind.size();
                    next.paletteMeshBind.resize(base + 16);
                    std::memcpy(&next.paletteMeshBind[base], &bind.m[0][0], 16 * sizeof(float));
                }
            }
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
            next.modelTopY = maxY;
        }
        next.ready = false;   // finalize_visual flips this after uploading textures
        return visualPtr;
    }

    bool MonsterManager::pump_visual_loads(phoenix::renderer::OpenGLRenderer& renderer)
    {
        for (auto it = visualLoads_.begin(); it != visualLoads_.end(); ++it)
        {
            if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue;
            const std::uint32_t model = it->first;
            std::shared_ptr<Visual> parsed = it->second.get();
            visualLoads_.erase(it);
            if (!parsed)
            {
                failedModels_.insert(model);
                status_ = "monster visual load failed: no mesh parts or animation.";
                return false;
            }
            if (!visuals_.contains(model))
                finalize_visual(model, std::move(parsed), mapTextureBaseSlot_, renderer);
            return true;
        }
        return false;
    }

    bool MonsterManager::load_map_monsters(
        const std::filesystem::path& dataRoot,
        const std::filesystem::path& svmapPath,
        float halfMap,
        std::uint32_t textureBaseSlot,
        std::uint32_t textureSlotReserve,
        phoenix::renderer::OpenGLRenderer& renderer)
    {
        placements_.clear();
        streamedPlacements_ = 0;
        mapDataRoot_ = dataRoot;
        mapTextureBaseSlot_ = textureBaseSlot;
        mapTextureReserve_ = textureSlotReserve;

        // The renderer recreates the whole texture array on each map load, wiping
        // the monster slots; drop cached visuals/slots so they re-upload fresh
        // (otherwise reused models render green). Cheap now that loads are async.
        visuals_.clear();
        visualLoads_.clear();
        failedModels_.clear();
        modelIndexOffset_.clear();
        textureSlotByPath_.clear();
        slotRefcount_.clear();
        textureKeyBySlot_.clear();
        freeTextureSlots_.clear();
        modelLastUsedFrame_.clear();
        nextTextureSlot_ = 0;

        if (!load_catalog(dataRoot))
            return false;

        const auto resolved = resolve_ci(svmapPath);
        if (resolved.empty())
        {
            status_ = "svmap not found for this map.";
            return false;
        }

        const auto data = phoenix::world::load_svmap_monsters(resolved);
        if (!data.parsed)
        {
            status_ = "svmap parse failed.";
            return false;
        }

        std::size_t unresolved = 0;
        for (const auto& area : data.areas)
        {
            // Raw map-space box -> engine/world space (matches WLD instances). The
            // box floor (min Y) is the spawn height; mobs keep it as they wander.
            const float minX = std::min(area.minX, area.maxX) - halfMap;
            const float maxX = std::max(area.minX, area.maxX) - halfMap;
            const float minZ = std::min(area.minZ, area.maxZ) - halfMap;
            const float maxZ = std::max(area.minZ, area.maxZ) - halfMap;
            const float groundY = std::min(area.minY, area.maxY);
            for (const auto& mob : area.mobs)
            {
                const auto it = catalogByMobId_.find(mob.mobId);
                if (it == catalogByMobId_.end())
                {
                    ++unresolved;
                    continue;
                }
                MapSpawn sp{};
                sp.catalogIndex = it->second;
                sp.modelIndex = catalog_[it->second].modelIndex;
                sp.minX = minX;
                sp.maxX = maxX;
                sp.minZ = minZ;
                sp.maxZ = maxZ;
                sp.groundY = groundY;
                sp.count = std::max<std::uint32_t>(1u, mob.count);
                placements_.push_back(sp);
            }
        }

        renderer.set_monster_character_visible(active() && visibleCount_ > 0);
        status_ = std::format("svmap mobs: {} groups placed{}", placements_.size(),
            unresolved ? std::format(", {} unresolved", unresolved) : std::string{});
        return !placements_.empty();
    }

    void MonsterManager::stream_map_monsters(
        const phoenix::renderer::CameraView& view,
        phoenix::renderer::OpenGLRenderer& renderer,
        phoenix::app::LoadingScheduler* workerPool)
    {
        pump_visual_loads(renderer);

        if (placements_.empty() || streamedPlacements_ >= placements_.size())
            return;

        constexpr float kStreamCenterY = 1.5f;

        bool newModel = false;
        for (auto& sp : placements_)
        {
            if (sp.spawned)
                continue;
            const float cx = (sp.minX + sp.maxX) * 0.5f;
            const float cz = (sp.minZ + sp.maxZ) * 0.5f;
            const float halfX = (sp.maxX - sp.minX) * 0.5f;
            const float halfZ = (sp.maxZ - sp.minZ) * 0.5f;
            const float areaRadius = std::sqrt(halfX * halfX + halfZ * halfZ) + 3.0f;
            if (!phoenix::renderer::sphere_visible(view, cx, sp.groundY + kStreamCenterY, cz, areaRadius))
                continue;

            const auto& entry = catalog_[sp.catalogIndex];
            const std::uint32_t model = entry.modelIndex;

            if (failedModels_.contains(model))
            {
                sp.spawned = true;
                ++streamedPlacements_;
                continue;
            }

            auto visualIt = visuals_.find(model);
            if (visualIt == visuals_.end())
            {
                if (!visualLoads_.contains(model))
                {
                    VisualLoadPlan plan;
                    if (!plan_visual(mapDataRoot_, entry, mapTextureBaseSlot_, mapTextureReserve_, plan))
                    {
                        failedModels_.insert(model);
                        sp.spawned = true;
                        ++streamedPlacements_;
                        continue;
                    }
                    if (workerPool)
                    {
                        visualLoads_.emplace(model,
                            workerPool->submit([plan = std::move(plan)]() { return parse_visual(plan); }));
                    }
                    else
                    {
                        auto parsed = parse_visual(plan);
                        if (!parsed)
                        {
                            failedModels_.insert(model);
                            sp.spawned = true;
                            ++streamedPlacements_;
                            continue;
                        }
                        finalize_visual(model, std::move(parsed), mapTextureBaseSlot_, renderer);
                        visualIt = visuals_.find(model);
                    }
                }
                if (visualIt == visuals_.end())
                    continue;   // still loading on the worker — spawn a later frame
            }

            Visual* visual = &visualIt->second;
            sp.spawned = true;
            ++streamedPlacements_;

            // size% -> scale (same curve as the manual spawn path).
            const float sizePct = static_cast<float>(entry.size);
            const float sizeScale = (sizePct >= 100.0f)
                ? 1.0f + (sizePct - 100.0f) * 0.001f
                : sizePct / 100.0f;
            const float scale = std::max(0.1f, sizeScale);

            const bool canWander = visual->walkAnimation != kInvalidAnimation
                && visual->walkAnimation < visual->animations.size();
            const bool firstOfModel = !modelIndexOffset_.contains(model);

            std::uniform_real_distribution<float> ux(sp.minX, sp.maxX);
            std::uniform_real_distribution<float> uz(sp.minZ, sp.maxZ);
            std::uniform_real_distribution<float> uyaw(0.0f, 6.2831853f);
            for (std::uint32_t n = 0; n < sp.count; ++n)
            {
                ActiveMonster monster{};
                monster.modelIndex = model;
                monster.catalogIndex = sp.catalogIndex;
                monster.placementIndex = static_cast<std::size_t>(&sp - placements_.data());
                monster.fromMap = true;
                monster.scale = scale;
                monster.x = ux(wanderRng_);
                monster.z = uz(wanderRng_);
                // Follow the terrain/floor at the spawn point (fall back to the
                // svmap area Y if no sampler is set).
                const float groundY = heightSampler_
                    ? heightSampler_(monster.x, monster.z, heightSamplerCtx_)
                    : sp.groundY;
                monster.y = groundY - visual->localGroundY * scale;
                monster.yaw = uyaw(wanderRng_);
                monster.activeAnimation = visual->breathAnimation;
                monster.previousAnimation = visual->breathAnimation;
                monster.idleGesturePick = static_cast<std::uint32_t>(active_.size() * 13u + model);
                if (canWander)
                {
                    monster.wander = true;
                    monster.areaMinX = sp.minX;
                    monster.areaMaxX = sp.maxX;
                    monster.areaMinZ = sp.minZ;
                    monster.areaMaxZ = sp.maxZ;
                    monster.wanderWaitInterval =
                        std::uniform_real_distribution<float>(5.0f, 14.0f)(wanderRng_);
                    monster.wanderWaitTimer = std::uniform_real_distribution<float>(
                        0.0f, monster.wanderWaitInterval)(wanderRng_);
                }
                active_.push_back(monster);
            }
            if (firstOfModel)
                newModel = true;
        }

        if (newModel)
            rebuild_render_mesh(renderer);
    }

    void MonsterManager::update_wander(float deltaSeconds, ActiveMonster& monster, const Visual& visual)
    {
        (void)visual;
        if (!monster.wander)
            return;

        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kWalkSpeed = 2.0f;
        constexpr float kRunSpeed = 5.0f;
        constexpr float kRunChance = 0.12f;     // very occasionally run instead of walk
        constexpr float kRunDistance = 6.0f;    // a short dash ("un poco")

        if (!monster.wanderMoving)
        {
            monster.wanderWaitTimer += deltaSeconds;
            if (monster.wanderWaitTimer < monster.wanderWaitInterval)
                return;
            monster.wanderWaitTimer = 0.0f;
            monster.wanderMoving = true;
            monster.wanderRunning =
                std::uniform_real_distribution<float>(0.0f, 1.0f)(wanderRng_) < kRunChance;
            if (monster.wanderRunning)
            {
                // Short dash in a random direction, clamped to the spawn area.
                const float ang = std::uniform_real_distribution<float>(0.0f, 6.2831853f)(wanderRng_);
                monster.targetX = std::clamp(monster.x + std::sin(ang) * kRunDistance,
                    monster.areaMinX, monster.areaMaxX);
                monster.targetZ = std::clamp(monster.z + std::cos(ang) * kRunDistance,
                    monster.areaMinZ, monster.areaMaxZ);
            }
            else
            {
                monster.targetX = std::uniform_real_distribution<float>(
                    monster.areaMinX, monster.areaMaxX)(wanderRng_);
                monster.targetZ = std::uniform_real_distribution<float>(
                    monster.areaMinZ, monster.areaMaxZ)(wanderRng_);
            }
            return;
        }

        const float dx = monster.targetX - monster.x;
        const float dz = monster.targetZ - monster.z;
        const float dist = std::sqrt(dx * dx + dz * dz);
        const float step = (monster.wanderRunning ? kRunSpeed : kWalkSpeed) * deltaSeconds;
        if (dist <= step || dist < 1e-4f)
        {
            monster.x = monster.targetX;
            monster.z = monster.targetZ;
            monster.wanderMoving = false;
            monster.wanderRunning = false;
            monster.wanderWaitInterval =
                std::uniform_real_distribution<float>(5.0f, 14.0f)(wanderRng_);
        }
        else
        {
            const float inv = 1.0f / dist;
            monster.x += dx * inv * step;
            monster.z += dz * inv * step;
            monster.yaw = std::atan2(dx, dz) + kPi;
        }

        // Keep feet on the terrain/floor at the new position.
        if (heightSampler_)
            monster.y = heightSampler_(monster.x, monster.z, heightSamplerCtx_)
                - visual.localGroundY * monster.scale;
    }

    void MonsterManager::clear_manual(phoenix::renderer::OpenGLRenderer& renderer)
    {
        const auto before = active_.size();
        active_.erase(std::remove_if(active_.begin(), active_.end(),
            [](const ActiveMonster& m) { return !m.fromMap; }), active_.end());
        if (active_.size() == before)
            return;

        labels_.clear();
        visibleCount_ = 0;
        rebuild_render_mesh(renderer);
        renderer.update_monster_character_instances(instances_, instanceBatches_);
        renderer.set_monster_character_visible(false);
    }

    void MonsterManager::despawn_distant(const phoenix::renderer::CameraView& view)
    {
        if (active_.empty())
            return;
        // Despawn by spawn-area: all mobs of an area go together (so the area can
        // re-stream as a unit). Hysteresis past view.distance avoids thrashing; a
        // hard budget caps the number of active areas.
        constexpr std::size_t kMaxAreas = 96;
        const float despawn = view.distance * 1.4f;
        float despawnSq = despawn * despawn;

        const auto centerDistSq = [&](const MapSpawn& sp) {
            const float cx = (sp.minX + sp.maxX) * 0.5f - view.x;
            const float cy = sp.groundY - view.y;
            const float cz = (sp.minZ + sp.maxZ) * 0.5f - view.z;
            return cx * cx + cy * cy + cz * cz;
        };
        const auto liveArea = [&](const MapSpawn& sp) {
            return sp.spawned && !failedModels_.contains(sp.modelIndex);
        };

        std::size_t areaCount = 0;
        for (const auto& sp : placements_)
            if (liveArea(sp))
                ++areaCount;
        if (areaCount > kMaxAreas)
        {
            static std::vector<float> dists;
            dists.clear();
            dists.reserve(areaCount);
            for (const auto& sp : placements_)
                if (liveArea(sp))
                    dists.push_back(centerDistSq(sp));
            std::nth_element(dists.begin(), dists.begin() + kMaxAreas, dists.end());
            despawnSq = std::min(despawnSq, dists[kMaxAreas]);
        }

        std::size_t despawnedAreas = 0;
        for (auto& sp : placements_)
        {
            if (!liveArea(sp))
                continue;
            if (centerDistSq(sp) > despawnSq)
            {
                sp.spawned = false;
                ++despawnedAreas;
            }
        }
        if (despawnedAreas == 0)
            return;

        active_.erase(std::remove_if(active_.begin(), active_.end(), [&](const ActiveMonster& m) {
            return m.fromMap && m.placementIndex < placements_.size()
                && !placements_[m.placementIndex].spawned;
        }), active_.end());
        streamedPlacements_ =
            (despawnedAreas <= streamedPlacements_) ? streamedPlacements_ - despawnedAreas : 0;
    }

    void MonsterManager::rebuild_render_mesh(phoenix::renderer::OpenGLRenderer& renderer)
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

        std::size_t desiredAnimation;
        if (monster.wander)
        {
            // Wandering mobs don't play idle gestures: walk/run while moving
            // (update_wander set the flags), breathe at rest.
            monster.playingIdle = false;
            if (monster.wanderMoving)
            {
                const bool canRun = monster.wanderRunning
                    && visual.runAnimation != kInvalidAnimation
                    && visual.runAnimation < visual.animations.size();
                const bool canWalk = visual.walkAnimation != kInvalidAnimation
                    && visual.walkAnimation < visual.animations.size();
                desiredAnimation = canRun ? visual.runAnimation
                    : (canWalk ? visual.walkAnimation : visual.breathAnimation);
            }
            else
            {
                desiredAnimation = visual.breathAnimation;
            }
        }
        else
        {
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
            desiredAnimation = monster.playingIdle ? visual.idleAnimation : visual.breathAnimation;
        }
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

    void MonsterManager::write_palette_at(const PaletteJob& job)
    {
        // Write this pose's model-space bone palette into paletteFloats_ at
        // job.baseBone (the vector is pre-sized; jobs own disjoint regions, so
        // this runs on worker threads — matrix scratch + stamp are thread-local).
        const Visual& visual = *job.visual;
        const std::size_t boneCount = visual.meshBones.size();
        const std::size_t baseFloat = static_cast<std::size_t>(job.baseBone) * 16u;
        for (std::size_t b = 0; b < boneCount; ++b)
        {
            float* dst = &paletteFloats_[baseFloat + b * 16];
            for (int i = 0; i < 16; ++i) dst[i] = 0.0f;
            dst[0] = dst[5] = dst[10] = dst[15] = 1.0f;
        }
        if (!visual.ready || visual.animations.empty()
            || job.activeAnimation >= visual.animations.size() || boneCount == 0)
            return;

        const auto& activeChoice = visual.animations[job.activeAnimation];
        static thread_local std::vector<Mat4> clientFinals;
        compute_client_finals(
            activeChoice.animation,
            activeChoice.bindLocals,
            sample_animation_frame(activeChoice.animation, job.animationSeconds),
            clientFinals);

        if (job.blend && job.animationBlendDuration > 0.0f
            && job.previousAnimation < visual.animations.size()
            && visual.animations[job.previousAnimation].animation.parsed
            && visual.animations[job.previousAnimation].animation.bones.size() == clientFinals.size())
        {
            const auto& previousChoice = visual.animations[job.previousAnimation];
            const auto& previousAnim = previousChoice.animation;
            const float previousFrame = job.previousAnimationHoldEnd
                ? static_cast<float>(previousAnim.endKeyframe)
                : sample_animation_frame(previousAnim, job.previousAnimationSeconds);
            static thread_local std::vector<Mat4> previousFinals;
            compute_client_finals(previousAnim, previousChoice.bindLocals, previousFrame, previousFinals);
            const float rawT = std::clamp(job.animationBlendSeconds / job.animationBlendDuration, 0.0f, 1.0f);
            const float t = rawT * rawT * (3.0f - 2.0f * rawT);
            for (std::size_t i = 0; i < clientFinals.size(); ++i)
                clientFinals[i] = blend_mat4(previousFinals[i], clientFinals[i], t);
        }

        // Walk only the referenced bones (precomputed): meshBind * animatedFinal
        // into each palette slot.
        const std::size_t referenced = visual.paletteGlobalBone.size();
        for (std::size_t e = 0; e < referenced; ++e)
        {
            const std::size_t localBone = visual.paletteLocalBone[e];
            if (localBone >= clientFinals.size())
                continue;
            const std::size_t meshBoneIdx = visual.paletteGlobalBone[e];
            Mat4 meshBone;
            std::memcpy(&meshBone.m[0][0], &visual.paletteMeshBind[e * 16], 16 * sizeof(float));
            const auto m = mat4_multiply(meshBone, clientFinals[localBone]);
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

    void MonsterManager::update(
        float deltaSeconds,
        const phoenix::renderer::CameraView& view,
        phoenix::renderer::OpenGLRenderer& renderer,
        phoenix::app::LoadingScheduler* workerPool)
    {
        // Stream in map (.svmap) mobs that entered camera range, loading visuals
        // asynchronously so a new mob type's first sighting doesn't hitch.
        stream_map_monsters(view, renderer, workerPool);
        // Free mobs whose spawn area moved out of range (bounds active count/CPU).
        despawn_distant(view);
        // Under texture-slot pressure, drop idle visuals to reclaim slots.
        evict_visuals_if_needed(renderer);

        if (!active())
        {
            renderer.set_monster_character_visible(false);
            return;
        }

        ++frameCounter_;
        instances_.clear();
        instanceBatches_.clear();
        paletteFloats_.clear();
        labels_.clear();
        visibleCount_ = 0;

        // GPU skinning: one pass — cull, advance animation, compute each unique
        // pose's bone palette (deduped: entities sharing model+animation+phase
        // reuse one palette), build the instance (placement transform; the
        // palette base bone index is bit-packed into right.w) + draw batches.
        // The bind mesh is static on the GPU; only the palette + instances are
        // uploaded per frame, and the GPU does all per-vertex skinning.
        struct PoseKey { std::uint32_t model; std::size_t anim; std::int32_t tier; std::int32_t bucket; std::uint32_t baseBone; };
        static std::vector<PoseKey> poseKeys;
        poseKeys.clear();
        // Visible entities with their built instance, kept per-entity so they can
        // be grouped by model into contiguous instanced draws below.
        struct VisInst { std::uint32_t model; phoenix::renderer::ObjectInstance inst; };
        static std::vector<VisInst> vis;
        vis.clear();
        // Pass 1 (serial): cull, advance animation, dedup poses; the heavy bone
        // skinning happens afterwards, in parallel.
        static std::vector<PaletteJob> jobs;
        jobs.clear();
        std::uint32_t runningBones = 0;

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

            update_wander(deltaSeconds, monster, visual);
            update_animation(deltaSeconds, monster, visual);
            monster.visible = true;
            ++visibleCount_;
            modelLastUsedFrame_[monster.modelIndex] = frameCounter_;   // LRU for eviction

            // Floating name label above the mob's head (scaled).
            if (monster.catalogIndex < catalog_.size())
            {
                const auto& entry = catalog_[monster.catalogIndex];
                labels_.push_back({ monster.x,
                    monster.y + visual.modelTopY * monster.scale, monster.z, &entry.name });
            }

            // Palette dedup: reuse an identical pose's palette base; otherwise
            // reserve a new region + job. Skipped while blending (transient).
            std::uint32_t baseBone = runningBones;
            bool reused = false;
            std::int32_t bucket = 0;
            std::int32_t tier = 0;
            const bool dedup = monster.animationBlendDuration <= 0.0f;
            if (dedup)
            {
                // Animation LOD: distant mobs quantize their pose to a coarser
                // rate so many more share one skinning job. tier keeps near/far
                // buckets from colliding.
                const float dx = monster.x - view.x, dy = monster.y - view.y, dz = monster.z - view.z;
                const float d2 = dx * dx + dy * dy + dz * dz;
                const float rate = d2 < kAnimLodNearSq ? 60.0f
                    : (d2 < kAnimLodMidSq ? 30.0f : 15.0f);
                tier = d2 < kAnimLodNearSq ? 0 : (d2 < kAnimLodMidSq ? 1 : 2);
                bucket = static_cast<std::int32_t>(std::lround(monster.animationSeconds * rate));
                for (const auto& k : poseKeys)
                    if (k.model == monster.modelIndex && k.anim == monster.activeAnimation
                        && k.tier == tier && k.bucket == bucket)
                    { baseBone = k.baseBone; reused = true; break; }
            }
            if (!reused)
            {
                PaletteJob job{};
                job.visual = &visual;
                job.baseBone = baseBone;
                job.activeAnimation = monster.activeAnimation;
                job.animationSeconds = monster.animationSeconds;
                job.blend = monster.animationBlendDuration > 0.0f;
                job.previousAnimation = monster.previousAnimation;
                job.previousAnimationSeconds = monster.previousAnimationSeconds;
                job.animationBlendSeconds = monster.animationBlendSeconds;
                job.animationBlendDuration = monster.animationBlendDuration;
                job.previousAnimationHoldEnd = monster.previousAnimationHoldEnd;
                jobs.push_back(job);
                runningBones += static_cast<std::uint32_t>(visual.meshBones.size());
                if (dedup)
                    poseKeys.push_back({ monster.modelIndex, monster.activeAnimation, tier, bucket, baseBone });
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

        // Pass 2: skin every unique pose into its reserved palette region. Serial
        // by default — waking the sleeping worker pool per frame costs ~1ms of
        // sync that dwarfs the (now cheap) skinning; only fan out for big crowds.
        paletteFloats_.resize(static_cast<std::size_t>(runningBones) * 16u);
        constexpr std::size_t kParallelSkinThreshold = 192;
        if (workerPool && jobs.size() >= kParallelSkinThreshold)
            phoenix::app::parallel_for_loading(*workerPool, jobs.size(),
                [&](std::size_t i) { write_palette_at(jobs[i]); });
        else
            for (const auto& job : jobs)
                write_palette_at(job);

        // Group visible entities by model into contiguous instance blocks, and
        // emit one instanced batch per (model, part): a single draw covers all
        // entities of a model, each reading its own transform + palette base.
        // Single pass: bucket by model while iterating vis once, instead of a
        // dedup scan followed by an O(models) rescan of the whole vis list.
        static std::vector<std::uint32_t> modelOrder;
        static std::unordered_map<std::uint32_t, std::vector<phoenix::renderer::ObjectInstance>> modelBuckets;
        modelOrder.clear();
        modelBuckets.clear();
        for (const auto& v : vis)
        {
            auto [it, inserted] = modelBuckets.try_emplace(v.model);
            if (inserted)
                modelOrder.push_back(v.model);
            it->second.push_back(v.inst);
        }
        for (const std::uint32_t model : modelOrder)
        {
            const auto geomIt = modelIndexOffset_.find(model);
            const auto visualIt = visuals_.find(model);
            if (geomIt == modelIndexOffset_.end() || visualIt == visuals_.end())
                continue;
            const std::uint32_t indexOffset = geomIt->second;
            const auto& bucket = modelBuckets[model];
            const auto firstInstance = static_cast<std::uint32_t>(instances_.size());
            instances_.insert(instances_.end(), bucket.begin(), bucket.end());
            const auto count = static_cast<std::uint32_t>(bucket.size());
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
