// SSGIBlur.hlsl

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

Texture2D<float4> InputTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);

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

float3 BlurAxis(float2 uv, float2 axis)
{
    const float weights[5] = { 0.204164, 0.304005, 0.193783, 0.07208, 0.016 }; // normalized-ish

    float centerDepth = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
    float3 color = InputTexture.SampleLevel(LinearClampSampler, uv, 0).rgb * weights[0];
    float weightSum = weights[0];

    [unroll]
    for (int i = 1; i < 5; ++i)
    {
        float2 offset = axis * float(i) * InverseScreenSize;

        float2 uvA = uv + offset;
        float2 uvB = uv - offset;

        float depthA = DepthTexture.SampleLevel(PointClampSampler, uvA, 0);
        float depthB = DepthTexture.SampleLevel(PointClampSampler, uvB, 0);

        float edgeA = exp(-abs(depthA - centerDepth) * 80.0);
        float edgeB = exp(-abs(depthB - centerDepth) * 80.0);

        float wA = weights[i] * edgeA;
        float wB = weights[i] * edgeB;

        color += InputTexture.SampleLevel(LinearClampSampler, uvA, 0).rgb * wA;
        color += InputTexture.SampleLevel(LinearClampSampler, uvB, 0).rgb * wB;
        weightSum += wA + wB;
    }

    return color / max(weightSum, 1e-5);
}

float4 PSMainH(VSOutput input) : SV_TARGET
{
    return float4(BlurAxis(input.TexCoord, float2(1, 0)), 1.0);
}

float4 PSMainV(VSOutput input) : SV_TARGET
{
    return float4(BlurAxis(input.TexCoord, float2(0, 1)), 1.0);
}
