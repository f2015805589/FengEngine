// SSGIDepthMax.hlsl

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

Texture2D<float> DepthTexture : register(t0);
SamplerState PointClampSampler : register(s1);

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
    float center = DepthTexture.SampleLevel(PointClampSampler, uv, 0);

    float maxDepth = center;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 sampleUV = uv + float2(x, y) * InverseScreenSize;
            float d = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
            maxDepth = max(maxDepth, d);
        }
    }

    return float4(maxDepth, maxDepth, maxDepth, 1.0);
}
