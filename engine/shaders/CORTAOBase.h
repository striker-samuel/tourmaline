// CORTAO Carbon Engine Implementation v1.0.0
//
// © 2026 Fenris Creations. All rights reserved.

#ifndef CORTAO_PACKED_NORMAL_BITS
#define CORTAO_PACKED_NORMAL_BITS 10
#endif

#if CORTAO_PACKED_NORMAL_BITS < 4 || CORTAO_PACKED_NORMAL_BITS > 20
#error "CORTAO_PACKED_NORMAL_BITS must be between 4 and 20, inclusive."
#endif

#if (CORTAO_PACKED_NORMAL_BITS & 1) == 1
#error                                                                                             \
    "CORTAO_PACKED_NORMAL_BITS must be even when CORTAO_PACKED_DATA is CORTAO_PACKED_DATA_NORMAL."
#endif

static const float CORTAO_PI = float(3.1415926535897932384626433832795);
static const float CORTAO_PI2 = float(6.283185307179586476925286766559); // PI * 2.0
static const float CORTAO_GOLDEN_ANGLE =
    float(2.3999632297286533222315555066336); // PI * (3.0 - sqrt(5.0))

float CortaoFma(float a, float b, float c) {
    return a * b + c;
}

float2 CortaoFma(float2 a, float2 b, float2 c) {
    return a * b + c;
}

float3 CortaoFma(float3 a, float3 b, float3 c) {
    return a * b + c;
}

float4 CortaoFma(float4 a, float4 b, float4 c) {
    return a * b + c;
}

uint CortaoSeed(uint2 coords) {
    uint2 q = coords * uint2(1719413u * 929u, 140473u * 2467u * 11u);
    return q.x ^ q.y;
}

uint CortaoHash(uint n) {
    return n * (n ^ (n >> 15u));
}

uint2 CortaoHash(uint2 n) {
    return n * (n ^ (n >> 15u));
}

uint CortaoEncodeNormal(float3 normal, float2 dither) {

    uint bitsPerComponent = uint(CORTAO_PACKED_NORMAL_BITS / 2);

    uint componentMask = (1u << bitsPerComponent) - 1u;
    uint componentMax = componentMask >> 1u;

    float2 encodedNormal = normal.xy * rsqrt(CortaoFma(normal.z, 2.0, 2.0));

    int2 values = int2(floor(CortaoFma(encodedNormal, float2(componentMax, componentMax), dither)));

    return (uint(values.x) & componentMask) |
           ((uint(values.y) & componentMask) << bitsPerComponent);
}

float3 CortaoDecodeNormal(uint packedData) {

    uint bitsPerComponent = uint(CORTAO_PACKED_NORMAL_BITS / 2);

    uint componentMask = (1u << bitsPerComponent) - 1u;
    uint componentMax = componentMask >> 1u;

    int2 leftShift = int2(32u - uint2(bitsPerComponent, bitsPerComponent * 2));
    int2 rightShift = int2(32u - bitsPerComponent, 32u - bitsPerComponent);
    int2 values = int2((int2(packedData, packedData) << leftShift) >> rightShift);

    float2 e = float2(values) * float(1.0 / float(componentMax));

    float z = CortaoFma(-e.x, e.x, CortaoFma(-e.y, e.y, float(1.0)));
    return float3(e * (float(2.0) * sqrt(z)), CortaoFma(z, float(2.0), float(-1.0)));
}

float CortaoUnpackDepth(uint packedData) {
    return asfloat(packedData);
}

float4 CortaoUnpackDepth(uint4 packedData) {
    return asfloat(packedData);
}

uint2 CortaoGetResolution(uint packedResolution) {
    return uint2(packedResolution & 0xFFFFu, packedResolution >> 16u);
}
