#version 450 core

in vec2 vUv;
flat in uint vTextureLayer;
in vec4 vColor;

out vec4 outColor;

CAMERA_BLOCK

layout(binding = 0) uniform sampler2DArray terrainTexture;
// Independent array for the "Effects" debug panel (arbitrary .eft files
// browsed outside the map's own, uploaded on demand — see
// OpenGLRenderer::upload_debug_effect_textures). A texture-layer index with
// its top bit set selects this array instead of the shared one.
layout(binding = 3) uniform sampler2DArray debugEffectTexture;

// Cheap value noise for the moon's mottled "maria" patches — no texture
// asset, just a hash grid smoothed with a Hermite blend.
float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float valueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

void main()
{
    // Texture-less sentinels for procedural weather particles (custom
    // rain/snow — see WeatherParticleSystem) that have no .dds backing at
    // all: a soft-edged streak (rain, fades along its long axis instead of a
    // hard rectangle), a soft circular falloff (snow dot), and a flat
    // rectangle (unused by weather currently, kept as the plain fallback).
    // All three are cheaper than a texture fetch and carry no asset-
    // resolution dependency. None of these values is ever assigned to a real
    // .eft particle (those return early instead of ever reaching the shader
    // without a valid layer — see EffectParticleSystem::select_texture_layer_for).
    vec3 texRgb;
    float texAlpha;
    if (vTextureLayer == 0xFFFFFFFFu)
    {
        texRgb = vec3(1.0);
        texAlpha = 1.0;
    }
    else if (vTextureLayer == 0xFFFFFFFEu)
    {
        vec2 centered = (vUv - vec2(0.5)) * 2.0;
        texRgb = vec3(1.0);
        texAlpha = 1.0 - smoothstep(0.55, 1.0, length(centered));
    }
    else if (vTextureLayer == 0xFFFFFFFDu)
    {
        // Sharp along the narrow (u) axis, soft-tapered along the long (v)
        // axis — a needle-like streak instead of a blocky rectangle.
        float edge = abs(vUv.y - 0.5) * 2.0;
        texRgb = vec3(1.0);
        texAlpha = 1.0 - smoothstep(0.25, 1.0, edge);
    }
    else if (vTextureLayer == 0xFFFFFFFBu || vTextureLayer == 0xFFFFFFFAu)
    {
        // CelestialSystem's sun (0xFFFFFFFBu) / moon (0xFFFFFFFAu): a
        // properly anti-aliased crisp disc (edge width from fwidth(), not a
        // pow()-falloff tail — earlier attempts using a wide soft falloff
        // for the whole shape always read as either a hard-edged blob or an
        // oversized halo, because a falloff curve doesn't have a real
        // "edge" to be crisp at) with its own interior gradient and a
        // small, tasteful rim glow just outside that edge.
        bool isMoon = vTextureLayer == 0xFFFFFFFAu;

        vec2 centered = (vUv - vec2(0.5)) * 2.0;
        float r = length(centered);

        // The disc occupies the inner 75% of the billboard; the outer 25%
        // is headroom for the AA edge and rim so neither gets clipped by
        // the quad's own boundary.
        const float diskRadius = 0.75;
        float aa = max(fwidth(r), 0.0015);
        float disk = 1.0 - smoothstep(diskRadius - aa, diskRadius + aa, r);

        // Small rim glow bleeding just past the disk edge — nowhere near
        // the old wide halo's extent (fades out entirely by ~1.5x the
        // disk radius).
        float rimZone = clamp(1.0 - (r - diskRadius) / (diskRadius * 0.5), 0.0, 1.0);
        float rim = rimZone * rimZone * (1.0 - disk);

        float insideT = clamp(r / diskRadius, 0.0, 1.0);
        vec3 color;
        float rimStrength;
        if (isMoon)
        {
            // Pale, faintly mottled disc (procedural "maria") shaded a
            // touch darker toward the edge like a lit sphere, with a very
            // subtle cool rim.
            float mottle = valueNoise(vUv * 5.5 + 11.0) * 0.65 + valueNoise(vUv * 11.0 + 47.0) * 0.35;
            vec3 moonLit = vec3(0.90, 0.92, 0.97);
            vec3 moonShadow = vec3(0.62, 0.65, 0.72);
            vec3 base = mix(moonLit, moonShadow, clamp(mottle * 1.15 - 0.1, 0.0, 1.0));
            float sphereShade = mix(1.0, 0.82, insideT);
            color = base * sphereShade;
            rimStrength = 0.10;
        }
        else
        {
            // Warm center fading to a richer gold at the limb (the sun's
            // actual edge is slightly redder/dimmer than its center), plus
            // a soft golden rim.
            vec3 sunCenter = vec3(1.0, 0.98, 0.90);
            vec3 sunEdge = vec3(1.0, 0.78, 0.38);
            color = mix(sunCenter, sunEdge, pow(insideT, 1.4));
            rimStrength = 0.22;
        }

        float shape = clamp(disk + rim * rimStrength, 0.0, 1.0);
        // This batch blends additively (GL_ONE, GL_ONE — see
        // OpenGLRenderer's effect-particle pass), where the GL blend
        // equation never reads outColor.a: only the RGB actually written
        // gets added to the framebuffer. Baking the shape into brightness
        // here (rather than leaving it in texAlpha alone, the convention
        // the SRC_ALPHA-blended sentinels above use) is what actually makes
        // it fade — without this, every fragment past the discard cutoff
        // below would add the same full-strength color regardless of shape.
        texRgb = color * (disk + rim * rimStrength);
        texAlpha = shape;
    }
    else
    {
        uint layer = vTextureLayer & 0x7FFFFFFFu;
        bool useDebugArray = (vTextureLayer & 0x80000000u) != 0u;
        vec4 texel = useDebugArray
            ? texture(debugEffectTexture, vec3(vUv, float(layer)))
            : texture(terrainTexture, vec3(vUv, float(layer)));
        texRgb = texel.rgb;
        texAlpha = texel.a;
    }

    vec3 rgb = texRgb * vColor.rgb;
    // Godot's effect materials explicitly use fog_disabled. World effects
    // therefore retain their authored color/alpha at every visible distance.
    float alpha = texAlpha * vColor.a;
    if (alpha <= 0.003)
        discard;
    outColor = vec4(rgb, alpha);
}
