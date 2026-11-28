// CORTAO Carbon Engine Implementation v1.0.0
//
// © 2026 Fenris Creations. All rights reserved.

#include "CORTAOBase.h"

// Preprocessor checks

#ifndef CORTAO_SAMPLE_COUNT
#error "Pleased define CORTAO_SAMPLE_COUNT to the number of samples to do per pixel."
#endif

#if CORTAO_SAMPLE_COUNT < 1
#error "CORTAO_SAMPLE_COUNT must be at least 1."
#endif

#ifndef CORTAO_RAY_COUNT
#error                                                                                             \
    "Please define CORTAO_RAY_COUNT to the number of rays to simulate. Valid values are 8 to 32, inclusive. A good default is 16. If using CORTAO_OCCLUDER_LOOKUP_TABLE, this must match what the lookup table contains."
#endif

#if CORTAO_RAY_COUNT < 8 || CORTAO_RAY_COUNT > 32
#error "CORTAO_RAY_COUNT must be between 8 and 32, inclusive."
#endif

#ifndef CORTAO_OCCLUDER_LOOKUP_TABLE
#define CORTAO_OCCLUDER_LOOKUP_TABLE 0
#endif

#ifndef CORTAO_BENT_NORMAL_LOOKUP_TABLE
#define CORTAO_BENT_NORMAL_LOOKUP_TABLE 0
#endif

// Functions to implement

float3 CortaoLoadNormal(int2 pixelCoords);

uint CortaoSamplePackedData(float2 texCoords, float mipmap);

#if CORTAO_OCCLUDER_LOOKUP_TABLE
uint CortaoSampleOccluderLookupTable(float3 texCoords);
#endif

#if CORTAO_BENT_NORMAL_LOOKUP_TABLE
float3 CortaoLoadBentNormalLookupTable(uint rayBits);
#endif

void CortaoWriteResult(int2 pixelCoords,
                       float occlusion,
                       float3 bentNormal,
                       float3x3 tangentSpaceTransform,
                       uint rayBits);

// Public structs

struct CORTAOMainPassParameters {

    uint packedResolution;
    uint randomSeed;
    float normalBias;
    float maxApparentCircleRadiusCoefficient;

    float4 unprojectParams;

    float4 projectionParams;

    float mipBias;
    float inverseMaxSlopeCompensation;
    float backfaceThreshold;
    float backfaceBoost;
};

// Public functions

float3 CortaoRayDirection(uint rayIndex) {

    float angle = float(rayIndex) * CORTAO_GOLDEN_ANGLE;
    float radius = sqrt((float(rayIndex) + 0.5) / float(CORTAO_RAY_COUNT));
    float3 rayDirection =
        float3(cos(angle) * radius, sin(angle) * radius, sqrt(float(1.0) - radius * radius));
    return rayDirection;
}

// Internal CORTAO stuff

float3 CortaoUnprojectWithOffset(float4 unprojectParameters,
                                 float2 texCoords,
                                 float linearDepth,
                                 float3 offset) {
    // Lerp between the frustum corner directions.
    // Optimized by precomputing the difference between the corners, to turn it into FMA.
    float2 direction = CortaoFma(texCoords, unprojectParameters.xy, unprojectParameters.zw);

    // Calculate the view space direction as direction * depth.
    // Also add an optional offset here, as it's free to do with FMA.
    return CortaoFma(
        float3(direction, -1.0), float3(linearDepth, linearDepth, linearDepth), offset);
}

float3 CortaoUnproject(float4 unprojectParameters, float2 texCoords, float linearDepth) {
    return CortaoUnprojectWithOffset(unprojectParameters, texCoords, linearDepth, float3(0, 0, 0));
}

uint CortaoComputeRayOcclusion(float offsetDither,
                               float3x3 offsetTransform,
                               float3x3 tangentSpaceTransform,
                               float mipBias,
                               float3 viewPosition,
                               float3 normal,
                               float3 viewDirection,
                               float maxRadius,
                               CORTAOMainPassParameters params) {

    uint rayBits = 0u;

// Hack to preload the occluder lookup table descriptor, which is marginally faster at higher sample
// counts.
#if CORTAO_OCCLUDER_LOOKUP_TABLE && CORTAO_SAMPLE_COUNT > 4
    rayBits = CortaoSampleOccluderLookupTable(float3(0, 0, 0));
#endif

// Don't unroll if we have too many samples, to prevent driver compilers from croaking.
#if CORTAO_SAMPLE_COUNT <= 64
    [unroll]
#endif
        for (uint i = 0u; i < CORTAO_SAMPLE_COUNT; i++) {

        // these are constant for an unrolled loop, so free

        // Calculate the golden angle spiral direction
        float a = float(i) * CORTAO_GOLDEN_ANGLE;
        float2 spiral = float2(cos(a), sin(a));

        float inverseSampleCount = 1.0 / float(CORTAO_SAMPLE_COUNT);

#if 0
			//WIP code for a new mathematically optimal sample pattern.
			float minRadius = 1.41 / (1400.0 * 0.2);

#if 1
				float radius = minRadius * pow(1.0 / minRadius, (float(i) + offsetDither) * inverseSampleCount);
				
				float2 innerSpiral = float2(spiral * radius);
				float2 outerSpiral = float2(spiral * radius);
				
				float sampleWeight = radius * sqrt(2.0 * log(1.0 / minRadius) * inverseSampleCount); //The square root of the percentage of the area of the circle that the sample can be placed in
	
				float sampleMipmap = log2(radius);
#else
				float innerRadius = minRadius * pow(1.0 / minRadius, (float(i) + 0.0) * inverseSampleCount);
				float outerRadius = minRadius * pow(1.0 / minRadius, (float(i) + 1.0) * inverseSampleCount);
	
				float2 innerSpiral = float2(spiral * innerRadius);
				float2 outerSpiral = float2(spiral * outerRadius);
	
				float sampleArea = outerRadius*outerRadius - innerRadius*innerRadius;
				float sampleWeight = sqrt(sampleArea); //The square root of the percentage of the area of the circle that the sample can be placed in
	
				float sampleMipmap = log2(outerRadius);
#endif
#else
        // The skew is how much we suck the sample distribution into the center.
        float skew = 3.5;

        // Calculate an inner and outer radius for this sample. Dithering is used to choose a radius
        // between these two values.
        float innerRadius = pow((float(i) + 0.0) * inverseSampleCount, skew * 0.5);
        float outerRadius = pow((float(i) + 1.0) * inverseSampleCount, skew * 0.5);

        // Precompute the inner and outer spiral positions. The dithered position will lie somewhere
        // on a lerp between the two.
        float2 innerSpiral = float2(spiral * innerRadius);
        float2 outerSpiral = float2(spiral * outerRadius);

        // Calculate the area of the sampling circle that the sample is randomly moved over,
        // based on both a random rotation and a random radius dither to form a circular strip.
        // The weight is sqaure-rooted to account for the fact that the weight is applied to a
        // radius, rather than an area.
        float sampleArea = outerRadius * outerRadius - innerRadius * innerRadius;
        float sampleWeight = sqrt(sampleArea); // The square root of the percentage of the area of
                                               // the circle that the sample can be placed in

        // The mip level depends on how far away we are from the center.
        float sampleMipmap = log2(outerRadius);
#endif

        // dynamic stuff from here on

        // Optimized lerp between the inner and outer sample positions based on a random dither
        // value.
        float2 sampleOffset =
            CortaoFma(outerSpiral - innerSpiral, float2(offsetDither, offsetDither), innerSpiral);

        // Apply the sample transformation matrix and do a perspective division.
        float3 projectedCoords = mul(float3(sampleOffset, 1.0), offsetTransform);
        float2 sampleCoords = projectedCoords.xy / projectedCoords.z;

        // Add the precomputed mip bias and the per-sample mip level together to get our mip level,
        // based on logarithmic identities.
        float mipmap = mipBias + sampleMipmap;

        // Sample the center pixel, and unpack depth only for now.
        uint packedData = CortaoSamplePackedData(sampleCoords, mipmap);
        float sampleDepth = CortaoUnpackDepth(packedData);

        // Calculate this sample's view space position relative to the center pixel.
        float3 dPos = CortaoUnprojectWithOffset(
            params.unprojectParams, sampleCoords, sampleDepth, -viewPosition);

        // Do an early out if the sample ends up outside the hemisphere (radius and normal check).
        // Skybox samples and samples outside the screen have a linear depth of 0.0, which is always
        // outside the hemisphere.
        float lengthSqrd = dot(dPos, dPos);
        if (lengthSqrd < maxRadius * maxRadius && dot(dPos, float3(normal)) > 0.0) {

            // Calculate the occluder direction.
            float inverseLength = rsqrt(lengthSqrd);
            float3 direction = float3(dPos * inverseLength);

            // Unpack the sample normal from the packed data we already read.
            float3 sampleNormal = CortaoDecodeNormal(packedData);

            // Calculate the slope compensation weight and use it to calculate the normalized visual
            // occluder radius.
            float slopeCompensation = rsqrt(
                max(dot(sampleNormal, viewDirection), float(params.inverseMaxSlopeCompensation)));
            float occluderRadius = (maxRadius * sampleWeight) * (slopeCompensation * inverseLength);

            // Heuristically check for backface occluders.
            float sampleDot = dot(sampleNormal, direction);
            if (sampleDot < float(params.backfaceThreshold)) {
                // Not a backface occluder. Apply a normal offset to prevent self-occlusion.
                // This moves the occluder circle by 1 radius into the occluder object. As this is
                // done after occluder normalization, Re-normalize the direction after we apply the
                // offset to get the new direction, but keep the occluder radius the same.
                float adjustedOffset =
                    float(-occluderRadius) /
                    float(max(1.0 - occluderRadius * occluderRadius * 0.5, 0.000001));
                direction =
                    normalize(CortaoFma(sampleNormal,
                                        float3(adjustedOffset, adjustedOffset, adjustedOffset),
                                        direction));
            } else {
                // This is a backface occluder. Don't do the normal offset, and optionally boost the
                // slope compensation to avoid light leaking.
                occluderRadius *= lerp(float(1.0), slopeCompensation, float(params.backfaceBoost));
            }

            // Transform the occluder direction into the ray tracing tangent space.
            float3 hemisphereDirection = mul(tangentSpaceTransform, direction);

            uint occluderRayBits;
#if CORTAO_OCCLUDER_LOOKUP_TABLE

            // Encode the normal as a spheremap projection, which is used as the lookup coordinates
            // together with the occluder radius.
            float mul = rsqrt(CortaoFma(hemisphereDirection.z, float(8.0), float(8.0)));
            float2 sphereMapCoordinates =
                CortaoFma(hemisphereDirection.xy, float2(mul, mul), float2(0.5, 0.5));
            float3 lookupCoords = float3(sphereMapCoordinates, occluderRadius * sqrt(0.5));

            // Sphere map projection fails if the tangent space direction is (0, 0, -1), so ignore
            // the invalid sample we do in that case.
            occluderRayBits = CortaoSampleOccluderLookupTable(lookupCoords) &
                              (hemisphereDirection.z > float(-0.99) ? 0xFFFFFFFFu : 0u);
#else
            {

                // Calculate the angular size of the occluder. This wraps the occluder around the
                // hemisphere, matching the occluded area of the hemisphere with the area of the
                // circle linearly, to improve the scalability of CORTAO.
                float occluderCosAngle =
                    float(max(0.0, 1.0 - occluderRadius * occluderRadius * 0.5));

                // Manually check each ray against hte occluder's angular size.
                occluderRayBits = 0u;
                for (uint j = 0u; j < CORTAO_RAY_COUNT; j++) {
                    float3 rayDirection = CortaoRayDirection(j);
                    if (dot(rayDirection, hemisphereDirection) > occluderCosAngle) {
                        occluderRayBits |= 1u << j;
                    }
                }
            }
#endif

            rayBits |= occluderRayBits;
        }
    }

    return rayBits;
}

void CortaoConstructSamplingBasis(float3 normal, out float3 tangent, out float3 bitangent) {

    // Frisvad's arbitrary orthonormal tangent basis construction.
    float a = float(1.0) / (float(1.0) + normal.z);
    float b = normal.x * normal.y * a;
    tangent = float3(CortaoFma(normal.x * normal.x, a, float(-1.0)), b, normal.x);
    bitangent = float3(b, CortaoFma(normal.y * normal.y, a, float(-1.0)), normal.y);
}

void CortaoConstructTangentBasis(float3 normal, out float3 tangent, out float3 bitangent) {
    // Also, frisvad's arbitrary orthonormal tangent basis construction, but copy-pasted and
    // slightly tweaked.
    float a = float(1.0) / (float(1.0) + normal.z);
    float b = normal.x * normal.y * a;
    tangent = float3(CortaoFma(normal.x * normal.x, a, float(-1.0)), b, normal.x);
    bitangent = float3(b, CortaoFma(normal.y * normal.y, a, float(-1.0)), normal.y);
}

float CortaoResolveOcclusion(uint rayBits) {
    // Calculate occlusion as 1.0 - occluded rays / total rays
    uint numBits = countbits(rayBits);
    return CortaoFma(float(numBits),
                     float(-1.0) / float(CORTAO_RAY_COUNT),
                     float(1.0)); // 1.0 - numBits/CORTAO_RAY_COUNT;
}

float3 CortaoResolveBentNormal(uint rayBits) {

#if CORTAO_BENT_NORMAL_LOOKUP_TABLE
    return CortaoLoadBentNormalLookupTable(rayBits);

#else
    // Calculate the bent normal as the sum of all unoccluded rays.
    float3 bentNormal = float3(0, 0, 0);
    [unroll] for (uint i = 0; i < CORTAO_RAY_COUNT; i++) {

        float3 rayDirection = CortaoRayDirection(i);

        bentNormal += rayDirection * ((rayBits & (1u << i)) == 0u ? float(1.0) : float(0.0));
    }
    return normalize(bentNormal); // Normalize the bent normal in the end.
#endif
}

void CortaoMainPass(uint2 pixelCoords, CORTAOMainPassParameters params) {

    // check if our pixel is out-of-bounds and early out in that case
    uint2 resolution = CortaoGetResolution(params.packedResolution);
    if (pixelCoords.x >= resolution.x || pixelCoords.y >= resolution.y) {
        return;
    }

    float2 texCoords = (float2(pixelCoords) + 0.5) / float2(resolution);

    float linearDepth = CortaoUnpackDepth(CortaoSamplePackedData(texCoords, 0.0));
    if (linearDepth == 0.0) {
        // This is a skybox pixel, ignore it.
        return;
    }

    float3 normal = CortaoLoadNormal(int2(pixelCoords));

    float3 viewPosition = CortaoUnproject(params.unprojectParams, texCoords, linearDepth);

    // Setup a set of random variables used during the setup code
    uint2 random = CortaoHash(CortaoSeed(pixelCoords) +
                              uint2(params.randomSeed & 0xFFFFu, params.randomSeed >> 16u));

    float randomSampleRotation = float(random.x) * (float(CORTAO_PI2) / float(0xFFFFFFFFu));
    float offsetDither = float(random.y) * (1.0 / float(0xFFFFFFFFu));
    float randomTangentRotation = float(float(random.x ^ random.y ^ params.randomSeed) *
                                        (float(CORTAO_PI2) / float(0xFFFFFFFFu)));
    float2 sampleRotation = float2(sin(randomSampleRotation), cos(randomSampleRotation));

    // Calculate the view space radius from the screen space radius.
    float radius = linearDepth * params.maxApparentCircleRadiusCoefficient;

    // As we get closer to the sampling sphere, it starts to appear as a larger and larger surface
    // due to the horizon. This code calculates the required expanded circle radius to sample the
    // entire sphere.
    float inverseCircleDistance = rsqrt(linearDepth * linearDepth - radius * radius);
    float apparentCircleRadius = radius * inverseCircleDistance;
    float circleRadius = apparentCircleRadius * linearDepth;

    float3 viewDirection = -normalize(viewPosition);

    // Precompute the constant portion of the mip bias.
    // Due to our sample projection, the sample pattern becomes stretched closer to the edge of the
    // screen. Increase the mip bias to compensate.
    float stretchMultiplier = rsqrt(viewDirection.z);
    float baseMipBias = log2(apparentCircleRadius * stretchMultiplier) + params.mipBias;

    // Push the center pixel's position out slightly in the direction of the normal to avoid
    // self-shadowing.
    float pixelSize = params.normalBias * linearDepth;
    viewPosition = CortaoFma(float3(normal), float3(pixelSize, pixelSize, pixelSize), viewPosition);

    // Build a 2D samnple rotation matrix.
    // Instead of scaling the circle tangents later, we bake the radius scaling in here, as it's
    // cheaper to precompute.
    float2 scaledRotation = sampleRotation * circleRadius;
    float3x3 rotationScaleMatrix = float3x3(scaledRotation.x,
                                            -scaledRotation.y,
                                            0.0,
                                            scaledRotation.y,
                                            scaledRotation.x,
                                            0.0,
                                            0.0,
                                            0.0,
                                            1.0);

    // Construct the 3D sampling circle's tangents.
    float3 right;
    float3 up;
    CortaoConstructSamplingBasis(viewDirection, right, up);

    // Randomly flip the right vector.
    // This randomly switches between a clockwise and counter-clockwise sample pattern spiral, which
    // cancels out some errors.
    if ((random.x & 1u) == 0u) {
        right = -right;
    }

    // Construct a 3x3 matrix that takes 2D sample positions and transforms them onto the 3D circle.
    float3x3 circleMatrix = float3x3(right, up, viewPosition);

    // A simplified, truncated projection matrix.
    // Only supports FOV and off-center projection; nothing else:
    // The NDC --> texture space transformation is baked into these constants on the CPU side.
    float3x3 projectionMatrix = float3x3(params.projectionParams.x,
                                         0.0,
                                         0.0,
                                         0.0,
                                         params.projectionParams.y,
                                         0.0,
                                         params.projectionParams.zw,
                                         -1.0);

    // Multiply the 3 matrices together
    float3x3 offsetTransform = mul(mul(rotationScaleMatrix, circleMatrix), projectionMatrix);

    // Generate a tangent basis for the ray directions
    float3 t;
    float3 bt;
    CortaoConstructTangentBasis(normal, t, bt);
#if 1
    // Randomly rotate the tangent and bitangent around the normal
    float s = sin(randomTangentRotation);
    float c = cos(randomTangentRotation);
    float3 tangent = c * t + s * bt;
    float3 bitangent = c * bt - s * t;
#else
    // It's cheaper to not do the rotation, but this is only acceptable with many rays and few
    // samples, so disabled for now.
    float3 tangent = t;
    float3 bitangent = bt;
#endif
    float3x3 tangentSpaceTransform = float3x3(tangent, bitangent, normal);

    uint rayBits = CortaoComputeRayOcclusion(offsetDither,
                                             offsetTransform,
                                             tangentSpaceTransform,
                                             baseMipBias,
                                             viewPosition,
                                             normal,
                                             float3(viewDirection),
                                             radius,
                                             params);

    float occlusion = CortaoResolveOcclusion(rayBits);
    float3 bentNormal =
        mul(CortaoResolveBentNormal(rayBits),
            tangentSpaceTransform); // Transform tangent space bent normal back to view space.

    CortaoWriteResult(int2(pixelCoords), occlusion, bentNormal, tangentSpaceTransform, rayBits);
}
