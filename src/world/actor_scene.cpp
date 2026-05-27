#include "world/actor_scene.h"

#include "world/character_loader.h"
#include "world/mon_loader.h"
#include "world/sdata_loader.h"
#include "world/svmap_loader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <span>
#include <string_view>

namespace phoenix::world
{
    namespace
    {
        constexpr float kActorMeshScale = 0.95f;
        constexpr float kNpcScale = 1.0f;
        constexpr float kCellSize = 512.0f;
        constexpr float kAniFramesPerSecond = 22.0f;

        float svmap_to_world(float value, float halfMap)
        {
            return value - halfMap;
        }

        std::string lower_ascii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        bool texture_uses_cutout(std::string_view name)
        {
            const auto lower = lower_ascii(std::string(name));
            return lower.find("hair") != std::string::npos
                || lower.find("face") != std::string::npos
                || lower.find("npc") != std::string::npos
                || lower.find("mon") != std::string::npos;
        }

        bool actor_asset_placeholder(const std::string& value)
        {
            return value.empty() || value == "LOAD" || value == "load";
        }

        std::filesystem::path resolve_actor_file(
            const phoenix::assets::DataIndex& assets,
            const std::filesystem::path& folder,
            std::string name)
        {
            if (name.empty())
                return {};
            auto path = assets.resolve(name);
            if (!path.empty())
                return path;
            path = assets.resolve((folder / name).generic_string());
            if (!path.empty())
                return path;
            return {};
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

        struct ModelBuild
        {
            std::uint32_t firstVertex{};
            std::uint32_t vertexCount{};
            std::uint32_t firstIndex{};
            std::uint32_t indexCount{};
            float radius{ 24.0f };
            float labelHeight{ 32.0f };
            ActorSkinData skinData;
            ActorAnimationSet animations;
            bool built{};
        };

        struct ResolvedActor
        {
            std::uint32_t modelId{};
            std::string label;
            float scale{ 1.0f };
        };

        void append_model_part(
            std::vector<phoenix::renderer::TerrainVertex>& vertices,
            std::vector<std::uint32_t>& indices,
            const CharacterModel& model,
            std::uint32_t textureLayer,
            float scale,
            ModelBuild& build)
        {
            const auto baseVertex = static_cast<std::uint32_t>(vertices.size());
            const auto meshBoneBase = static_cast<std::uint32_t>(build.skinData.meshBones.size());
            const auto meshBoneCount = static_cast<std::uint32_t>(model.bones.size());
            build.skinData.meshBones.insert(build.skinData.meshBones.end(), model.bones.begin(), model.bones.end());
            for (const auto& src : model.vertices)
            {
                phoenix::renderer::TerrainVertex v{};
                v.position[0] = src.position[0] * kActorMeshScale * scale;
                v.position[1] = src.position[1] * kActorMeshScale * scale;
                v.position[2] = src.position[2] * kActorMeshScale * scale;
                v.color[0] = 1.0f;
                v.color[1] = 1.0f;
                v.color[2] = 1.0f;
                v.normal[0] = src.normal[0];
                v.normal[1] = src.normal[1];
                v.normal[2] = src.normal[2];
                v.uv[0] = src.uv[0];
                v.uv[1] = src.uv[1];
                v.textureLayer = textureLayer;
                vertices.push_back(v);

                const auto len = std::sqrt(v.position[0] * v.position[0] + v.position[1] * v.position[1] + v.position[2] * v.position[2]);
                build.radius = std::max(build.radius, len);

                ActorSourceVertex source{};
                std::copy(std::begin(src.position), std::end(src.position), std::begin(source.position));
                std::copy(std::begin(src.normal), std::end(src.normal), std::begin(source.normal));
                std::copy(std::begin(src.boneWeights), std::end(src.boneWeights), std::begin(source.weights));
                std::copy(std::begin(src.boneIndices), std::end(src.boneIndices), std::begin(source.bones));
                source.meshBoneBase = meshBoneBase;
                source.meshBoneCount = meshBoneCount;
                source.outputScale = kActorMeshScale * scale;
                build.skinData.sourceVertices.push_back(source);
            }

            for (const auto& face : model.faces)
            {
                indices.push_back(baseVertex + face.indices[0]);
                indices.push_back(baseVertex + face.indices[1]);
                indices.push_back(baseVertex + face.indices[2]);
            }
        }

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
            d[5] = s[0]*s[10]*s[15] - s[0]*s[11]*s[14] - s[8]*s[2]*s[15] + s[8]*s[3]*s[14] + s[12]*s[2]*s[11];
            d[5] -= s[12]*s[3]*s[10];
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

        Quat sample_rotation(const CharacterAnimationBone& bone, float frame)
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
                        { next.quaternion[0], next.quaternion[1], next.quaternion[2], next.quaternion[3] },
                        t);
                }
                previous = next;
            }
            const auto& last = bone.rotationFrames.back();
            return normalize_quat({ last.quaternion[0], last.quaternion[1], last.quaternion[2], last.quaternion[3] });
        }

        Vec3 sample_translation(const CharacterAnimationBone& bone, float frame)
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

        static constexpr std::size_t kMaxBones = 128;

        void compute_client_finals(const CharacterAnimation& animation, float frame, Mat4* finals, std::size_t maxBones)
        {
            const auto boneCount = std::min(animation.bones.size(), maxBones);
            Mat4 rawMatrices[kMaxBones];
            for (std::size_t i = 0; i < boneCount; ++i)
                rawMatrices[i] = mat4_from_shaiya_transposed(animation.bones[i].matrix);

            Mat4 locals[kMaxBones];
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

            for (std::size_t i = 0; i < boneCount; ++i)
                finals[i] = Mat4::identity();
            for (std::size_t i = 0; i < boneCount; ++i)
            {
                auto matrix = locals[i];
                const auto parent = animation.bones[i].parentBoneIndex;
                if (parent >= 0 && static_cast<std::size_t>(parent) < i)
                    matrix = mat4_multiply(matrix, finals[static_cast<std::size_t>(parent)]);
                finals[i] = matrix;
            }
        }

        void skin_actor_frame_inplace(
            const ActorSkinData& skin,
            std::span<phoenix::renderer::TerrainVertex> animated,
            const CharacterAnimation& animation,
            float frame)
        {
            if (!animation.parsed || animation.bones.empty() || skin.sourceVertices.size() != animated.size())
                return;

            Mat4 clientFinals[kMaxBones];
            const auto boneCount = std::min(animation.bones.size(), static_cast<std::size_t>(kMaxBones));
            compute_client_finals(animation, frame, clientFinals, kMaxBones);

            const auto meshBoneCount = std::min(skin.meshBones.size(), static_cast<std::size_t>(kMaxBones));
            Mat4 skinMatrices[kMaxBones];
            bool skinMatrixComputed[kMaxBones];
            std::memset(skinMatrixComputed, 0, sizeof(bool) * meshBoneCount);

            for (std::size_t i = 0; i < skin.sourceVertices.size(); ++i)
            {
                const auto& source = skin.sourceVertices[i];
                Vec3 position{};
                Vec3 normal{};
                float totalWeight = 0.0f;

                for (std::size_t influence = 0; influence < 3; ++influence)
                {
                    const auto boneIndex = static_cast<std::size_t>(source.bones[influence]);
                    if (boneIndex >= source.meshBoneCount || boneIndex >= boneCount)
                        continue;
                    const float weight = source.weights[influence];
                    if (weight <= 0.0001f)
                        continue;
                    const auto meshBoneIdx = static_cast<std::size_t>(source.meshBoneBase) + boneIndex;
                    if (meshBoneIdx >= meshBoneCount)
                        continue;

                    if (!skinMatrixComputed[meshBoneIdx])
                    {
                        skinMatrices[meshBoneIdx] = mat4_multiply(
                            mat4_from_shaiya_transposed(skin.meshBones[meshBoneIdx].matrix),
                            clientFinals[boneIndex]);
                        skinMatrixComputed[meshBoneIdx] = true;
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
                const float nx = normal.x * invWeight;
                const float ny = normal.y * invWeight;
                const float nz = normal.z * invWeight;
                const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
                const float invLen = nlen > 0.0001f ? 1.0f / nlen : 0.0f;

                animated[i].position[0] = position.x * source.outputScale;
                animated[i].position[1] = position.y * source.outputScale;
                animated[i].position[2] = position.z * source.outputScale;
                animated[i].normal[0] = nx * invLen;
                animated[i].normal[1] = ny * invLen;
                animated[i].normal[2] = nz * invLen;
            }
        }

        std::uint32_t resolve_texture_layer(
            ActorScene& scene,
            std::unordered_map<std::string, std::uint32_t>& textureSlotByPath,
            const phoenix::assets::DataIndex& assets,
            const std::string& textureName,
            std::uint32_t baseLayer)
        {
            auto path = phoenix::assets::resolve_texture_asset(assets, textureName);
            if (path.empty())
                path = assets.resolve(textureName);
            if (path.empty())
                return UINT32_MAX;

            const auto key = lower_ascii(path.string());
            auto it = textureSlotByPath.find(key);
            if (it == textureSlotByPath.end())
            {
                const auto localSlot = static_cast<std::uint32_t>(scene.texturePaths.size());
                it = textureSlotByPath.emplace(key, localSlot).first;
                scene.texturePaths.push_back(path);
            }

            auto layer = baseLayer + it->second;
            if (texture_uses_cutout(textureName))
                layer += 2048u;
            return layer;
        }

        ModelBuild build_model(
            ActorScene& scene,
            std::unordered_map<std::string, std::uint32_t>& textureSlotByPath,
            const phoenix::assets::DataIndex& assets,
            std::vector<phoenix::renderer::TerrainVertex>& vertices,
            std::vector<std::uint32_t>& indices,
            const std::filesystem::path& folder,
            const MonsterDefinition& def,
            std::uint32_t textureLayerBase,
            float scale,
            bool buildAnimation)
        {
            ModelBuild result{};
            result.firstVertex = static_cast<std::uint32_t>(vertices.size());
            result.firstIndex = static_cast<std::uint32_t>(indices.size());
            result.radius = 0.0f;

            for (const auto& part : def.parts)
            {
                const auto meshPath = resolve_actor_file(assets, folder, part.meshFileName);
                if (meshPath.empty())
                    continue;
                const auto textureLayer = resolve_texture_layer(scene, textureSlotByPath, assets, part.textureFileName, textureLayerBase);
                if (textureLayer == UINT32_MAX)
                    continue;

                auto model = load_character_3dc(meshPath);
                if (!model.parsed || model.vertices.empty() || model.faces.empty())
                    continue;

                append_model_part(vertices, indices, model, textureLayer, scale, result);
            }

            result.vertexCount = static_cast<std::uint32_t>(vertices.size()) - result.firstVertex;
            result.indexCount = static_cast<std::uint32_t>(indices.size()) - result.firstIndex;
            result.radius = std::max(18.0f, result.radius);
            if (def.height > 0.0f)
            {
                const float baseHeight = buildAnimation
                    ? def.height * kActorMeshScale * 0.92f   // NPCs use MON height directly.
                    : scale * kActorMeshScale * 0.72f;       // Mobs already bake MON height into visual scale.
                result.labelHeight = std::max(0.75f, baseHeight);
            }
            else
            {
                result.labelHeight = std::max(0.75f, result.radius * 0.16f);
            }
            result.built = result.indexCount > 0;

            if (buildAnimation && result.built && result.vertexCount > 0)
            {
                // Store animation keyframes for runtime skinning (no pre-baked frames).
                const auto breathName = !actor_asset_placeholder(def.animationSlots[6]) ? def.animationSlots[6] : def.animationSlots[8];
                const auto breathPath = resolve_actor_file(assets, folder, breathName);
                if (!breathPath.empty())
                    result.animations.breath = load_character_ani(breathPath);

                if (!actor_asset_placeholder(def.animationSlots[8]))
                {
                    const auto idlePath = resolve_actor_file(assets, folder, def.animationSlots[8]);
                    if (!idlePath.empty())
                        result.animations.idle = load_character_ani(idlePath);
                }

                if (!actor_asset_placeholder(def.animationSlots[0]))
                {
                    const auto walkPath = resolve_actor_file(assets, folder, def.animationSlots[0]);
                    if (!walkPath.empty())
                        result.animations.walk = load_character_ani(walkPath);
                }

                if (!actor_asset_placeholder(def.animationSlots[1]))
                {
                    const auto runPath = resolve_actor_file(assets, folder, def.animationSlots[1]);
                    if (!runPath.empty())
                        result.animations.run = load_character_ani(runPath);
                }
            }
            return result;
        }

        phoenix::renderer::ObjectInstance make_instance(float x, float y, float z, float yaw)
        {
            const auto actorYaw = yaw + std::numbers::pi_v<float>;
            const auto s = std::sin(actorYaw);
            const auto c = std::cos(actorYaw);
            phoenix::renderer::ObjectInstance instance{};
            instance.right[0] = c;
            instance.right[1] = 0.0f;
            instance.right[2] = -s;
            instance.up[0] = 0.0f;
            instance.up[1] = 1.0f;
            instance.up[2] = 0.0f;
            instance.forward[0] = s;
            instance.forward[1] = 0.0f;
            instance.forward[2] = c;
            instance.position[0] = x;
            instance.position[1] = y;
            instance.position[2] = z;
            instance.position[3] = 1.0f;
            return instance;
        }

        float sample_height(float x, float z, float fallback, float (*heightSampler)(float, float, void*), void* userData)
        {
            if (!heightSampler)
                return fallback;
            const auto h = heightSampler(x, z, userData);
            return std::isfinite(h) ? h : fallback;
        }

        void append_batch(
            ActorScene& scene,
            const ModelBuild& model,
            const std::vector<phoenix::renderer::ObjectInstance>& instances,
            const std::string& labelText)
        {
            if (!model.built || instances.empty())
                return;

            phoenix::renderer::ObjectBatch batch{};
            batch.firstIndex = model.firstIndex;
            batch.indexCount = model.indexCount;
            batch.firstInstance = static_cast<std::uint32_t>(scene.instances.size());
            batch.instanceCount = static_cast<std::uint32_t>(instances.size());

            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = -std::numeric_limits<float>::max();
            float maxY = -std::numeric_limits<float>::max();
            float maxZ = -std::numeric_limits<float>::max();
            for (const auto& instance : instances)
            {
                scene.instances.push_back(instance);
                const auto x = instance.position[0];
                const auto y = instance.position[1];
                const auto z = instance.position[2];
                const auto radius = model.radius;
                minX = std::min(minX, x - radius);
                minY = std::min(minY, y);
                minZ = std::min(minZ, z - radius);
                maxX = std::max(maxX, x + radius);
                maxY = std::max(maxY, y + std::max(model.labelHeight, radius));
                maxZ = std::max(maxZ, z + radius);

                ActorScene::Label label{};
                label.text = labelText;
                label.x = x;
                label.y = y + model.labelHeight;
                label.z = z;
                label.radius = radius;
                label.offsetY = model.labelHeight;
                scene.labels.push_back(std::move(label));
            }

            ActorScene::BatchBounds bounds{};
            bounds.x = (minX + maxX) * 0.5f;
            bounds.y = (minY + maxY) * 0.5f;
            bounds.z = (minZ + maxZ) * 0.5f;
            const auto ex = (maxX - minX) * 0.5f;
            const auto ey = (maxY - minY) * 0.5f;
            const auto ez = (maxZ - minZ) * 0.5f;
            bounds.radius = std::sqrt(ex * ex + ey * ey + ez * ez);

            scene.batches.push_back(batch);
            scene.batchBounds.push_back(bounds);
        }

        void append_animated_batch(
            ActorScene& scene,
            const ModelBuild& model,
            const std::vector<phoenix::renderer::ObjectInstance>& instances,
            const std::string& labelText)
        {
            if (!model.built || instances.empty())
                return;

            phoenix::renderer::ObjectBatch batch{};
            batch.firstIndex = model.firstIndex;
            batch.indexCount = model.indexCount;
            batch.firstInstance = static_cast<std::uint32_t>(scene.animatedInstances.size());
            batch.instanceCount = static_cast<std::uint32_t>(instances.size());

            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = -std::numeric_limits<float>::max();
            float maxY = -std::numeric_limits<float>::max();
            float maxZ = -std::numeric_limits<float>::max();
            for (const auto& instance : instances)
            {
                const auto instanceIndex = static_cast<std::uint32_t>(scene.animatedInstances.size());
                scene.animatedBaseInstances.push_back(instance);
                scene.animatedInstances.push_back(instance);
                const auto x = instance.position[0];
                const auto y = instance.position[1];
                const auto z = instance.position[2];
                const auto radius = model.radius;
                minX = std::min(minX, x - radius);
                minY = std::min(minY, y);
                minZ = std::min(minZ, z - radius);
                maxX = std::max(maxX, x + radius);
                maxY = std::max(maxY, y + std::max(model.labelHeight, radius));
                maxZ = std::max(maxZ, z + radius);

                ActorScene::Label label{};
                label.text = labelText;
                label.x = x;
                label.y = y + model.labelHeight;
                label.z = z;
                label.radius = radius;
                label.offsetY = model.labelHeight;
                label.animatedInstanceIndex = instanceIndex;
                label.followsAnimatedInstance = true;
                scene.labels.push_back(std::move(label));
            }

            ActorScene::BatchBounds bounds{};
            bounds.x = (minX + maxX) * 0.5f;
            bounds.y = (minY + maxY) * 0.5f;
            bounds.z = (minZ + maxZ) * 0.5f;
            const auto ex = (maxX - minX) * 0.5f;
            const auto ey = (maxY - minY) * 0.5f;
            const auto ez = (maxZ - minZ) * 0.5f;
            bounds.radius = std::sqrt(ex * ex + ey * ey + ez * ez);
            scene.animatedBatches.push_back(batch);
            scene.animatedBatchBounds.push_back(bounds);
        }
    }

    void skin_actor_vertices(
        const ActorSkinData& skin,
        std::span<phoenix::renderer::TerrainVertex> vertices,
        const CharacterAnimation& animation,
        float frame)
    {
        skin_actor_frame_inplace(skin, vertices, animation, frame);
    }

    ActorScene build_actor_scene(
        const std::filesystem::path& dataRoot,
        const std::string& mapStem,
        const phoenix::assets::DataIndex& assets,
        std::uint32_t textureLayerBase,
        float (*heightSampler)(float worldX, float worldZ, void* userData),
        void* heightUserData)
    {
        ActorScene scene{};
        const auto svmap = load_svmap(dataRoot / "World" / (mapStem + ".svmap"));
        if (!svmap.parsed)
        {
            std::ofstream log("PhoenixEngine.log", std::ios::app);
            log << "Actor scene: failed to parse svmap " << (dataRoot / "World" / (mapStem + ".svmap")).string() << "\n";
            return scene;
        }

        const auto monsterTable = load_monster_table(dataRoot / "Monster" / "mob.csv");
        const auto npcTable = load_monster_table(dataRoot / "Npc" / "npc.csv");
        const auto monsterSData = load_monster_sdata(dataRoot / "Monster" / "monster.csv");
        const auto npcQuestSData = load_npcquest_sdata(dataRoot / "Npc" / "NpcQuest.csv");
        if (!monsterTable.parsed && !npcTable.parsed)
        {
            std::ofstream log("PhoenixEngine.log", std::ios::app);
            log << "Actor scene: MON tables unavailable monster=" << monsterTable.monsters.size()
                << " npc=" << npcTable.monsters.size() << "\n";
            return scene;
        }

        std::unordered_map<std::string, std::uint32_t> textureSlotByPath;
        std::unordered_map<std::uint32_t, ModelBuild> monsterModels;
        std::unordered_map<std::uint32_t, ModelBuild> npcModels;
        std::unordered_map<std::uint32_t, ResolvedActor> resolvedMonsters;
        std::unordered_map<std::uint32_t, ResolvedActor> resolvedNpcs;
        std::unordered_map<std::uint32_t, std::size_t> npcKeyToAnimIndex;
        std::unordered_map<std::uint32_t, std::size_t> monsterKeyToAnimIndex;
        const auto halfMap = static_cast<float>(std::max(1, svmap.mapSize)) * 0.5f;

        for (const auto& npc : svmap.npcs)
        {
            if (npc.npcId < 0)
                continue;
            const auto key = (static_cast<std::uint32_t>(std::clamp(npc.npcType, 0, 255)) << 16)
                | static_cast<std::uint16_t>(npc.npcId);
            if (!resolvedNpcs.contains(key))
            {
                ResolvedActor resolved{};
                if (const auto it = npcQuestSData.records.find(key); it != npcQuestSData.records.end())
                {
                    resolved.modelId = static_cast<std::uint32_t>(std::max(0, it->second.modelId));
                    resolved.label = it->second.name;
                }
                else
                {
                    resolved.modelId = static_cast<std::uint32_t>(npc.npcId);
                    resolved.label = "NPC " + std::to_string(npc.npcId);
                }
                if (resolved.label.empty())
                    resolved.label = "NPC " + std::to_string(npc.npcId);
                resolvedNpcs.emplace(key, resolved);
            }

            const auto modelId = resolvedNpcs[key].modelId;
            if (static_cast<std::size_t>(modelId) >= npcTable.monsters.size())
                continue;
            if (!npcModels.contains(key))
            {
                const auto& def = npcTable.monsters[modelId];
                auto built = build_model(
                    scene,
                    textureSlotByPath,
                    assets,
                    scene.animatedVertices,
                    scene.animatedIndices,
                    "Npc",
                    def,
                    textureLayerBase,
                    kNpcScale,
                    true);
                if (!built.skinData.sourceVertices.empty())
                {
                    ActorScene::VertexAnimation animation{};
                    animation.firstVertex = built.firstVertex;
                    animation.vertexCount = built.vertexCount;
                    animation.skinData = std::move(built.skinData);
                    animation.animations = std::move(built.animations);
                    animation.hasActorSkin = true;
                    animation.boundingRadius = built.radius;
                    npcKeyToAnimIndex[key] = scene.vertexAnimations.size();
                    scene.vertexAnimations.push_back(std::move(animation));
                }
                npcModels.emplace(key, std::move(built));
            }
        }

        for (const auto& area : svmap.monsterAreas)
        {
            for (const auto& spawn : area.spawns)
            {
                if (!resolvedMonsters.contains(spawn.mobId))
                {
                    ResolvedActor resolved{};
                    if (const auto it = monsterSData.records.find(spawn.mobId); it != monsterSData.records.end())
                    {
                        resolved.modelId = static_cast<std::uint32_t>(std::max<std::int16_t>(0, it->second.modelId));
                        resolved.label = it->second.name;
                        resolved.scale = 1.0f;
                    }
                    else
                    {
                        resolved.modelId = spawn.mobId;
                        resolved.label = "Mob " + std::to_string(spawn.mobId);
                    }
                    if (resolved.label.empty())
                        resolved.label = "Mob " + std::to_string(spawn.mobId);
                    resolvedMonsters.emplace(spawn.mobId, std::move(resolved));
                }

                const auto& resolved = resolvedMonsters[spawn.mobId];
                if (static_cast<std::size_t>(resolved.modelId) >= monsterTable.monsters.size())
                    continue;
                if (!monsterModels.contains(spawn.mobId))
                {
                    const auto& def = monsterTable.monsters[resolved.modelId];
                    const auto height = def.height > 0.0f ? def.height : 1.0f;
                    const auto scale = std::clamp(height * 0.48f, 0.95f, 3.6f);
                    auto built = build_model(
                        scene,
                        textureSlotByPath,
                        assets,
                        scene.animatedVertices,
                        scene.animatedIndices,
                        "Monster",
                        def,
                        textureLayerBase,
                        scale,
                        true);
                    if (!built.skinData.sourceVertices.empty())
                    {
                        ActorScene::VertexAnimation animation{};
                        animation.firstVertex = built.firstVertex;
                        animation.vertexCount = built.vertexCount;
                        animation.skinData = std::move(built.skinData);
                        animation.animations = std::move(built.animations);
                        animation.hasActorSkin = true;
                        animation.boundingRadius = built.radius;
                        animation.isMob = true;
                        monsterKeyToAnimIndex[spawn.mobId] = scene.vertexAnimations.size();
                        scene.vertexAnimations.push_back(std::move(animation));
                    }
                    monsterModels.emplace(spawn.mobId, std::move(built));
                }
            }
        }

        std::map<std::tuple<std::uint32_t, int, int>, std::vector<phoenix::renderer::ObjectInstance>> monsterGroups;
        for (const auto& area : svmap.monsterAreas)
        {
            const auto minX = svmap_to_world(std::min(area.area.min.x, area.area.max.x), halfMap);
            const auto maxX = svmap_to_world(std::max(area.area.min.x, area.area.max.x), halfMap);
            const auto minZ = svmap_to_world(std::min(area.area.min.z, area.area.max.z), halfMap);
            const auto maxZ = svmap_to_world(std::max(area.area.min.z, area.area.max.z), halfMap);
            const auto width = std::max(1.0f, maxX - minX);
            const auto depth = std::max(1.0f, maxZ - minZ);
            for (const auto& spawn : area.spawns)
            {
                const auto modelIt = monsterModels.find(spawn.mobId);
                if (modelIt == monsterModels.end() || !modelIt->second.built)
                    continue;
                const auto count = std::min<std::uint32_t>(spawn.count, 24);
                const auto grid = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<float>(std::max(1u, count)))));
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const auto gx = i % grid;
                    const auto gz = i / grid;
                    const auto fx = (static_cast<float>(gx) + 0.5f) / static_cast<float>(grid);
                    const auto fz = (static_cast<float>(gz) + 0.5f) / static_cast<float>(grid);
                    const auto jitter = static_cast<float>((spawn.mobId * 1103515245u + i * 12345u) & 1023u) / 1023.0f - 0.5f;
                    const auto x = minX + width * std::clamp(fx + jitter * 0.12f, 0.05f, 0.95f);
                    const auto z = minZ + depth * std::clamp(fz - jitter * 0.12f, 0.05f, 0.95f);
                    const auto y = sample_height(x, z, (area.area.min.y + area.area.max.y) * 0.5f, heightSampler, heightUserData);
                    const auto yaw = jitter * std::numbers::pi_v<float> * 2.0f;
                    const auto cellX = static_cast<int>(std::floor(x / kCellSize));
                    const auto cellZ = static_cast<int>(std::floor(z / kCellSize));
                    monsterGroups[{ spawn.mobId, cellX, cellZ }].push_back(make_instance(x, y, z, yaw));
                    ++scene.monsterCount;
                }
            }
        }

        for (const auto& [key, instances] : monsterGroups)
        {
            const auto mobId = std::get<0>(key);
            const auto resolvedIt = resolvedMonsters.find(mobId);
            const auto label = resolvedIt != resolvedMonsters.end() ? resolvedIt->second.label : ("Mob " + std::to_string(mobId));
            append_animated_batch(scene, monsterModels[mobId], instances, label);
        }

        std::map<std::tuple<std::uint32_t, int, int>, std::vector<phoenix::renderer::ObjectInstance>> npcGroups;
        for (const auto& npc : svmap.npcs)
        {
            if (npc.npcId < 0)
                continue;
            const auto key = (static_cast<std::uint32_t>(std::clamp(npc.npcType, 0, 255)) << 16)
                | static_cast<std::uint16_t>(npc.npcId);
            const auto modelIt = npcModels.find(key);
            if (modelIt == npcModels.end() || !modelIt->second.built)
                continue;
            for (const auto& pos : npc.positions)
            {
                const auto x = svmap_to_world(pos.position.x, halfMap);
                const auto z = svmap_to_world(pos.position.z, halfMap);
                const auto y = sample_height(x, z, pos.position.y, heightSampler, heightUserData);
                const auto cellX = static_cast<int>(std::floor(x / kCellSize));
                const auto cellZ = static_cast<int>(std::floor(z / kCellSize));
                npcGroups[{ key, cellX, cellZ }].push_back(make_instance(x, y, z, pos.yaw));
                ++scene.npcCount;
            }
        }

        // Set world positions on vertex animations from monster instance centroids.
        std::unordered_map<std::uint32_t, std::pair<float, float>> mobCentroids;
        std::unordered_map<std::uint32_t, std::uint32_t> mobCounts;
        for (const auto& [key, instances] : monsterGroups)
        {
            const auto mobId = std::get<0>(key);
            for (const auto& inst : instances)
            {
                auto& c = mobCentroids[mobId];
                c.first += inst.position[0];
                c.second += inst.position[2];
                ++mobCounts[mobId];
            }
        }
        for (const auto& [mobId, animIdx] : monsterKeyToAnimIndex)
        {
            if (animIdx < scene.vertexAnimations.size())
            {
                auto& anim = scene.vertexAnimations[animIdx];
                const auto countIt = mobCounts.find(mobId);
                if (countIt != mobCounts.end() && countIt->second > 0)
                {
                    const auto& centroid = mobCentroids[mobId];
                    anim.worldX = centroid.first / static_cast<float>(countIt->second);
                    anim.worldZ = centroid.second / static_cast<float>(countIt->second);
                    if (heightSampler)
                        anim.worldY = heightSampler(anim.worldX, anim.worldZ, heightUserData);
                    // Expand bounding radius to cover the farthest instance from centroid.
                    float maxDistSq = 0.0f;
                    for (const auto& [key, instances] : monsterGroups)
                    {
                        if (std::get<0>(key) != mobId)
                            continue;
                        for (const auto& inst : instances)
                        {
                            const float dx = inst.position[0] - anim.worldX;
                            const float dz = inst.position[2] - anim.worldZ;
                            maxDistSq = std::max(maxDistSq, dx * dx + dz * dz);
                        }
                    }
                    anim.boundingRadius = std::max(anim.boundingRadius, std::sqrt(maxDistSq) + 48.0f);
                }
            }
        }

        // Set world positions on vertex animations from NPC instance centroids.
        std::unordered_map<std::uint32_t, std::pair<float, float>> npcCentroids; // key -> (sumX, sumZ), count tracked via npcGroups
        std::unordered_map<std::uint32_t, std::uint32_t> npcCounts;
        for (const auto& npc : svmap.npcs)
        {
            if (npc.npcId < 0)
                continue;
            const auto key = (static_cast<std::uint32_t>(std::clamp(npc.npcType, 0, 255)) << 16)
                | static_cast<std::uint16_t>(npc.npcId);
            for (const auto& pos : npc.positions)
            {
                const auto x = svmap_to_world(pos.position.x, halfMap);
                const auto z = svmap_to_world(pos.position.z, halfMap);
                auto& centroid = npcCentroids[key];
                centroid.first += x;
                centroid.second += z;
                ++npcCounts[key];
            }
        }
        for (const auto& [key, animIdx] : npcKeyToAnimIndex)
        {
            if (animIdx < scene.vertexAnimations.size())
            {
                auto& anim = scene.vertexAnimations[animIdx];
                const auto countIt = npcCounts.find(key);
                if (countIt != npcCounts.end() && countIt->second > 0)
                {
                    const auto& centroid = npcCentroids[key];
                    anim.worldX = centroid.first / static_cast<float>(countIt->second);
                    anim.worldZ = centroid.second / static_cast<float>(countIt->second);
                    if (heightSampler)
                        anim.worldY = heightSampler(anim.worldX, anim.worldZ, heightUserData);
                    // Expand bounding radius to cover the farthest instance from centroid,
                    // just like mobs. NPCs of the same type (e.g. animals) can be spread
                    // across the map � without this, culling kills their animation.
                    float maxDistSq = 0.0f;
                    for (const auto& [groupKey, instances] : npcGroups)
                    {
                        if (std::get<0>(groupKey) != key)
                            continue;
                        for (const auto& inst : instances)
                        {
                            const float dx = inst.position[0] - anim.worldX;
                            const float dz = inst.position[2] - anim.worldZ;
                            maxDistSq = std::max(maxDistSq, dx * dx + dz * dz);
                        }
                    }
                    anim.boundingRadius = std::max(anim.boundingRadius, std::sqrt(maxDistSq) + 48.0f);
                }
            }
        }

        for (const auto& [groupKey, instances] : npcGroups)
        {
            const auto npcKey = std::get<0>(groupKey);
            const auto resolvedIt = resolvedNpcs.find(npcKey);
            const auto label = resolvedIt != resolvedNpcs.end() ? resolvedIt->second.label : ("NPC " + std::to_string(npcKey & 0xFFFFu));
            append_animated_batch(scene, npcModels[npcKey], instances, label);
        }

        std::size_t animMemBytes = 0;
        for (const auto& va : scene.vertexAnimations)
        {
            for (const auto& f : va.frames) animMemBytes += f.size() * sizeof(phoenix::renderer::TerrainVertex);
            animMemBytes += va.skinData.sourceVertices.size() * sizeof(ActorSourceVertex);
            animMemBytes += va.skinData.meshBones.size() * sizeof(CharacterBone);
        }
        std::ofstream log("PhoenixEngine.log", std::ios::app);
        log << "Actor scene: svmap=" << mapStem
            << " npcs=" << scene.npcCount
            << " monsters=" << scene.monsterCount
            << " models=" << (npcModels.size() + monsterModels.size())
            << " textures=" << scene.texturePaths.size()
            << " batches=" << scene.batches.size()
            << " npcAnimatedBatches=" << scene.animatedBatches.size()
            << " vertexAnimations=" << scene.vertexAnimations.size()
            << " animFrameMemMB=" << (animMemBytes / (1024 * 1024))
            << " monsterSData=" << monsterSData.records.size()
            << " npcSData=" << npcQuestSData.records.size()
            << "\n";
        return scene;
    }
}
