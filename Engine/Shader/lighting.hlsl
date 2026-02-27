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
    float4x4 CurrentViewProjectionMatrix; // [156-171]
    float ShadowMode; // [172] 0=Hard, 1=PCF, 2=PCSS
    float3 _Padding5; // [173-175]
};

// 输入纹理
Texture2D g_DepthBuffer : register(t0);  // 深度缓冲（用于重建世界坐标）
Texture2D g_ShadowMap : register(t1);    // Shadow Map

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

// PCSS参数
static const float SHADOW_MAP_SIZE = 2048.0f;
static const float LIGHT_SIZE = 0.02f;  // 光源大小（控制软阴影范围）
static const int BLOCKER_SEARCH_SAMPLES = 16;
static const int PCF_SAMPLES = 25;

// Poisson Disk采样点（用于随机采样）
static const float2 poissonDisk[25] = {
    float2(-0.978698, -0.0884121),
    float2(-0.841121, 0.521165),
    float2(-0.71746, -0.50322),
    float2(-0.702933, 0.903134),
    float2(-0.663198, 0.15482),
    float2(-0.495102, -0.232887),
    float2(-0.364238, -0.961791),
    float2(-0.345866, -0.564379),
    float2(-0.325663, 0.64037),
    float2(-0.182714, 0.321329),
    float2(-0.142613, -0.0227363),
    float2(-0.0564287, -0.36729),
    float2(-0.0185858, 0.918882),
    float2(0.0381787, -0.728996),
    float2(0.16599, 0.093112),
    float2(0.253639, 0.719535),
    float2(0.369549, -0.655019),
    float2(0.423627, 0.429975),
    float2(0.530747, -0.364971),
    float2(0.566027, -0.940489),
    float2(0.639332, 0.0284127),
    float2(0.652089, 0.669668),
    float2(0.773797, 0.345012),
    float2(0.968871, 0.840449),
    float2(0.991882, -0.657338)
};

// 步骤1: 搜索遮挡物平均深度
float FindBlockerDepth(float2 shadowUV, float receiverDepth, float searchRadius)
{
    float blockerSum = 0.0f;
    int blockerCount = 0;
    float2 texelSize = 1.0f / SHADOW_MAP_SIZE;
    float bias = 0.0005f;  // 减小bias

    for (int i = 0; i < BLOCKER_SEARCH_SAMPLES; ++i)
    {
        float2 offset = poissonDisk[i] * searchRadius * texelSize;
        float shadowMapDepth = g_ShadowMap.Sample(g_Sampler, shadowUV + offset).r;

        if (shadowMapDepth < receiverDepth - bias)
        {
            blockerSum += shadowMapDepth;
            blockerCount++;
        }
    }

    if (blockerCount == 0)
        return -1.0f;  // 没有遮挡物

    return blockerSum / float(blockerCount);
}

// 步骤2: 根据遮挡物深度计算半影大小
float EstimatePenumbraSize(float receiverDepth, float blockerDepth)
{
    // 半影大小 = lightSize * (receiver - blocker) / blocker
    return LIGHT_SIZE * (receiverDepth - blockerDepth) / blockerDepth;
}

// 步骤3: PCF滤波
float PCF_Filter(float2 shadowUV, float receiverDepth, float filterRadius)
{
    float shadow = 0.0f;
    float2 texelSize = 1.0f / SHADOW_MAP_SIZE;
    float bias = 0.0005f;  // 减小bias

    for (int i = 0; i < PCF_SAMPLES; ++i)
    {
        float2 offset = poissonDisk[i] * filterRadius * texelSize;
        float shadowMapDepth = g_ShadowMap.Sample(g_Sampler, shadowUV + offset).r;
        shadow += (receiverDepth - bias > shadowMapDepth) ? 0.0f : 1.0f;
    }

    return shadow / float(PCF_SAMPLES);
}

// PCSS阴影计算
float CalculateShadowPCSS(float3 positionWS)
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
    float receiverDepth = projCoords.z;

    // 5. 边界检查
    if (shadowUV.x < 0.0f || shadowUV.x > 1.0f ||
        shadowUV.y < 0.0f || shadowUV.y > 1.0f ||
        receiverDepth < 0.0f || receiverDepth > 1.0f)
    {
        return 1.0f;
    }

    // 6. PCSS步骤1: 搜索遮挡物
    float searchRadius = LIGHT_SIZE * receiverDepth;  // 搜索半径随深度增加
    float blockerDepth = FindBlockerDepth(shadowUV, receiverDepth, searchRadius * 20.0f);

    // 没有遮挡物，完全被照亮
    if (blockerDepth < 0.0f)
        return 1.0f;

    // 7. PCSS步骤2: 计算半影大小
    float penumbraSize = EstimatePenumbraSize(receiverDepth, blockerDepth);

    // 8. PCSS步骤3: PCF滤波
    float filterRadius = penumbraSize * 30.0f;  // 放大滤波半径
    filterRadius = clamp(filterRadius, 1.0f, 15.0f);  // 限制范围

    return PCF_Filter(shadowUV, receiverDepth, filterRadius);
}

// 计算阴影因子（PCF软阴影 - 保留作为备选）
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

// 硬阴影（单点采样，无滤波）
float CalculateHardShadow(float3 positionWS)
{
    float4 positionLS = mul(LightViewProjectionMatrix, float4(positionWS, 1.0f));
    float3 projCoords = positionLS.xyz / positionLS.w;

    float2 shadowUV;
    shadowUV.x = projCoords.x * 0.5f + 0.5f;
    shadowUV.y = -projCoords.y * 0.5f + 0.5f;

    float currentDepth = projCoords.z;

    if (shadowUV.x < 0.0f || shadowUV.x > 1.0f ||
        shadowUV.y < 0.0f || shadowUV.y > 1.0f ||
        currentDepth < 0.0f || currentDepth > 1.0f)
    {
        return 1.0f;
    }

    float bias = 0.001f;
    float shadowMapDepth = g_ShadowMap.Sample(g_Sampler, shadowUV).r;
    return (currentDepth - bias > shadowMapDepth) ? 0.0f : 1.0f;
}

float4 LightPS(VSOut inPSInput) : SV_TARGET
{
    // 从GBuffer采样数据
    float depth = g_DepthBuffer.Sample(g_Sampler, inPSInput.texcoord).r;

    // 跳过天空（深度为1）
    if (depth >= 1.0f)
        return float4(1.0f, 1.0f, 1.0f, 1.0f);

    // 从深度重建世界空间位置
    float3 positionWS = ReconstructWorldPosition(inPSInput.texcoord, depth);

    // 根据阴影模式选择算法
    float shadow;
    if (ShadowMode < 0.5f)
        shadow = CalculateHardShadow(positionWS);
    else if (ShadowMode < 1.5f)
        shadow = CalculateShadow(positionWS);
    else
        shadow = CalculateShadowPCSS(positionWS);

    // 输出阴影因子
    return float4(shadow, shadow, shadow, 1.0f);
}
