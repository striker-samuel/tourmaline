float3 SafeNormalize(float3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-6f) ? (v * rsqrt(len2)) : float3(0, -1, 0);
}

float3 ChooseUp(float3 dir)
{
    // If direction nearly collinear with world up, use +Z as up
    float d = abs(dot(dir, float3(0, 1, 0)));
    return (d > 0.99f) ? float3(0, 0, 1) : float3(0, 1, 0);
}

float4x4 LookAtLH(float3 eye, float3 target, float3 up)
{
    float3 f = normalize(target - eye);
    float3 r = normalize(cross(up, f));
    float3 u = cross(f, r);

    return float4x4(
        r.x, u.x, f.x, 0.0,
        r.y, u.y, f.y, 0.0,
        r.z, u.z, f.z, 0.0,
        -dot(r, eye), -dot(u, eye), -dot(f, eye), 1.0
    );
}

float4x4 GetLightView(float3 lightPos, float3 lightDir)
{
    float3 dir = SafeNormalize(lightDir);
    float3 up = ChooseUp(dir);

    return LookAtLH(lightPos, lightPos + dir, up);
}

float4x4 GetLightProj(float outerAngle, float aspect, float nearZ, float farZ)
{
    float fov = radians(outerAngle * 2.0); // cone opening
    return mul(
        float4x4(
            1 / tan(fov / 2.0), 0, 0, 0,
            0, aspect / tan(fov / 2.0), 0, 0,
            0, 0, farZ / (farZ - nearZ), 1,
            0, 0, (-nearZ * farZ) / (farZ - nearZ), 0
        ),
        float4x4(1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1)
    );
}

