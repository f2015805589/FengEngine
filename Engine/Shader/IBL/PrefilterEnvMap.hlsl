// Pre-filtered Environment Map Generator
// 使用 GGX 重要性采样生成预过滤的环境贴图
// 每个 mip level 对应不同的粗糙度

#define PI 3.14159265359

TextureCube<float4> EnvironmentMap : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2DArray<float4> OutputPrefilter : register(u0);

cbuffer PrefilterParams : register(b0)
{
    float Roughness;
    uint MipLevel;
    uint OutputWidth;
    uint OutputHeight;
};

// Van der Corput 序列
float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

// GGX 重要性采样
float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    // 球面坐标转笛卡尔坐标（切线空间）
    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    // 构建切线空间
    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    // 切线空间转世界空间
    float3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}

// GGX 分布函数
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom);
}

// 将立方体贴图面索引和UV转换为采样方向
float3 GetSamplingDirection(uint face, float2 uv)
{
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
    uint face = DTid.z;
    float2 uv = (float2(DTid.xy) + 0.5) / float2(OutputWidth, OutputHeight);

    float3 N = GetSamplingDirection(face, uv);
    float3 R = N;
    float3 V = R;

    float3 prefilteredColor = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;

    const uint SAMPLE_COUNT = 1024u;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, Roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0)
        {
            // 根据 PDF 计算 mip level 以减少 artifact
            float D = DistributionGGX(N, H, Roughness);
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001;

            float resolution = 512.0; // 原始环境贴图分辨率
            float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);

            float mipLevel = Roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

            prefilteredColor += EnvironmentMap.SampleLevel(LinearSampler, L, mipLevel).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor = prefilteredColor / totalWeight;

    OutputPrefilter[DTid] = float4(prefilteredColor, 1.0);
}
