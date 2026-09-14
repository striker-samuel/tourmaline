// Main forward lighting fragment body, shared by:
//   default_lit.frag.hlsl       (classic alpha-test discard)
//   default_lit_a2c.frag.hlsl   (ALPHA_TO_COVERAGE: coverage from output
//                                alpha, zero discard in the module so the
//                                driver keeps early-Z; bound ONLY for
//                                Masked materials in 2x MSAA Main)
// Entry files include light/common/environment/sggi first and define (or
// not) ALPHA_TO_COVERAGE before including this body.

struct PSIn
{
    float4 clipPos : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 tangent : TANGENT;
    bool isFrontFace : SV_IsFrontFace;
};
// NOTE: deliberately NO SV_Depth output. Bench data (RTX 5070 Ti, bistro
// foliage view): DepthReplacing routes Main's depth writes through the
// post-shader path for every material — in 2x MSAA that costs early-write
// on ALL opaque geometry (~+6ms), and in 1x Main depth is read-only so the
// declared bound buys nothing (drivers already early-test demote-discard).
// Conservative depth stays on the discard+write passes (shadow_body,
// shadow_evsm) where seeded depth can pre-reject masked fragments.
struct PSOut
{
    float4 outColor : SV_Target;
};

// Must be defined after common.hlsli symbols
#include "shadows.hlsli"

static const float PI = 3.14159265359f;

// -------------------------------------------------------------------------
// PBR helpers – Cook-Torrance microfacet
// -------------------------------------------------------------------------

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0f);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = PI * denom * denom;
    return a2 / max(denom, 1e-7f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float denom = NdotV * (1.0f - k) + k;
    return NdotV / max(denom, 1e-7f);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

// Optional: with roughness adjustment for smoother Fresnel at grazing
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

// -------------------------------------------------------------------------
// Geometric specular AA (Filament-style, GGX-alpha^2 domain).
// Widens the microfacet lobe from screen-space normal variance so a
// sub-pixel normal-mapped lobe at rough~0.04 does not sparkle.
// Input/output are perceptual roughness (shader convention: alpha=rough^2,
// D uses alpha^2 == rough^4, hence the fourth root).
// N must be the final normalized shading normal (after normal mapping).
// -------------------------------------------------------------------------
float SpecularAARoughness(float perceptualRoughness, float3 N)
{
    float3 dNdx = ddx(N);
    float3 dNdy = ddy(N);
    float variance = 0.15f * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
    float alpha = perceptualRoughness * perceptualRoughness;
    // Clamp keeps silhouettes / UV seams from turning everything rough.
    float kernel = min(2.0f * variance, 0.20f);
    float alpha2AA = saturate(alpha * alpha + kernel);
    return sqrt(sqrt(alpha2AA)); // pow(rough^4 + kernel, 0.25)
}

// -------------------------------------------------------------------------
// Unlit / helpers
// -------------------------------------------------------------------------

float3 LinearToSRGB(float3 c)
{
    // approximate, not used for output if swapchain is sRGB
    return c;
}

// -------------------------------------------------------------------------

// Per-forward-plus-light accumulation shared by the inline forward+
// shadow path and the deferred-shadow consumer (below): bounds/cone
// rejects, Frostbite windowed falloff, cookie + light function and
// Cook-Torrance. Only the shadow factor source differs: sampled inline
// (useInlineShadow, A/B path) or precomputed visibility from the
// deferred-shadow pipeline (vis).
void EvaluateForwardPlusLight(ForwardPlusLight pl, float3 worldPos,
    float3 geomN, float3 N, float3 V, float3 F0, float roughness,
    float3 diffuse, float NdotV, bool useInlineShadow, float vis,
    float2 pix, inout float3 direct)
{
    // Cheapest reject first: squared distance vs squared range.
    // No sqrt / division until the light survives this test.
    float3 toLight = pl.position - worldPos;
    float dist2 = dot(toLight, toLight);
    if (pl.range > 0.0f && dist2 > pl.range * pl.range)
        return;
    float dist = sqrt(dist2);
    float3 Lp = toLight / max(dist, 1e-6f);

    // Backface reject before any spot-cone / falloff work.
    float NdotLp = max(dot(N, Lp), 0.0f);
    if (NdotLp <= 0.0f)
        return;

    // Angular falloff for spots.
    float angular = 1.0f;
    float2 spotUV = float2(0.5f, 0.5f);
    if (pl.type == 1)
    {
        float3 axis = normalize(pl.direction);
        float cosA = dot(-Lp, axis);
        float cosInner = cos(pl.innerAngle);
        float cosOuter = cos(pl.outerAngle);
        if (cosA < cosOuter)
            return;
        angular = smoothstep(cosOuter, cosInner, cosA);
        // Cone UV for the cookie / spatial light functions
        // (same projection as the spot-shadow tiles).
        spotUV = SpotConeUV(axis, -Lp, pl.outerAngle);
    }

    // Frostbite-style windowed falloff, candela convention:
    // radiance = color * intensity * window / (4 * PI * r^2).
    float window_ = 1.0f;
    if (pl.range > 0.0f)
    {
        float x = dist / max(pl.range, 1e-6f);
        window_ = pow(saturate(1.0f - x * x * x * x), 2.0f);
    }
    float atten = window_ / max(4.0f * PI * dist * dist, 1e-2f);
    float3 plColor = pl.color.rgb * pl.intensity;

    // Light cookie (spot): gobo texture projected through the
    // cone; modulates the light color (diffuse + specular).
    if (pl.type == 1 && pl.cookieIndex >= 0)
    {
        uint cookieIdx = NonUniformResourceIndex((uint)pl.cookieIndex);
        plColor *= textures[cookieIdx].Sample(samplers[0], spotUV).rgb;
    }

    // Light function: procedural per-light intensity curve
    // (flicker / pulse / cone bands).
    // Uses the engine-globals clock (globals.hlsli g_time()).
    if (pl.functionType > 0)
    {
        plColor *= EvaluateLightFunction(
            pl.functionType, pl.functionSpeed, pl.functionScale,
            g_time(), spotUV, pl.type == 1);
    }

    // Shadow factor: inline spot-atlas sample (legacy path) or the
    // deferred-shadow pipeline's precomputed per-pixel visibility.
    // EVSM (frame.evsmMode): pl.shadowMapIndex addresses the blurred
    // moments instead of the depth atlas; linear sampler 0. Soft variant:
    // PCSS penumbra from the moment blocker (tile pads carry the knobs).
    float shadowFactor = vis;
    if (useInlineShadow && pl.type == 1 && pl.shadowTile >= 0)
    {
        uint evsm = pc.frameUniforms.Get().evsmMode;
        if (evsm != 0u && pl.shadowMapIndex >= 0)
            shadowFactor = GetSpotShadowFactorEvsmFromPC(0, worldPos, geomN,
                pl.direction, pl.outerAngle, pl.shadowTile,
                (uint)pl.shadowMapIndex, EVSM_EXPONENT, pix);
        else
            shadowFactor = GetSpotShadowFactorFromPC(1, worldPos, geomN,
                pl.direction, pl.shadowTile);
    }
    if (shadowFactor <= 0.0f)
        return;

    float3 Hp = normalize(V + Lp);
    float Dp = DistributionGGX(N, Hp, roughness);
    float Gp = GeometrySmith(N, V, Lp, roughness);
    float3 Fp = FresnelSchlick(max(dot(Hp, V), 0.0f), F0);
    float3 specP = (Dp * Gp * Fp) / max(4.0f * NdotV * NdotLp, 1e-7f);

    direct += (diffuse + specP) * plColor * (NdotLp * atten * angular * shadowFactor);
}

// -------------------------------------------------------------------------
// Forward+ tiled decals (projected splats and glow/wear projections).
// Iterates the pixel's tile decal list (see forwardplus.cs.hlsl) and blends
// each decal over the composed material parameters:
//   albedo  – lerp(baseColor, tex.rgb * tint, a)
//   normal  – tangent-space map through the decal basis, lerped by a
//   rough/metal – lerp toward the decal targets by a (wet blood reads
//             glossy at roughnessTarget ~0.1, metal stays 0)
//   emissive – additive color * strength * a (optional glow decals)
// a = texture coverage * opacity * facing fade * depth fade. The facing
// test uses the GEOMETRIC normal (geomN): a normal-mapped surface must
// not lose its splat where the map happens to point away; the depth fade
// (smoothstep from depthFadeStart to 1 over |local.z|) keeps offset boxes
// from popping on curved surfaces. FrameUniforms carries the decal buffer
// + tile list pointers; decalCount > 0 implies the cull dispatch ran and
// the lists are fresh (see forwardplus.cs.hlsl).
// -------------------------------------------------------------------------
void ApplyForwardPlusDecals(FrameUniforms frame, float2 pix, float3 worldPos,
    float3 geomN, inout float3 baseColor, inout float3 N,
    inout float metallic, inout float roughness, inout float3 emissive)
{
    const uint decalCount = frame.decalCount;
    if (decalCount == 0u)
        return;

    // Tile lookup (same 16px tiling as the forward+ light lists).
    uint fpTileSizeD = max(pc.fpTileSize, 1u);
    uint tx = (uint)(pix.x / (float)fpTileSizeD);
    uint ty = (uint)(pix.y / (float)fpTileSizeD);
    if (tx >= pc.fpTileCountX)
        return;
    uint tileIdx = ty * pc.fpTileCountX + tx;
    if (tileIdx >= (uint)MAX_FP_TILES)
        return;

    const uint perTile = min(frame.decalMaxPerTile, (uint)MAX_FP_DECALS_PER_TILE);
    const uint stride = max(frame.decalMaxPerTile, 1u);
    const uint listCount = min(frame.decalTileCounts.Get().counts[tileIdx], perTile);

    [loop]
    for (uint s = 0u; s < listCount; ++s)
    {
        uint di = frame.decalTileIndices.Get().indices[tileIdx * stride + s];
        if (di >= decalCount)
            continue;
        ForwardPlusDecal D = frame.decals.Get().decals[di];

        // World position -> decal unit cube.
        float3 lp;
        lp.x = dot(D.xform0.xyz, worldPos) + D.xform0.w;
        lp.y = dot(D.xform1.xyz, worldPos) + D.xform1.w;
        lp.z = dot(D.xform2.xyz, worldPos) + D.xform2.w;
        if (any(abs(lp) > 1.0f))
            continue;

        // Surface must face the projector (axisZ points at the surface).
        float3 axisZ = normalize(D.xform2.xyz);
        float facing = dot(geomN, axisZ);
        if (facing > -0.05f)
            continue; // backfacing / grazing to the projection
        const float faceFade = saturate((-facing - 0.05f) * 4.0f);

        // Depth fade toward the box faces (no hard pops on offsets).
        const float fadeStart = clamp(D.params.w, 0.1f, 0.99f);
        const float depthFade = 1.0f - smoothstep(fadeStart, 1.0f, abs(lp.z));

        const float2 uv = lp.xy * 0.5f + 0.5f;

        float4 albedoS = float4(1.0f, 1.0f, 1.0f, 1.0f);
        if (D.material.x >= 0)
        {
            albedoS = textures[NonUniformResourceIndex((uint)D.material.x)]
                .Sample(samplers[0], uv);
        }
        const float a = saturate(albedoS.a) * saturate(D.color.a) * faceFade * depthFade;
        if (a <= 1.0f / 255.0f)
            continue;

        if ((D.material.z & FP_DECAL_ALBEDO) != 0)
        {
            baseColor = lerp(baseColor, albedoS.rgb * D.color.rgb, a);
        }

        if (D.emissive.a > 0.0f)
        {
            emissive += D.emissive.rgb * D.emissive.a * albedoS.rgb * a;
        }

        if ((D.material.z & FP_DECAL_NORMAL) != 0 && D.material.y >= 0)
        {
            // Tangent-space map through the decal's world basis (Z
            // reconstructed on the unit sphere like engine normal maps).
            float2 mapXY = textures[NonUniformResourceIndex((uint)D.material.y)]
                .Sample(samplers[0], uv).xy * 2.0f - 1.0f;
            mapXY *= D.params.x;
            float mapZ = sqrt(saturate(1.0f - dot(mapXY, mapXY)));
            float3 axisX = normalize(D.xform0.xyz);
            float3 axisY = normalize(D.xform1.xyz);
            float3 dn = normalize(axisX * mapXY.x + axisY * mapXY.y + axisZ * mapZ);
            N = normalize(lerp(N, dn, a));
        }

        if ((D.material.z & FP_DECAL_MATERIAL) != 0)
        {
            roughness = clamp(lerp(roughness, D.params.y, a), 0.04f, 1.0f);
            metallic = saturate(lerp(metallic, D.params.z, a));
        }
    }
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    // ---------------------------------------------------------------
    // Layered materials (vertex-color blending).
    // mat.layerMaterials.w == 0: legacy path (vertex color tints albedo).
    // Otherwise 2..4: blend that many layers with the vertex color RGBA
    // channels as weights (normalized below). Layer 0 is this material;
    // layers 1..3 are the materials addressed by layerMaterials.xyz
    // (world material ids, remapped at load; -1 = reuse layer 0).
    // Factors and sampled texture results blend per layer; enum-like
    // state (shading model, alpha mode) always comes from layer 0.
    // ---------------------------------------------------------------
    int layerMode = mat.layerMaterials.w;
    const bool layered = (layerMode > 1);
    float4 layerW = layered
        ? IN.color / max(IN.color.x + IN.color.y + IN.color.z + IN.color.w, 1e-6f)
        : float4(1.0f, 0.0f, 0.0f, 0.0f);
    MaterialValues ml0 = mat;
    MaterialValues ml1 = mat;
    MaterialValues ml2 = mat;
    MaterialValues ml3 = mat;
    if (layered)
    {
        int id1 = mat.layerMaterials.x;
        int id2 = mat.layerMaterials.y;
        int id3 = mat.layerMaterials.z;
        if (layerMode > 1 && id1 >= 0 && id1 < MAX_MATERIALS)
            ml1 = pc.bMats.Get().mats[id1];
        if (layerMode > 2 && id2 >= 0 && id2 < MAX_MATERIALS)
            ml2 = pc.bMats.Get().mats[id2];
        if (layerMode > 3 && id3 >= 0 && id3 < MAX_MATERIALS)
            ml3 = pc.bMats.Get().mats[id3];
    }

    // ---------------------------------------------------------------
    // Decode material indices and factors
    // tex: x=baseColor, y=metallicRoughness, z=normal, w=occlusion
    // tex2: x=emissive
    // params: x=alphaCutoff, y=metallic, z=roughness, w=shadingModel
    // params2: x=occlusionStrength, y=normalScale, z=emissiveStrength
    // (Layered: factors blend below; indices are per-layer.)
    // ---------------------------------------------------------------
    int baseColorIdx = mat.tex.x;
    int mrIdx        = mat.tex.y;
    int normalIdx    = mat.tex.z;
    int occIdx       = mat.tex.w;
    int emissiveIdx  = mat.tex2.x;

    float alphaCutoff      = mat.params.x;
    float metallicFactor   = layered
        ? ml0.params.y * layerW.x + ml1.params.y * layerW.y
        + ml2.params.y * layerW.z + ml3.params.y * layerW.w
        : mat.params.y;
    float roughnessFactor  = layered
        ? ml0.params.z * layerW.x + ml1.params.z * layerW.y
        + ml2.params.z * layerW.z + ml3.params.z * layerW.w
        : mat.params.z;
    float shadingModelF    = mat.params.w;
    float occlusionStrength= layered
        ? ml0.params2.x * layerW.x + ml1.params2.x * layerW.y
        + ml2.params2.x * layerW.z + ml3.params2.x * layerW.w
        : mat.params2.x;
    float normalScale      = layered
        ? ml0.params2.y * layerW.x + ml1.params2.y * layerW.y
        + ml2.params2.y * layerW.z + ml3.params2.y * layerW.w
        : mat.params2.y;
    float emissiveStrength = layered
        ? ml0.params2.z * layerW.x + ml1.params2.z * layerW.y
        + ml2.params2.z * layerW.z + ml3.params2.z * layerW.w
        : mat.params2.z;

    float3 baseColorFactor = layered
        ? ml0.diffuse.rgb * layerW.x + ml1.diffuse.rgb * layerW.y
        + ml2.diffuse.rgb * layerW.z + ml3.diffuse.rgb * layerW.w
        : mat.diffuse.rgb;
    float  baseAlphaFactor = layered
        ? ml0.diffuse.a * layerW.x + ml1.diffuse.a * layerW.y
        + ml2.diffuse.a * layerW.z + ml3.diffuse.a * layerW.w
        : mat.diffuse.a;
    float3 emissiveFactor  = layered
        ? ml0.emissiveFactor.rgb * layerW.x + ml1.emissiveFactor.rgb * layerW.y
        + ml2.emissiveFactor.rgb * layerW.z + ml3.emissiveFactor.rgb * layerW.w
        : mat.emissiveFactor.rgb;

    // ---------------------------------------------------------------
    // Sample baseColor / alpha (blended across layers when layered)
    // ---------------------------------------------------------------
    float4 baseSample = float4(1,1,1,1);
    if (!layered && baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        baseSample = baseTex.Sample(samplers[0], IN.uv);
        TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, IN.clipPos.xy);
    }
    if (layered)
    {
        baseSample = float4(0, 0, 0, 0);
        int4 bcIdx = int4(ml0.tex.x, ml1.tex.x, ml2.tex.x, ml3.tex.x);
        [unroll]
        for (int li = 0; li < 4; ++li)
        {
            float w = (li == 0) ? layerW.x : (li == 1) ? layerW.y : (li == 2) ? layerW.z : layerW.w;
            int idx = (li == 0) ? bcIdx.x : (li == 1) ? bcIdx.y : (li == 2) ? bcIdx.z : bcIdx.w;
            if (w < 0.004f)
                continue; // below RGBA8 quantization: saves a fetch
            float4 s = float4(1, 1, 1, 1);
            if (idx >= 0)
            {
                uint slot = (uint)idx;
                Texture2D t = textures[NonUniformResourceIndex(slot)];
                s = t.Sample(samplers[0], IN.uv);
                TS_ReportUsage(slot, t, samplers[0], IN.uv, IN.clipPos.xy);
            }
            baseSample += s * w;
        }
    }
    // glTF: baseColor = baseColorFactor * vertexColor * texture
    // (layered: vertex color already consumed as weights, tint stays white).
    float3 vcolTint = layered ? float3(1.0f, 1.0f, 1.0f) : IN.color.rgb;
    float3 baseColor = baseColorFactor * vcolTint * baseSample.rgb;
    float  baseAlpha = baseAlphaFactor * baseSample.a;

#if defined(ALPHA_TO_COVERAGE)
    // A2C variant: NO discard anywhere in the module (OpKill disables the
    // early-Z path the variant exists for). Cutout is expressed through
    // the coverage remap at the output below; the dithered-Blend leg is
    // never bound to this PSO (bindForMaterial selects it for Masked
    // materials only), so its discard is compiled out here.
    const float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
#else
    // Alpha test for Masked (ShadingModel::Masked == 2)
    // Also respect alphaMode Mask – both encode to 2 historically.
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;

    // Dithered transparency for Blend (alphaMode == 2 in params2.w):
    // screen-door discard, opaque output. Blend mode skips this and
    // relies on the AlphaBlend pipeline state instead.
    float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(baseAlpha, IN.clipPos.xy))
        discard;
#endif

    // ---------------------------------------------------------------
    // Metallic-roughness
    // glTF metallicRoughnessTexture: G=roughness, B=metallic
    // (Layered: per-layer G/B multipliers blend by weight.)
    // ---------------------------------------------------------------
    float metallic = metallicFactor;
    float roughness = roughnessFactor;
    if (!layered && mrIdx >= 0)
    {
        uint slot = (uint)mrIdx;
        Texture2D mrTex = textures[NonUniformResourceIndex(slot)];
        float4 mrSample = mrTex.Sample(samplers[0], IN.uv);
        TS_ReportUsage(slot, mrTex, samplers[0], IN.uv, IN.clipPos.xy);
        // Spec says: roughness = G * factor, metallic = B * factor
        roughness *= mrSample.g;
        metallic  *= mrSample.b;
    }
    if (layered)
    {
        float roughMult = 0.0f;
        float metalMult = 0.0f;
        int4 mIdx = int4(ml0.tex.y, ml1.tex.y, ml2.tex.y, ml3.tex.y);
        [unroll]
        for (int li = 0; li < 4; ++li)
        {
            float w = (li == 0) ? layerW.x : (li == 1) ? layerW.y : (li == 2) ? layerW.z : layerW.w;
            int idx = (li == 0) ? mIdx.x : (li == 1) ? mIdx.y : (li == 2) ? mIdx.z : mIdx.w;
            if (w < 0.004f)
                continue;
            float2 gb = float2(1.0f, 1.0f);
            if (idx >= 0)
            {
                uint slot = (uint)idx;
                Texture2D mrTex = textures[NonUniformResourceIndex(slot)];
                float4 mrSample = mrTex.Sample(samplers[0], IN.uv);
                TS_ReportUsage(slot, mrTex, samplers[0], IN.uv, IN.clipPos.xy);
                gb = mrSample.gb;
            }
            roughMult += gb.x * w;
            metalMult += gb.y * w;
        }
        roughness *= roughMult;
        metallic *= metalMult;
    }
    metallic = saturate(metallic);
    roughness = clamp(roughness, 0.04f, 1.0f); // avoid 0 roughness singularities

    // ---------------------------------------------------------------
    // Normal mapping
    // ---------------------------------------------------------------
    float3 N = normalize(IN.normal);
    // Double-sided handling: flip if backface when no culling
    if (!IN.isFrontFace)
        N = -N;
    // Geometric (unmapped) normal for shadow-bias facing classification.
    // ODB must key off the face orientation, not the normal-mapped shading
    // normal: map perturbation would flip the +/-k sign per-pixel and the
    // bias would sprinkle acne/detachment noise over textured surfaces.
    float3 geomN = N;

    if (!layered && normalIdx >= 0)
    {
        // Build TBN from interpolated vertex data. Gram-Schmidt re-orthogonalize
        // the tangent against the normal so interpolation doesn't skew the frame.
        // Guard the degenerate case (T near-parallel to N, e.g. mirrored UV
        // seams interpolating through zero): normalize(~0) is NaN/INF and would
        // poison B, then N, then the whole pixel down to HDR.
        float3 T = IN.tangent.xyz;
        float3 Tg = T - N * dot(N, T);
        float tgLen = length(Tg);
        if (tgLen > 1e-6f) { T = Tg / tgLen; }
        else {
            // Fallback: any axis non-parallel to N gives a valid frame.
            // N is unit here, so cross() is safely non-zero.
            float3 axis = abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            T = normalize(cross(axis, N));
        }

        // tangent.w carries the tangent-space handedness (bitangent sign).
        // Sanitize it: 0 (placeholder) or NaN (corrupt data) would zero/poison
        // B below. Default +1 matches the importer's Lengyel convention.
        float tw = IN.tangent.w;
        tw = (tw == 0.0f || isnan(tw)) ? 1.0f : sign(tw);
        float3 B = cross(N, T) * tw;

        float2 mapXY = textures[NonUniformResourceIndex((uint)normalIdx)]
            .Sample(samplers[0], IN.uv).xy * 2.0f - 1.0f;
        TS_ReportUsage((uint)normalIdx,
            textures[NonUniformResourceIndex((uint)normalIdx)], samplers[0], IN.uv, IN.clipPos.xy);

        // glTF normalTexture.scale applies to XY.
        mapXY *= normalScale;

        // Normal maps are cooked two-channel (toktx --normal_mode, UASTC
        // RRRG): the Z channel is dropped and transcodes to B=0. Sampled
        // as Z, 0*2-1 = -1 flips every normal into the surface and
        // peppered black/white acne. Reconstruct Z on the unit sphere.
        float mapZ = sqrt(saturate(1.0f - dot(mapXY, mapXY)));
        float3 mapN = normalize(float3(mapXY, mapZ));

        // Tangent-space -> world-space. Final length check: with T,B unit and
        // orthogonal this is near-unit already, but never normalize blind.
        float3 Nc = T * mapN.x + B * mapN.y + N * mapN.z;
        float ncLen = length(Nc);
        N = ncLen > 1e-12f ? Nc / ncLen : N;
    }
    if (layered)
    {
        // Layered normal blend: build the TBN frame once (shared by all
        // layers — same vertex), blend each layer's tangent-space normal
        // by weight (missing maps contribute flat +Z), then transform once.
        float3 T = IN.tangent.xyz;
        float3 Tg = T - N * dot(N, T);
        float tgLen = length(Tg);
        if (tgLen > 1e-6f) { T = Tg / tgLen; }
        else {
            float3 axis = abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            T = normalize(cross(axis, N));
        }
        float tw = IN.tangent.w;
        tw = (tw == 0.0f || isnan(tw)) ? 1.0f : sign(tw);
        float3 B = cross(N, T) * tw;

        float3 mapAcc = float3(0.0f, 0.0f, 0.0f);
        int4 nIdx = int4(ml0.tex.z, ml1.tex.z, ml2.tex.z, ml3.tex.z);
        [unroll]
        for (int li = 0; li < 4; ++li)
        {
            float w = (li == 0) ? layerW.x : (li == 1) ? layerW.y : (li == 2) ? layerW.z : layerW.w;
            int idx = (li == 0) ? nIdx.x : (li == 1) ? nIdx.y : (li == 2) ? nIdx.z : nIdx.w;
            if (w < 0.004f)
                continue;
            float3 mapN = float3(0.0f, 0.0f, 1.0f);
            if (idx >= 0)
            {
                uint slot = (uint)idx;
                Texture2D nTex = textures[NonUniformResourceIndex(slot)];
                float2 mapXY = nTex.Sample(samplers[0], IN.uv).xy * 2.0f - 1.0f;
                TS_ReportUsage(slot, nTex, samplers[0], IN.uv, IN.clipPos.xy);
                mapXY *= normalScale;
                float mapZ = sqrt(saturate(1.0f - dot(mapXY, mapXY)));
                mapN = float3(mapXY, mapZ);
            }
            mapAcc += mapN * w;
        }
        float accLen = length(mapAcc);
        if (accLen > 1e-12f)
        {
            float3 mapN = mapAcc / accLen;
            float3 Nc = T * mapN.x + B * mapN.y + N * mapN.z;
            float ncLen = length(Nc);
            N = ncLen > 1e-12f ? Nc / ncLen : N;
        }
    }
    // ---------------------------------------------------------------
    // Occlusion (layered: per-layer R samples blend by weight)
    // ---------------------------------------------------------------
    float occlusion = 1.0f;
    if (!layered && occIdx >= 0)
    {
        uint slot = (uint)occIdx;
        Texture2D occTex = textures[NonUniformResourceIndex(slot)];
        float occSample = occTex.Sample(samplers[0], IN.uv).r;
        TS_ReportUsage(slot, occTex, samplers[0], IN.uv, IN.clipPos.xy);
        occlusion = lerp(1.0f, occSample, occlusionStrength);
    }
    if (layered)
    {
        float occAcc = 0.0f;
        int4 oIdx = int4(ml0.tex.w, ml1.tex.w, ml2.tex.w, ml3.tex.w);
        [unroll]
        for (int li = 0; li < 4; ++li)
        {
            float w = (li == 0) ? layerW.x : (li == 1) ? layerW.y : (li == 2) ? layerW.z : layerW.w;
            int idx = (li == 0) ? oIdx.x : (li == 1) ? oIdx.y : (li == 2) ? oIdx.z : oIdx.w;
            if (w < 0.004f)
                continue;
            float s = 1.0f;
            if (idx >= 0)
            {
                uint slot = (uint)idx;
                Texture2D occTex = textures[NonUniformResourceIndex(slot)];
                s = occTex.Sample(samplers[0], IN.uv).r;
                TS_ReportUsage(slot, occTex, samplers[0], IN.uv, IN.clipPos.xy);
            }
            occAcc += s * w;
        }
        occlusion = lerp(1.0f, occAcc, occlusionStrength);
    }

    // ---------------------------------------------------------------
    // Emissive (layered: per-layer RGB samples blend by weight)
    // ---------------------------------------------------------------
    float3 emissive = emissiveFactor * emissiveStrength;
    if (!layered && emissiveIdx >= 0)
    {
        uint slot = (uint)emissiveIdx;
        Texture2D eTex = textures[NonUniformResourceIndex(slot)];
        float3 eSample = eTex.Sample(samplers[0], IN.uv).rgb;
        TS_ReportUsage(slot, eTex, samplers[0], IN.uv, IN.clipPos.xy);
        emissive *= eSample;
    }
    if (layered)
    {
        float3 eAcc = float3(0.0f, 0.0f, 0.0f);
        int4 eIdx = int4(ml0.tex2.x, ml1.tex2.x, ml2.tex2.x, ml3.tex2.x);
        [unroll]
        for (int li = 0; li < 4; ++li)
        {
            float w = (li == 0) ? layerW.x : (li == 1) ? layerW.y : (li == 2) ? layerW.z : layerW.w;
            int idx = (li == 0) ? eIdx.x : (li == 1) ? eIdx.y : (li == 2) ? eIdx.z : eIdx.w;
            if (w < 0.004f)
                continue;
            float3 s = float3(1.0f, 1.0f, 1.0f);
            if (idx >= 0)
            {
                uint slot = (uint)idx;
                Texture2D eTex = textures[NonUniformResourceIndex(slot)];
                s = eTex.Sample(samplers[0], IN.uv).rgb;
                TS_ReportUsage(slot, eTex, samplers[0], IN.uv, IN.clipPos.xy);
            }
            eAcc += s * w;
        }
        emissive *= eAcc;
    }

    // ---------------------------------------------------------------
    // Unlit path – early out (KHR_materials_unlit)
    // ---------------------------------------------------------------
    // ShadingModel::Unlit == 1
    if (shadingModelF == 1.0f)
    {
        float3 color = baseColor + emissive;
        // Fog still applies so unlit cards (e.g. foliage impostors) fade
        // with the rest of the scene.
        FrameUniforms frameU = pc.frameUniforms.Get();
        float viewDistU = length(frameU.cameraPos - IN.worldPos);
        float fogFU = ComputeFogFactor(viewDistU, IN.worldPos.y, frameU.env);
        if (fogFU > 0.0f)
        {
            float3 fogHdrU = frameU.env.fogColor.rgb * max(frameU.env.fogColor.w, 0.0f);
            color = lerp(color, fogHdrU, fogFU);
        }
        // Still apply occlusion? Spec says occlusion not for unlit, but we keep none.
        // Dithered Blend writes opaque (alpha 1): no blending downstream.
        // Clamp to finite half range: the RGBA16F target rounds anything
        // above 65504 to +inf (specular x lux-scale intensity overflows),
        // and inf texels NaN the bloom/tonemap chain.
    #if defined(ALPHA_TO_COVERAGE)
    // Coverage remap (2x MSAA): below-cutoff texels drop to 0 covered
    // samples (matches the discard cutout plane of the 1x depth passes);
    // above-cutoff scales into partial coverage for AA'd silhouettes.
    OUT.outColor = float4(SanitizeHdr(color),
        saturate((baseAlpha - alphaCutoff) / max(1.0f - alphaCutoff, 1e-4f)));
#else
    OUT.outColor = float4(SanitizeHdr(color), (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
#endif
        return OUT;
    }

    // ---------------------------------------------------------------
    // PBR lighting – directional light + ambient
    // ---------------------------------------------------------------
    FrameUniforms frame = pc.frameUniforms.Get();

    // ---------------------------------------------------------------
    // Forward+ tiled decals (projected splats; per-tile lists written
    // by the forward+ cull dispatch — see forwardplus.cs.hlsl). Applied
    // to the composed material BEFORE the lighting terms are derived, so
    // decal albedo / normal / roughness / metallic feed V, F0, specular
    // AA and every light below. Gating: pixel inside the decal OBB unit
    // cube + geometric normal facing the projector + depth fade near the
    // box faces. geomN keys the facing test so normal-map perturbation
    // cannot punch holes at texel granularity.
    // ---------------------------------------------------------------
    ApplyForwardPlusDecals(frame, IN.clipPos.xy, IN.worldPos, geomN,
        baseColor, N, metallic, roughness, emissive);

    float3 V = normalize(frame.cameraPos - IN.worldPos);
    float NdotV = max(dot(N, V), 0.0f);

    // F0 for dielectrics 0.04, lerped to baseColor for metals
    float3 F0 = float3(0.04f, 0.04f, 0.04f);
    F0 = lerp(F0, baseColor, metallic);

    // Variance-based specular AA: filter the GGX lobe width from the final
    // shading normal's screen-space variance. Material F0/albedo stay on the
    // unfiltered values; every specular width below uses shadingRoughness.
    float shadingRoughness = SpecularAARoughness(roughness, N);

    Light light = pc.bLights.Get().lights[0];
    float3 L = normalize(-light.direction);
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0f);
    float NdotH = max(dot(N, H), 0.0f);
    float HdotV = max(dot(H, V), 0.0f);

    float3 lightColor = light.color.rgb * light.intensity;

    float shadow = 1.0f;
    if (light.shadow_map_index >= 0)
    {
        if (pc.numCascades > 0)
        {
            // EVSM (frame.evsmMode): shadow_map_index addresses the blurred
            // moments instead of the depth atlas; linear sampler 0. Soft
            // variant: solar penumbra from the moment blocker (per-cascade
            // pad2.x scale, pad2.y softness UI).
            uint evsm = pc.frameUniforms.Get().evsmMode;
            if (evsm != 0u)
                shadow = GetShadowFactorEvsmCascadedFromPC(0, IN.worldPos, geomN, light,
                    (uint)light.shadow_map_index, EVSM_EXPONENT, IN.clipPos.xy);
            else
                shadow = GetShadowFactorCascadedFromPC(1, IN.worldPos, geomN, light);
        }
        // PARKED: legacy single-map path kept in shadows.hlsli but disabled.
        // else
        //     shadow = GetShadowFactor(1, IN.worldPos, light);
    }

    // Cook-Torrance BRDF
    float D = DistributionGGX(N, H, shadingRoughness);
    float G = GeometrySmith(N, V, L, shadingRoughness);
    float3 F = FresnelSchlick(HdotV, F0);

    float3 kS = F;
    float3 kD = float3(1.0f, 1.0f, 1.0f) - kS;
    kD *= 1.0f - metallic;

    float3 numerator = D * G * F;
    float denominator = max(4.0f * NdotV * NdotL, 1e-7f);
    float3 specular = numerator / denominator;

    // Diffuse is Lambert * (1/PI) folded into kD
    float3 diffuse = kD * baseColor / PI;

    // Thin cutout foliage (MASK) transmits light: wrap the diffuse NdotL
    // so backlit/shaded leaves stay readable instead of pitch black.
    // Front-lit (NdotL=1) is unchanged; specular keeps the true NdotL.
    // Opaque PBR materials are untouched.
    float NdotLWrap = NdotL;
    if (shadingModelF == 2.0f)
    {
        const float w = 0.5f;
        float t = saturate((NdotL + w) / (1.0f + w));
        NdotLWrap = t * t;
    }

    float3 direct = (diffuse * NdotLWrap + specular * NdotL) * lightColor * shadow;

    // ---------------------------------------------------------------
    // Forward+ tiled point/spot lights (depth-prepassed, GPU-culled),
    // two shadow paths (H toggles at runtime):
    //  - Deferred shadows (frame.dsEnabled): iterate the phase-4 FINAL
    //    light list of the pixel's 8x8 Shadow Tile; per-light shadow
    //    factor is the precomputed VisibilityBuffer value (fully-lit
    //    flag or one byte from the tile's 64B slot) — no atlas sampling
    //    in the lighting pass.
    //  - Legacy: iterate the forward+ tile index lists and sample the
    //    spot-shadow atlas tile inline.
    // Directional light + CSM above is shared by both paths.
    // ---------------------------------------------------------------
    const uint nlights = min(pc.fpLightCount, (uint)MAX_FP_LIGHTS);
    if (frame.dsEnabled != 0u && frame.dsTileCountX > 0u)
    {
        const uint2 pix = (uint2)IN.clipPos.xy;
        if (pix.x < (uint)frame.viewportSize.x && pix.y < (uint)frame.viewportSize.y)
        {
            const uint tileIdx = (pix.y >> 3) * frame.dsTileCountX + (pix.x >> 3);
            const uint2 st = uint2(
                frame.dsTileState.Get().states[tileIdx * 4u],
                min(frame.dsTileState.Get().states[tileIdx * 4u + 1u],
                    (uint)DS_MAX_LIGHTS_PER_TILE));
            // Pixel index inside the Shadow Tile (phase-4 byte mapping).
            const uint localIdx = (pix.x & 7u) + (pix.y & 7u) * 8u;
            for (uint i = 0u; i < st.y; ++i)
            {
                if (st.x + i >= (uint)DS_MAX_LIST_UINTS)
                    break; // saturated/invalid slot: nothing valid follows
                const uint entry = frame.dsLightList.Get().entries[st.x + i];
                const uint li = entry & 0x3FFu;
                const uint slotInfo = entry >> DS_LIGHT_IDX_BITS;
                float vis = 1.0f;
                if (slotInfo != DS_SLOT_ALL_LIT)
                    vis = DsLoadVisibility(frame.dsVisibility, slotInfo, localIdx);
                if (li >= nlights)
                    continue;
                EvaluateForwardPlusLight(pc.fpLights.Get().lights[li],
                    IN.worldPos, geomN, N, V, F0, shadingRoughness, diffuse, NdotV,
                    false, vis, IN.clipPos.xy, direct);
            }
        }
    }
    else if (pc.fpLightCount > 0)
    {
        uint fpTileSize = max(pc.fpTileSize, 1u);
        uint tx = (uint)(IN.clipPos.x / (float)fpTileSize);
        uint ty = (uint)(IN.clipPos.y / (float)fpTileSize);
        if (tx < pc.fpTileCountX)
        {
            uint tileIdx = ty * pc.fpTileCountX + tx;
            uint perTile = min(pc.fpMaxPerTile, (uint)MAX_FP_PER_TILE);
            uint stride = max(pc.fpMaxPerTile, 1u);
            // Hard cap: a tile can pass more lights than the list holds
            // (big depth discontinuities). counts[] stores the true count
            // (debug value) but shading walks only the first perTile list
            // entries — overflow lights pop instead of the old
            // walk-every-light fallback (accepted trade-off; spots are
            // written to the light buffer first so they keep their slots).
            uint tcount = min(pc.fpTileCounts.Get().counts[tileIdx], perTile);
            for (uint s = 0; s < tcount; ++s)
            {
                uint li = pc.fpTileIndices.Get().indices[tileIdx * stride + s];
                if (li >= nlights)
                    continue;
                EvaluateForwardPlusLight(pc.fpLights.Get().lights[li],
                    IN.worldPos, geomN, N, V, F0, shadingRoughness, diffuse, NdotV,
                    true, 1.0f, IN.clipPos.xy, direct);
            }
        }
    }

    // Ambient / sky-IBL floor (hemispherical sky) plus sun-bounce GI.
    // Two additive sources:
    //  1. Configurable analytic IBL from the environment block: diffuse
    //     irradiance from EvaluateIrradiance() plus a cheap specular env
    //     reflection. When IBL is disabled (iblEnabled=0) falls back to
    //     the legacy constant-ambient path (baseColor * mat.ambient).
    //  2. Baked SG-GI probe volume (F8 debug view; N toggles): SUN-BOUNCE
    //     ONLY, ADDED on top of the analytic sky ambient (wall-aware,
    //     spatially varying). Diffuse follows the direct-lighting
    //     convention (albedo/PI * E).
    float3 irradiance = EvaluateIrradiance(N, frame.env);
    float iblOn = saturate(frame.env.fogEndAndFlags.w);
    // SSAO: the SsaoCompose pass already upscaled the half-res AO to full
    // res with full-res depth authority (ao.hlsli), so this shader only
    // .Loads the composed R8 — never the depth image: Main ATTACHES depth,
    // and sampling an attached image is a layout clash (UB, garbage reads).
    // AO is a lighting term: it scales ambient/indirect only. This shader
    // only shades pixels that passed the prepass depth test, so background
    // (sky) can structurally never receive AO.
    float ao = 1.0f;
    if (frame.aoTextureIndex != 0xffffffffu && frame.aoIntensity > 0.0f)
    {
        Texture2D aoTex = textures[NonUniformResourceIndex(frame.aoTextureIndex)];
        ao = aoTex.Load(int3(int2(IN.clipPos.xy), 0)).r;
        ao = lerp(1.0f, saturate(ao), saturate(frame.aoIntensity));
    }
    float3 legacyAmbient = baseColor * mat.ambient.rgb * occlusion * ao;
    float3 ambientDiffuse = kD * baseColor * irradiance * occlusion * ao;
    float3 ambient = lerp(legacyAmbient, ambientDiffuse, iblOn);
    // Specular IBL: reflection-vector env sample weighted by Fresnel.
    float3 R = reflect(-V, N);
    float3 Fenv = FresnelSchlickRoughness(NdotV, F0, shadingRoughness);
    float3 ambientSpec = EvaluateSpecularEnv(R, Fenv, shadingRoughness, frame.env) * occlusion * ao;

    // Baked SG-GI: sun-bounce only, added on top of the analytic sky
    // ambient (the probes carry no sky; uncovered pixels simply keep
    // the sky IBL with no seam).
    if (frame.giEnabled != 0u)
    {
        float3 giAmps[SG_GI_LOBES];
        float giWeightSum = 0.0;
        uint giContributors = 0u;
        bool giFellBack = false;
        uint giBestLevel = 0u;
        const uint giMode = frame.giDebug & 0xffu;
        const bool giCovered = SgGiSample(frame, IN.worldPos, giAmps,
            giWeightSum, giContributors, giFellBack, giBestLevel);
        // F8 debug views (cycled): 1 = flat irradiance (probe diffuse, no
        // albedo, exposure-scaled), 2 = interpolation diagnostics per
        // the polka-dot bisect: R = SG sharpness / max (the basis is
        // globally fixed, so 1.0 = constant), G = weight sum (scaled:
        // saturated = healthy overlap), B = contributor count / 16.
        // 3 = level-isolated irradiance (byte 1 of giDebug selects the
        // level; the gather skips every other octave — coarse-leak
        // bisect), 4 = dominant level palette (level of the strongest
        // weighted contributor: red L0 -> yellow L1 -> green L2 ->
        // cyan L3 -> blue L4 -> violet L5 -> pink L6 -> white L7).
        // Magenta = nothing covered; the green channel dropping to zero
        // with low blue in the dotted regions = starving blends.
        if (giMode != 0u)
        {
            if (!giCovered)
            {
                OUT.outColor = float4(0.8f, 0.0f, 0.8f, 1.0f);
                return OUT;
            }
            if (giMode == 1u || giMode == 3u)
            {
                const float3 giEDbg = SgGiDiffuseIrradiance(giAmps, N)
                    * max(frame.giIntensity, 0.0f) * max(frame.exposure, 1e-6f);
                OUT.outColor = float4(SanitizeHdr(giEDbg), 1.0f);
                return OUT;
            }
            if (giMode == 4u)
            {
                // Magenta: exactly one meaning — no visibility-valid
                // contributor (uncovered, or covered only via the
                // visibility-ignoring fallback). Everything else is a
                // dominant-level color: which octree level carries most
                // of this pixel's interpolation weight (argmax over the
                // per-level weight sums).
                if (giWeightSum <= 1e-4)
                {
                    OUT.outColor = float4(SanitizeHdr(float3(0.8f, 0.0f, 0.8f)
                        * (0.5f / max(frame.exposure, 1e-6f))), 1.0f);
                    return OUT;
                }
                float3 pal;
                if (giBestLevel == 0u)      pal = float3(1.0f, 0.0f, 0.0f);
                else if (giBestLevel == 1u) pal = float3(1.0f, 1.0f, 0.0f);
                else if (giBestLevel == 2u) pal = float3(0.0f, 1.0f, 0.0f);
                else if (giBestLevel == 3u) pal = float3(0.0f, 1.0f, 1.0f);
                else if (giBestLevel == 4u) pal = float3(0.15f, 0.25f, 1.0f);
                else                        pal = float3(1.0f, 1.0f, 1.0f);
                OUT.outColor = float4(SanitizeHdr(pal
                    * (0.18f / max(frame.exposure, 1e-6f))), 1.0f);
                return OUT;
            }
            // Fallback pixels read as solid magenta (binary flag): anywhere
            // the visibility-weighted blend found nothing and the shader
            // fell back to inverse-square nearest probes (visibility
            // ignored). Magenta region == fallback-placed GI.
            if (giFellBack)
            {
                OUT.outColor = float4(SanitizeHdr(float3(1.0f, 0.0f, 1.0f)
                    * (0.5f / max(frame.exposure, 1e-6f))), 1.0f);
                return OUT;
            }
            const float giSharp = SG_GI_SHARPNESS / 4.0f; // fixed basis: 1.0
            const float giW = saturate(giWeightSum / 4.0f);
            const float giCount = saturate(float(giContributors) / 16.0f);
            float3 diag = float3(giSharp, giW, giCount);
            // Fixed 0..1.4 palette: map it into the tonemapper's visible
            // window (18% grey = 1.0) instead of 1/exposure, which pegs
            // everything to white under physical-units exposure (~0.001).
            OUT.outColor = float4(SanitizeHdr(diag * (0.18f / max(frame.exposure, 1e-6f))), 1.0f);
            return OUT;
        }
        if (giCovered)
        {
            const float giScale = max(frame.giIntensity, 0.0f);
            const float3 giE = SgGiDiffuseIrradiance(giAmps, N) * giScale;
            // Same convention as the direct sun term: albedo/PI * E.
            // Sun-bounce ADDS to the sky ambient above (no replace, so
            // uncovered pixels blend seamlessly).
            const float3 giDiffuse = kD * baseColor * (giE / PI) * occlusion * ao;
            // Rough glossy sun-bounce reflections; very smooth surfaces
            // keep the analytic sky env only (9 broad lobes cannot
            // represent mirrors). The probe specular is EXPENSIVE
            // (9 SG x SG product integrals with exp()); roughW == 0
            // surfaces must not pay it just to multiply by zero.
            const float roughW = saturate((shadingRoughness - 0.15f) / 0.25f);
            if (roughW > 0.0f)
            {
                const float3 giSpecular =
                    SgGiSpecular(giAmps, R, shadingRoughness) * giScale * Fenv * occlusion * ao;
                ambientSpec = lerp(ambientSpec, giSpecular, roughW);
            }
            ambient += giDiffuse;
        }
    }

    float3 color = direct + ambient + ambientSpec;
    color = color * occlusion; // re-apply for direct as well per spec (optional)
    // Actually spec: occludedColor = lerp(color, color * occTex, strength) applied to final? We split above.

    // Emissive additive
    color += emissive;

    // Distance + height fog (configurable, HDR pre-tonemap).
    // viewDepth from camera distance; fog color is HDR (fogColor * intensity).
    float viewDist = length(frame.cameraPos - IN.worldPos);
    float fogF = ComputeFogFactor(viewDist, IN.worldPos.y, frame.env);
    if (fogF > 0.0f)
    {
        float3 fogHdr = frame.env.fogColor.rgb * max(frame.env.fogColor.w, 0.0f);
        color = lerp(color, fogHdr, fogF);
    }

#if defined(DS_DEBUG_VIZ)
    // Deferred-shadow debug visualizers (paper 5.3/5.4): per-tile light
    // counts + per-light culling/list diagnostics, over the lit image
    // (debug builds only; compiled out of release entirely). Like the
    // other debug views the output is pre-scaled by 1/exposure so the
    // flat palette survives the physical-units HDR + ACES chain.
    if (frame.dsEnabled != 0u && frame.dsDebugMode != (uint)DS_VIZ_OFF)
    {
        const uint2 vizPix = (uint2)IN.clipPos.xy;
        if (vizPix.x < (uint)frame.viewportSize.x
            && vizPix.y < (uint)frame.viewportSize.y)
        {
            float3 viz = DsVisualize(frame, pc.fpLights, nlights, IN.worldPos,
                vizPix, frame.dsDebugMode, frame.dsDebugLight);
            viz /= max(frame.exposure, 1e-6f);
            if ((frame.dsDebugFlags & 1u) != 0u)
                color = lerp(color, viz, 0.65f); // over the game
            else
                color = viz;                    // flat replacement
        }
    }
#endif

    // Handle alpha blending output
    // For Opaque/Masked we output baseAlpha, for Blend pipeline handles blending.
    // Dithered Blend outputs opaque alpha (the discard above did the work).
    // SanitizeHdr: Cook-Torrance x lux-scale lightColor can exceed the half-
    // float max of the RGBA16F target (65504); unclamped it stores as +inf
    // and NaNs the tonemap (inf/inf) / bloom chain.
#if defined(ALPHA_TO_COVERAGE)
    // Coverage remap (2x MSAA): below-cutoff texels drop to 0 covered
    // samples (matches the discard cutout plane of the 1x depth passes);
    // above-cutoff scales into partial coverage for AA'd silhouettes.
    OUT.outColor = float4(SanitizeHdr(color),
        saturate((baseAlpha - alphaCutoff) / max(1.0f - alphaCutoff, 1e-4f)));
#else
    OUT.outColor = float4(SanitizeHdr(color), (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
#endif
    return OUT;
}
