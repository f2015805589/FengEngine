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
Texture2D<float> DepthTexture : register(t2);

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

    float3 current = CurrentSSGITexture.SampleLevel(PointClampSampler, uv, 0).rgb;
    float3 history = HistorySSGITexture.SampleLevel(LinearClampSampler, uv, 0).rgb;

    float centerDepth = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
    float2 k = InverseScreenSize;
    float minDepth = centerDepth;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 sampleUV = uv + float2(x, y) * k;
            float d = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
            minDepth = min(minDepth, d);
        }
    }

    float edge = saturate(abs(centerDepth - minDepth) * 120.0);
    float blend = lerp(TemporalBlend, 0.2, edge);

    float3 result = lerp(current, history, saturate(blend));
    return float4(result, 1.0);
}
