// Irradiance Convolution Shader
// 对环境立方体贴图进行余弦加权卷积，生成漫反射辐照度贴图

#define PI 3.14159265359

TextureCube<float4> EnvironmentMap : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2DArray<float4> OutputIrradiance : register(u0);

// 将立方体贴图面索引和UV转换为采样方向
float3 GetSamplingDirection(uint face, float2 uv)
{
    // uv 范围 [0, 1] -> [-1, 1]
    float2 st = uv * 2.0 - 1.0;

    float3 dir;
    switch (face)
    {
        case 0: dir = float3( 1.0, -st.y, -st.x); break; // +X
        case 1: dir = float3(-1.0, -st.y,  st.x); break; // -X
        case 2: dir = float3( st.x,  1.0,  st.y); break; // +Y
        case 3: dir = float3( st.x, -1.0, -st.y); break; // -Y
        case 4: dir = float3( st.x, -st.y,  1.0); break; // +Z
        case 5: dir = float3(-st.x, -st.y, -1.0); break; // -Z
    }

    return normalize(dir);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    uint width, height, faces;
    OutputIrradiance.GetDimensions(width, height, faces);

    uint face = DTid.z;
    float2 uv = (float2(DTid.xy) + 0.5) / float2(width, height);

    float3 N = GetSamplingDirection(face, uv);

    // 构建切线空间
    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
    float3 right = normalize(cross(up, N));
    up = cross(N, right);

    float3 irradiance = float3(0.0, 0.0, 0.0);
    float sampleCount = 0.0;

    // 半球采样
    float sampleDelta = 0.025; // 采样密度
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            // 球面坐标转笛卡尔坐标（切线空间）
            float3 tangentSample = float3(
                sin(theta) * cos(phi),
                sin(theta) * sin(phi),
                cos(theta)
            );

            // 切线空间转世界空间
            float3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;

            // 采样环境贴图
            float3 envColor = EnvironmentMap.SampleLevel(LinearSampler, sampleVec, 0).rgb;

            // 余弦加权
            irradiance += envColor * cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }

    irradiance = PI * irradiance / sampleCount;

    OutputIrradiance[DTid] = float4(irradiance, 1.0);
}
