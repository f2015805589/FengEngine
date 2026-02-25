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
    float4x4 LightViewProjectionMatrix; // 112-127 (LiSPSM矩阵)
    float4x4 PreviousViewProjectionMatrix; // 128-143
    float2 JitterOffset; // 144-145
    float2 PreviousJitterOffset; // 146-147
    float2 ScreenSize; // 148-149
    float2 InverseScreenSize; // 150-151
    float NearPlane; // 152
    float FarPlane; // 153
    float2 _Padding4; // 154-155
};

// GBuffer纹理
Texture2D g_ColorBuffer : register(t0);
Texture2D g_NormalBuffer : register(t1);
Texture2D g_MetallicRoughnessBuffer : register(t2);
Texture2D g_DepthBuffer : register(t3);  // 深度缓冲（用于重建世界坐标）
Texture2D g_ShadowMap : register(t4);    // Shadow Map

// 采样器
SamplerState g_Sampler : register(s0);
SamplerComparisonState g_ShadowSampler : register(s1);  // 阴影比较采样器

// 从深度值重建世界空间位置
float3 ReconstructWorldPosition(float2 uv, float depth)
{
    // 1. 从UV和深度构建NDC坐标
    float4 ndcPos;
    ndcPos.x = uv.x * 2.0f - 1.0f;
    ndcPos.y = (1.0f - uv.y) * 2.0f - 1.0f;  // Y轴翻转
    ndcPos.z = depth;
    ndcPos.w = 1.0f;

    // 2. NDC -> View空间
    float4 viewPos = mul(InverseProjectionMatrix, ndcPos);
    viewPos /= viewPos.w;

    // 3. View空间 -> World空间
    float4 worldPos = mul(InverseViewMatrix, viewPos);

    return worldPos.xyz;
}

// 计算阴影因子（PCF软阴影）
float CalculateShadow(float3 positionWS)
{
    // 1. 将世界坐标变换到光源裁剪空间
    float4 positionLS = mul(LightViewProjectionMatrix, float4(positionWS, 1.0f));

    // 2. 透视除法，得到NDC坐标
    float3 projCoords = positionLS.xyz / positionLS.w;

    // 3. 将NDC坐标从[-1,1]映射到[0,1]（用于纹理采样）
    float2 shadowUV;
    shadowUV.x = projCoords.x * 0.5f + 0.5f;
    shadowUV.y = -projCoords.y * 0.5f + 0.5f;  // Y轴翻转

    // 4. 当前片元的深度（在光源空间）
    float currentDepth = projCoords.z;

    // 5. 边界检查：超出Shadow Map范围的区域不在阴影中
    if (shadowUV.x < 0.0f || shadowUV.x > 1.0f ||
        shadowUV.y < 0.0f || shadowUV.y > 1.0f ||
        currentDepth < 0.0f || currentDepth > 1.0f)
    {
        return 1.0f;  // 不在阴影中
    }

    // 6. PCF (Percentage Closer Filtering) 软阴影
    float shadow = 0.0f;
    float2 texelSize = 1.0f / float2(2048.0f, 2048.0f);  // Shadow Map尺寸

    // 3x3 PCF核
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            float2 offset = float2(x, y) * texelSize;
            float shadowMapDepth = g_ShadowMap.Sample(g_Sampler, shadowUV + offset).r;

            // 深度比较：如果当前深度大于Shadow Map深度，则在阴影中
            // 添加小偏移防止自阴影（Shadow Acne）
            float bias = 0.001f;
            shadow += (currentDepth - bias > shadowMapDepth) ? 0.0f : 1.0f;
        }
    }

    shadow /= 9.0f;  // 平均9个采样点

    return shadow;
}

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
    float depth = g_DepthBuffer.Sample(g_Sampler, inPSInput.texcoord).r;

    // 解包数据
    float3 baseColor = albedo.rgb;
    float3 normalWS = normalize(normal.xyz);
    float metallic = mr.r;
    float roughness = mr.g;

    // 从深度重建世界空间位置
    float3 positionWS = ReconstructWorldPosition(inPSInput.texcoord, depth);

    // 计算视线方向
    float3 viewDir = normalize(CameraPositionWS - positionWS);

    // 计算光照方向（LightDirection是从表面指向光源）
    float3 lightDir = normalize(-LightDirection);

    // 简单光照计算
    float NdotL = max(dot(normalWS, lightDir), 0.0);

    // 计算阴影
    float shadow = CalculateShadow(positionWS);

    // 最终光照 = NdotL * 阴影因子
    float finalLight = NdotL * shadow;

    // 输出光照强度（带阴影）
    return float4(finalLight, finalLight, finalLight, 1.0f);
}
