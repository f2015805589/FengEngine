// Engine/Shader/skybox.hlsl
cbuffer cbViewProj : register(b0)
{
    float4x4 viewProj;
};

struct VS_INPUT
{
    float3 position : POSITION;
};

struct PS_INPUT
{
    float4 position : SV_POSITION;
    float3 texCoord : TEXCOORD0;
};

PS_INPUT VS(VS_INPUT input)
{
    PS_INPUT output;
    output.position = mul(float4(input.position, 1.0f), viewProj);
    output.texCoord = input.position; // ʹ��λ����Ϊ��������
    return output;
}

Texture2D skyboxTexture : register(t0);
SamplerState skyboxSampler : register(s0);

float4 PS(PS_INPUT input) : SV_TARGET
{
    // ��3D��������ת��Ϊ2D�������꣨�򻯵�����ӳ�䣩
    float3 normal = normalize(input.texCoord);
    float2 uv;
    
    uv.x = 0.5f + atan2(normal.z, normal.x) / (2 * 3.1415926535f);
    uv.y = 0.5f - asin(normal.y) / 3.1415926535f;
    
    return skyboxTexture.Sample(skyboxSampler, uv);
}