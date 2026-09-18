#include "common.hlsli"

// Opaque receiver-material layer for SSR (see ssr.h + the app's
// GlassMask pass): redraw of the OPAQUE list into the SSR mask target,
// the material twin of the pane (glassmask) redraw. Writes
//   R, G = 0 (pane presence/depth stay the pane layer's channels),
//   B     = effective metallic (factor x metallicRoughness.b, layered
//           blend — same evaluation as default_lit_body.hlsli),
//   A     = gloss = 1 - roughness (factor x metallicRoughness.g).
// The trace reads B/A to gate reflections per receiver: metal F0 rises
// to ~1, rough surfaces fade out (stone stops mirroring), glass and
// polished metals keep their full read. Max blend (see the pass):
// presence ORs, and coplanar overlaps keep the shiniest writer — same
// "strongest reflector wins" approximation the pane depth already
// makes. Depth-tests read-only against the opaque prepass depth
// (GreaterEqual: the redraw lands exactly on its own surface), never
// writes it.
//
// Cutout (ShadingModel::Masked) discards whole-draw: alpha-tested
// geometry must not stencil gloss through its cutout holes onto the
// background (per-pixel alpha sampling lives in the lit FS only), and
// foliage-style cutouts read better reflectionless anyway.
//
// Pairs with the same default.vert VS as the lit legs — the location
// subset (color, uv) links fine, same trick as glassmask.frag.hlsl.

struct PSIn
{
    float4 clipPos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;   // layer weights (layered mats)
    [[vk::location(1)]] float2 uv : TEXCOORD0;  // metallicRoughness sample
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    // Cutout: no receiver material (see header — no hole bleed, no
    // foliage mirrors).
    if (mat.params.w == 2.0f)
        discard;

    // Layered materials (vertex-color blending): same weight/layer
    // evaluation as default_lit_body.hlsli — factors blend across
    // layers 0..3, texture results blend per layer.
    const int layerMode = mat.layerMaterials.w;
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

    // glTF metallicRoughnessTexture: G=roughness, B=metallic; factor
    // multiplies the texel (spec). Layered: per-layer G/B multipliers
    // blend by weight — the lit body's discipline, TS feedback included.
    float metallic = layered
        ? ml0.params.y * layerW.x + ml1.params.y * layerW.y
        + ml2.params.y * layerW.z + ml3.params.y * layerW.w
        : mat.params.y;
    float roughness = layered
        ? ml0.params.z * layerW.x + ml1.params.z * layerW.y
        + ml2.params.z * layerW.z + ml3.params.z * layerW.w
        : mat.params.z;
    if (!layered && mat.tex.y >= 0)
    {
        uint slot = (uint)mat.tex.y;
        Texture2D mrTex = textures[NonUniformResourceIndex(slot)];
        float4 mrSample = mrTex.Sample(samplers[0], IN.uv);
        TS_ReportUsage(slot, mrTex, samplers[0], IN.uv, IN.clipPos.xy);
        roughness *= mrSample.g;
        metallic *= mrSample.b;
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
                continue; // below RGBA8 quantization: skip the fetch
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
    roughness = clamp(roughness, 0.04f, 1.0f);

    // (presence=0, paneDepth=0, metallic, gloss)
    OUT.outColor = float4(0.0f, 0.0f, metallic, saturate(1.0f - roughness));
    return OUT;
}
