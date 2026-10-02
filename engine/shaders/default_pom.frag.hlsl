#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"

// Must be defined after common.hlsli symbols
#include "shadows.hlsli"

// Parallax occlusion mapping view (F11): the full sun/sky shading of the
// lit path (minus baked GI / decals / forward+, isolated on purpose so
// the technique itself is the only delta vs F1) with every material
// texture sampled at a ray-marched parallax uv instead of the surface uv.
//
// Height field: the glTF material set has no height slot, so the
// OCCLUSION map doubles as the height field — AO 1 = open surface
// (depth 0, no displacement), AO 0 = cavity (depth 1, deepest carve).
// That is the classic AO-as-height pairing: cavities sink, open areas
// stay at the polygon plane, and a flat plate of AO=1 marches to an
// immediate hit at the entry uv (POM degenerates to plain sampling).
// Materials without an occlusion map get a flat depth-0 field: same
// degenerate no-op, the view stays meaningful through normal mapping.
//
// Pipeline (steep POM, tartan-demo style):
//   - 16 fixed linear steps along the tangent-space eye ray,
//   - 4 binary-refinement steps between the last free sample and the hit,
//   - 8-step sun self-shadow march from the hit point toward the light
//     (blocker = texel surface above the ray, linearly softened),
//   - grazing fade back to the surface uv below NdotV ~ 0.08..0.22 so
//     silhouette rays (|shift| explodes as 1/NdotV) don't swim.
//
// Pairs with default.vert.hlsl (same PSIn as default_lit). Alpha cutout
// and dithered Blend discard run on the DISPLACED uv (holes carve with
// the surface); silhouettes can therefore differ from F1 by design —
// that difference is part of what the view exists to show.
//
// Layered materials shade as layer 0 only (vertex weights would need a
// per-layer height field; documented limitation of the demo view).
//
// Real physical-units lighting like F1: output stays in HDR and survives
// the tonemap chain (no 1/exposure debug pre-scale). Fog applies.

struct PSIn
{
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 tangent : TANGENT;
    bool isFrontFace : SV_IsFrontFace;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

static const float PI = 3.14159265359f;

// POM knobs (demo constants; scale is in uv units at full depth, so the
// world-space carve depth scales with the texture's tiling rate).
static const float kPomHeightScale = 0.08f;
static const uint  kPomLinearSteps = 16u;
static const uint  kPomRefineSteps = 4u;
static const uint  kPomShadowSteps = 8u;
static const float kPomShadowSoft  = 4.0f;

// -------------------------------------------------------------------------
// PBR helpers (same formulas as default_lit_body.hlsli)
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
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

float SpecularAARoughness(float perceptualRoughness, float3 N)
{
    float3 dNdx = ddx(N);
    float3 dNdy = ddy(N);
    float variance = 0.15f * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
    float alpha = perceptualRoughness * perceptualRoughness;
    float kernel = min(2.0f * variance, 0.20f);
    float alpha2AA = saturate(alpha * alpha + kernel);
    return sqrt(sqrt(alpha2AA));
}

// -------------------------------------------------------------------------
// Height field: depth of the texel, 0 = surface plane, 1 = deepest carve
// (AO-as-height: depth = 1 - AO). SampleLevel on purpose — marching taps
// are a divergent loop, implicit derivatives there are undefined.
// -------------------------------------------------------------------------

float PomTexelDepth(int occIdx, float2 uv)
{
    if (occIdx < 0)
        return 0.0f; // flat field: intersection at the entry uv
    return 1.0f - textures[NonUniformResourceIndex((uint)occIdx)]
        .SampleLevel(samplers[0], uv, 0).r;
}

// Sun self-shadow: march from the hit point up toward the light; a
// blocker is a texel whose surface sits ABOVE the ray (depth < ray
// depth). Softened by the blocker's height margin so ridge backsides
// shade gradually instead of snapping black. Returns 1 for no field /
// light behind the surface plane.
float PomSelfShadow(int occIdx, float2 uvHit, float sHit, float3 lts)
{
    if (occIdx < 0 || lts.z <= 0.05f)
        return 1.0f;
    const float2 duvL = lts.xy / lts.z; // uv shift per unit depth toward the light
    float shadow = 1.0f;
    [loop]
    for (uint i = 1u; i <= kPomShadowSteps; ++i)
    {
        const float s = sHit * (1.0f - (float)i / (float)kPomShadowSteps);
        const float d = PomTexelDepth(occIdx, uvHit + duvL * (sHit - s));
        if (d < s)
            shadow = min(shadow, saturate(1.0f - (s - d) * kPomShadowSoft));
    }
    return shadow;
}

PSOut mainPS(float4 clipPos : SV_Position, PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];
    const bool layered = (mat.layerMaterials.w > 1);

    int baseColorIdx = mat.tex.x;
    int mrIdx        = mat.tex.y;
    int normalIdx    = mat.tex.z;
    int occIdx       = mat.tex.w;
    int emissiveIdx  = mat.tex2.x;

    float alphaCutoff       = mat.params.x;
    float metallicFactor    = mat.params.y;
    float roughnessFactor   = mat.params.z;
    float occlusionStrength = mat.params2.x;
    float normalScale       = mat.params2.y;
    float emissiveStrength  = mat.params2.z;

    FrameUniforms frame = pc.frameUniforms.Get();

    // ---------------------------------------------------------------
    // TBN frame (same guarded build as default_lit_body.hlsli:
    // Gram-Schmidt re-orthogonalization, degenerate-tangent fallback,
    // sanitized handedness).
    // ---------------------------------------------------------------
    float3 N = normalize(IN.normal);
    if (!IN.isFrontFace)
        N = -N;
    // Geometric (unmapped) normal for shadow-bias facing classification.
    float3 geomN = N;

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

    // ---------------------------------------------------------------
    // Parallax occlusion mapping: march the tangent-space eye ray
    // through the AO height field. s parameterizes the ray (0 = surface
    // entry, 1 = full depth); uv(s) = entry + shift * s stays linear
    // along the ray, so s doubles as the ray depth at every sample.
    // ---------------------------------------------------------------
    float3 V = normalize(frame.cameraPos - IN.worldPos);
    float3 viewTS = float3(dot(V, T), dot(V, B), dot(V, N));
    const float NdotV = max(dot(N, V), 0.0f);

    // Full-depth uv shift: the ray continues BELOW the surface away from
    // the eye, hence -viewTS.xy (viewTS points surface -> eye). Grazing
    // guard on the divisor: 1/NdotV explodes the shift at silhouettes.
    const float2 pomShift = -viewTS.xy
        * (kPomHeightScale / max(viewTS.z, 1e-3f));
    const float layer = 1.0f / (float)kPomLinearSteps;

    // Linear march: first sample whose texel depth is at/below the ray
    // depth (texel surface above the ray) is the crossing layer.
    float sHit = 1.0f;
    bool hit = false;
    [loop]
    for (uint i = 0u; i < kPomLinearSteps; ++i)
    {
        const float s = (float)i * layer;
        if (PomTexelDepth(occIdx, IN.uv + pomShift * s) <= s)
        {
            sHit = s;
            hit = true;
            break;
        }
    }
    // Binary refinement between the last free sample and the hit.
    if (hit)
    {
        float sLo = max(sHit - layer, 0.0f);
        float sHi = sHit;
        [loop]
        for (uint r = 0u; r < kPomRefineSteps; ++r)
        {
            const float sMid = 0.5f * (sLo + sHi);
            if (PomTexelDepth(occIdx, IN.uv + pomShift * sMid) <= sMid)
                sHi = sMid;
            else
                sLo = sMid;
        }
        sHit = sHi;
    }
    float2 uvP = IN.uv + pomShift * sHit;

    // Grazing fade: below NdotV ~ 0.08..0.22 the shift (and its marching
    // error) grows without bound; fall back to the surface uv so edges
    // don't swim. POM's perceptual payoff is at moderate angles anyway.
    const float pomFade = saturate((NdotV - 0.08f) / 0.14f);
    uvP = lerp(IN.uv, uvP, pomFade);

    // Height-field self-shadowing from the sun (multiplies the direct
    // sun term below only — ambient already carries the AO channel).
    Light light = pc.bLights.Get().lights[0];
    float3 L = normalize(-light.direction);
    float3 lts = float3(dot(L, T), dot(L, B), dot(L, N));
    float pomShadow = PomSelfShadow(occIdx, uvP, sHit, lts);

    // ---------------------------------------------------------------
    // Sample baseColor / alpha at the displaced uv (layered materials
    // shade layer 0 only; weights would need per-layer height fields).
    // ---------------------------------------------------------------
    float4 baseSample = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (!layered && baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        baseSample = baseTex.Sample(samplers[0], uvP);
        TS_ReportUsage(slot, baseTex, samplers[0], uvP, clipPos.xy, pc.textureFeedbackEnabled != 0u);
    }
    float3 vcolTint = layered ? float3(1.0f, 1.0f, 1.0f) : IN.color.rgb;
    float3 baseColor = mat.diffuse.rgb * vcolTint * baseSample.rgb;
    float  baseAlpha = mat.diffuse.a * baseSample.a;

    // Alpha cutout + dithered Blend discard at the DISPLACED uv: holes
    // carve with the surface (differs from F1 by design, see header).
    float shadingModelF = mat.params.w;
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;

    float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(baseAlpha, clipPos.xy))
        discard;

    // ---------------------------------------------------------------
    // Metallic-roughness at the displaced uv (glTF: G=rough, B=metal)
    // ---------------------------------------------------------------
    float metallic = metallicFactor;
    float roughness = roughnessFactor;
    if (mrIdx >= 0)
    {
        uint slot = (uint)mrIdx;
        Texture2D mrTex = textures[NonUniformResourceIndex(slot)];
        float4 mrSample = mrTex.Sample(samplers[0], uvP);
        TS_ReportUsage(slot, mrTex, samplers[0], uvP, clipPos.xy, pc.textureFeedbackEnabled != 0u);
        roughness *= mrSample.g;
        metallic  *= mrSample.b;
    }
    metallic = saturate(metallic);
    roughness = clamp(roughness, 0.04f, 1.0f);

    // ---------------------------------------------------------------
    // Normal mapping at the displaced uv (cooked two-channel RRRG maps:
    // Z reconstructed on the unit sphere, same as the lit path).
    // ---------------------------------------------------------------
    if (normalIdx >= 0)
    {
        uint slot = (uint)normalIdx;
        Texture2D nTex = textures[NonUniformResourceIndex(slot)];
        float2 mapXY = nTex.Sample(samplers[0], uvP).xy * 2.0f - 1.0f;
        TS_ReportUsage(slot, nTex, samplers[0], uvP, clipPos.xy, pc.textureFeedbackEnabled != 0u);
        mapXY *= normalScale;
        float mapZ = sqrt(saturate(1.0f - dot(mapXY, mapXY)));
        float3 mapN = normalize(float3(mapXY, mapZ));
        float3 Nc = T * mapN.x + B * mapN.y + N * mapN.z;
        float ncLen = length(Nc);
        if (ncLen > 1e-12f)
            N = Nc / ncLen;
    }

    // ---------------------------------------------------------------
    // Occlusion at the displaced uv (the height field IS the occlusion
    // map, so cavities darken exactly where they sank).
    // ---------------------------------------------------------------
    float occlusion = 1.0f;
    if (occIdx >= 0)
    {
        uint slot = (uint)occIdx;
        Texture2D occTex = textures[NonUniformResourceIndex(slot)];
        float occSample = occTex.Sample(samplers[0], uvP).r;
        TS_ReportUsage(slot, occTex, samplers[0], uvP, clipPos.xy, pc.textureFeedbackEnabled != 0u);
        occlusion = lerp(1.0f, occSample, occlusionStrength);
    }

    // Emissive at the displaced uv.
    float3 emissive = mat.emissiveFactor.rgb * emissiveStrength;
    if (emissiveIdx >= 0)
    {
        uint slot = (uint)emissiveIdx;
        Texture2D eTex = textures[NonUniformResourceIndex(slot)];
        float3 eSample = eTex.Sample(samplers[0], uvP).rgb;
        TS_ReportUsage(slot, eTex, samplers[0], uvP, clipPos.xy, pc.textureFeedbackEnabled != 0u);
        emissive *= eSample;
    }

    // ---------------------------------------------------------------
    // Sun (Cook-Torrance) + cascaded shadow, same convention as the lit
    // path (PCF depth atlas or EVSM moments per frame.evsmMode; regular
    // sampler 0, never the comparison sampler). The POM self-shadow
    // factors into the direct sun term only.
    // ---------------------------------------------------------------
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0f);
    float NdotH = max(dot(N, H), 0.0f);
    float HdotV = max(dot(H, V), 0.0f);

    float3 lightColor = light.color.rgb * light.intensity;

    float shadow = 1.0f;
    if (light.shadow_map_index >= 0 && pc.numCascades > 0)
    {
        uint evsm = pc.frameUniforms.Get().evsmMode;
        if (evsm != 0u)
            shadow = GetShadowFactorEvsmCascadedFromPC(0, IN.worldPos, geomN, light,
                (uint)light.shadow_map_index, EVSM_EXPONENT, clipPos.xy);
        else
            shadow = GetShadowFactorCascadedFromPC(0, IN.worldPos, geomN, light);
    }
    shadow *= pomShadow;

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
    float shadingRoughness = SpecularAARoughness(roughness, N);

    float D = DistributionGGX(N, H, shadingRoughness);
    float G = GeometrySmith(N, V, L, shadingRoughness);
    float3 F = FresnelSchlick(HdotV, F0);

    float3 kS = F;
    float3 kD = (float3(1.0f, 1.0f, 1.0f) - kS) * (1.0f - metallic);
    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-7f);
    float3 diffuse = kD * baseColor / PI;

    float3 direct = (diffuse * NdotL + specular * NdotL) * lightColor * shadow;

    // ---------------------------------------------------------------
    // Ambient: the analytic sky IBL + legacy material ambient (the lit
    // path's UNCOVERED fallback) + SSAO composite. Baked GI, decals and
    // forward+ lights stay off — this view isolates POM against F1.
    // ---------------------------------------------------------------
    float3 irradiance = EvaluateIrradiance(N, frame.env);
    float iblOn = saturate(frame.env.fogEndAndFlags.w);
    float ao = 1.0f;
    if (frame.aoTextureIndex != 0xffffffffu && frame.aoIntensity > 0.0f)
    {
        Texture2D aoTex = textures[NonUniformResourceIndex(frame.aoTextureIndex)];
        ao = aoTex.Load(int3(int2(clipPos.xy), 0)).r;
        ao = lerp(1.0f, saturate(ao), saturate(frame.aoIntensity));
    }
    float3 legacyAmbient = baseColor * mat.ambient.rgb * occlusion * ao;
    float3 ambientDiffuse = kD * baseColor * irradiance * occlusion * ao;
    float3 ambient = lerp(legacyAmbient, ambientDiffuse, iblOn);

    float3 R = reflect(-V, N);
    float3 Fenv = FresnelSchlickRoughness(NdotV, F0, shadingRoughness);
    // Tile index for the bucketed probe path (same tiling as the FP lists;
    // 0xffffffffu = no tile -> global probe scan in EvaluateSpecularEnv).
    uint specTileIdx = 0xffffffffu;
    {
        uint specTs = max(pc.fpTileSize, 1u);
        uint stx = (uint)(clipPos.x / (float)specTs);
        uint sty = (uint)(clipPos.y / (float)specTs);
        if (stx < pc.fpTileCountX)
            specTileIdx = sty * pc.fpTileCountX + stx;
    }
    float3 ambientSpec = EvaluateSpecularEnv(R, Fenv, shadingRoughness, frame.env, IN.worldPos, pc.frameUniforms.Get(), specTileIdx,
        pc.fpLightCount, pc.fpMaxPerTile, pc.fpTileCounts, pc.fpTileIndices, pc.fpLights)
        * occlusion * ao;

    float3 color = direct + ambient + ambientSpec;
    color += emissive;

    // Fog in HDR, same as the lit path, so depth cues match F1.
    float viewDist = length(frame.cameraPos - IN.worldPos);
    float fogF = ComputeFogFactor(viewDist, IN.worldPos.y, frame.env);
    if (fogF > 0.0f)
    {
        float3 fogHdr = frame.env.fogColor.rgb * max(frame.env.fogColor.w, 0.0f);
        color = lerp(color, fogHdr, fogF);
    }

    OUT.outColor = float4(SanitizeHdr(color),
        (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
    return OUT;
}
