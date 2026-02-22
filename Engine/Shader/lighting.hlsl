struct VertexData
{
    float4 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct VSOut
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

// 全屏幕的顶点着色器
VSOut LightVS(VertexData inVertex)
{
    VSOut vo;
    vo.position = inVertex.position;
    vo.texcoord = inVertex.texcoord;
    return vo;
}

// 场景常量缓冲区
cbuffer DefaultVertexCB : register(b0)
{
    float4x4 ProjectionMatrix; // 0-15
    float4x4 ViewMatrix; // 16-31
    float4x4 ModelMatrix; // 32-47
    float4x4 IT_ModelMatrix; // 48-63
    float3 LightDirection; // 64-66（实际占用64-67，有padding）
    float _Padding0;
    float3 CameraPositionWS; // 68-70（实际占用68-71，有padding）
    float _Padding1;
    float Skylight; // 72
    float3 _Padding2; // 73-75
    float4x4 InverseProjectionMatrix; // 76-91
    float4x4 InverseViewMatrix; // 92-107
    float3 SkylightColor; // 108-110
    float _Padding3; // 111
    float4x4 LightViewProjectionMatrix; // 112-127
};

// GBuffer纹理
Texture2D g_ColorBuffer : register(t0);
Texture2D g_NormalBuffer : register(t1);
Texture2D g_MetallicRoughnessBuffer : register(t2);

// 采样器
SamplerState g_Sampler : register(s0);

// 光照计算函数
float3 CalculateLighting(float3 albedo, float3 normal, float metallic, float roughness,
                         float3 positionWS, float3 viewDir, float3 lightDir, float3 lightColor)
{
    // 漫反射项
    float NdotL = max(dot(normal, lightDir), 0.0f);
    float3 diffuse = albedo * lightColor * NdotL;

    // 高光项 (简化版)
    float3 halfDir = normalize(viewDir + lightDir);
    float NdotH = max(dot(normal, halfDir), 0.0f);
    float specularPower = roughness > 0.0f ? 1.0f / (roughness * roughness) : 1000.0f;
    float3 specular = lightColor * pow(NdotH, specularPower) * metallic;

    return diffuse + specular;
}

float4 LightPS(VSOut inPSInput) : SV_TARGET
{
    // 从GBuffer采样数据
    float4 albedo = g_ColorBuffer.Sample(g_Sampler, inPSInput.texcoord);
    float4 normal = g_NormalBuffer.Sample(g_Sampler, inPSInput.texcoord);
    float4 mr = g_MetallicRoughnessBuffer.Sample(g_Sampler, inPSInput.texcoord);

    // 解包数据
    float3 baseColor = albedo.rgb;
    float3 normalWS = normalize(normal.xyz);
    float metallic = mr.r;
    float roughness = mr.g;

    // 计算光照方向
    float3 positionWS = float3(inPSInput.texcoord, 0.0f); // 实际应用中需要从深度缓冲重建
    float3 viewDir = normalize(CameraPositionWS - positionWS);

    // 简单光照计算
    float NdotL = max(dot(normalWS, -LightDirection), 0.0);

    // 输出光照强度（与screen.hlsl的方向一致）
    return float4(NdotL, NdotL, NdotL, 1);
    //return float4(1, 1, 1, 1);
}
