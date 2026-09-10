#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"

struct PSIn
{
    float4 clipPos : SV_Position;
    float3 color : COLOR0;
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
// Unlit / helpers
// -------------------------------------------------------------------------

float3 LinearToSRGB(float3 c)
{
    // approximate, not used for output if swapchain is sRGB
    return c;
}

// -------------------------------------------------------------------------

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    // ---------------------------------------------------------------
    // Decode material indices and factors
    // tex: x=baseColor, y=metallicRoughness, z=normal, w=occlusion
    // tex2: x=emissive
    // params: x=alphaCutoff, y=metallic, z=roughness, w=shadingModel
    // params2: x=occlusionStrength, y=normalScale, z=emissiveStrength
    // ---------------------------------------------------------------
    int baseColorIdx = mat.tex.x;
    int mrIdx        = mat.tex.y;
    int normalIdx    = mat.tex.z;
    int occIdx       = mat.tex.w;
    int emissiveIdx  = mat.tex2.x;

    float alphaCutoff      = mat.params.x;
    float metallicFactor   = mat.params.y;
    float roughnessFactor  = mat.params.z;
    float shadingModelF    = mat.params.w;
    float occlusionStrength= mat.params2.x;
    float normalScale      = mat.params2.y;
    float emissiveStrength = mat.params2.z;

    float3 baseColorFactor = mat.diffuse.rgb;
    float  baseAlphaFactor = mat.diffuse.a;
    float3 emissiveFactor  = mat.emissiveFactor.rgb;

    // ---------------------------------------------------------------
    // Sample baseColor / alpha
    // ---------------------------------------------------------------
    float4 baseSample = float4(1,1,1,1);
    if (baseColorIdx >= 0)
        baseSample = textures[NonUniformResourceIndex((uint)baseColorIdx)].Sample(samplers[0], IN.uv);
    // glTF: baseColor = baseColorFactor * vertexColor * texture
    float3 baseColor = baseColorFactor * IN.color * baseSample.rgb;
    float  baseAlpha = baseAlphaFactor * baseSample.a;

    // Alpha test for Masked (ShadingModel::Masked == 2)
    // Also respect alphaMode Mask – both encode to 2 historically.
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;

    // ---------------------------------------------------------------
    // Metallic-roughness
    // glTF metallicRoughnessTexture: G=roughness, B=metallic
    // ---------------------------------------------------------------
    float metallic = metallicFactor;
    float roughness = roughnessFactor;
    if (mrIdx >= 0)
    {
        float4 mrSample = textures[NonUniformResourceIndex((uint)mrIdx)].Sample(samplers[0], IN.uv);
        // Spec says: roughness = G * factor, metallic = B * factor
        roughness *= mrSample.g;
        metallic  *= mrSample.b;
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

    if (normalIdx >= 0)
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
    // ---------------------------------------------------------------
    // Occlusion
    // ---------------------------------------------------------------
    float occlusion = 1.0f;
    if (occIdx >= 0)
    {
        float occSample = textures[NonUniformResourceIndex((uint)occIdx)].Sample(samplers[0], IN.uv).r;
        occlusion = lerp(1.0f, occSample, occlusionStrength);
    }

    // ---------------------------------------------------------------
    // Emissive
    // ---------------------------------------------------------------
    float3 emissive = emissiveFactor * emissiveStrength;
    if (emissiveIdx >= 0)
    {
        float3 eSample = textures[NonUniformResourceIndex((uint)emissiveIdx)].Sample(samplers[0], IN.uv).rgb;
        emissive *= eSample;
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
        OUT.outColor = float4(color, baseAlpha);
        return OUT;
    }

    // ---------------------------------------------------------------
    // PBR lighting – directional light + ambient
    // ---------------------------------------------------------------
    FrameUniforms frame = pc.frameUniforms.Get();
    float3 V = normalize(frame.cameraPos - IN.worldPos);
    float NdotV = max(dot(N, V), 0.0f);

    // F0 for dielectrics 0.04, lerped to baseColor for metals
    float3 F0 = float3(0.04f, 0.04f, 0.04f);
    F0 = lerp(F0, baseColor, metallic);

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
            shadow = GetShadowFactorCascadedFromPC(1, IN.worldPos, geomN, light);
        // PARKED: legacy single-map path kept in shadows.hlsli but disabled.
        // else
        //     shadow = GetShadowFactor(1, IN.worldPos, light);
    }

    // Cook-Torrance BRDF
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
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
    // Forward+ tiled point/spot lights (depth-prepassed, GPU-culled).
    // Tile lists live in fpTileCounts/fpTileIndices, lights in fpLights.
    // Spots with shadowTile >= 0 sample the spot-shadow atlas tile
    // (perspective, selected per frame – see SelectSpotShadowCasters).
    // ---------------------------------------------------------------
    if (pc.fpLightCount > 0)
    {
        uint fpTileSize = max(pc.fpTileSize, 1u);
        uint tx = (uint)(IN.clipPos.x / (float)fpTileSize);
        uint ty = (uint)(IN.clipPos.y / (float)fpTileSize);
        if (tx < pc.fpTileCountX)
        {
            uint tileIdx = ty * pc.fpTileCountX + tx;
            uint perTile = min(pc.fpMaxPerTile, (uint)MAX_FP_PER_TILE);
            uint stride = max(pc.fpMaxPerTile, 1u);
            uint nlights = min(pc.fpLightCount, (uint)MAX_FP_LIGHTS);
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
                ForwardPlusLight pl = pc.fpLights.Get().lights[li];

                // Cheapest reject first: squared distance vs squared range.
                // No sqrt / division until the light survives this test.
                float3 toLight = pl.position - IN.worldPos;
                float dist2 = dot(toLight, toLight);
                if (pl.range > 0.0f && dist2 > pl.range * pl.range)
                    continue;
                float dist = sqrt(dist2);
                float3 Lp = toLight / max(dist, 1e-6f);

                // Backface reject before any spot-cone / falloff work.
                float NdotLp = max(dot(N, Lp), 0.0f);
                if (NdotLp <= 0.0f)
                    continue;

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
                        continue;
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

                // Spot shadow: tile assigned by the per-frame selection;
                // unshadowed lights (shadowTile < 0) contribute fully.
                // Oriented NDC bias uses geometric normal + spot travel dir.
                float spotShadow = 1.0f;
                if (pl.type == 1 && pl.shadowTile >= 0)
                    spotShadow = GetSpotShadowFactorFromPC(1, IN.worldPos, geomN, pl.direction, pl.shadowTile);
                if (spotShadow <= 0.0f)
                    continue;

                float3 Hp = normalize(V + Lp);
                float Dp = DistributionGGX(N, Hp, roughness);
                float Gp = GeometrySmith(N, V, Lp, roughness);
                float3 Fp = FresnelSchlick(max(dot(Hp, V), 0.0f), F0);
                float3 specP = (Dp * Gp * Fp) / max(4.0f * NdotV * NdotLp, 1e-7f);

                direct += (diffuse + specP) * plColor * (NdotLp * atten * angular * spotShadow);
            }
        }
    }

    // Ambient / sky-IBL floor (hemispherical sky).
    // Configurable analytic IBL from the environment block:
    // diffuse irradiance from EvaluateIrradiance() plus a cheap specular
    // env reflection. When IBL is disabled (iblEnabled=0) falls back to
    // the legacy constant-ambient path (baseColor * mat.ambient).
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
    float3 ambientDiffuse = kD * baseColor * irradiance * occlusion * ao;
    float3 legacyAmbient = baseColor * mat.ambient.rgb * occlusion * ao;
    float3 ambient = lerp(legacyAmbient, ambientDiffuse, iblOn);
    // Specular IBL: reflection-vector env sample weighted by Fresnel.
    float3 R = reflect(-V, N);
    float3 Fenv = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 ambientSpec = EvaluateSpecularEnv(R, Fenv, roughness, frame.env) * occlusion * ao;

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

    // Handle alpha blending output
    // For Opaque/Masked we output baseAlpha, for Blend pipeline handles blending
    OUT.outColor = float4(color, baseAlpha);
    return OUT;
}
