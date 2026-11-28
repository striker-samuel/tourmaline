// Copyright (c) 2026 Fenris Creations (CORTAO, MIT)
// Ported to Tourmaline's glm/matrix conventions. The parameter-struct layouts
// below MUST match the HLSL structs in engine/shaders/CORTAO*.h exactly (they
// are pushed as raw push constants).

#pragma once

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

namespace CORTAO {

struct CORTAOConfigurationParameters {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 6;
    uint32_t randomSeed = 0;

    bool flipY = false;
    glm::mat4 projectionMatrix{1.0f};
    glm::mat4 inverseProjectionMatrix{1.0f};
    float nearPlane = 0.01f;
    float farPlane = 100000.0f;

    float maxScreenspaceRadiusHeightRatio = 0.2f;
    float mipBias = -4.0f;
    float maxSlopeCompensation = 4.0f;
    float backfaceThreshold = 0.5f;
    float backfaceBoost = 0.5f;

    float filterDepthSensitivity = 512.0f;
    float filterNormalSensitivity = 16.0f;
};

struct CORTAOPrepassParameters {
    uint32_t packedResolution = 0;
    float depthScale = 0.0f;
    float depthOffset = 0.0f;
    uint32_t randomSeed = 0;
    uint32_t mipCount = 0;
    uint32_t _padding0 = 0;
    uint32_t _padding1 = 0;
    uint32_t _padding2 = 0;
};

inline void PopulatePrepassParameters(const CORTAOConfigurationParameters& configuration,
                                      CORTAOPrepassParameters* parameters) {
    parameters->packedResolution = configuration.width | (configuration.height << 16u);

    const float nearPlane = configuration.nearPlane;
    const float farPlane = configuration.farPlane;
    if (std::isinf(nearPlane)) {
        parameters->depthScale = 1.0f / farPlane;
        parameters->depthOffset = 0.0f;
    } else if (std::isinf(farPlane)) {
        parameters->depthScale = -1.0f / nearPlane;
        parameters->depthOffset = 1.0f / nearPlane;
    } else {
        parameters->depthScale = (nearPlane - farPlane) / (nearPlane * farPlane);
        parameters->depthOffset = 1.0f / nearPlane;
    }
    parameters->randomSeed = configuration.randomSeed;
    parameters->mipCount = configuration.mipCount;
}

struct CORTAOMainPassParameters {
    uint32_t packedResolution = 0;
    uint32_t randomSeed = 0;
    float normalBias = 0.0f;
    float maxApparentCircleRadiusCoefficient = 0.0f;
    float unprojectParamsX = 0.0f;
    float unprojectParamsY = 0.0f;
    float unprojectParamsZ = 0.0f;
    float unprojectParamsW = 0.0f;
    float projectionParamsX = 0.0f;
    float projectionParamsY = 0.0f;
    float projectionParamsZ = 0.0f;
    float projectionParamsW = 0.0f;
    float mipBias = 0.0f;
    float inverseMaxSlopeCompensation = 0.0f;
    float backfaceThreshold = 0.0f;
    float backfaceBoost = 0.0f;
};

inline void PopulateMainPassParameters(const CORTAOConfigurationParameters& configuration,
                                       CORTAOMainPassParameters* parameters) {
    const float projX = configuration.projectionMatrix[0][0];
    const float projY = configuration.projectionMatrix[1][1];
    const float projOffsetX = configuration.projectionMatrix[2][0];
    const float projOffsetY = configuration.projectionMatrix[2][1];

    const float invX = configuration.inverseProjectionMatrix[0][0];
    const float invY = configuration.inverseProjectionMatrix[1][1];
    const float invOffsetX = configuration.inverseProjectionMatrix[3][0];
    const float invOffsetY = configuration.inverseProjectionMatrix[3][1];

    parameters->packedResolution = configuration.width | (configuration.height << 16u);
    parameters->randomSeed = configuration.randomSeed;
    parameters->normalBias = invX / (float)configuration.width * std::sqrtf(2.0f);

    if (configuration.flipY) {
        parameters->unprojectParamsX = +2.0f * invX;
        parameters->unprojectParamsY = -2.0f * invY;
        parameters->unprojectParamsZ = -invX + invOffsetX;
        parameters->unprojectParamsW = +invY - invOffsetY;
        parameters->projectionParamsX = +0.5f * projX;
        parameters->projectionParamsY = -0.5f * projY;
        parameters->projectionParamsZ = -0.5f + 0.5f * projOffsetX;
        parameters->projectionParamsW = -0.5f - 0.5f * projOffsetY;
    } else {
        parameters->unprojectParamsX = +2.0f * invX;
        parameters->unprojectParamsY = +2.0f * invY;
        parameters->unprojectParamsZ = -invX + invOffsetX;
        parameters->unprojectParamsW = -invY + invOffsetY;
        parameters->projectionParamsX = +0.5f * projX;
        parameters->projectionParamsY = +0.5f * projY;
        parameters->projectionParamsZ = -0.5f + 0.5f * projOffsetX;
        parameters->projectionParamsW = -0.5f + 0.5f * projOffsetY;
    }

    const float maxApparentCircleRadius =
        invY * configuration.maxScreenspaceRadiusHeightRatio * 2.0f;
    parameters->maxApparentCircleRadiusCoefficient =
        maxApparentCircleRadius /
        std::sqrtf(maxApparentCircleRadius * maxApparentCircleRadius + 1.0f);
    parameters->mipBias =
        std::log2f(projY * (float)configuration.height * 0.5f) + configuration.mipBias;
    parameters->inverseMaxSlopeCompensation = 1.0f / configuration.maxSlopeCompensation;
    parameters->backfaceThreshold = configuration.backfaceThreshold;
    parameters->backfaceBoost = configuration.backfaceBoost;
}

struct CORTAOFilterPassParameters {
    uint32_t packedResolution = 0;
    float depthSensitivity = 0.0f;
    float normalSensitivity = 0.0f;
    uint32_t _padding0 = 0;
};

inline void PopulateFilterPassParameters(const CORTAOConfigurationParameters& configuration,
                                         CORTAOFilterPassParameters* parameters) {
    parameters->packedResolution = configuration.width | (configuration.height << 16u);
    parameters->depthSensitivity = configuration.filterDepthSensitivity;
    parameters->normalSensitivity = configuration.filterNormalSensitivity;
}

} // namespace CORTAO
