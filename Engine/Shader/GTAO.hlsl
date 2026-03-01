// GTAO.hlsl
// Ground Truth Ambient Occlusion
// 
// 算法流程：
// 1. 从深度缓冲重建视图空间坐标
// 2. 在法线半球内沿多个方向切片（slice）进行视线方向积分
// 3. 对每个方向，沿射线步进并与深度图求交，找到最大仰角
// 4. 利用 cos 积分公式计算该方向的可见性
// 5. 所有方向平均得到最终 AO

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

// GTAO 参数常量缓冲区
cbuffer GtaoConstants : register(b1)
{
    float2 AOResolution;
    float2 AOInverseResolution;
    float AORadius;
    float AOIntensity;
    int AOSliceCount;
    int AOStepsPerSlice;
    int FrameCounter;
    int AOType;               // 0=Off, 1=SSAO, 2=GTAO
    float FalloffStart;
    float FalloffEnd;
};

// 输入纹理
Texture2D<float> DepthTexture : register(t0);    // 深度缓冲
Texture2D<float4> NormalTexture : register(t1);   // GBuffer法线

// 采样器（与全局Root Signature静态采样器对应：s0=PointWrap, s1=PointClamp, s3=LinearClamp）
SamplerState PointClampSampler : register(s1);
SamplerState LinearClampSampler : register(s3);

// 顶点结构
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

// ==================== 工具函数 ====================

#define PI 3.14159265359
#define HALF_PI 1.57079632679

// 从深度值重建视图空间位置
float3 ReconstructViewPosition(float2 uv, float depth)
{
    // UV -> NDC
    float2 ndcXY = uv * 2.0 - 1.0;
    ndcXY.y = -ndcXY.y;
    
    // NDC -> 裁剪空间
    float4 clipPos = float4(ndcXY, depth, 1.0);
    
    // 裁剪空间 -> 视图空间
    float4 viewPos = mul(InverseProjectionMatrix, clipPos);
    viewPos /= viewPos.w;
    
    return viewPos.xyz;
}

// 从视图空间位置获取屏幕UV
float2 ViewToScreenUV(float3 viewPos)
{
    float4 clipPos = mul(ProjectionMatrix, float4(viewPos, 1.0));
    clipPos.xy /= clipPos.w;
    float2 uv = clipPos.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    return uv;
}

// 采样深度并获取线性深度（视图空间Z）
float GetLinearDepth(float2 uv)
{
    float depth = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
    float4 clipPos = float4(0, 0, depth, 1.0);
    float4 viewPos = mul(InverseProjectionMatrix, clipPos);
    return viewPos.z / viewPos.w;
}

// 获取视图空间法线（从GBuffer法线纹理）
float3 GetViewNormal(float2 uv)
{
    // GBuffer法线RT是R16G16B16A16_FLOAT格式，直接存储[-1,1]范围的世界空间法线，无需解码
    float3 worldNormal = normalize(NormalTexture.SampleLevel(PointClampSampler, uv, 0).xyz);
    // 世界法线 -> 视图空间法线
    float3 viewNormal = mul((float3x3)ViewMatrix, worldNormal);
    return normalize(viewNormal);
}

// 空间哈希噪声（用于时域旋转和抖动）
float InterleavedGradientNoise(float2 position)
{
    return frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
}

// 距离衰减（线性衰减）
float FalloffFunction(float distanceSq)
{
    float falloffStartSq = FalloffStart * FalloffStart;
    float falloffEndSq = FalloffEnd * FalloffEnd;
    return saturate((falloffEndSq - distanceSq) / (falloffEndSq - falloffStartSq + 1e-6));
}

// ==================== SSAO 核心算法 ====================
// 参考：Screen Space Ambient Occlusion (Crytek)
//
// 核心思路：
// 在当前像素的法线半球内随机采样多个点，将这些点投影到屏幕空间，
// 比较采样点的深度与实际深度缓冲中的深度，判断是否被遮挡。
// 被遮挡的样本越多，AO 值越大（越暗）。
//
// 关键定义（左手系 LH，视图空间）：
// - viewPos.z > 0（物体在相机前方）
// - viewNormal：视图空间法线，指向相机方向
// - 采样半球：以 viewNormal 为轴的半球，半径为 AORadius
// - 深度比较：如果采样点在实际几何体内部，则认为被遮挡
//
// 算法步骤：
// 1. 在法线半球内生成随机采样点
// 2. 将采样点投影到屏幕空间，获取该位置的实际深度
// 3. 比较采样点深度与实际深度，判断遮挡
// 4. 应用距离衰减和范围检查
// 5. 累加遮挡贡献，归一化输出

float ComputeSSAO(float2 uv, float3 viewPos, float3 viewNormal, float2 pixelPos)
{
    float occlusion = 0.0;
    const int sampleCount = 16;
    
    // 噪声旋转角度（改善采样分布）
    float noiseAngle = InterleavedGradientNoise(pixelPos) * 2.0 * PI;
    float cosNoise = cos(noiseAngle);
    float sinNoise = sin(noiseAngle);

    for (int i = 0; i < sampleCount; i++)
    {
        // ========== 生成半球采样方向 ==========
        // 使用 Fibonacci 球面分布 + 噪声
        float t = (float(i) + 0.5) / float(sampleCount); // [0, 1]
        
        // 极角（从法线方向）：使用 sqrt 让采样更集中在法线附近
        float phi = acos(1.0 - t); // [0, π/2] 半球
        
        // 方位角：黄金角分布 + 噪声
        float theta = (float(i) * 2.399963229728653) + noiseAngle; // 黄金角 ≈ 137.5°
        
        // 球面坐标转笛卡尔坐标（局部空间，Z 轴为法线方向）
        float sinPhi = sin(phi);
        float3 localDir;
        localDir.x = sinPhi * cos(theta);
        localDir.y = sinPhi * sin(theta);
        localDir.z = cos(phi);
        
        // ========== 构建切线空间到视图空间的变换 ==========
        // 找一个垂直于法线的切线向量
        float3 tangent = abs(viewNormal.z) < 0.999 
            ? normalize(cross(viewNormal, float3(0, 0, 1))) 
            : normalize(cross(viewNormal, float3(1, 0, 0)));
        float3 bitangent = cross(viewNormal, tangent);
        
        // 将局部方向转换到视图空间
        float3 sampleDir = localDir.x * tangent + localDir.y * bitangent + localDir.z * viewNormal;
        
        // ========== 计算采样距离（非线性分布）==========
        // 使用平方分布让采样点更集中在近处
        float sampleDist = AORadius * (0.1 + 0.9 * t * t);
        
        // ========== 计算采样点的视图空间位置 ==========
        float3 samplePos = viewPos + sampleDir * sampleDist;
        
        // ========== 投影到屏幕空间 ==========
        float4 sampleClipPos = mul(ProjectionMatrix, float4(samplePos, 1.0));
        
        // 透视除法
        float3 sampleNDC = sampleClipPos.xyz / sampleClipPos.w;
        
        // NDC 转 UV（注意 Y 轴翻转）
        float2 sampleUV = sampleNDC.xy * 0.5 + 0.5;
        sampleUV.y = 1.0 - sampleUV.y;
        
        // ========== 边界检查 ==========
        if (any(sampleUV < 0.0) || any(sampleUV > 1.0))
        {
            continue;
        }
        
        // ========== 采样深度缓冲 ==========
        float sampleDepth = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
        
        // 跳过天空盒
        if (sampleDepth >= 1.0)
        {
            continue;
        }
        
        // ========== 重建实际几何体的视图空间位置 ==========
        float3 actualPos = ReconstructViewPosition(sampleUV, sampleDepth);
        
        // ========== 深度比较（遮挡判断）==========
        // depthDiff > 0：采样点在实际几何体后面（被遮挡）
        // depthDiff < 0：采样点在实际几何体前面（未遮挡）
        float depthDiff = samplePos.z - actualPos.z;
        
        // ========== 范围检查 ==========
        // 1. depthDiff > 0.01：避免自遮挡（深度精度问题）
        // 2. depthDiff < AORadius：只考虑合理范围内的遮挡
        if (depthDiff > 0.01 && depthDiff < AORadius)
        {
            // ========== 距离衰减 ==========
            // 采样点离中心越远，权重越低
            float distance = length(actualPos - viewPos);
            float rangeFalloff = saturate(1.0 - distance / AORadius);
            
            // ========== 深度衰减 ==========
            // 深度差越大，遮挡越明显，但也要衰减避免过度遮挡
            float depthFalloff = saturate(depthDiff / (AORadius * 0.5));
            
            // ========== 法线权重（可选优化）==========
            // 如果实际几何体的法线背向采样方向，减少遮挡贡献
            // 这需要法线缓冲，如果没有可以省略
            // float3 actualNormal = LoadNormal(sampleUV);
            // float normalWeight = saturate(dot(actualNormal, -sampleDir));
            
            // ========== 累加遮挡 ==========
            float occlusionWeight = rangeFalloff * depthFalloff;
            occlusion += occlusionWeight;
        }
    }
    
    // ========== 归一化并转换为 AO ==========
    // 归一化到 [0, 1]
    occlusion = occlusion / float(sampleCount);
    
    // 应用强度
    occlusion = saturate(occlusion * AOIntensity);
    
    // 转换为 AO（1 = 无遮挡，0 = 完全遮挡）
    float ao = 1.0 - occlusion;
    
    return ao;
}


// ==================== XeGTAO 核心算法 ====================
// 参考：XeGTAO - Intel's optimized implementation of GTAO
// 论文：https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
// 实现：https://github.com/GameTechDev/XeGTAO
//
// ========== 算法概述 ==========
// GTAO 通过在屏幕空间的多个方向（切片）上采样深度，计算每个方向的地平线角度，
// 然后使用解析积分公式计算可见性。最终的 AO = 1 - visibility。
//
// ========== 坐标系统 ==========
// 左手系（LH），视图空间：
// - X 轴：右
// - Y 轴：上
// - Z 轴：前（深度增加方向）
// - 物体在相机前方，viewPos.z > 0
// - viewDir = normalize(-viewPos)，从表面指向相机
//
// ========== 切片平面（Slice Plane）==========
// 对于每个切片角度 phi：
// 1. 屏幕空间方向：omega = (cos(phi), sin(phi))
// 2. 视图空间方向：directionVec = (cos(phi), sin(phi), 0)
// 3. 正交化方向：orthoDirectionVec = directionVec - dot(directionVec, viewDir) * viewDir
//    （投影到垂直于 viewDir 的平面）
// 4. 切片平面法向量：axisVec = normalize(cross(orthoDirectionVec, viewDir))
//    （垂直于切片平面，用于投影法线）
//
// ========== 投影法线（Projected Normal）==========
// 1. 投影：projectedNormalVec = viewNormal - axisVec * dot(viewNormal, axisVec)
//    （移除法线在 axisVec 方向的分量，得到切片平面内的投影）
// 2. 归一化：projectedNormalVec /= length(projectedNormalVec)
// 3. 角度计算：
//    - cosNorm = dot(projectedNormalVec, viewDir)
//    - signNorm = sign(dot(orthoDirectionVec, projectedNormalVec))
//    - n = signNorm * acos(cosNorm)
//    n 是投影法线相对于 viewDir 的带符号角度
//
// ========== 地平线跟踪（Horizon Tracking）==========
// XeGTAO 在余弦空间跟踪地平线，而非角度空间：
// 1. 初始化：
//    - lowHorizonCos0 = cos(n + π/2)  // 法线下方 90°
//    - lowHorizonCos1 = cos(n - π/2)  // 法线上方 90°
//    - horizonCos0 = lowHorizonCos0   // 正方向地平线余弦
//    - horizonCos1 = lowHorizonCos1   // 负方向地平线余弦
//
// 2. 采样更新：
//    对于每个采样点：
//    - sampleHorizonVec = normalize(samplePos - viewPos)
//    - shc = dot(sampleHorizonVec, viewDir)  // 采样点的地平线余弦
//    - weight = 距离衰减权重（远处样本权重降低）
//    - shc = lerp(lowHorizonCos, shc, weight)  // 应用衰减
//    - horizonCos = max(horizonCos, shc)  // 更新最大仰角
//
// 3. 为什么用余弦空间？
//    - 减少 acos 调用（只在最后转换一次）
//    - max() 操作在余弦空间等价于角度空间的 max()（因为 cos 在 [0,π] 单调递减）
//    - lerp() 在余弦空间近似角度空间的 lerp（虽然不完全等价，但足够快且质量可接受）
//
// ========== 距离衰减（Falloff）==========
// XeGTAO 使用线性衰减 + 薄物体补偿：
// 1. 薄物体补偿距离：
//    falloffBase = length(float3(delta.xy, delta.z * (1 + thinOccluderCompensation)))
//    增大 Z 方向权重，让后方样本更快衰减，减少薄物体过度遮蔽
//
// 2. 线性衰减：
//    weight = saturate(falloffBase * falloffMul + falloffAdd)
//    其中 falloffMul = -1 / falloffRange, falloffAdd = falloffFrom / falloffRange + 1
//
// 3. 应用衰减：
//    shc = lerp(lowHorizonCos, shc, weight)
//    远处样本被拉回到 lowHorizonCos（法线下方 90°），减少影响
//
// ========== 采样分布（Sample Distribution）==========
// 1. 基础分布：s = (step + noise) / stepsPerSlice  // [0, 1]
// 2. 非线性调整：s = pow(s, sampleDistributionPower)  // 默认 2.0
//    让采样点集中在近处（更重要的区域）
// 3. 最小距离：s += minS  // 避免采样中心像素
// 4. 像素对齐：sampleOffset = round(s * omega) * pixelSize
//    对齐到像素中心，减少采样位置与深度不匹配的伪影
//
// ========== 可见性积分（Visibility Integration）==========
// 论文 Algorithm 1 的解析积分公式：
// 1. 转换回角度：
//    h0 = -acos(horizonCos1)  // 负方向地平线角
//    h1 = acos(horizonCos0)   // 正方向地平线角
//
// 2. 计算积分：
//    iarc0 = (cosNorm + 2*h0*sin(n) - cos(2*h0-n)) / 4
//    iarc1 = (cosNorm + 2*h1*sin(n) - cos(2*h1-n)) / 4
//
// 3. 加权可见性：
//    localVisibility = projectedNormalVecLength * (iarc0 + iarc1)
//    projectedNormalVecLength 表示法线在切片平面的投影长度，
//    越接近切片平面，该切片对可见性的贡献越大
//
// 4. 投影法线长度修正：
//    projectedNormalVecLength = lerp(projectedNormalVecLength, 1.0, 0.05)
//    减少高坡度表面（法线几乎垂直于切片平面）的过暗问题
//
// ========== 最终输出 ==========
// 1. 归一化：visibility /= sliceCount
// 2. 转换为 AO：ao = 1 - saturate(visibility)
// 3. 应用强度：ao = pow(ao, AOIntensity)
//
// ========== 性能优化 ==========
// - 余弦空间计算：减少三角函数调用
// - 像素对齐：减少伪影，允许更少的采样数
// - 非线性采样：集中采样在重要区域
// - R1 序列噪声：低差异序列，更好的时空稳定性
// - 小半径淡出：避免半径过小时的突变和噪声

float ComputeGTAO(float2 uv, float3 viewPos, float3 viewNormal, float2 pixelPos)
{
    float3 viewDir = normalize(-viewPos);
    float viewspaceZ = viewPos.z;

    // 计算屏幕空间半径
    float pixelDirRBViewspaceSizeAtCenterZ = viewspaceZ * (ProjectionMatrix[0][0] * InverseScreenSize.x);
    float screenRadius = AORadius / pixelDirRBViewspaceSizeAtCenterZ;

    // 小半径淡出
    float visibility = saturate((10.0 - screenRadius) / 100.0) * 0.5;
    
    if (screenRadius < 1.3)
    {
        return 1.0 - saturate(visibility);
    }

    screenRadius = min(screenRadius, 100.0);

    // 噪声（使用 R2 序列或 IGN）
    float noiseSlice = InterleavedGradientNoise(pixelPos);
    float noiseSample = InterleavedGradientNoise(pixelPos + float2(0.5, 0.5));

    // 采样参数
    const float pixelTooCloseThreshold = 1.3;
    const float minS = pixelTooCloseThreshold / screenRadius;
    const float sampleDistributionPower = 2.0; // 可调整采样分布
    
    // 距离衰减参数
    float falloffRange = AORadius * 0.5; // 衰减范围
    float falloffFrom = AORadius - falloffRange;
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = falloffFrom / falloffRange + 1.0;
    
    // 薄物体补偿
    const float thinOccluderCompensation = 0.0; // 0-1，越大越减少薄物体遮蔽

    for (int slice = 0; slice < AOSliceCount; slice++)
    {
        // 计算切片角度
        float sliceK = (float(slice) + noiseSlice) / float(AOSliceCount);
        float phi = sliceK * PI;
        float cosPhi = cos(phi);
        float sinPhi = sin(phi);
        float2 omega = float2(cosPhi, -sinPhi);
        
        // 转换为屏幕像素单位
        omega *= screenRadius;

        // 切片方向向量（视图空间）
        float3 directionVec = float3(cosPhi, sinPhi, 0.0);
        
        // 正交化方向向量（投影到垂直于视图方向的平面）
        float3 orthoDirectionVec = directionVec - dot(directionVec, viewDir) * viewDir;
        
        // 轴向量（垂直于切片平面）
        float3 axisVec = normalize(cross(orthoDirectionVec, viewDir));
        
        // 投影法线到切片平面
        float3 projectedNormalVec = viewNormal - axisVec * dot(viewNormal, axisVec);
        float projectedNormalVecLength = length(projectedNormalVec);
        
        if (projectedNormalVecLength < 0.001)
        {
            visibility += 1.0;
            continue;
        }
        
        projectedNormalVec /= projectedNormalVecLength;

        // 计算法线角度
        float signNorm = sign(dot(orthoDirectionVec, projectedNormalVec));
        float cosNorm = saturate(dot(projectedNormalVec, viewDir));
        float n = signNorm * acos(cosNorm);

        // 初始化地平线角度（余弦值）
        const float lowHorizonCos0 = cos(n + HALF_PI);
        const float lowHorizonCos1 = cos(n - HALF_PI);
        float horizonCos0 = lowHorizonCos0;
        float horizonCos1 = lowHorizonCos1;

        // 步进采样
        for (int step = 0; step < AOStepsPerSlice; step++)
        {
            // R1 序列噪声
            const float stepBaseNoise = float(slice + step * AOStepsPerSlice) * 0.6180339887498948482;
            float stepNoise = frac(noiseSample + stepBaseNoise);
            
            // 计算采样距离
            float s = (float(step) + stepNoise) / float(AOStepsPerSlice);
            s = pow(s, sampleDistributionPower); // 非线性分布
            s += minS; // 避免采样中心像素
            
            // 采样偏移（像素单位）
            float2 sampleOffset = s * omega;
            float sampleOffsetLength = length(sampleOffset);
            
            // 对齐到像素中心（减少伪影）
            sampleOffset = round(sampleOffset) * InverseScreenSize;
            
            // 正方向采样
            float2 sampleUV0 = uv + sampleOffset;
            if (all(sampleUV0 >= 0.0) && all(sampleUV0 <= 1.0))
            {
                float sampleDepth = DepthTexture.SampleLevel(PointClampSampler, sampleUV0, 0);
                if (sampleDepth < 1.0)
                {
                    float3 samplePos = ReconstructViewPosition(sampleUV0, sampleDepth);
                    float3 sampleDelta = samplePos - viewPos;
                    float sampleDist = length(sampleDelta);
                    
                    // 归一化地平线向量
                    float3 sampleHorizonVec = sampleDelta / sampleDist;
                    
                    // 薄物体补偿的距离衰减
                    float falloffBase = length(float3(sampleDelta.xy, sampleDelta.z * (1.0 + thinOccluderCompensation)));
                    float weight = saturate(falloffBase * falloffMul + falloffAdd);
                    
                    // 计算地平线余弦值
                    float shc = dot(sampleHorizonVec, viewDir);
                    
                    // 应用权重
                    shc = lerp(lowHorizonCos0, shc, weight);
                    
                    // 更新地平线
                    horizonCos0 = max(horizonCos0, shc);
                }
            }

            // 负方向采样
            float2 sampleUV1 = uv - sampleOffset;
            if (all(sampleUV1 >= 0.0) && all(sampleUV1 <= 1.0))
            {
                float sampleDepth = DepthTexture.SampleLevel(PointClampSampler, sampleUV1, 0);
                if (sampleDepth < 1.0)
                {
                    float3 samplePos = ReconstructViewPosition(sampleUV1, sampleDepth);
                    float3 sampleDelta = samplePos - viewPos;
                    float sampleDist = length(sampleDelta);
                    
                    float3 sampleHorizonVec = sampleDelta / sampleDist;
                    
                    float falloffBase = length(float3(sampleDelta.xy, sampleDelta.z * (1.0 + thinOccluderCompensation)));
                    float weight = saturate(falloffBase * falloffMul + falloffAdd);
                    
                    float shc = dot(sampleHorizonVec, viewDir);
                    shc = lerp(lowHorizonCos1, shc, weight);
                    
                    horizonCos1 = max(horizonCos1, shc);
                }
            }
        }

        // 修正投影法线长度（减少高坡度过暗）
        projectedNormalVecLength = lerp(projectedNormalVecLength, 1.0, 0.05);

        // 将余弦值转回角度
        float h0 = -acos(horizonCos1);
        float h1 = acos(horizonCos0);

        // 计算可见性积分（论文公式）
        float iarc0 = (cosNorm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
        float iarc1 = (cosNorm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
        
        float localVisibility = projectedNormalVecLength * (iarc0 + iarc1);
        visibility += localVisibility;
    }

    // 归一化
    visibility /= float(AOSliceCount);

    // 转换为 AO
    float ao = 1.0 - saturate(visibility);
    
    // 应用强度
    ao = 1 - pow(saturate(ao), AOIntensity);

    return ao;
}






float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 uv = input.TexCoord;

    // 采样深度
    float depth = DepthTexture.SampleLevel(PointClampSampler, uv, 0);

    // 天空像素不计算AO
    if (depth >= 1.0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }

    // 重建视图空间位置
    float3 viewPos = ReconstructViewPosition(uv, depth);

    // 获取视图空间法线
    float3 viewNormal = GetViewNormal(uv);

    float ao = 1.0;

    // 根据AO类型选择算法
    if (AOType == 1)
    {
        // SSAO
        ao = ComputeSSAO(uv, viewPos, viewNormal, input.Position.xy);
    }
    else if (AOType == 2)
    {
        // GTAO
        ao = ComputeGTAO(uv, viewPos, viewNormal, input.Position.xy);
    }

    return float4(ao, ao, ao, 1.0);
}
