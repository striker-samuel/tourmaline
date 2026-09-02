#include "light.hlsli"
#include "common.hlsli"

struct PSIn
{
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

    //if (normalIdx >= 0)
    //{
    //    float3 tangent = normalize(IN.tangent.xyz);
    //    float3 bitangent = normalize(cross(N, tangent) * IN.tangent.w);
    //    float3x3 TBN = float3x3(tangent, bitangent, N);

    //    float3 mapN = textures[NonUniformResourceIndex((uint)normalIdx)].Sample(samplers[0], IN.uv).xyz * 2.0f - 1.0f;
    //    // Apply normalScale to XY per spec
    //    mapN.xy *= normalScale;
    //    // Renormalize after scale
    //    mapN = normalize(mapN);
    //    N = normalize(mul(mapN, TBN));
    //}
    
    if (normalIdx >= 0)
    {
        float3 T = IN.tangent.xyz;

    // T and N are interpolated, so make them perpendicular again.
        T = normalize(T - N * dot(N, T));

    // tangent.w is the tangent-space handedness.
    // IMPORTANT:
    // If the mesh/model transform is mirrored, this should already
    // include the model determinant sign:
    //
    //     tangent.w = gltfTangent.w * mirrorSign;
    //
        float handedness = IN.tangent.w;

        float3 B = normalize(cross(N, T)) * handedness;

        float3 mapN =
        textures[NonUniformResourceIndex((uint) normalIdx)]
        .Sample(samplers[0], IN.uv)
        .xyz * 2.0f - 1.0f;

    // glTF normalTexture.scale applies to XY.
        mapN.xy *= normalScale;
        mapN = normalize(mapN);

    // Tangent-space -> world-space.
        N = normalize(
        T * mapN.x +
        B * mapN.y +
        N * mapN.z
    );
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
            shadow = GetShadowFactorCascadedFromPC(1, IN.worldPos, light);
        else
            shadow = GetShadowFactor(1, IN.worldPos, light);
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

    float3 direct = (diffuse + specular) * lightColor * NdotL * shadow;

    // Ambient / IBL approximation: baseColor * ambient factor * occlusion
    // Legacy ambient floor kept for look; scaled by occlusion.
    float3 ambient = baseColor * mat.ambient.rgb * occlusion;
    // Add a subtle fake skylight based on N.y
    float hemis = 0.5f + 0.5f * N.y;
    ambient += baseColor * hemis * 0.02f * occlusion; // very low fill

    float3 color = direct + ambient;
    color = color * occlusion; // re-apply for direct as well per spec (optional)
    // Actually spec: occludedColor = lerp(color, color * occTex, strength) applied to final? We split above.

    // Emissive additive
    color += emissive;

    // Handle alpha blending output
    // For Opaque/Masked we output baseAlpha, for Blend pipeline handles blending
    OUT.outColor = float4(color, baseAlpha);
    
//    float3 T = normalize(IN.tangent.xyz);
//    OUT.outColor = float4(
//    T * 0.5f + 0.5f,
//    1.0f
//);
    return OUT;
    
}
