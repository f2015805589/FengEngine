// SSGITAA.hlsl

cbuffer SceneConstants : register(b0)
{
    float4x4 ProjectionMatrix;
    float4x4 ViewMatrix;
    float4x4 ModelMatrix;
    float4x4 NormalMatrix;
    float3 LightDirection;
    float padding1;
    float3 CameraPosition;
    float padding2;
    float Skylight;
    float3 _Padding0;
    float4x4 InverseProjectionMatrix;
    float4x4 InverseViewMatrix;
    float3 SkylightColor;
    float padding3;
    float4x4 LightViewProjectionMatrix;
    float4x4 PreviousViewProjectionMatrix;
    float2 JitterOffset;
    float2 PreviousJitterOffset;
    float2 ScreenSize;
    float2 InverseScreenSize;
    float NearPlane;
    float FarPlane;
    float2 padding4;
};

cbuffer SsgiConstants : register(b1)
{
    float2 SSGIResolution;
    float2 SSGIInverseResolution;
    float SSGIRadius;
    float SSGIIntensity;
    int SSGIStepCount;
    int SSGIDirectionCount;
    int SSGIFrameCounter;
    int DepthPyramidPasses;
    float DepthThickness;
    float TemporalBlend;
    float2 padding5;
};

Texture2D<float4> CurrentSSGITexture : register(t0);
Texture2D<float4> HistorySSGITexture : register(t1);
Texture2D<float>  DepthTexture       : register(t2);
Texture2D<float4> VelocityTexture    : register(t3);  // Motion Vector from GBuffer

SamplerState PointClampSampler : register(s1);
SamplerState LinearClampSampler : register(s3);

struct VSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = float4(input.Position, 1.0);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 uv = input.TexCoord;

    // 采样当前帧SSGI
    float4 currentSSGI = CurrentSSGITexture.SampleLevel(PointClampSampler, uv, 0);
    float3 current = currentSSGI.rgb;
    float currentWeight = currentSSGI.a; // SSGI命中率

    // 采样motion vector（从GBuffer）
    float2 velocity = VelocityTexture.SampleLevel(PointClampSampler, uv, 0).xy;

    // 计算历史帧UV（使用motion vector重投影）
    float2 historyUV = uv - velocity;

    // 检查历史UV是否在屏幕范围内
    bool validHistory = all(historyUV >= 0.0) && all(historyUV <= 1.0);

    float3 result = current;

    if (validHistory)
    {
        // 采样历史帧SSGI
        float3 history = HistorySSGITexture.SampleLevel(LinearClampSampler, historyUV, 0).rgb;

        // 3x3邻域采样，用于颜色裁剪（减少ghosting）
        float3 colorMin = current;
        float3 colorMax = current;
        float3 colorAvg = current;

        float2 k = InverseScreenSize;
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                if (x == 0 && y == 0) continue;

                float2 sampleUV = uv + float2(x, y) * k;
                float3 neighbor = CurrentSSGITexture.SampleLevel(PointClampSampler, sampleUV, 0).rgb;

                colorMin = min(colorMin, neighbor);
                colorMax = max(colorMax, neighbor);
                colorAvg += neighbor;
            }
        }
        colorAvg /= 9.0;

        // 颜色裁剪：将历史帧颜色限制在当前帧邻域范围内（减少ghosting）
        history = clamp(history, colorMin, colorMax);

        // 深度不连续检测（边缘处降低混合权重）
        float centerDepth = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
        float historyDepth = DepthTexture.SampleLevel(PointClampSampler, historyUV, 0);
        float depthDiff = abs(centerDepth - historyDepth);
        float depthWeight = saturate(1.0 - depthDiff * 100.0);

        // 自适应混合权重
        // - 边缘处使用更低的混合权重（减少ghosting）
        // - SSGI命中率低时使用更高的混合权重（减少噪点）
        float baseBlend = TemporalBlend; // 0.9
        float edgeBlend = 0.3;

        float blend = lerp(edgeBlend, baseBlend, depthWeight);

        // 如果SSGI命中率很低，增加时域混合来减少噪点
        blend = lerp(blend, 0.95, 1.0 - currentWeight);

        // 时域累积
        result = lerp(current, history, blend);
    }

    return float4(result, 1.0);
}
