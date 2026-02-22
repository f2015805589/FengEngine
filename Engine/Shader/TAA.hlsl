// TAA.hlsl
// Temporal Anti-Aliasing 着色器

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

Texture2D<float4> CurrentColorTexture : register(t0);
Texture2D<float4> HistoryColorTexture : register(t1);
Texture2D<float2> MotionVectorTexture : register(t2);
Texture2D<float> DepthTexture : register(t3);

SamplerState LinearClampSampler : register(s0);
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

// RGB 转 YCoCg
float3 RGBToYCoCg(float3 RGB)
{
    float Y = dot(RGB, float3(1, 2, 1));
    float Co = dot(RGB, float3(2, 0, -2));
    float Cg = dot(RGB, float3(-1, 2, -1));
    return float3(Y, Co, Cg);
}

// YCoCg 转 RGB
float3 YCoCgToRGB(float3 YCoCg)
{
    float Y = YCoCg.x * 0.25;
    float Co = YCoCg.y * 0.25;
    float Cg = YCoCg.z * 0.25;
    float R = Y + Co - Cg;
    float G = Y + Cg;
    float B = Y - Co - Cg;
    return float3(R, G, B);
}

// 历史颜色裁剪（AABB方式）
float3 ClipHistory(float3 History, float3 BoxMin, float3 BoxMax)
{
    float3 Filtered = (BoxMin + BoxMax) * 0.5f;
    float3 RayOrigin = History;
    float3 RayDir = Filtered - History;
    RayDir = abs(RayDir) < (1.0 / 65536.0) ? (1.0 / 65536.0) : RayDir;
    float3 InvRayDir = rcp(RayDir);

    float3 MinIntersect = (BoxMin - RayOrigin) * InvRayDir;
    float3 MaxIntersect = (BoxMax - RayOrigin) * InvRayDir;
    float3 EnterIntersect = min(MinIntersect, MaxIntersect);
    float ClipBlend = max(EnterIntersect.x, max(EnterIntersect.y, EnterIntersect.z));
    ClipBlend = saturate(ClipBlend);
    return lerp(History, Filtered, ClipBlend);
}

// 获取最近深度的片元UV（4角采样）
float2 GetClosestFragment(float2 uv)
{
    float2 k = InverseScreenSize;

    // 采样4个角落的深度
    float4 neighborhood = float4(
        DepthTexture.SampleLevel(PointClampSampler, uv + float2(-k.x, -k.y), 0),
        DepthTexture.SampleLevel(PointClampSampler, uv + float2(k.x, -k.y), 0),
        DepthTexture.SampleLevel(PointClampSampler, uv + float2(-k.x, k.y), 0),
        DepthTexture.SampleLevel(PointClampSampler, uv + float2(k.x, k.y), 0)
    );

    // D3D12 使用 reversed-z，深度值越大越近
    #define COMPARE_DEPTH(a, b) step(b, a)

    // 读取当前像素中心深度
    float3 result = float3(0.0, 0.0, DepthTexture.SampleLevel(PointClampSampler, uv, 0));

    // 依次比较4角深度，选出最靠近相机的
    result = lerp(result, float3(-1.0, -1.0, neighborhood.x), COMPARE_DEPTH(neighborhood.x, result.z));
    result = lerp(result, float3(1.0, -1.0, neighborhood.y), COMPARE_DEPTH(neighborhood.y, result.z));
    result = lerp(result, float3(-1.0, 1.0, neighborhood.z), COMPARE_DEPTH(neighborhood.z, result.z));
    result = lerp(result, float3(1.0, 1.0, neighborhood.w), COMPARE_DEPTH(neighborhood.w, result.z));

    return uv + result.xy * k;
}

// 3x3邻域偏移
static const int2 kOffsets3x3[9] = {
    int2(-1, -1), int2(0, -1), int2(1, -1),
    int2(-1,  0), int2(0,  0), int2(1,  0),
    int2(-1,  1), int2(0,  1), int2(1,  1)
};

// 调试模式：0=正常TAA, 1=只显示当前帧, 2=只显示历史帧, 3=显示Motion Vector
#define DEBUG_MODE 0

// Reinhard色调映射（带亮度权重，参考文章中的公式）
float3 ToneMap(float3 color)
{
    float luma = dot(color, float3(0.299, 0.587, 0.114));
    return color / (1.0 + luma);
}

// 逆Reinhard色调映射
float3 ToneMapInverse(float3 color)
{
    float luma = dot(color, float3(0.299, 0.587, 0.114));
    return color / max(1.0 - luma, 0.001);
}

// Variance Clipping（方差裁剪）- 参考文章中的改进方法
// 使用均值和标准差来确定AABB，比直接使用min/max更稳定
float3 ClipHistoryVariance(float3 history, float3 mean, float3 stdDev, float gamma)
{
    float3 boxMin = mean - gamma * stdDev;
    float3 boxMax = mean + gamma * stdDev;
    return ClipHistory(history, boxMin, boxMax);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 uv = input.TexCoord;

    // 1. 采样当前帧图像
    float3 currentColor = CurrentColorTexture.SampleLevel(PointClampSampler, uv, 0).rgb;

    #if DEBUG_MODE == 1
    return float4(currentColor, 1.0);
    #endif

    // 2. 获取最近深度的片元UV（用于Motion Vector膨胀，减少边缘锯齿）
    float2 closestUV = GetClosestFragment(uv);

    // 3. 采样Motion Vector并计算历史UV
    float2 motionVector = MotionVectorTexture.SampleLevel(PointClampSampler, closestUV, 0).xy;
    float2 historyUV = uv - motionVector;

    #if DEBUG_MODE == 3
    // 调试：显示Motion Vector（放大显示）
    return float4(abs(motionVector) * 50.0, 0.0, 1.0);
    #endif

    // 4. 采样历史帧
    float4 historyColorAlpha = HistoryColorTexture.SampleLevel(LinearClampSampler, historyUV, 0);
    float3 historyColor = historyColorAlpha.rgb;

    #if DEBUG_MODE == 2
    // 调试：显示历史帧（如果是黑色，说明历史缓冲没有内容）
    // 添加一个小的偏移来区分"真正的黑色"和"空缓冲"
    return float4(historyColor, 1.0);
    #endif

    // 5. 检查历史帧有效性
    // 注意：历史缓冲的alpha可能是0（初始状态），也可能是1（TAA写入后）
    // 我们用亮度来判断是否有有效内容
    float historyLuma = dot(historyColor, float3(0.299, 0.587, 0.114));
    bool historyValid = (historyLuma > 0.001 || historyColorAlpha.a > 0.01);
    bool historyInBounds = (historyUV.x >= 0.0 && historyUV.x <= 1.0 &&
                            historyUV.y >= 0.0 && historyUV.y <= 1.0);

    // 如果历史无效或越界，直接返回当前帧
    if (!historyValid || !historyInBounds)
    {
        return float4(currentColor, 1.0);
    }

    // 6. 采样3x3邻域，计算均值和方差（用于Variance Clipping）
    float3 m1 = float3(0.0, 0.0, 0.0);  // 一阶矩（均值）
    float3 m2 = float3(0.0, 0.0, 0.0);  // 二阶矩（用于计算方差）
    float3 neighborMin = float3(1e10, 1e10, 1e10);
    float3 neighborMax = float3(-1e10, -1e10, -1e10);

    // 在色调映射空间进行操作（参考文章建议）
    [unroll]
    for (int i = 0; i < 9; i++)
    {
        float2 sampleUV = uv + kOffsets3x3[i] * InverseScreenSize;
        float3 sampleColor = CurrentColorTexture.SampleLevel(PointClampSampler, sampleUV, 0).rgb;

        // 转换到YCoCg空间进行裁剪（参考文章推荐）
        float3 sampleYCoCg = RGBToYCoCg(ToneMap(sampleColor));

        m1 += sampleYCoCg;
        m2 += sampleYCoCg * sampleYCoCg;
        neighborMin = min(neighborMin, sampleYCoCg);
        neighborMax = max(neighborMax, sampleYCoCg);
    }

    // 计算均值和标准差
    m1 /= 9.0;
    m2 /= 9.0;
    float3 stdDev = sqrt(max(m2 - m1 * m1, 0.0));

    // 7. 历史颜色裁剪（Variance Clipping）
    // gamma参数控制裁剪范围，1.0-1.25是常用值
    float gamma = 1.0;
    float3 historyYCoCg = RGBToYCoCg(ToneMap(historyColor));

    // 使用方差裁剪（更稳定，减少鬼影）
    float3 clippedHistoryYCoCg = ClipHistoryVariance(historyYCoCg, m1, stdDev, gamma);

    // 同时使用AABB裁剪作为硬边界
    clippedHistoryYCoCg = clamp(clippedHistoryYCoCg, neighborMin, neighborMax);

    // 转换回RGB空间
    float3 clippedHistory = ToneMapInverse(YCoCgToRGB(clippedHistoryYCoCg));

    // 8. 计算混合因子
    // 基础混合因子：当前帧权重（UE4默认约0.04-0.1）
    float blendFactor = 0.1;

    // 根据运动速度动态调整（运动越快，当前帧权重越高，减少拖影）
    float motionLength = length(motionVector * ScreenSize);  // 转换为像素单位
    blendFactor = lerp(blendFactor, 0.3, saturate(motionLength * 0.5));

    // 根据裁剪程度调整（裁剪越多说明历史越不可靠）
    float clipAmount = length(historyYCoCg - clippedHistoryYCoCg);
    blendFactor = lerp(blendFactor, 0.5, saturate(clipAmount * 2.0));

    // 9. 亮度加权混合（参考文章中的公式，减少高亮闪烁）
    float3 currentToneMapped = ToneMap(currentColor);
    float3 historyToneMapped = ToneMap(clippedHistory);

    float lumaCurrent = dot(currentToneMapped, float3(0.299, 0.587, 0.114));
    float lumaHistory = dot(historyToneMapped, float3(0.299, 0.587, 0.114));

    // 亮度权重：1/(1+luma)
    float weightCurrent = 1.0 / (1.0 + lumaCurrent);
    float weightHistory = 1.0 / (1.0 + lumaHistory);

    // 加权混合
    float3 resultToneMapped = (currentToneMapped * weightCurrent * blendFactor +
                               historyToneMapped * weightHistory * (1.0 - blendFactor)) /
                              (weightCurrent * blendFactor + weightHistory * (1.0 - blendFactor));

    // 逆色调映射回线性空间
    float3 result = ToneMapInverse(resultToneMapped);

    // 防止NaN和Inf
    result = max(result, 0.0);
    result = min(result, 65504.0);  // half float最大值

    return float4(result, 1.0);
}
