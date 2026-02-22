// TaaCopy.hlsl
// 简单的纹理复制着色器，用于将TAA结果复制到交换链

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

Texture2D<float4> SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = float4(input.Position, 1.0);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return SourceTexture.Sample(LinearSampler, input.TexCoord);
}
