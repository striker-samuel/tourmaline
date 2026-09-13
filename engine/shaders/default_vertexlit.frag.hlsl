#include "common.hlsli"
#include "environment.hlsli"

// Fragment half of the vertex-lit shader-LOD program (see
// default_vertexlit.vert.hlsl): the per-vertex radiance arrives
// interpolated in COLOR0; this stage only resolves fog and the alpha
// conventions (MASK cutout + dithered Blend) so silhouettes and
// transparency state stay consistent with the lit path. One material
// storage read for the cutoff/mode flags — no textures, no lighting.

struct PSIn
{
    float4 clipPos : SV_Position;
    float4 color : COLOR0;    // rgb = interpolated lit radiance, a = alpha
    float3 worldPos : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    float alphaCutoff = mat.params.x;
    float shadingModelF = mat.params.w;
    float alphaModeF = mat.params2.w;
    float baseAlpha = IN.color.a;

    // Masked cutout + dithered transparency (matches lit/unlit).
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;

    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(baseAlpha, IN.clipPos.xy))
        discard;

    float3 color = IN.color.rgb;

    // Fog in HDR (same as the lit/unlit views so depth cues survive).
    FrameUniforms frame = pc.frameUniforms.Get();
    float viewDist = length(frame.cameraPos - IN.worldPos);
    float fogF = ComputeFogFactor(viewDist, IN.worldPos.y, frame.env);
    if (fogF > 0.0f)
    {
        float3 fogHdr = frame.env.fogColor.rgb * max(frame.env.fogColor.w, 0.0f);
        color = lerp(color, fogHdr, fogF);
    }

    OUT.outColor = float4(SanitizeHdr(color), (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
    return OUT;
}
