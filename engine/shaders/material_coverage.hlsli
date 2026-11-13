// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// THE SHARED MATERIAL COVERAGE EVALUATOR.
//
// One coverage-alpha computation + one MASK/dithered-BLEND keep test, used
// by every surface that must agree on which geometry survives a cutout:
//   - the masked depth prepass (shadow_body.hlsli),
//   - the thin G-buffer masked twin (gbuffer_masked.frag.hlsl),
//   - the lit body (default_lit_body.hlsli),
//   - EvaluatePbrMaterial (material_evaluation.hlsli),
//   - every shading debug FS (default_unlit / pbr / cascade / ... / uvflat0).
//
// Invariant: for the same instance and pixel, switching a Lit view to a
// shading debug view must NOT change which geometry samples survive. Hand
// copies of this logic drifted (the debug FS dropped the vertex-color alpha
// factor), so it is now ONE function.
//
// Requires common.hlsli (DitherKeep) to be included first.

#ifndef TM_MATERIAL_COVERAGE_HLSLI
#define TM_MATERIAL_COVERAGE_HLSLI

// Coverage alpha = baseColorFactor.a * baseColorTexel.a * vertexColor.a.
// Layered materials pass colorAlpha = 1.0 (the per-layer alpha has already
// been folded into factorAlpha by the caller).
float TM_CoverageAlpha(float factorAlpha, float texelAlpha, float colorAlpha) {
    return factorAlpha * texelAlpha * colorAlpha;
}

// MASK cutout (shadingModel == 2) + dithered-BLEND screen-door (alphaMode ==
// 2). Returns false when the fragment must be discarded; the caller emits
// `discard`. clipPos is SV_Position.xy (the dither's screen coordinate).
bool TM_CoverageKeep(float coverageAlpha,
                     float alphaCutoff,
                     float shadingModel,
                     float alphaMode,
                     bool dithered,
                     float2 clipPos) {
    if (shadingModel == 2.0f && coverageAlpha < alphaCutoff)
        return false;
    if (alphaMode == 2.0f && dithered && !DitherKeep(coverageAlpha, clipPos))
        return false;
    return true;
}

#endif // TM_MATERIAL_COVERAGE_HLSLI
