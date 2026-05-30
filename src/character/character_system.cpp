#include "character/character_system.h"

#include "assets/data_index.h"
#include "character/weapon_bone_map.h"
#include "core/logging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
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
        // Mounted movement is faster than running on foot (base > kFastRunSpeed).
        constexpr float kMountSpeed = 9.5f;
        constexpr float kMountFastSpeed = 14.0f;
        constexpr float kWaterSurface = 0.0f;
        constexpr float kWaterEnterDepth = 2.0f;
        constexpr float kFloatFeetDepth = 1.20f;
        constexpr float kBuoyancySpeed = 1.05f;
        constexpr float kJumpImpulse = 2.7f;
        constexpr float kGravity = 7.5f;
        // Native animation frame rate, recovered by reverse-engineering the retail
        // client (game-pt-ps0182.exe). The keyframe advancer FUN_00411470 computes
        //   currentFrame += dtSeconds * 30.0f
        // where dtSeconds is a QueryPerformanceCounter delta (FUN_004057c0 mode 6)
        // and 30.0f is the read-only constant DAT_0074c074. Every .ANI (mobs,
        // characters, vehicles) advances at this single global rate and loops at
        // endKeyframe — there are no per-animation or per-state rate factors.
        constexpr float kAniFramesPerSecond = 30.0f;
        constexpr float kPi = 3.1415926535f;

        // CSV token cleanup (strips trailing '\r' from CRLF files) and Linux-safe
        // case-insensitive path resolution, both delegating to the shared assets
        // helpers so behaviour stays identical across modules.
        inline std::string trim(const std::string& value)
        {
            return assets::trim_ascii(value);
        }

        inline std::filesystem::path resolve_ci(const std::filesystem::path& path)
        {
            return assets::resolve_existing_path_case_insensitive(path);
        }

        // Read a little-endian uint32 from a raw byte pointer.
        std::uint32_t read_u32_le(const std::uint8_t* data, std::size_t offset)
        {
            return static_cast<std::uint32_t>(data[offset])
                | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
                | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
                | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
        }

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

        // ======================================================================
        // Animation action IDs — native game action CSV record indices.
        // Each character type has its own _action.csv with these fixed IDs.
        // ======================================================================

        // -- Movement --
        constexpr std::size_t kActionIdle          = 0;   // Normal / breathing / default
        constexpr std::size_t kActionWalk           = 1;   // Walking
        constexpr std::size_t kActionRun            = 2;   // Run (standard)
        constexpr std::size_t kActionBackstep       = 3;   // Backward step
        constexpr std::size_t kActionLeftStep       = 4;   // Left step
        constexpr std::size_t kActionRightStep      = 5;   // Right step
        constexpr std::size_t kActionSwimIdle       = 6;   // Floating on water
        constexpr std::size_t kActionSwim           = 7;   // Actively swimming
        constexpr std::size_t kActionJump           = 8;   // Jump
        constexpr std::size_t kActionDie            = 9;   // Death
        constexpr std::size_t kActionSitDown        = 10;  // Sit down transition
        constexpr std::size_t kActionSitUp          = 11;  // Stand up transition
        constexpr std::size_t kActionSit            = 12;  // Sitting (loop)
        constexpr std::size_t kActionDodgeBack      = 13;  // Backward dodge (double-tap)
        constexpr std::size_t kActionDodgeLeft      = 14;  // Left dodge (double-tap)
        constexpr std::size_t kActionDodgeRight     = 15;  // Right dodge (double-tap)
        constexpr std::size_t kActionIdle1          = 16;  // Periodic idle variation 1
        constexpr std::size_t kActionIdle2          = 17;  // Periodic idle variation 2
        constexpr std::size_t kActionLadder         = 18;  // Climbing ladder
        constexpr std::size_t kActionSelect         = 19;  // Character selection screen

        // -- Mounted --
        constexpr std::size_t kActionVehicleRun1    = 20;  // Vehicle run
        constexpr std::size_t kActionVehicleIdle    = 21;  // Vehicle idle / breathing
        constexpr std::size_t kActionVehicleRun2    = 22;  // Vehicle run (variant)

        // -- Two-hand weapons --
        constexpr std::size_t kAction2HReady        = 23;  // Two-hand stance
        constexpr std::size_t kAction2HAttack1      = 24;
        constexpr std::size_t kAction2HAttack2      = 25;
        constexpr std::size_t kAction2HAttack3      = 26;
        constexpr std::size_t kAction2HAttack4      = 27;
        constexpr std::size_t kAction2HDamage       = 28;
        constexpr std::size_t kAction2HRun          = 29;

        // -- Bows --
        constexpr std::size_t kActionBowReady       = 30;
        constexpr std::size_t kActionBowAttack      = 31;
        constexpr std::size_t kActionBowDamage      = 32;
        constexpr std::size_t kActionBowRun         = 33;

        // -- One-hand weapons --
        constexpr std::size_t kAction1HReady        = 34;
        constexpr std::size_t kAction1HAttack1      = 35;
        constexpr std::size_t kAction1HAttack2      = 36;
        constexpr std::size_t kAction1HAttack3      = 37;
        constexpr std::size_t kAction1HAttack4      = 38;
        constexpr std::size_t kAction1HDamage       = 39;
        constexpr std::size_t kAction1HRun          = 40;

        // -- Dual weapons --
        constexpr std::size_t kActionDualReady      = 41;
        constexpr std::size_t kActionDualAttack1    = 42;
        constexpr std::size_t kActionDualAttack2    = 43;
        constexpr std::size_t kActionDualAttack3    = 44;
        constexpr std::size_t kActionDualAttack4    = 45;
        constexpr std::size_t kActionDualDamage     = 46;
        constexpr std::size_t kActionDualRun        = 47;

        // -- Spears --
        constexpr std::size_t kActionSpearReady     = 48;
        constexpr std::size_t kActionSpearAttack1   = 49;
        constexpr std::size_t kActionSpearAttack2   = 50;
        constexpr std::size_t kActionSpearAttack3   = 51;
        constexpr std::size_t kActionSpearAttack4   = 52;
        constexpr std::size_t kActionSpearDamage    = 53;
        constexpr std::size_t kActionSpearRun       = 54;

        // -- Crossbow --
        constexpr std::size_t kActionCrossbowReady  = 55;
        constexpr std::size_t kActionCrossbowAttack = 56;
        constexpr std::size_t kActionCrossbowDamage = 57;
        constexpr std::size_t kActionCrossbowRun    = 58;

        // -- Staff (magic stave) --
        constexpr std::size_t kActionStaffReady     = 59;
        constexpr std::size_t kActionStaffAttack1   = 60;
        constexpr std::size_t kActionStaffAttack2   = 61;
        constexpr std::size_t kActionStaffDamage    = 62;
        constexpr std::size_t kActionStaffRun       = 63;

        // -- Reverse dagger --
        constexpr std::size_t kActionRevDaggerReady    = 64;
        constexpr std::size_t kActionRevDaggerAttack1  = 65;
        constexpr std::size_t kActionRevDaggerAttack2  = 66;
        constexpr std::size_t kActionRevDaggerAttack3  = 67;
        constexpr std::size_t kActionRevDaggerAttack4  = 68;
        constexpr std::size_t kActionRevDaggerDamage   = 69;
        constexpr std::size_t kActionRevDaggerRun      = 70;

        // -- Knuckles / claws --
        constexpr std::size_t kActionKnuckleReady   = 71;
        constexpr std::size_t kActionKnuckleAttack1 = 72;
        constexpr std::size_t kActionKnuckleAttack2 = 73;
        constexpr std::size_t kActionKnuckleAttack3 = 74;
        constexpr std::size_t kActionKnuckleAttack4 = 75;
        constexpr std::size_t kActionKnuckleDamage  = 76;
        constexpr std::size_t kActionKnuckleRun     = 77;

        // -- Dagger --
        constexpr std::size_t kActionDaggerReady    = 78;
        constexpr std::size_t kActionDaggerAttack1  = 79;
        constexpr std::size_t kActionDaggerAttack2  = 80;
        constexpr std::size_t kActionDaggerAttack3  = 81;
        constexpr std::size_t kActionDaggerAttack4  = 82;
        constexpr std::size_t kActionDaggerDamage   = 83;
        constexpr std::size_t kActionDaggerRun      = 84;

        // -- Magic casting --
        constexpr std::size_t kActionMagicReady1    = 85;  // Post-cast stance
        constexpr std::size_t kActionMagicCast1     = 86;  // Casting loop (weapon-agnostic)
        constexpr std::size_t kActionMagicAttack1   = 87;  // Magic attack release
        constexpr std::size_t kActionMagicReady2    = 88;  // Post-cast stance (variant)
        constexpr std::size_t kActionMagicCast2     = 89;  // Casting loop (variant)
        constexpr std::size_t kActionMagicAttack2   = 90;  // Magic attack release (variant)

        // -- Buffs --
        constexpr std::size_t kActionBuffReady1     = 91;
        constexpr std::size_t kActionBuffCast1      = 92;
        constexpr std::size_t kActionBuffAttack1    = 93;
        constexpr std::size_t kActionBuffReady2     = 94;
        constexpr std::size_t kActionBuffCast2      = 95;
        constexpr std::size_t kActionBuffAttack2    = 96;
        constexpr std::size_t kActionBuffReady3     = 97;
        constexpr std::size_t kActionBuffCast3      = 98;
        constexpr std::size_t kActionBuffAttack3    = 99;

        // -- Skills (weapon-agnostic, triggered by specific skill IDs) --
        constexpr std::size_t kActionSkill1         = 100;
        constexpr std::size_t kActionSkill2         = 101;
        constexpr std::size_t kActionSkill3         = 102;
        constexpr std::size_t kActionSkill4         = 103;
        constexpr std::size_t kActionSkill5         = 104;
        constexpr std::size_t kActionSkill6         = 105;
        constexpr std::size_t kActionSkill7         = 106;
        constexpr std::size_t kActionSkill8         = 107;
        constexpr std::size_t kActionSkill9         = 108;
        constexpr std::size_t kActionSkill10        = 109;
        constexpr std::size_t kActionSkill11        = 110;

        // -- Other --
        constexpr std::size_t kActionUnknown111     = 111;
        constexpr std::size_t kActionUnknown112     = 112;
        constexpr std::size_t kActionUnknown113     = 113;
        constexpr std::size_t kActionStun           = 114; // Dizzy / stunned
        constexpr std::size_t kActionUnknown115     = 115;

        // -- Emotes --
        constexpr std::size_t kActionEmote1         = 116;
        constexpr std::size_t kActionEmote2         = 117;
        constexpr std::size_t kActionEmote3         = 118;
        constexpr std::size_t kActionEmote4         = 119;
        constexpr std::size_t kActionEmote5         = 120;
        constexpr std::size_t kActionEmote6         = 121;
        constexpr std::size_t kActionEmote7         = 122;
        constexpr std::size_t kActionEmote8         = 123;
        constexpr std::size_t kActionEmote9         = 124;
        constexpr std::size_t kActionEmote10        = 125;

        constexpr std::size_t kActionCount          = 126; // Total known action slots

        // Hand bone indices for weapon/shield attachment.
        // Verified from skeleton hierarchy dump:
        //   Bone 11: right wrist (parent=10 elbow, rotFrames=31)
        //   Bone 21: left wrist  (parent=20 elbow, rotFrames=31)
        // Bones 16/26 are static shoulder-parented nodes with no animation — not usable.
        constexpr std::size_t kRightHandBone = 11;
        constexpr std::size_t kLeftHandBone  = 21;

        // Resolve a weapon/shield entry from an Item CSV.
        struct ItemEntry
        {
            std::string meshName;
            std::string textureName;
            bool alphaCutout{};
        };

        std::optional<ItemEntry> resolve_item_from_csv(
            const std::filesystem::path& csvPath, int recordIndex)
        {
            std::ifstream file(csvPath);
            if (!file)
                return std::nullopt;
            std::string line;
            std::getline(file, line); // skip header
            while (std::getline(file, line))
            {
                if (line.empty())
                    continue;
                // Strip quotes.
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
                next(); // SourceFile
                next(); // Header
                const auto idx = std::atoi(next().c_str()); // RecordIndex
                if (idx != recordIndex)
                    continue;
                next(); // MeshIndex
                auto meshName = trim(next()); // MeshName
                next(); // TextureIndex
                auto textureName = trim(next()); // TextureName
                auto alphaMode = trim(next()); // AlphaBlendingMode

                if (meshName.empty())
                    return std::nullopt;

                ItemEntry entry{};
                entry.meshName = std::move(meshName);
                entry.textureName = std::move(textureName);
                entry.alphaCutout = (alphaMode == "Alpha" || alphaMode == "Visibility");
                return entry;
            }
            return std::nullopt;
        }

        // Load animations from the action CSV, indexed by RecordIndex.
        // The vector is sized so that animations[recordIndex] gives the correct animation.
        // Slots with missing or invalid files get an empty (unparsed) entry.
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
                const auto recordIndex = static_cast<std::size_t>(std::atoi(next().c_str()));
                next(); // Header (AL3)
                auto aniFileName = trim(next());

                // Ensure vector is large enough for this record index.
                if (recordIndex >= animations.size())
                    animations.resize(recordIndex + 1);

                if (aniFileName.empty())
                    continue;

                auto aniPath = resolve_ci(animationRoot / aniFileName);
                if (aniPath.empty())
                    continue;

                CharacterAnimationChoice choice{};
                choice.path = aniPath;
                auto stem = aniPath.stem().string();
                std::transform(stem.begin(), stem.end(), stem.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                choice.name = std::move(stem);
                choice.animation = world::load_character_ani(choice.path);
                animations[recordIndex] = std::move(choice);
            }
            return animations;
        }

        std::vector<CharacterAnimationChoice> load_animation_choices(const std::filesystem::path& animationRoot, const std::string& prefix)
        {
            const auto csvName = prefix.substr(0, prefix.size() - (prefix.ends_with("_") ? 1 : 0)) + "_action.csv";
            const auto csvPath = resolve_ci(animationRoot.parent_path() / csvName);
            if (csvPath.empty())
                return {};
            return load_animation_choices_from_csv(csvPath, animationRoot);
        }

        // Resolve an action ID to its vector index, with fallback to 0 (idle).
        std::size_t resolve_action(const std::vector<CharacterAnimationChoice>& animations, std::size_t actionId)
        {
            if (actionId < animations.size() && animations[actionId].animation.parsed)
                return actionId;
            // Fallback: return first valid animation.
            for (std::size_t i = 0; i < animations.size(); ++i)
                if (animations[i].animation.parsed)
                    return i;
            return 0;
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
                auto meshName = trim(next());
                auto textureName = trim(next());
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
            auto csvPath = resolve_ci(raceRoot / std::format("{}_{}.csv", prefix, partFile));
            if (csvPath.empty())
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
                sv.gpuIndex = static_cast<std::uint32_t>(data.bindVertices.size());
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

    const renderer::DdsTexture* CharacterSystem::bc3_texture_for(const std::filesystem::path& path) const
    {
        const auto key = path_key(path);
        const auto it = cachedBc3Textures_.find(key);
        return it != cachedBc3Textures_.end() ? &it->second : nullptr;
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
        cachedBc3Textures_.clear();
        bc3CacheReady_ = false;

        const auto characterRoot = dataRoot / "Character";
        if (!std::filesystem::exists(characterRoot))
            return false;

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

            // Load animations from action CSVs — respects RecordIndex for correct ID mapping.
            // Scan for *_action.csv files in the race folder to discover all prefixes.
            for (const auto& entry : std::filesystem::directory_iterator(raceEntry.path()))
            {
                if (!entry.is_regular_file())
                    continue;
                const auto fileName = lower_ascii(entry.path().filename().string());
                if (!fileName.ends_with("_action.csv"))
                    continue;
                // Extract prefix: "humf_action.csv" → "humf_"
                const auto actionPos = fileName.find("_action.csv");
                if (actionPos == std::string::npos || actionPos == 0)
                    continue;
                const auto prefix = fileName.substr(0, actionPos + 1); // includes trailing '_'
                const auto animationRoot = raceEntry.path() / "ANI";
                const auto cacheKey = animation_cache_key(animationRoot, prefix);
                if (cachedAnimations_.contains(cacheKey))
                    continue;
                auto choices = load_animation_choices_from_csv(entry.path(), animationRoot);
                if (!choices.empty())
                    cachedAnimations_.emplace(cacheKey, std::move(choices));
            }
        }

        // ---- Pre-cache all character textures as BC3 in RAM ----
        // This runs during the loading screen so appearance swaps are instant at runtime.
        {
            // Determine dominant resolution from a quick scan of the first few textures.
            std::uint32_t targetW = 256, targetH = 256;
            std::map<std::uint64_t, std::uint32_t> sizeCounts;
            const std::size_t probeSampleCount = std::min(cachedTexturePaths_.size(), std::size_t(64));
            for (std::size_t i = 0; i < probeSampleCount; ++i)
            {
                auto tex = renderer::load_dds(cachedTexturePaths_[i]);
                if (!tex.valid || tex.width == 0 || tex.height == 0) continue;
                const auto key = (static_cast<std::uint64_t>(tex.width) << 32) | tex.height;
                sizeCounts[key]++;
            }
            std::uint32_t bestCount = 0;
            for (const auto& [key, count] : sizeCounts)
            {
                if (count > bestCount)
                {
                    bestCount = count;
                    targetW = static_cast<std::uint32_t>(key >> 32);
                    targetH = static_cast<std::uint32_t>(key & 0xFFFFFFFF);
                }
            }
            const auto maxDim = std::max(targetW, targetH);
            const auto fullMips = static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(maxDim)))) + 1u;
            const auto targetMips = std::min(fullMips,
                static_cast<std::uint32_t>(std::max(1.0, std::log2(static_cast<double>(maxDim)) - 1.0)));

            bc3TargetWidth_ = targetW;
            bc3TargetHeight_ = targetH;
            bc3TargetMips_ = targetMips;

            // Parallel load + BC3 convert all character textures.
            struct Bc3Result
            {
                std::string key;
                renderer::DdsTexture texture;
            };
            std::vector<Bc3Result> results(cachedTexturePaths_.size());
            std::atomic<std::size_t> nextIdx{ 0 };
            const auto workerCount = std::min(
                static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency())),
                cachedTexturePaths_.size());
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (std::size_t w = 0; w < workerCount; ++w)
            {
                workers.emplace_back([&]() {
                    for (;;)
                    {
                        const auto idx = nextIdx.fetch_add(1, std::memory_order_relaxed);
                        if (idx >= cachedTexturePaths_.size()) break;
                        auto tex = renderer::load_dds(cachedTexturePaths_[idx]);
                        renderer::convert_texture_to_bc3(tex, targetW, targetH, targetMips);
                        // Free decoded RGBA — only keep BC3 mip data.
                        std::vector<std::uint8_t>().swap(tex.rgba);
                        results[idx].key = path_key(cachedTexturePaths_[idx]);
                        results[idx].texture = std::move(tex);
                    }
                });
            }
            for (auto& w : workers) w.join();

            std::size_t bc3Bytes = 0;
            for (auto& r : results)
            {
                if (!r.texture.valid) continue;
                for (const auto& mip : r.texture.mipData)
                    bc3Bytes += mip.size();
                cachedBc3Textures_.emplace(std::move(r.key), std::move(r.texture));
            }
            bc3CacheReady_ = true;

            std::ofstream bc3Log(phoenix::core::engine_log_path(), std::ios::app);
            bc3Log << "Character BC3 cache: " << cachedBc3Textures_.size()
                << " textures, " << (bc3Bytes / (1024 * 1024)) << " MB"
                << " target=" << targetW << "x" << targetH
                << " mips=" << targetMips << "\n";
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
                for (const auto& c : choices)
                {
                    if (!c.animation.parsed) continue;
                    ++totalAnims;
                    for (const auto& bone : c.animation.bones)
                        animBytes += bone.rotationFrames.capacity() * sizeof(world::CharacterAnimationRotationFrame)
                            + bone.translationFrames.capacity() * sizeof(world::CharacterAnimationTranslationFrame)
                            + sizeof(world::CharacterAnimationBone);
                }
            }
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
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

    bool CharacterSystem::preload_items(const std::filesystem::path& dataRoot)
    {
        if (itemCacheReady_)
            return true;

        const auto itemRoot = dataRoot / "Item";
        const auto meshRoot = itemRoot / "3do";
        if (!std::filesystem::exists(meshRoot))
            return false;

        for (const auto& entry : std::filesystem::directory_iterator(meshRoot))
        {
            if (!entry.is_regular_file() || lower_ascii(entry.path().extension().string()) != ".3do")
                continue;
            auto model = world::load_item_3do(entry.path());
            if (model.parsed)
                cachedItemModels_.emplace(path_key(entry.path()), std::move(model));
        }

        // Pre-cache item DDS textures as BC3.
        const auto ddsRoot = itemRoot / "dds";
        if (std::filesystem::exists(ddsRoot) && bc3CacheReady_)
        {
            std::vector<std::filesystem::path> itemTexPaths;
            for (const auto& entry : std::filesystem::directory_iterator(ddsRoot))
            {
                if (!entry.is_regular_file() || lower_ascii(entry.path().extension().string()) != ".dds")
                    continue;
                const auto key = path_key(entry.path());
                if (cachedBc3Textures_.contains(key))
                    continue;
                // Also register in the texture slot map for later lookup.
                if (!cachedTextureSlotByPath_.contains(key))
                {
                    const auto slot = static_cast<std::uint32_t>(cachedTexturePaths_.size());
                    cachedTextureSlotByPath_.emplace(key, slot);
                    cachedTexturePaths_.push_back(entry.path());
                }
                itemTexPaths.push_back(entry.path());
            }

            // Parallel BC3 conversion.
            if (!itemTexPaths.empty())
            {
                struct Bc3Result { std::string key; renderer::DdsTexture texture; };
                std::vector<Bc3Result> results(itemTexPaths.size());
                std::atomic<std::size_t> nextIdx{ 0 };
                const auto workerCount = std::min(
                    static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency())),
                    itemTexPaths.size());
                std::vector<std::thread> workers;
                workers.reserve(workerCount);
                for (std::size_t w = 0; w < workerCount; ++w)
                {
                    workers.emplace_back([&]() {
                        for (;;)
                        {
                            const auto idx = nextIdx.fetch_add(1, std::memory_order_relaxed);
                            if (idx >= itemTexPaths.size()) break;
                            auto tex = renderer::load_dds(itemTexPaths[idx]);
                            renderer::convert_texture_to_bc3(tex, bc3TargetWidth_, bc3TargetHeight_, bc3TargetMips_);
                            std::vector<std::uint8_t>().swap(tex.rgba);
                            results[idx].key = path_key(itemTexPaths[idx]);
                            results[idx].texture = std::move(tex);
                        }
                    });
                }
                for (auto& w : workers) w.join();

                for (auto& r : results)
                {
                    if (!r.texture.valid) continue;
                    cachedBc3Textures_.emplace(std::move(r.key), std::move(r.texture));
                }
            }
        }

        itemCacheReady_ = true;
        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "Item cache: models=" << cachedItemModels_.size() << "\n";
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
        mountAnimationSeconds_ = 0.0f;
        mountActiveAnimation_ = 0;
        mountIdleTimer_ = 0.0f;

        // Apply per-character default attach bones (ranged weapons on archer/hunter
        // classes use a different bone). Still overridable live from the UI.
        apply_default_attach_bones(appearance.prefix, appearance.weaponType,
                                   weaponBoneIndex, shieldBoneIndex);

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

        // Load animations — resolved by action CSV record index (native game behaviour).
        const auto prefix = appearance.prefix + "_";
        if (const auto it = cachedAnimations_.find(animation_cache_key(animationRoot, prefix)); it != cachedAnimations_.end())
            data_.animations = it->second;
        else
            data_.animations = load_animation_choices(animationRoot, prefix);
        // Resolve all action IDs to vector indices.
        // Movement
        data_.idleAnimation         = resolve_action(data_.animations, kActionIdle);
        data_.walkAnimation         = resolve_action(data_.animations, kActionWalk);
        data_.runAnimation          = resolve_action(data_.animations, kActionRun);
        data_.backAnimation         = resolve_action(data_.animations, kActionBackstep);
        data_.leftAnimation         = resolve_action(data_.animations, kActionLeftStep);
        data_.rightAnimation        = resolve_action(data_.animations, kActionRightStep);
        data_.swimIdleAnimation     = resolve_action(data_.animations, kActionSwimIdle);
        data_.swimAnimation         = resolve_action(data_.animations, kActionSwim);
        data_.jumpAnimation         = resolve_action(data_.animations, kActionJump);
        data_.dieAnimation          = resolve_action(data_.animations, kActionDie);
        data_.sitDownAnimation      = resolve_action(data_.animations, kActionSitDown);
        data_.sitUpAnimation        = resolve_action(data_.animations, kActionSitUp);
        data_.sitAnimation          = resolve_action(data_.animations, kActionSit);
        data_.dodgeBackAnimation    = resolve_action(data_.animations, kActionDodgeBack);
        data_.dodgeLeftAnimation    = resolve_action(data_.animations, kActionDodgeLeft);
        data_.dodgeRightAnimation   = resolve_action(data_.animations, kActionDodgeRight);
        data_.idle1Animation        = resolve_action(data_.animations, kActionIdle1);
        data_.idle2Animation        = resolve_action(data_.animations, kActionIdle2);
        data_.ladderAnimation       = resolve_action(data_.animations, kActionLadder);
        data_.selectAnimation       = resolve_action(data_.animations, kActionSelect);
        // Mounted
        data_.vehicleRun1Animation  = resolve_action(data_.animations, kActionVehicleRun1);
        data_.vehicleIdleAnimation  = resolve_action(data_.animations, kActionVehicleIdle);
        data_.vehicleRun2Animation  = resolve_action(data_.animations, kActionVehicleRun2);
        // Two-hand weapons
        data_.twoHandReadyAnimation   = resolve_action(data_.animations, kAction2HReady);
        data_.twoHandAttack1Animation = resolve_action(data_.animations, kAction2HAttack1);
        data_.twoHandAttack2Animation = resolve_action(data_.animations, kAction2HAttack2);
        data_.twoHandAttack3Animation = resolve_action(data_.animations, kAction2HAttack3);
        data_.twoHandAttack4Animation = resolve_action(data_.animations, kAction2HAttack4);
        data_.twoHandDamageAnimation  = resolve_action(data_.animations, kAction2HDamage);
        data_.twoHandRunAnimation     = resolve_action(data_.animations, kAction2HRun);
        // Bows
        data_.bowReadyAnimation     = resolve_action(data_.animations, kActionBowReady);
        data_.bowAttackAnimation    = resolve_action(data_.animations, kActionBowAttack);
        data_.bowDamageAnimation    = resolve_action(data_.animations, kActionBowDamage);
        data_.bowRunAnimation       = resolve_action(data_.animations, kActionBowRun);
        // One-hand weapons
        data_.oneHandReadyAnimation   = resolve_action(data_.animations, kAction1HReady);
        data_.oneHandAttack1Animation = resolve_action(data_.animations, kAction1HAttack1);
        data_.oneHandAttack2Animation = resolve_action(data_.animations, kAction1HAttack2);
        data_.oneHandAttack3Animation = resolve_action(data_.animations, kAction1HAttack3);
        data_.oneHandAttack4Animation = resolve_action(data_.animations, kAction1HAttack4);
        data_.oneHandDamageAnimation  = resolve_action(data_.animations, kAction1HDamage);
        data_.oneHandRunAnimation     = resolve_action(data_.animations, kAction1HRun);
        // Dual weapons
        data_.dualReadyAnimation    = resolve_action(data_.animations, kActionDualReady);
        data_.dualAttack1Animation  = resolve_action(data_.animations, kActionDualAttack1);
        data_.dualAttack2Animation  = resolve_action(data_.animations, kActionDualAttack2);
        data_.dualAttack3Animation  = resolve_action(data_.animations, kActionDualAttack3);
        data_.dualAttack4Animation  = resolve_action(data_.animations, kActionDualAttack4);
        data_.dualDamageAnimation   = resolve_action(data_.animations, kActionDualDamage);
        data_.dualRunAnimation      = resolve_action(data_.animations, kActionDualRun);
        // Spears
        data_.spearReadyAnimation   = resolve_action(data_.animations, kActionSpearReady);
        data_.spearAttack1Animation = resolve_action(data_.animations, kActionSpearAttack1);
        data_.spearAttack2Animation = resolve_action(data_.animations, kActionSpearAttack2);
        data_.spearAttack3Animation = resolve_action(data_.animations, kActionSpearAttack3);
        data_.spearAttack4Animation = resolve_action(data_.animations, kActionSpearAttack4);
        data_.spearDamageAnimation  = resolve_action(data_.animations, kActionSpearDamage);
        data_.spearRunAnimation     = resolve_action(data_.animations, kActionSpearRun);
        // Crossbow
        data_.crossbowReadyAnimation  = resolve_action(data_.animations, kActionCrossbowReady);
        data_.crossbowAttackAnimation = resolve_action(data_.animations, kActionCrossbowAttack);
        data_.crossbowDamageAnimation = resolve_action(data_.animations, kActionCrossbowDamage);
        data_.crossbowRunAnimation    = resolve_action(data_.animations, kActionCrossbowRun);
        // Staff
        data_.staffReadyAnimation   = resolve_action(data_.animations, kActionStaffReady);
        data_.staffAttack1Animation = resolve_action(data_.animations, kActionStaffAttack1);
        data_.staffAttack2Animation = resolve_action(data_.animations, kActionStaffAttack2);
        data_.staffDamageAnimation  = resolve_action(data_.animations, kActionStaffDamage);
        data_.staffRunAnimation     = resolve_action(data_.animations, kActionStaffRun);
        // Reverse dagger
        data_.revDaggerReadyAnimation   = resolve_action(data_.animations, kActionRevDaggerReady);
        data_.revDaggerAttack1Animation = resolve_action(data_.animations, kActionRevDaggerAttack1);
        data_.revDaggerAttack2Animation = resolve_action(data_.animations, kActionRevDaggerAttack2);
        data_.revDaggerAttack3Animation = resolve_action(data_.animations, kActionRevDaggerAttack3);
        data_.revDaggerAttack4Animation = resolve_action(data_.animations, kActionRevDaggerAttack4);
        data_.revDaggerDamageAnimation  = resolve_action(data_.animations, kActionRevDaggerDamage);
        data_.revDaggerRunAnimation     = resolve_action(data_.animations, kActionRevDaggerRun);
        // Knuckles / claws
        data_.knuckleReadyAnimation   = resolve_action(data_.animations, kActionKnuckleReady);
        data_.knuckleAttack1Animation = resolve_action(data_.animations, kActionKnuckleAttack1);
        data_.knuckleAttack2Animation = resolve_action(data_.animations, kActionKnuckleAttack2);
        data_.knuckleAttack3Animation = resolve_action(data_.animations, kActionKnuckleAttack3);
        data_.knuckleAttack4Animation = resolve_action(data_.animations, kActionKnuckleAttack4);
        data_.knuckleDamageAnimation  = resolve_action(data_.animations, kActionKnuckleDamage);
        data_.knuckleRunAnimation     = resolve_action(data_.animations, kActionKnuckleRun);
        // Dagger
        data_.daggerReadyAnimation   = resolve_action(data_.animations, kActionDaggerReady);
        data_.daggerAttack1Animation = resolve_action(data_.animations, kActionDaggerAttack1);
        data_.daggerAttack2Animation = resolve_action(data_.animations, kActionDaggerAttack2);
        data_.daggerAttack3Animation = resolve_action(data_.animations, kActionDaggerAttack3);
        data_.daggerAttack4Animation = resolve_action(data_.animations, kActionDaggerAttack4);
        data_.daggerDamageAnimation  = resolve_action(data_.animations, kActionDaggerDamage);
        data_.daggerRunAnimation     = resolve_action(data_.animations, kActionDaggerRun);
        // Magic casting
        data_.magicReady1Animation  = resolve_action(data_.animations, kActionMagicReady1);
        data_.magicCast1Animation   = resolve_action(data_.animations, kActionMagicCast1);
        data_.magicAttack1Animation = resolve_action(data_.animations, kActionMagicAttack1);
        data_.magicReady2Animation  = resolve_action(data_.animations, kActionMagicReady2);
        data_.magicCast2Animation   = resolve_action(data_.animations, kActionMagicCast2);
        data_.magicAttack2Animation = resolve_action(data_.animations, kActionMagicAttack2);
        // Buffs
        data_.buffReady1Animation   = resolve_action(data_.animations, kActionBuffReady1);
        data_.buffCast1Animation    = resolve_action(data_.animations, kActionBuffCast1);
        data_.buffAttack1Animation  = resolve_action(data_.animations, kActionBuffAttack1);
        data_.buffReady2Animation   = resolve_action(data_.animations, kActionBuffReady2);
        data_.buffCast2Animation    = resolve_action(data_.animations, kActionBuffCast2);
        data_.buffAttack2Animation  = resolve_action(data_.animations, kActionBuffAttack2);
        data_.buffReady3Animation   = resolve_action(data_.animations, kActionBuffReady3);
        data_.buffCast3Animation    = resolve_action(data_.animations, kActionBuffCast3);
        data_.buffAttack3Animation  = resolve_action(data_.animations, kActionBuffAttack3);
        // Skills
        data_.skill1Animation       = resolve_action(data_.animations, kActionSkill1);
        data_.skill2Animation       = resolve_action(data_.animations, kActionSkill2);
        data_.skill3Animation       = resolve_action(data_.animations, kActionSkill3);
        data_.skill4Animation       = resolve_action(data_.animations, kActionSkill4);
        data_.skill5Animation       = resolve_action(data_.animations, kActionSkill5);
        data_.skill6Animation       = resolve_action(data_.animations, kActionSkill6);
        data_.skill7Animation       = resolve_action(data_.animations, kActionSkill7);
        data_.skill8Animation       = resolve_action(data_.animations, kActionSkill8);
        data_.skill9Animation       = resolve_action(data_.animations, kActionSkill9);
        data_.skill10Animation      = resolve_action(data_.animations, kActionSkill10);
        data_.skill11Animation      = resolve_action(data_.animations, kActionSkill11);
        // Other
        data_.stunAnimation         = resolve_action(data_.animations, kActionStun);
        // Emotes
        data_.emote1Animation       = resolve_action(data_.animations, kActionEmote1);
        data_.emote2Animation       = resolve_action(data_.animations, kActionEmote2);
        data_.emote3Animation       = resolve_action(data_.animations, kActionEmote3);
        data_.emote4Animation       = resolve_action(data_.animations, kActionEmote4);
        data_.emote5Animation       = resolve_action(data_.animations, kActionEmote5);
        data_.emote6Animation       = resolve_action(data_.animations, kActionEmote6);
        data_.emote7Animation       = resolve_action(data_.animations, kActionEmote7);
        data_.emote8Animation       = resolve_action(data_.animations, kActionEmote8);
        data_.emote9Animation       = resolve_action(data_.animations, kActionEmote9);
        data_.emote10Animation      = resolve_action(data_.animations, kActionEmote10);

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

        // ---- Load weapon/shield (3DO items) ----
        const auto itemRoot = dataRoot / "Item";
        const auto itemMeshRoot = itemRoot / "3do";
        const auto itemDdsRoot = itemRoot / "dds";

        auto loadItemPart = [&](WeaponType type, int index, CharacterData::WeaponPart& outPart) -> bool {
            if (type == WeaponType::None || index < 0)
                return false;
            const auto typeId = std::format("{:02d}", static_cast<int>(type));
            const auto csvPath = resolve_ci(itemRoot / (typeId + ".csv"));
            auto entry = resolve_item_from_csv(csvPath, index);
            if (!entry)
                return false;

            // Lookup cached 3DO model.
            const auto meshPath = itemMeshRoot / entry->meshName;
            const world::ItemModel* itemModel = nullptr;
            if (const auto it = cachedItemModels_.find(path_key(meshPath)); it != cachedItemModels_.end())
                itemModel = &it->second;
            else
            {
                auto loaded = world::load_item_3do(meshPath);
                if (loaded.parsed)
                {
                    auto [insertedIt, _] = cachedItemModels_.emplace(path_key(meshPath), std::move(loaded));
                    itemModel = &insertedIt->second;
                }
            }
            if (!itemModel || !itemModel->parsed)
                return false;

            // Resolve texture.
            std::filesystem::path texPath;
            if (!entry->textureName.empty())
                texPath = itemDdsRoot / entry->textureName;
            if (texPath.empty())
                return false;

            const auto textureKey = path_key(texPath);
            std::uint32_t textureIndex = 0;
            if (const auto localIt = selectedTextureSlotByPath.find(textureKey); localIt != selectedTextureSlotByPath.end())
                textureIndex = localIt->second;
            else
            {
                textureIndex = static_cast<std::uint32_t>(data_.texturePaths.size());
                selectedTextureSlotByPath.emplace(textureKey, textureIndex);
                data_.texturePaths.push_back(texPath);
            }

            // Append item vertices into the bind-pose buffer (position=0 for now, transformed at skin time).
            outPart.vertices = itemModel->vertices;
            outPart.faces = itemModel->faces;
            outPart.textureIndex = textureIndex;
            outPart.alphaCutout = entry->alphaCutout;
            outPart.vertexOffset = static_cast<std::uint32_t>(data_.bindVertices.size());
            outPart.indexOffset = static_cast<std::uint32_t>(data_.indices.size());
            outPart.vertexCount = static_cast<std::uint32_t>(itemModel->vertices.size());
            outPart.indexCount = static_cast<std::uint32_t>(itemModel->faces.size() * 3);

            // Append placeholder vertices (will be positioned during skin_and_transform).
            for (const auto& sv : itemModel->vertices)
            {
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
                gv.textureLayer = textureIndex + (entry->alphaCutout ? 2048u : 0u);
                data_.bindVertices.push_back(gv);
            }

            // Append indices.
            CharacterBatch batch{};
            batch.textureIndex = textureIndex;
            batch.startIndex = outPart.indexOffset;
            batch.alphaCutout = entry->alphaCutout;
            for (const auto& face : itemModel->faces)
            {
                data_.indices.push_back(outPart.vertexOffset + face.indices[0]);
                data_.indices.push_back(outPart.vertexOffset + face.indices[1]);
                data_.indices.push_back(outPart.vertexOffset + face.indices[2]);
            }
            batch.indexCount = outPart.indexCount;
            data_.batches.push_back(batch);
            return true;
        };

        preload_items(dataRoot);
        // Mounted characters never wield weapons/shields — they ride with empty
        // hands, so skip loading them entirely while mounted.
        if (appearance.mounted)
        {
            data_.hasWeapon = false;
            data_.hasShield = false;
        }
        else
        {
            data_.hasWeapon = loadItemPart(appearance.weaponType, appearance.weaponIndex, data_.weapon);
            data_.hasShield = loadItemPart(appearance.shieldType, appearance.shieldIndex, data_.shield);
        }

        // ---- Cloak ----
        data_.hasCloak = false;
        data_.cloakBody = {};
        data_.cloakShoulder = {};
        if (appearance.cloakIndex > 0)
        {
            // Resolve race abbreviation for cloak paths (hu, de, el, vi).
            const auto raceAbbrev = [&]() -> std::string {
                auto lower = appearance.raceFolder;
                for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower == "human") return "hu";
                if (lower == "deatheater") return "de";
                if (lower == "elf") return "el";
                if (lower == "vile") return "vi";
                return lower.substr(0, 2);
            }();

            // New flattened layout: Data/Cloak/3DC/ (all races' meshes),
            // Data/Cloak/DDS/ (all races' textures), Data/Cloak/cloak_{race}.csv.
            const auto cloakMeshDir = dataRoot / "Cloak" / "3DC";

            // Resolve texture name from the per-race CSV (cloak_index,dds).
            const auto csvPath = resolve_ci(dataRoot / "Cloak" / ("cloak_" + raceAbbrev + ".csv"));
            std::string cloakTextureName;
            {
                std::ifstream csvStream(csvPath);
                if (csvStream)
                {
                    std::string line;
                    std::getline(csvStream, line); // header
                    while (std::getline(csvStream, line))
                    {
                        if (line.empty()) continue;
                        const auto comma = line.find(',');
                        if (comma == std::string::npos) continue;
                        int idx = 0;
                        try { idx = std::stoi(line.substr(0, comma)); }
                        catch (...) { continue; }
                        if (idx == appearance.cloakIndex)
                        {
                            cloakTextureName = line.substr(comma + 1);
                            // Trim trailing CR/whitespace.
                            while (!cloakTextureName.empty() &&
                                   (cloakTextureName.back() == '\r' || cloakTextureName.back() == '\n' ||
                                    cloakTextureName.back() == ' ' || cloakTextureName.back() == '\t'))
                                cloakTextureName.pop_back();
                            break;
                        }
                    }
                }
            }

            // Load a cloak 3DC mesh by filename from the 3dc/ folder.
            // Phase 1: all cloak designs use the same generic {prefix}_mentle_*.3dc meshes.
            // The _l variant is the main cloth body; _hl is a longer heavy variant (fallback).
            // The _s variant is a shorter duplicate of _l — skipped for Phase 1.
            auto loadCloakMesh = [&](const std::string& filename, CharacterData::WeaponPart& outPart) -> bool {
                const auto meshPath = resolve_ci(cloakMeshDir / filename);
                auto model = world::load_cloak_3dc(meshPath);
                if (!model.parsed || model.vertices.empty())
                    return false;

                // Resolve texture slot.
                std::filesystem::path texPath;
                if (!cloakTextureName.empty())
                    texPath = resolve_ci(dataRoot / "Cloak" / "DDS" / cloakTextureName);
                if (texPath.empty())
                    return false;

                const auto textureKey = path_key(texPath);
                std::uint32_t textureIndex = 0;
                if (const auto localIt = selectedTextureSlotByPath.find(textureKey); localIt != selectedTextureSlotByPath.end())
                    textureIndex = localIt->second;
                else
                {
                    textureIndex = static_cast<std::uint32_t>(data_.texturePaths.size());
                    selectedTextureSlotByPath.emplace(textureKey, textureIndex);
                    data_.texturePaths.push_back(texPath);
                }

                outPart.vertices.resize(model.vertices.size());
                for (std::size_t v = 0; v < model.vertices.size(); ++v)
                {
                    auto& dst = outPart.vertices[v];
                    const auto& src = model.vertices[v];
                    dst.position[0] = src.position[0];
                    dst.position[1] = src.position[1];
                    dst.position[2] = src.position[2];
                    dst.normal[0] = src.normal[0];
                    dst.normal[1] = src.normal[1];
                    dst.normal[2] = src.normal[2];
                    dst.uv[0] = src.uv[0];
                    dst.uv[1] = src.uv[1];
                }
                outPart.faces = model.faces;
                outPart.textureIndex = textureIndex;
                outPart.alphaCutout = false;
                outPart.vertexOffset = static_cast<std::uint32_t>(data_.bindVertices.size());
                outPart.indexOffset = static_cast<std::uint32_t>(data_.indices.size());
                outPart.vertexCount = static_cast<std::uint32_t>(model.vertices.size());
                outPart.indexCount = static_cast<std::uint32_t>(model.faces.size() * 3);

                for (const auto& sv : model.vertices)
                {
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
                    gv.textureLayer = textureIndex;
                    data_.bindVertices.push_back(gv);
                }

                CharacterBatch batch{};
                batch.textureIndex = textureIndex;
                batch.startIndex = outPart.indexOffset;
                batch.alphaCutout = false;
                for (const auto& face : model.faces)
                {
                    data_.indices.push_back(outPart.vertexOffset + face.indices[0]);
                    data_.indices.push_back(outPart.vertexOffset + face.indices[1]);
                    data_.indices.push_back(outPart.vertexOffset + face.indices[2]);
                }
                batch.indexCount = outPart.indexCount;
                data_.batches.push_back(batch);
                return true;
            };

            // Build mesh filename from prefix: {prefix}_mentle_l.3dc
            // Lowercase prefix to match filenames on disk.
            auto lowerPrefix = appearance.prefix;
            for (auto& c : lowerPrefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            const auto bodyFilename    = lowerPrefix + "_mentle_l.3dc";
            const auto bodyFilenameFallback = lowerPrefix + "_mentle_hl.3dc";

            bool bodyLoaded = loadCloakMesh(bodyFilename, data_.cloakBody);
            if (!bodyLoaded)
                bodyLoaded = loadCloakMesh(bodyFilenameFallback, data_.cloakBody);

            // ---- Cloak shoulder (static skinned collar) ----
            // The real shoulder pieces live in 3dc/static/{prefix}_mentle{NNN}_l.3dc.
            // Unlike the boneless body cloth, these are full skinned meshes in the
            // regular character 3DC format (int32 version + bone matrices + skinned
            // vertices), rigged to the body skeleton. We route them through the normal
            // body-part path (load_character_3dc + append_loaded_part) so the main skin
            // loop animates them in sync with the body via clientFinals.
            // 8 designs (000-007) exist per race; design = (cloakIndex-1) % 8.
            // The shoulder shares the body's cloth texture (resolved from the CSV).
            if (bodyLoaded && !cloakTextureName.empty())
            {
                const std::filesystem::path shoulderTexPath =
                    resolve_ci(dataRoot / "Cloak" / "DDS" / cloakTextureName);
                if (!shoulderTexPath.empty())
                {
                    const int design = (appearance.cloakIndex - 1) % 8;
                    const auto shoulderFilename =
                        std::format("{}_mentle{:03d}_l.3dc", lowerPrefix, design);
                    const auto shoulderPath = resolve_ci(cloakMeshDir / shoulderFilename);
                    auto shoulderModel = world::load_character_3dc(shoulderPath);
                    if (shoulderModel.parsed && !shoulderModel.vertices.empty())
                    {
                        const auto textureKey = path_key(shoulderTexPath);
                        std::uint32_t textureIndex = 0;
                        if (const auto it = selectedTextureSlotByPath.find(textureKey);
                            it != selectedTextureSlotByPath.end())
                            textureIndex = it->second;
                        else
                        {
                            textureIndex = static_cast<std::uint32_t>(data_.texturePaths.size());
                            selectedTextureSlotByPath.emplace(textureKey, textureIndex);
                            data_.texturePaths.push_back(shoulderTexPath);
                        }
                        append_loaded_part(data_, shoulderModel, textureIndex, false);
                    }
                }
            }

            data_.hasCloak = bodyLoaded;
        }

        // ---- Mount (vehicle) ----
        // Vehicles are monsters game-mechanically and use the monster formats:
        // a skinned 3DC body + ANI clips (idle/walk/run/jump/br). The class CSV
        // (vehicle_{class}_01.csv) maps RecordIndex -> meshes/textures/anim files.
        if (appearance.mounted)
        {
            const auto vehicleRoot = dataRoot / "Vehicle";
            const auto csvPath = resolve_ci(vehicleRoot / ("vehicle_" + appearance.mountClass + "_01.csv"));

            // Minimal quoted-CSV field splitter (handles "" escapes).
            auto splitCsv = [](const std::string& line) {
                std::vector<std::string> fields;
                std::size_t pos = 0;
                while (pos <= line.size())
                {
                    std::string field;
                    if (pos < line.size() && line[pos] == '"')
                    {
                        ++pos;
                        while (pos < line.size())
                        {
                            if (line[pos] == '"')
                            {
                                if (pos + 1 < line.size() && line[pos + 1] == '"') { field += '"'; pos += 2; }
                                else { ++pos; break; }
                            }
                            else field += line[pos++];
                        }
                        if (pos < line.size() && line[pos] == ',') ++pos;
                    }
                    else
                    {
                        auto comma = line.find(',', pos);
                        if (comma == std::string::npos) { field = line.substr(pos); pos = line.size() + 1; }
                        else { field = line.substr(pos, comma - pos); pos = comma + 1; }
                    }
                    fields.push_back(std::move(field));
                    if (pos > line.size()) break;
                }
                return fields;
            };

            std::vector<std::string> row;
            {
                std::ifstream csv(csvPath);
                if (csv)
                {
                    std::string line;
                    std::getline(csv, line); // header
                    while (std::getline(csv, line))
                    {
                        if (line.empty()) continue;
                        auto fields = splitCsv(line);
                        if (fields.size() < 26) continue;
                        int rec = -1;
                        try { rec = std::stoi(fields[2]); } catch (...) { continue; }
                        if (rec == appearance.mountIndex) { row = std::move(fields); break; }
                    }
                }
            }

            if (!row.empty())
            {
                // Column layout (MON->CSV): 5=Walk 6=Run 7=Jump 11=Breath
                // 13=Idle 24=Objects 25=Height.
                const std::string& objects = row[24];

                // Append a mount mesh part: GPU verts into bindVertices/indices/
                // batches (shared render path); source verts + bones into mount.*
                // so the rider's skin loop never sees them.
                auto appendMountPart = [&](const world::CharacterModel& model, std::uint32_t textureIndex) {
                    const auto baseVertex = static_cast<std::uint32_t>(data_.bindVertices.size());
                    const auto meshBoneBase = static_cast<std::uint32_t>(data_.mount.meshBones.size());
                    const auto meshBoneCount = static_cast<std::uint32_t>(model.bones.size());
                    data_.mount.meshBones.insert(data_.mount.meshBones.end(), model.bones.begin(), model.bones.end());

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
                        sv.gpuIndex = static_cast<std::uint32_t>(data_.bindVertices.size());
                        data_.mount.sourceVertices.push_back(sv);

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
                        gv.textureLayer = textureIndex;
                        data_.bindVertices.push_back(gv);
                    }

                    CharacterBatch batch{};
                    batch.textureIndex = textureIndex;
                    batch.startIndex = static_cast<std::uint32_t>(data_.indices.size());
                    batch.alphaCutout = false;
                    for (const auto& face : model.faces)
                    {
                        data_.indices.push_back(baseVertex + face.indices[0]);
                        data_.indices.push_back(baseVertex + face.indices[1]);
                        data_.indices.push_back(baseVertex + face.indices[2]);
                    }
                    batch.indexCount = static_cast<std::uint32_t>(data_.indices.size()) - batch.startIndex;
                    data_.batches.push_back(batch);
                };

                data_.mount.vertexOffset = static_cast<std::uint32_t>(data_.bindVertices.size());
                bool anyPart = false;
                std::size_t objStart = 0;
                while (objStart < objects.size())
                {
                    auto bar = objects.find('|', objStart);
                    const auto chunk = objects.substr(objStart, bar == std::string::npos ? std::string::npos : bar - objStart);
                    objStart = (bar == std::string::npos) ? objects.size() : bar + 1;
                    const auto colon = chunk.find(':');
                    if (colon == std::string::npos) continue;
                    const auto meshName = trim(chunk.substr(0, colon));
                    const auto texName = trim(chunk.substr(colon + 1));
                    if (meshName.empty() || texName.empty()) continue;

                    const auto meshPath = resolve_ci(vehicleRoot / "3dc" / meshName);
                    auto model = world::load_character_3dc(meshPath);
                    if (!model.parsed || model.vertices.empty()) continue;

                    const auto texPath = resolve_ci(vehicleRoot / "dds" / texName);
                    if (texPath.empty()) continue;
                    const auto textureKey = path_key(texPath);
                    std::uint32_t textureIndex = 0;
                    if (const auto it = selectedTextureSlotByPath.find(textureKey); it != selectedTextureSlotByPath.end())
                        textureIndex = it->second;
                    else
                    {
                        textureIndex = static_cast<std::uint32_t>(data_.texturePaths.size());
                        selectedTextureSlotByPath.emplace(textureKey, textureIndex);
                        data_.texturePaths.push_back(texPath);
                    }
                    appendMountPart(model, textureIndex);
                    anyPart = true;
                }
                data_.mount.vertexCount =
                    static_cast<std::uint32_t>(data_.bindVertices.size()) - data_.mount.vertexOffset;

                if (anyPart)
                {
                    // Load mount animation clips (exact filenames from the CSV).
                    auto loadMountAni = [&](const std::string& rawFile) -> std::size_t {
                        const auto file = trim(rawFile);
                        if (file.empty() || file == "LOAD" || file == "load")
                            return 0;
                        const auto aniPath = resolve_ci(vehicleRoot / "ani" / file);
                        CharacterAnimationChoice choice{};
                        choice.path = aniPath;
                        choice.name = file;
                        choice.animation = world::load_character_ani(aniPath);
                        data_.mount.animations.push_back(std::move(choice));
                        return data_.mount.animations.size() - 1;
                    };
                    data_.mount.idleAnimation   = loadMountAni(row[13]);
                    data_.mount.walkAnimation   = loadMountAni(row[5]);
                    data_.mount.runAnimation    = loadMountAni(row[6]);
                    data_.mount.jumpAnimation   = loadMountAni(row[7]);
                    data_.mount.breathAnimation = loadMountAni(row[11]);
                    data_.mount.scale = 1.0f;
                    data_.mount.loaded = true;
                    data_.hasMount = true;
                }
            }

            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "Mount load: class=" << appearance.mountClass
                << " index=" << appearance.mountIndex
                << " hasMount=" << data_.hasMount
                << " verts=" << data_.mount.vertexCount
                << " bones=" << data_.mount.meshBones.size()
                << " anims=" << data_.mount.animations.size() << "\n";
        }

        const bool hasValidAnimation = std::any_of(data_.animations.begin(), data_.animations.end(),
            [](const auto& c) { return c.animation.parsed; });
        data_.loaded = parsedParts > 0 && hasValidAnimation;
        worldVertices_ = data_.bindVertices;
        reset_cloth();

        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
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
                << " weapon=" << data_.hasWeapon
                << " shield=" << data_.hasShield
                << " ready=" << data_.loaded << "\n";

            // Dump bone hierarchy from idle animation for skeleton inspection.
            if (data_.loaded && data_.idleAnimation < data_.animations.size()
                && data_.animations[data_.idleAnimation].animation.parsed)
            {
                const auto& anim = data_.animations[data_.idleAnimation].animation;
                log << "Bone hierarchy (" << anim.bones.size() << " bones):\n";
                // Compute finals at frame 0 to get world-space positions.
                const auto finals = compute_client_finals(anim, static_cast<float>(anim.startKeyframe));
                for (std::size_t i = 0; i < anim.bones.size(); ++i)
                {
                    const auto& bone = anim.bones[i];
                    float wx = finals[i].m[3][0];
                    float wy = finals[i].m[3][1];
                    float wz = finals[i].m[3][2];
                    log << "  Bone " << i
                        << " parent=" << bone.parentBoneIndex
                        << " pos=(" << wx << ", " << wy << ", " << wz << ")"
                        << " rotFrames=" << bone.rotationFrames.size()
                        << " transFrames=" << bone.translationFrames.size()
                        << "\n";
                }

                // Also dump mesh bone bind-pose positions for reference.
                log << "Mesh bones (" << data_.meshBones.size() << "):\n";
                for (std::size_t i = 0; i < data_.meshBones.size(); ++i)
                {
                    const auto m = mat4_from_shaiya_transposed(data_.meshBones[i].matrix);
                    log << "  MeshBone " << i
                        << " pos=(" << m.m[3][0] << ", " << m.m[3][1] << ", " << m.m[3][2] << ")\n";
                }
            }
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
                inWater_ = characterY_ <= swimStartY;
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
            const float speed = data_.hasMount
                ? (input.fast ? kMountFastSpeed : kMountSpeed)
                : (inWater_ ? kSwimSpeed : (input.fast ? kFastRunSpeed : kRunSpeed));
            characterX_ += moveX * speed * clampedDelta;
            if (inWater_)
                characterY_ += moveY * speed * clampedDelta;
            characterZ_ += moveZ * speed * clampedDelta;
            const bool strafingOrBacking = backwardPressed || ((leftPressed || rightPressed) && !forwardPressed);
            // A mount cannot strafe sideways — it always turns to face the
            // direction of travel. On foot, strafing/backing keeps facing forward.
            characterYaw_ = (data_.hasMount || !strafingOrBacking)
                ? std::atan2(moveX, moveZ)
                : cameraYaw_;
        }

        // ---- Collision with world objects ----
        if (collisionFn_ && moving)
        {
            float adjustedX = characterX_;
            float adjustedZ = characterZ_;
            const float speed = data_.hasMount
                ? (input.fast ? kMountFastSpeed : kMountSpeed)
                : (inWater_ ? kSwimSpeed : (input.fast ? kFastRunSpeed : kRunSpeed));
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
            const float floatY = floatLevelY;
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
        if (data_.hasMount)
        {
            // Mounted: the rider plays its seated (vehicle) animations. Standard
            // mounts use the same seated pose for walk and run (vehicleRun1);
            // vehicleRun2 is reserved for special mounts (e.g. skateboard).
            if (moving)
                desiredAnimation = data_.vehicleRun1Animation;
            else
                desiredAnimation = data_.vehicleIdleAnimation;
        }
        else if (inWater_)
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
        // Native playback is a single fixed rate (30 fps) for all states — the retail
        // client applies no per-state speedups. Foot cadence is kept in lock-step with
        // ground travel by deriving the rate from the real movement speed: the rate is
        // the ratio of the character's current locomotion speed to the speed the cycle
        // was authored for. At native movement speeds this ratio is 1.0 (pure native
        // playback); it only departs from 1.0 if a future speed modifier is applied,
        // in which case the stride automatically tracks the new speed instead of
        // sliding. Non-locomotion states (idle, jump, swim-idle) always play at 1.0.
        float animationRate = 1.0f;
        if (moving && grounded_ && !inWater_)
        {
            const float locomotionSpeed = input.fast ? kFastRunSpeed : kRunSpeed;
            const float authoredCycleSpeed = input.fast ? kFastRunSpeed : kRunSpeed;
            animationRate = locomotionSpeed / authoredCycleSpeed;
        }
        else if (inWater_ && moving)
        {
            animationRate = kSwimSpeed / kSwimSpeed;
        }
        animationSeconds_ += clampedDelta * animationRate;

        // ---- Mount animation selection ----
        if (data_.hasMount)
        {
            // Stationary default is breathing (br); every so often play the idle
            // variation once, then return to breathing.
            std::size_t desiredMount = data_.mount.breathAnimation;
            if (!grounded_)
            {
                desiredMount = data_.mount.jumpAnimation;
                mountIdleTimer_ = 0.0f;
            }
            else if (moving)
            {
                desiredMount = input.fast ? data_.mount.runAnimation : data_.mount.walkAnimation;
                mountIdleTimer_ = 0.0f;
            }
            else
            {
                constexpr float kIdleEvery = 11.0f;   // seconds of breathing between idles
                constexpr float kIdleWindow = 3.0f;   // how long the idle variation plays
                mountIdleTimer_ += clampedDelta;
                if (mountIdleTimer_ >= kIdleEvery)
                {
                    desiredMount = data_.mount.idleAnimation;
                    if (mountIdleTimer_ >= kIdleEvery + kIdleWindow)
                        mountIdleTimer_ = 0.0f;
                }
            }
            if (desiredMount != mountActiveAnimation_)
            {
                mountActiveAnimation_ = desiredMount;
                mountAnimationSeconds_ = 0.0f;
            }
            mountAnimationSeconds_ += clampedDelta * animationRate;
        }

        // ---- Skinning + world transform ----
        lastDeltaSeconds_ = clampedDelta;
        skin_and_transform();
    }

    void CharacterSystem::skin_and_transform()
    {
        weaponAttachment_.valid = false;

        const auto& anim = data_.animations[activeAnimation_].animation;
        if (!anim.parsed || anim.endKeyframe <= anim.startKeyframe)
        {
            worldVertices_ = data_.bindVertices;
            return;
        }

        const float startFrame = static_cast<float>(anim.startKeyframe);
        const float endFrame = static_cast<float>(anim.endKeyframe);
        const float frameCount = std::max(1.0f, endFrame - startFrame);

        // Jump/fall: play the take-off once, then HOLD a mid-air pose until landing
        // instead of looping the whole jump clip (which looked like the character
        // re-jumping repeatedly during a long fall). When the character lands,
        // grounded_ flips and the state machine switches back to idle/run.
        float frame;
        const bool airborneJump = !grounded_ && !inWater_ && !data_.hasMount
            && activeAnimation_ == data_.jumpAnimation;
        if (airborneJump)
        {
            constexpr float kJumpHoldFrameFraction = 0.80f;  // mid-air "falling" pose
            const float holdFrame = frameCount * kJumpHoldFrameFraction;
            const float raw = animationSeconds_ * kAniFramesPerSecond;
            frame = startFrame + std::min(raw, holdFrame);
        }
        else
        {
            frame = startFrame + std::fmod(animationSeconds_ * kAniFramesPerSecond, frameCount);
        }
        const auto clientFinals = compute_client_finals(anim, frame);

        // Skin into local-space animated vertices.
        std::vector<CharacterGpuVertex> animated = data_.bindVertices;
        float minLocalY = std::numeric_limits<float>::max();

        for (std::size_t i = 0; i < data_.sourceVertices.size(); ++i)
        {
            const auto& source = data_.sourceVertices[i];
            const auto vi = static_cast<std::size_t>(source.gpuIndex);
            if (vi >= animated.size())
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

            animated[vi].position[0] = position.x * kCharacterScale;
            animated[vi].position[1] = position.y * kCharacterScale;
            animated[vi].position[2] = position.z * kCharacterScale;
            animated[vi].normal[0] = normal.x;
            animated[vi].normal[1] = normal.y;
            animated[vi].normal[2] = normal.z;

            minLocalY = std::min(minLocalY, animated[vi].position[1]);
        }

        // ---- Transform weapon/shield vertices by hand bone ----
        auto transformItemPart = [&](const CharacterData::WeaponPart& part, std::size_t boneIndex) {
            if (part.vertexCount == 0 || boneIndex >= clientFinals.size())
                return;
            const auto boneMatrix = clientFinals[boneIndex];
            for (std::uint32_t i = 0; i < part.vertexCount; ++i)
            {
                const auto vi = static_cast<std::size_t>(part.vertexOffset) + i;
                if (vi >= animated.size()) break;
                const auto& sv = part.vertices[i];
                const Vec3 srcPos{ sv.position[0], sv.position[1], sv.position[2] };
                const Vec3 srcNrm{ sv.normal[0], sv.normal[1], sv.normal[2] };
                const auto p = transform_point(boneMatrix, srcPos);
                const auto n = normalize_vec3(transform_normal(boneMatrix, srcNrm));
                animated[vi].position[0] = p.x * kCharacterScale;
                animated[vi].position[1] = p.y * kCharacterScale;
                animated[vi].position[2] = p.z * kCharacterScale;
                animated[vi].normal[0] = n.x;
                animated[vi].normal[1] = n.y;
                animated[vi].normal[2] = n.z;
                minLocalY = std::min(minLocalY, animated[vi].position[1]);
            }
        };
        if (data_.hasWeapon && weaponBoneIndex >= 0)
            transformItemPart(data_.weapon, static_cast<std::size_t>(weaponBoneIndex));
        if (data_.hasShield && shieldBoneIndex >= 0)
            transformItemPart(data_.shield, static_cast<std::size_t>(shieldBoneIndex));
        // Cloak vertices are in character-local space (not bone-local), so they
        // are left at their bind positions here. The world transform applied below
        // (yaw + translation) correctly positions them in the world. Bone-following
        // cloth simulation is deferred to Phase 2.
        (void)cloakBodyBoneIndex;
        (void)cloakShoulderBoneIndex;

        // ---- Mount skinning + rider seating ----
        // The mount has its own skeleton and animation timeline. We skin it into
        // its own vertex range, find the saddle bone's world-local position, then
        // shift the entire rider (rider + weapon/shield/cloak) so it sits on that
        // bone. Both rider and mount then go through the shared yaw/translate pass.
        Vec3 riderSaddleOffset{ 0.0f, 0.0f, 0.0f };
        if (data_.hasMount && data_.mount.vertexCount > 0)
        {
            const float mountScale = kCharacterScale * data_.mount.scale;
            const auto& mountChoice = data_.mount.animations.empty()
                ? data_.animations[activeAnimation_]
                : data_.mount.animations[std::min(mountActiveAnimation_, data_.mount.animations.size() - 1)];
            const auto& mountAnim = mountChoice.animation;

            std::vector<Mat4> mountFinals;
            if (mountAnim.parsed && mountAnim.endKeyframe > mountAnim.startKeyframe)
            {
                const float ms = static_cast<float>(mountAnim.startKeyframe);
                const float me = static_cast<float>(mountAnim.endKeyframe);
                const float mfc = std::max(1.0f, me - ms);
                // Jump/fall: hold a mid-air pose instead of looping the jump clip
                // (same behaviour as the on-foot rider, see skin_and_transform frame).
                float mFrame;
                const bool mountAirborneJump = !grounded_ && !inWater_
                    && mountActiveAnimation_ == data_.mount.jumpAnimation;
                if (mountAirborneJump)
                {
                    constexpr float kJumpHoldFrameFraction = 0.80f;
                    const float holdFrame = mfc * kJumpHoldFrameFraction;
                    mFrame = ms + std::min(mountAnimationSeconds_ * kAniFramesPerSecond, holdFrame);
                }
                else
                {
                    mFrame = ms + std::fmod(mountAnimationSeconds_ * kAniFramesPerSecond, mfc);
                }
                mountFinals = compute_client_finals(mountAnim, mFrame);

                for (const auto& source : data_.mount.sourceVertices)
                {
                    const auto vi = static_cast<std::size_t>(source.gpuIndex);
                    if (vi >= animated.size())
                        continue;
                    Vec3 position{};
                    Vec3 normal{};
                    float totalWeight = 0.0f;
                    for (std::size_t influence = 0; influence < 3; ++influence)
                    {
                        const auto boneIndex = static_cast<std::size_t>(source.bones[influence]);
                        if (boneIndex >= source.meshBoneCount || boneIndex >= mountFinals.size())
                            continue;
                        const float weight = std::max(0.0f, source.weights[influence]);
                        if (weight <= 0.0001f)
                            continue;
                        const auto meshBoneIdx = static_cast<std::size_t>(source.meshBoneBase) + boneIndex;
                        if (meshBoneIdx >= data_.mount.meshBones.size())
                            continue;
                        const auto meshBone = mat4_from_shaiya_transposed(data_.mount.meshBones[meshBoneIdx].matrix);
                        const auto skinMatrix = mat4_multiply(meshBone, mountFinals[boneIndex]);
                        const Vec3 srcPos{ source.position[0], source.position[1], source.position[2] };
                        const Vec3 srcNrm{ source.normal[0], source.normal[1], source.normal[2] };
                        const auto p = transform_point(skinMatrix, srcPos);
                        const auto n = transform_normal(skinMatrix, srcNrm);
                        position.x += p.x * weight; position.y += p.y * weight; position.z += p.z * weight;
                        normal.x += n.x * weight; normal.y += n.y * weight; normal.z += n.z * weight;
                        totalWeight += weight;
                    }
                    if (totalWeight <= 0.0001f)
                        continue;
                    position.x /= totalWeight; position.y /= totalWeight; position.z /= totalWeight;
                    normal = normalize_vec3({ normal.x / totalWeight, normal.y / totalWeight, normal.z / totalWeight });
                    animated[vi].position[0] = position.x * mountScale;
                    animated[vi].position[1] = position.y * mountScale;
                    animated[vi].position[2] = position.z * mountScale;
                    animated[vi].normal[0] = normal.x;
                    animated[vi].normal[1] = normal.y;
                    animated[vi].normal[2] = normal.z;
                }
            }

            // Seat the rider on the mount's saddle bone (default bone 25).
            Vec3 saddle{ 0.0f, 0.0f, 0.0f };
            if (!mountFinals.empty())
            {
                const auto b = static_cast<std::size_t>(std::max(0, mountBoneIndex));
                if (b < mountFinals.size())
                {
                    const auto t = mat4_get_translation(mountFinals[b]);
                    saddle = { t.x * mountScale, t.y * mountScale, t.z * mountScale };
                }
            }
            riderSaddleOffset = saddle;
            for (std::size_t i = 0; i < static_cast<std::size_t>(data_.mount.vertexOffset) && i < animated.size(); ++i)
            {
                animated[i].position[0] += saddle.x;
                animated[i].position[1] += saddle.y;
                animated[i].position[2] += saddle.z;
            }

            // Re-derive ground contact from the full (mount + seated rider) set.
            minLocalY = std::numeric_limits<float>::max();
            for (const auto& v : animated)
                minLocalY = std::min(minLocalY, v.position[1]);
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

        // ---- Weapon attach-bone world transform (for weapon aura effects) ----
        // Mirrors the item-vertex path: bone matrix -> *kCharacterScale -> (+saddle
        // if mounted) -> yaw rotate + ground offset + world translate. Computed from
        // clientFinals (not the skinned verts) so it is exact regardless of mesh.
        if (data_.hasWeapon && weaponBoneIndex >= 0
            && static_cast<std::size_t>(weaponBoneIndex) < clientFinals.size())
        {
            const auto& boneMatrix = clientFinals[static_cast<std::size_t>(weaponBoneIndex)];
            const Vec3 boneT = mat4_get_translation(boneMatrix);
            const float lx = boneT.x * kCharacterScale + riderSaddleOffset.x;
            const float ly = boneT.y * kCharacterScale + riderSaddleOffset.y;
            const float lz = boneT.z * kCharacterScale + riderSaddleOffset.z;

            weaponAttachment_.position[0] = lx * cosYaw + lz * sinYaw + characterX_;
            weaponAttachment_.position[1] = ly - minLocalY + characterY_ + kGroundClearance;
            weaponAttachment_.position[2] = -lx * sinYaw + lz * cosYaw + characterZ_;

            const Vec3 axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            for (int a = 0; a < 3; ++a)
            {
                Vec3 v = transform_normal(boneMatrix, axes[a]);
                v.x *= kCharacterScale; v.y *= kCharacterScale; v.z *= kCharacterScale;
                weaponAttachment_.basis[a * 3 + 0] = v.x * cosYaw + v.z * sinYaw;
                weaponAttachment_.basis[a * 3 + 1] = v.y;
                weaponAttachment_.basis[a * 3 + 2] = -v.x * sinYaw + v.z * cosYaw;
            }
            weaponAttachment_.valid = true;
        }

        // ---- Cloak cloth simulation (Verlet, world space) ----
        // At this point `animated` holds final world-space positions for every
        // vertex, including the cloak body sitting at its rest (bind) world pose.
        // We treat the cloak body as a 5xR grid: row 0 (top) is pinned to its
        // rest world position (so it rides with the body), rows 1..R-1 are free
        // particles that lag behind via Verlet inertia, producing real cloth sway
        // when the character moves or turns. Distance constraints (rest lengths =
        // authored bind grid spacing, recovered from the .PC format) preserve the
        // cloak's shape.
        if (data_.hasCloak && data_.cloakBody.vertexCount >= (kClothCols * 2u) &&
            (data_.cloakBody.vertexCount % kClothCols) == 0u)
        {
            const std::uint32_t off  = data_.cloakBody.vertexOffset;
            const std::uint32_t n    = data_.cloakBody.vertexCount;
            const std::uint32_t cols = kClothCols;
            const std::uint32_t rows = n / cols;
            const auto idx = [&](std::uint32_t r, std::uint32_t c) { return r * cols + c; };

            // (Re)initialize state to the current rest world pose.
            if (!clothInitialized_ || clothRows_ != rows ||
                clothWorld_.size() != static_cast<std::size_t>(n) * 3u)
            {
                clothRows_ = rows;
                clothWorld_.assign(static_cast<std::size_t>(n) * 3u, 0.0f);
                clothPrev_.assign(static_cast<std::size_t>(n) * 3u, 0.0f);
                clothRestUp_.assign(n, 0.0f);
                clothRestLeft_.assign(n, 0.0f);
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    for (int a = 0; a < 3; ++a)
                    {
                        clothWorld_[i * 3 + a] = animated[off + i].position[a];
                        clothPrev_[i * 3 + a]  = animated[off + i].position[a];
                    }
                }
                // Rest lengths are invariant (rigid bind shape), so compute them
                // from the bind world positions in worldVertices_/bindVertices is
                // not stable across frames; use the current rest world pose, which
                // equals the bind shape rigidly transformed (distances preserved).
                auto restDist = [&](std::uint32_t ia, std::uint32_t ib) {
                    const float dx = animated[off + ia].position[0] - animated[off + ib].position[0];
                    const float dy = animated[off + ia].position[1] - animated[off + ib].position[1];
                    const float dz = animated[off + ia].position[2] - animated[off + ib].position[2];
                    return std::sqrt(dx * dx + dy * dy + dz * dz);
                };
                for (std::uint32_t r = 0; r < rows; ++r)
                    for (std::uint32_t c = 0; c < cols; ++c)
                    {
                        const std::uint32_t i = idx(r, c);
                        clothRestUp_[i]   = (r > 0) ? restDist(i, idx(r - 1, c)) : 0.0f;
                        clothRestLeft_[i] = (c > 0) ? restDist(i, idx(r, c - 1)) : 0.0f;
                    }

                // Anchor the pinned top row to the nearest skinned body vertices
                // (the collar / upper back). These follow the animation — including
                // jumps and turns — so the cloak stays attached coherently, exactly
                // like a bone-attached weapon. The stored offset is the constant
                // local displacement between the cloak's bind position and the body
                // vertex's bind position; re-added (yaw-rotated) every frame.
                clothPinBody_.assign(cols, UINT32_MAX);
                clothPinOffset_.assign(static_cast<std::size_t>(cols) * 3u, 0.0f);
                for (std::uint32_t c = 0; c < cols; ++c)
                {
                    const std::uint32_t ci = off + idx(0, c);
                    const float cx = data_.bindVertices[ci].position[0];
                    const float cy = data_.bindVertices[ci].position[1];
                    const float cz = data_.bindVertices[ci].position[2];
                    float bestDist = std::numeric_limits<float>::max();
                    std::uint32_t bestSlot = UINT32_MAX;
                    for (const auto& sv : data_.sourceVertices)
                    {
                        const std::uint32_t slot = sv.gpuIndex;
                        if (slot >= data_.bindVertices.size()) continue;
                        // Skip the cloak body's own vertices.
                        if (slot >= off && slot < off + n) continue;
                        const float dx = data_.bindVertices[slot].position[0] - cx;
                        const float dy = data_.bindVertices[slot].position[1] - cy;
                        const float dz = data_.bindVertices[slot].position[2] - cz;
                        const float d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < bestDist) { bestDist = d2; bestSlot = slot; }
                    }
                    clothPinBody_[c] = bestSlot;
                    if (bestSlot != UINT32_MAX)
                    {
                        clothPinOffset_[c * 3 + 0] = cx - data_.bindVertices[bestSlot].position[0];
                        clothPinOffset_[c * 3 + 1] = cy - data_.bindVertices[bestSlot].position[1];
                        clothPinOffset_[c * 3 + 2] = cz - data_.bindVertices[bestSlot].position[2];
                    }
                }
                clothInitialized_ = true;
            }

            // Pin row 0 to the skinned body anchor (collar/upper back) plus the
            // constant local offset, yaw-rotated into world. This makes the cloak's
            // attachment follow the animation exactly (idle lean, turns, jumps).
            const float pinYaw = characterYaw_ + kPi;
            const float pinCos = std::cos(pinYaw);
            const float pinSin = std::sin(pinYaw);
            for (std::uint32_t c = 0; c < cols; ++c)
            {
                const std::uint32_t i = idx(0, c);
                const std::uint32_t slot = (c < clothPinBody_.size()) ? clothPinBody_[c] : UINT32_MAX;
                float px, py, pz;
                if (slot != UINT32_MAX && slot < animated.size())
                {
                    const float ox = clothPinOffset_[c * 3 + 0];
                    const float oy = clothPinOffset_[c * 3 + 1];
                    const float oz = clothPinOffset_[c * 3 + 2];
                    px = animated[slot].position[0] + (ox * pinCos + oz * pinSin);
                    py = animated[slot].position[1] + oy;
                    pz = animated[slot].position[2] + (-ox * pinSin + oz * pinCos);
                }
                else
                {
                    px = animated[off + i].position[0];
                    py = animated[off + i].position[1];
                    pz = animated[off + i].position[2];
                }
                clothWorld_[i * 3 + 0] = px; clothPrev_[i * 3 + 0] = px;
                clothWorld_[i * 3 + 1] = py; clothPrev_[i * 3 + 1] = py;
                clothWorld_[i * 3 + 2] = pz; clothPrev_[i * 3 + 2] = pz;
            }

            // Verlet integration for free particles (rows 1..R-1).
            const float dt = std::clamp(lastDeltaSeconds_, 0.001f, 0.05f);
            constexpr float kDamping = 0.985f;       // velocity retention
            constexpr float kClothGravity = -2.4f;   // world units / s^2 (downward)
            const float gStep = kClothGravity * dt * dt;
            for (std::uint32_t i = cols; i < n; ++i)
            {
                for (int a = 0; a < 3; ++a)
                {
                    const float cur  = clothWorld_[i * 3 + a];
                    const float prev = clothPrev_[i * 3 + a];
                    float next = cur + (cur - prev) * kDamping;
                    if (a == 1) next += gStep;
                    clothPrev_[i * 3 + a] = cur;
                    clothWorld_[i * 3 + a] = next;
                }
            }

            // Distance-constraint relaxation. Row 0 is immovable (pinned).
            auto satisfy = [&](std::uint32_t ia, std::uint32_t ib, float rest) {
                if (rest <= 1e-6f) return;
                const float dx = clothWorld_[ib * 3 + 0] - clothWorld_[ia * 3 + 0];
                const float dy = clothWorld_[ib * 3 + 1] - clothWorld_[ia * 3 + 1];
                const float dz = clothWorld_[ib * 3 + 2] - clothWorld_[ia * 3 + 2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d <= 1e-6f) return;
                const bool aPinned = (ia < cols);
                const bool bPinned = (ib < cols);
                if (aPinned && bPinned) return;
                const float diff = (d - rest) / d;
                float wa = aPinned ? 0.0f : 1.0f;
                float wb = bPinned ? 0.0f : 1.0f;
                const float inv = 1.0f / (wa + wb);
                wa *= inv; wb *= inv;
                clothWorld_[ia * 3 + 0] += dx * diff * wa;
                clothWorld_[ia * 3 + 1] += dy * diff * wa;
                clothWorld_[ia * 3 + 2] += dz * diff * wa;
                clothWorld_[ib * 3 + 0] -= dx * diff * wb;
                clothWorld_[ib * 3 + 1] -= dy * diff * wb;
                clothWorld_[ib * 3 + 2] -= dz * diff * wb;
            };
            constexpr int kIterations = 10;
            for (int it = 0; it < kIterations; ++it)
            {
                for (std::uint32_t r = 1; r < rows; ++r)
                    for (std::uint32_t c = 0; c < cols; ++c)
                    {
                        const std::uint32_t i = idx(r, c);
                        satisfy(i, idx(r - 1, c), clothRestUp_[i]);   // vertical
                        if (c > 0)
                            satisfy(i, idx(r, c - 1), clothRestLeft_[i]); // horizontal
                    }
            }

            // Write simulated positions back into the render buffer.
            for (std::uint32_t i = 0; i < n; ++i)
            {
                animated[off + i].position[0] = clothWorld_[i * 3 + 0];
                animated[off + i].position[1] = clothWorld_[i * 3 + 1];
                animated[off + i].position[2] = clothWorld_[i * 3 + 2];
            }

            // Recompute normals from the actual triangle winding of the cloak mesh
            // and accumulate per vertex. Using the mesh's own faces (consistent
            // winding) avoids the orientation flips that produced dark patches.
            // Reference direction (the world-transformed bind normals) lets us
            // globally flip if the winding points the opposite way.
            float refDot = 0.0f;
            std::vector<float> refNrm(static_cast<std::size_t>(n) * 3u);
            for (std::uint32_t i = 0; i < n; ++i)
            {
                refNrm[i * 3 + 0] = animated[off + i].normal[0];
                refNrm[i * 3 + 1] = animated[off + i].normal[1];
                refNrm[i * 3 + 2] = animated[off + i].normal[2];
                animated[off + i].normal[0] = 0.0f;
                animated[off + i].normal[1] = 0.0f;
                animated[off + i].normal[2] = 0.0f;
            }
            for (const auto& face : data_.cloakBody.faces)
            {
                const std::uint32_t a = face.indices[0];
                const std::uint32_t b = face.indices[1];
                const std::uint32_t cI = face.indices[2];
                if (a >= n || b >= n || cI >= n) continue;
                const Vec3 e1{
                    clothWorld_[b * 3 + 0] - clothWorld_[a * 3 + 0],
                    clothWorld_[b * 3 + 1] - clothWorld_[a * 3 + 1],
                    clothWorld_[b * 3 + 2] - clothWorld_[a * 3 + 2] };
                const Vec3 e2{
                    clothWorld_[cI * 3 + 0] - clothWorld_[a * 3 + 0],
                    clothWorld_[cI * 3 + 1] - clothWorld_[a * 3 + 1],
                    clothWorld_[cI * 3 + 2] - clothWorld_[a * 3 + 2] };
                const Vec3 fn{
                    e1.y * e2.z - e1.z * e2.y,
                    e1.z * e2.x - e1.x * e2.z,
                    e1.x * e2.y - e1.y * e2.x };
                for (std::uint32_t k : { a, b, cI })
                {
                    animated[off + k].normal[0] += fn.x;
                    animated[off + k].normal[1] += fn.y;
                    animated[off + k].normal[2] += fn.z;
                }
            }
            for (std::uint32_t i = 0; i < n; ++i)
                refDot += animated[off + i].normal[0] * refNrm[i * 3 + 0]
                        + animated[off + i].normal[1] * refNrm[i * 3 + 1]
                        + animated[off + i].normal[2] * refNrm[i * 3 + 2];
            const float gsign = (refDot < 0.0f) ? -1.0f : 1.0f;
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const Vec3 nn = normalize_vec3({
                    animated[off + i].normal[0] * gsign,
                    animated[off + i].normal[1] * gsign,
                    animated[off + i].normal[2] * gsign });
                animated[off + i].normal[0] = nn.x;
                animated[off + i].normal[1] = nn.y;
                animated[off + i].normal[2] = nn.z;
            }
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
        reset_cloth();
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

    int CharacterSystem::animation_bone_count() const
    {
        if (!data_.loaded || data_.idleAnimation >= data_.animations.size())
            return 0;
        return static_cast<int>(data_.animations[data_.idleAnimation].animation.bones.size());
    }

    int CharacterSystem::mount_bone_count() const
    {
        if (!data_.hasMount || data_.mount.idleAnimation >= data_.mount.animations.size())
            return 0;
        return static_cast<int>(data_.mount.animations[data_.mount.idleAnimation].animation.bones.size());
    }
}
