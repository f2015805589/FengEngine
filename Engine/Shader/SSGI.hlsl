// SSGI.hlsl - 屏幕空间全局光照
// 基于屏幕空间光线追踪的间接漫反射照明（Path Tracing）

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

// t0: 保留（后续深度金字塔用）
Texture2D<float>  DepthMaxTexture  : register(t0);
Texture2D<float4> BaseColorTexture : register(t1);
Texture2D<float4> NormalTexture    : register(t2);
Texture2D<float4> NoiseTexture     : register(t3);
Texture2D<float>  DepthTexture     : register(t4);
Texture2D<float4> HistoryTexture   : register(t5);  // 历史帧SSGI
Texture2D<float2> VelocityTexture  : register(t6);  // Motion Vector (RG only)

SamplerState PointClampSampler  : register(s1);
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

// ========== 工具函数 ==========

#define PI 3.14159265359

// 从深度缓冲重建视图空间位置（与GTAO完全一致）
float3 ReconstructViewPos(float2 uv, float depthNdc)
{
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clipPos = float4(ndc, depthNdc, 1.0);
    float4 viewPos = mul(InverseProjectionMatrix, clipPos);
    return viewPos.xyz / viewPos.w;
}

// 获取线性深度（视图空间Z值）
float GetLinearDepth(float depthNdc)
{
    float4 clipPos = float4(0, 0, depthNdc, 1.0);
    float4 viewPos = mul(InverseProjectionMatrix, clipPos);
    return viewPos.z / viewPos.w;
}

// 视图空间位置投影到屏幕UV（与GTAO的ViewToScreenUV完全一致）
float2 ProjectToUV(float3 viewPos)
{
    float4 clipPos = mul(ProjectionMatrix, float4(viewPos, 1.0));
    float2 ndc = clipPos.xy / clipPos.w;
    float2 uv = ndc * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    return uv;
}

// 视图空间位置投影到像素坐标
// 关键：z 存 1/viewZ（倒数深度），因为 1/z 才能在屏幕空间线性插值！
float3 ProjectToPixel(float3 viewPos)
{
    float2 uv = ProjectToUV(viewPos);
    float3 pixel;
    pixel.xy = uv * SSGIResolution;
    pixel.z = 1.0 / viewPos.z; // 存倒数深度，透视校正插值
    return pixel;
}

// 低差异序列 - R2 准随机序列
float2 R2Sequence(int index)
{
    const float g = 1.32471795724;
    const float a1 = 1.0 / g;
    const float a2 = 1.0 / (g * g);
    return frac(float2(a1 * index, a2 * index));
}

// Interleaved Gradient Noise（与GTAO相同）
float InterleavedGradientNoise(float2 position)
{
    return frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
}

// 构建余弦加权半球采样方向（视图空间法线 -> 视图空间方向）
float3 CosineWeightedHemisphere(float3 normalV, float2 rand2)
{
    float phi = 2.0 * PI * rand2.x;
    float cosTheta = sqrt(rand2.y);
    float sinTheta = sqrt(1.0 - rand2.y);

    // 构建TBN
    float3 up = (abs(normalV.z) < 0.999) ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 T = normalize(cross(up, normalV));
    float3 B = cross(normalV, T);

    return normalize(T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + normalV * cosTheta);
}

// ========== 屏幕空间射线结构 ==========

struct ScreenSpaceRay
{
    float3 o;       // 起点 (pixel.x, pixel.y, 1/viewZ)
    float3 d;       // 方向 (归一化使xy步长为1像素)
    float tmax;     // 最大步进距离（像素长度）
    bool valid;     // 射线是否有效
};

// 创建屏幕空间射线
// 参考文章：投影视图空间射线的两个端点到像素空间
ScreenSpaceRay CreateSSRay(float3 originV, float3 dirV, float tmax)
{
    ScreenSpaceRay ray;
    ray.o = float3(0, 0, 0);
    ray.d = float3(0, 0, 0);
    ray.tmax = 0;
    ray.valid = false;

    // 如果光线方向朝向近平面(z递减方向)，裁剪tmax避免穿过近平面
    if (dirV.z < 0.0)
    {
        float maxT = (originV.z - NearPlane * 1.01) / (-dirV.z);
        tmax = min(tmax, max(maxT, 0.0));
    }

    if (tmax < 0.001)
        return ray;

    // 视图空间终点
    float3 endV = originV + dirV * tmax;

    // 投影两端点到像素空间（.z = 1/viewZ）
    float3 p0 = ProjectToPixel(originV);
    float3 p1 = ProjectToPixel(endV);

    // 屏幕空间方向和长度
    float3 delta = p1 - p0;
    float screenLen = length(delta.xy);

    if (screenLen < 0.5)
        return ray; // 屏幕投影太短

    // 归一化：每步约1像素
    ray.o = p0;
    ray.d = delta / screenLen;
    ray.tmax = screenLen;
    ray.valid = true;

    return ray;
}

// 屏幕空间光线追踪
// 核心：沿屏幕空间步进，线性插值 1/z（透视校正），与场景深度比较
bool TraceSSRay(ScreenSpaceRay ray, float thickness, out float2 hitUV)
{
    hitUV = float2(0, 0);

    if (!ray.valid || ray.tmax < 1.0)
        return false;

    float marchStep = 1.01; // 每步约1个SSGI像素
    float t = marchStep;    // 跳过起点
    int2 prevPixel = int2(-1, -1);

    int maxSteps = min(SSGIStepCount, 256); // 使用可调节的步数，上限256

    [loop]
    for (int i = 0; i < maxSteps && t < ray.tmax; ++i)
    {
        float3 p = ray.o + ray.d * t;
        int2 pixel = int2(p.xy);

        // 出界检查
        if (any(pixel < int2(0, 0)) || any(pixel >= int2(SSGIResolution)))
            break;

        // 跳过重复像素
        if (all(pixel == prevPixel))
        {
            t += marchStep;
            continue;
        }
        prevPixel = pixel;

        // 像素中心UV
        float2 sampleUV = (float2(pixel) + 0.5) * SSGIInverseResolution;

        // 采样场景NDC深度
        float sceneDepthNdc = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
        if (sceneDepthNdc >= 1.0)
        {
            t += marchStep;
            continue; // 天空，跳过
        }

        // 场景的线性深度（视图空间Z）
        float sceneLinearDepth = GetLinearDepth(sceneDepthNdc);

        // 光线在当前像素处的线性深度
        // p.z 存的是 1/viewZ，需要转回 viewZ
        float rayLinearDepth = 1.0 / p.z;

        // 深度差：光线深度 - 场景深度
        // 正值 = 光线在场景表面后面（穿过了几何体）
        float depthDiff = rayLinearDepth - sceneLinearDepth;

        // 自适应厚度：随深度线性增长
        float adaptiveThickness = thickness + abs(sceneLinearDepth) * 0.02;

        // 命中条件：光线穿过场景表面，但没有穿太深
        if (depthDiff > 0.0 && depthDiff < adaptiveThickness)
        {
            hitUV = sampleUV;
            return true;
        }

        t += marchStep;
    }

    return false;
}

// ========== 主函数 ==========

float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 uv = input.TexCoord;

    // 采样中心像素深度
    float centerDepthNdc = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
    if (centerDepthNdc >= 1.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    // 世界法线 → 视图空间法线
    float3 centerNormalW = normalize(NormalTexture.SampleLevel(PointClampSampler, uv, 0).xyz);
    float3 centerNormalV = normalize(mul((float3x3)ViewMatrix, centerNormalW));

    // 重建视图空间位置
    float3 centerPosV = ReconstructViewPos(uv, centerDepthNdc);

    // 噪声：使用视图空间位置 + 帧计数，避免固定模式
    // 视图空间位置相对稳定，不会随屏幕旋转
    float3 noiseInput = centerPosV * 0.5 + float3(SSGIFrameCounter * 0.1, 0, 0);
    float noise1 = frac(sin(dot(noiseInput, float3(12.9898, 78.233, 45.164))) * 43758.5453);

    int directions = max(SSGIDirectionCount, 1);

    // 计算分辨率缩放（在循环外定义，供后续使用）
    float resolutionScale = ScreenSize.x / SSGIResolution.x; // 1=全分辨率, 2=半分辨率, 4=1/4分辨率

    float3 giSum = 0.0;
    float hitCount = 0.0;

    [loop]
    for (int dirIndex = 0; dirIndex < directions; ++dirIndex)
    {
        // 准随机数用于半球采样
        // 使用R2序列 + 空间噪声 + 帧间抖动（用于时域累积降噪）
        float frameNoise = frac(float(SSGIFrameCounter) * 0.618033988749895); // 黄金比例
        float2 r2 = R2Sequence(dirIndex);
        r2.x = frac(r2.x + noise1 * 0.5 + frameNoise * 0.3); // 增大帧间抖动到30%
        r2.y = frac(r2.y + noise1 * 0.3 + frameNoise * 0.15); // 仰角抖动15%

        // 在法线半球内余弦加权采样一个3D方向（视图空间）
        float3 dirV = CosineWeightedHemisphere(centerNormalV, r2);

        // 根据分辨率调整半径：低分辨率下减小半径，使用自定义缩放
        float radiusScale;
        if (resolutionScale <= 1.0)
            radiusScale = 1.0;      // 全分辨率: 6.0
        else if (resolutionScale <= 2.0)
            radiusScale = 2.1;      // 半分辨率: 6.0/2.1 = 2.86
        else
            radiusScale = 3.33;     // 1/4分辨率: 6.0/3.33 = 1.8
        float adjustedRadius = SSGIRadius / radiusScale;

        // 将视图空间射线投影到屏幕空间
        ScreenSpaceRay ssRay = CreateSSRay(centerPosV, dirV, adjustedRadius);

        if (!ssRay.valid)
            continue;

        // 在屏幕空间追踪，透视校正深度插值
        float2 hitUV;
        bool hit = TraceSSRay(ssRay, DepthThickness, hitUV);

        if (hit)
        {
            // 采样命中处的 albedo
            float3 hitAlbedo = BaseColorTexture.SampleLevel(LinearClampSampler, hitUV, 0).rgb;

            // 距离衰减
            float hitDepthNdc = DepthTexture.SampleLevel(PointClampSampler, hitUV, 0);
            float3 hitPosV = ReconstructViewPos(hitUV, hitDepthNdc);
            float dist = length(hitPosV - centerPosV);
            float distAtt = saturate(1.0 - dist / adjustedRadius); // 使用调整后的半径

            // 蒙特卡洛：余弦加权半球采样 PDF = cos(θ)/π
            // Lambert BRDF = albedo/π
            // 积分: Σ (BRDF × cos(θ)) / PDF / N = Σ albedo / N
            giSum += hitAlbedo * distAtt;
            hitCount += 1.0;
        }
        // 未命中的射线不贡献任何光照，留给 IBL 在合成阶段通过 alpha 权重补充
    }

    // 蒙特卡洛归一化
    float3 indirect = giSum / float(directions);

    // 根据分辨率调整强度：低分辨率下增强强度补偿半径减小
    float intensityScale;
    if (resolutionScale <= 1.0)
        intensityScale = 1.0;      // 全分辨率: 1x强度
    else if (resolutionScale <= 2.0)
        intensityScale = 1.25;      // 半分辨率: 1.25x强度
    else
        intensityScale = 1.5;      // 1/4分辨率: 1.5x强度

    // 应用强度
    indirect *= SSGIIntensity * intensityScale;

    // alpha 通道输出 SSGI 命中率（0=全部未命中，1=全部命中）
    float ssgiWeight = hitCount / float(directions);

    // ========== Temporal Accumulation (时域累积) ==========
    // 采样motion vector进行历史帧重投影
    float2 velocity = VelocityTexture.SampleLevel(PointClampSampler, uv, 0).xy;
    float2 historyUV = uv - velocity;

    // 运动幅度检测
    float motionAmount = length(velocity * SSGIResolution);
    bool fastMotion = motionAmount > 5.0; // 超过5像素认为是快速运动（放宽阈值）

    // 检查历史UV是否有效
    bool validHistory = all(historyUV >= 0.0) && all(historyUV <= 1.0) && !fastMotion;

    float3 result = indirect;

    if (validHistory && SSGIFrameCounter > 0)
    {
        // 3x3邻域颜色裁剪（用于当前帧）
        float3 colorMin = indirect;
        float3 colorMax = indirect;
        float3 colorAvg = indirect;
        int validSamples = 1;

        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                if (x == 0 && y == 0) continue;
                float2 sampleUV = uv + float2(x, y) * SSGIInverseResolution;
                float3 neighbor = BaseColorTexture.SampleLevel(PointClampSampler, sampleUV, 0).rgb;
                colorMin = min(colorMin, neighbor);
                colorMax = max(colorMax, neighbor);
                colorAvg += neighbor;
                validSamples++;
            }
        }
        colorAvg /= float(validSamples);

        // 亚像素精度修正：在历史帧周围3x3区域搜索最佳匹配
        float3 bestHistory = HistoryTexture.SampleLevel(LinearClampSampler, historyUV, 0).rgb;
        float bestScore = 99999.0;

        // 采样当前位置的深度作为参考
        float centerDepthForMatch = centerDepthNdc;

        [unroll]
        for (int dy = -1; dy <= 1; ++dy)
        {
            [unroll]
            for (int dx = -1; dx <= 1; ++dx)
            {
                float2 testUV = historyUV + float2(dx, dy) * SSGIInverseResolution;

                // 边界检查
                if (any(testUV < 0.0) || any(testUV > 1.0))
                    continue;

                // 采样历史帧深度
                float testDepth = DepthTexture.SampleLevel(PointClampSampler, testUV, 0);
                float depthDiff = abs(centerDepthForMatch - testDepth);

                // 深度相似度评分（深度差越小越好）
                float depthScore = depthDiff * 1000.0;

                // 如果这个位置的深度更接近，选择它
                if (depthScore < bestScore)
                {
                    bestScore = depthScore;
                    bestHistory = HistoryTexture.SampleLevel(LinearClampSampler, testUV, 0).rgb;
                }
            }
        }

        // 如果最佳匹配的深度差异仍然太大，拒绝历史帧
        if (bestScore > 3.0) // 深度差 > 0.003
        {
            return float4(max(indirect, 0.0), ssgiWeight);
        }

        // 颜色裁剪：将历史帧限制在当前帧邻域范围内
        float3 history = clamp(bestHistory, colorMin, colorMax);

        // 自适应混合：大幅提高混合权重来稳定结果
        float baseBlend = TemporalBlend * 0.95; // 基础权重0.95（0.7 * 0.95 = 0.665）
        float motionWeight = saturate(1.0 - motionAmount * 0.1); // 运动衰减减弱
        float blend = baseBlend * motionWeight;

        // 命中率低时进一步增加混合来减少噪点
        blend = lerp(blend, min(baseBlend * 0.98, 0.95), 1.0 - ssgiWeight);

        // 时域累积
        result = lerp(indirect, history, blend);
    }

    return float4(max(result, 0.0), ssgiWeight);
}