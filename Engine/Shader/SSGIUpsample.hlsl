// SSGIUpsample.hlsl - SSGI 双线性升采样
// 将低分辨率SSGI结果升采样到全分辨率

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

Texture2D<float4> SSGILowResTexture : register(t0);  // 低分辨率SSGI
Texture2D<float>  DepthTexture      : register(t1);  // 全分辨率深度

SamplerState LinearClampSampler : register(s3);
SamplerState PointClampSampler  : register(s1);

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

// 获取线性深度
float GetLinearDepth(float depthNdc)
{
    float4 clipPos = float4(0, 0, depthNdc, 1.0);
    float4 viewPos = mul(InverseProjectionMatrix, clipPos);
    return viewPos.z / viewPos.w;
}

// 深度感知双线性升采样
float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 fullResUV = input.TexCoord;

    // 采样全分辨率深度
    float centerDepthNdc = DepthTexture.SampleLevel(PointClampSampler, fullResUV, 0);
    if (centerDepthNdc >= 1.0)
        return float4(0, 0, 0, 0);

    // 双线性采样低分辨率SSGI，使用clamp边界模式避免黑边
    float4 ssgiLowRes = SSGILowResTexture.SampleLevel(LinearClampSampler, fullResUV, 0);

    return ssgiLowRes;
}
