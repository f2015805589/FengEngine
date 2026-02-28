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
    float FalloffStart;
    float FalloffEnd;
    float AOPadding;
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

// ==================== GTAO 核心算法 ====================
// 参考：Practical Realtime Strategies for Accurate Indirect Occlusion (Jimenez et al.)
// 以及 Ground Truth Ambient Occlusion (Activision)
//
// 核心思路：
// 对每个屏幕空间方向（切片），定义一个由 viewDir 和该方向构成的平面。
// 在此平面内，将法线投影得到投影法线角度 n，然后在正负两侧
// 通过射线步进找到最大的 horizon angle（h0, h1），最后用 cos 权重积分
// 计算被遮挡的可见性。
//
// 关键定义（左手系 LH）：
// - 视图空间中物体在 +Z 方向，viewPos.z > 0
// - viewDir = normalize(-viewPos)，大致指向 (0,0,-1) 方向（从表面朝向相机）
// - horizon angle 是采样向量在切片平面内相对于 viewDir 的角度
//   （使用 atan2 计算，向法线方向为正角度）
// - 法线角度 n 也是在同一切片平面内相对于 viewDir 的角度

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
    
    // 视图方向（从表面指向相机）
    // 左手系（LH）：视图空间中物体在 +Z 方向，viewPos.z > 0
    // 从表面指向相机 = normalize(-viewPos)，方向大致指向 (0,0,-1)
    // 但GTAO中viewDir应该是从相机看向表面的方向的反方向，即从表面朝向相机
    float3 viewDir = normalize(-viewPos);
    
    // 在屏幕空间的采样半径（根据距离调整）
    // 投影半径：世界空间半径 -> 屏幕空间像素
    // 注意：左手系（LH），viewPos.z > 0，所以直接用 viewPos.z
    float projectedRadius = AORadius * ProjectionMatrix[0][0] / viewPos.z;
    float screenRadius = projectedRadius * ScreenSize.x * 0.5;
    
    // 如果投影半径太小（< 1像素），不计算AO
    if (screenRadius < 1.0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    
    // 限制最大屏幕空间步长
    screenRadius = min(screenRadius, 256.0);
    
    // 步长（屏幕空间像素）
    float stepSize = screenRadius / (float)AOStepsPerSlice;
    
    // 时域旋转噪声：每帧旋转基础角度，减少banding
    float noiseAngle = InterleavedGradientNoise(input.Position.xy + float2(FrameCounter * 0.6180339887, 0.0)) * PI;
    float noiseStep = InterleavedGradientNoise(input.Position.xy * 1.37 + float2(0.0, FrameCounter * 0.6180339887));
    
    // ========== GTAO 方向切片积分 ==========
    float totalAO = 0.0;
    
    for (int slice = 0; slice < AOSliceCount; slice++)
    {
        // 方向角度：均匀分布 + 时域噪声旋转
        float phi = (PI / (float)AOSliceCount) * ((float)slice + noiseAngle);
        
        // 屏幕空间方向（2D）
        float2 direction = float2(cos(phi), sin(phi));
        
        // 构建切片平面的3D方向向量
        // 屏幕空间方向对应视图空间中的一个水平方向
        // 由于视图空间中相机看向 -Z，屏幕X对应视图X，屏幕Y对应视图Y
        float3 sliceDirVS = float3(direction.x, direction.y, 0.0);
        sliceDirVS = normalize(sliceDirVS);
        
        // 切片平面的法向量（垂直于 viewDir 和 sliceDir 构成的平面）
        float3 slicePlaneNormal = normalize(cross(sliceDirVS, viewDir));
        
        // 将法线投影到切片平面
        // projectedNormal = N - (N·planeNormal)*planeNormal
        float3 projectedNormal = viewNormal - slicePlaneNormal * dot(viewNormal, slicePlaneNormal);
        float projNormalLen = length(projectedNormal);
        
        // 如果投影法线长度接近0，说明法线几乎垂直于切片平面，跳过
        if (projNormalLen < 1e-4)
        {
            totalAO += 1.0; // 无遮蔽
            continue;
        }
        
        projectedNormal /= projNormalLen;
        
        // 计算法线在切片平面内相对于 viewDir 的角度 n
        // n > 0 表示法线倾向于正方向（direction侧）
        // n < 0 表示法线倾向于负方向（-direction侧）
        float cosN = clamp(dot(projectedNormal, viewDir), -1.0, 1.0);
        float sinN = clamp(dot(projectedNormal, sliceDirVS), -1.0, 1.0);
        float n = atan2(sinN, cosN);  // 法线角度，范围 [-π/2, π/2]
        
        // 对每个方向，在两侧找最大仰角
        // h0: 正方向（+direction）的 horizon angle
        // h1: 负方向（-direction）的 horizon angle  
        // 初始化为 -HALF_PI（视线方向以下 = 完全可见）
        float h0 = -HALF_PI;
        float h1 = -HALF_PI;
        
        // 追踪每个方向是否有有效采样
        // 如果整个方向的所有步进都在屏幕外或天空，则该方向视为无遮蔽
        bool h0HasValidSample = false;
        bool h1HasValidSample = false;
        
        for (int step = 1; step <= AOStepsPerSlice; step++)
        {
            float t = ((float)step + noiseStep) * stepSize;
            
            // === 正方向采样 ===
            {
                float2 sampleUV = uv + direction * t * InverseScreenSize;
                
                // 超出屏幕边界：视为无遮蔽（不更新horizon angle）
                if (sampleUV.x >= 0.0 && sampleUV.x <= 1.0 && sampleUV.y >= 0.0 && sampleUV.y <= 1.0)
                {
                    float sampleDepth = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
                    // 天空像素也视为无遮蔽
                    if (sampleDepth < 1.0)
                    {
                        float3 sampleViewPos = ReconstructViewPosition(sampleUV, sampleDepth);
                        float3 horizonVec = sampleViewPos - viewPos;
                        float distanceSq = dot(horizonVec, horizonVec);
                        float falloff = FalloffFunction(distanceSq);
                        
                        float3 horizonDir = normalize(horizonVec);
                        float cosH = dot(horizonDir, sliceDirVS);
                        float sinH = dot(horizonDir, viewDir);
                        float elevationAngle = atan2(sinH, cosH);
                        
                        float weightedAngle = lerp(-HALF_PI, elevationAngle, falloff);
                        h0 = max(h0, weightedAngle);
                        h0HasValidSample = true;
                    }
                }
            }
            
            // === 负方向采样 ===
            {
                float2 sampleUV = uv - direction * t * InverseScreenSize;
                
                // 超出屏幕边界：视为无遮蔽
                if (sampleUV.x >= 0.0 && sampleUV.x <= 1.0 && sampleUV.y >= 0.0 && sampleUV.y <= 1.0)
                {
                    float sampleDepth = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
                    // 天空像素也视为无遮蔽
                    if (sampleDepth < 1.0)
                    {
                        float3 sampleViewPos = ReconstructViewPosition(sampleUV, sampleDepth);
                        float3 horizonVec = sampleViewPos - viewPos;
                        float distanceSq = dot(horizonVec, horizonVec);
                        float falloff = FalloffFunction(distanceSq);
                        
                        float3 horizonDir = normalize(horizonVec);
                        float cosH = dot(horizonDir, -sliceDirVS);
                        float sinH = dot(horizonDir, viewDir);
                        float elevationAngle = atan2(sinH, cosH);
                        
                        float weightedAngle = lerp(-HALF_PI, elevationAngle, falloff);
                        h1 = max(h1, weightedAngle);
                        h1HasValidSample = true;
                    }
                }
            }
        }
        
        // 如果某个方向完全没有有效采样（全部在屏幕外或天空），
        // 将该方向的horizon angle设为-HALF_PI（无遮蔽）
        if (!h0HasValidSample) h0 = -HALF_PI;
        if (!h1HasValidSample) h1 = -HALF_PI;
        
        // ========== 将 horizon angle 限制在法线半球内 ==========
        h0 = clamp(h0, -HALF_PI, HALF_PI);
        h1 = clamp(h1, -HALF_PI, HALF_PI);
        
        // ========== 积分可见性 ==========
        // GTAO 使用 cos 权重积分公式（Jimenez 2016）：
        // 对于一侧的积分：
        //   I(h, n) = 1/4 * (-cos(2h - n) + cos(n) + 2h * sin(n))
        // 
        // 两侧合计（注意负方向的法线角度取反）：
        //   AO = ( I(h0, n) + I(h1, -n) ) * projNormalLen
        
        // 对于没有有效采样的方向，h保持在-HALF_PI，
        // 此时积分结果为最大可见性（无遮蔽）
        float innerIntegral0 = -cos(2.0 * h0 - n) + cos(n) + 2.0 * h0 * sin(n);
        float innerIntegral1 = -cos(2.0 * h1 + n) + cos(n) - 2.0 * h1 * sin(n);
        
        // 乘以投影法线长度作为权重（法线越垂直于切片平面，权重越小）
        float sliceAO = (innerIntegral0 + innerIntegral1) * 0.25 * projNormalLen;
        
        totalAO += sliceAO;
    }
    
    // 归一化
    totalAO /= (float)AOSliceCount;
    
    // 应用强度并钳制
    float ao = saturate(pow(saturate(totalAO), AOIntensity));
    
    // ========== 屏幕边缘淡化 ==========
    // 靠近屏幕边缘的像素，GTAO采样方向不完整（很多射线指向屏幕外），
    // 会产生不准确的AO值。通过在边缘区域将AO淡化到1.0（无遮蔽）来消除伪影。
    // 这是工业界标准做法（UE、Unity HDRP 等均使用类似策略）。
    float2 edgeDist = min(uv, 1.0 - uv);  // 到四边的最小距离 [0, 0.5]
    float edgeMin = min(edgeDist.x, edgeDist.y);  // 到最近边缘的距离
    // 以采样半径对应的屏幕空间范围作为淡化区域
    // screenRadius是像素单位，转为UV空间
    float fadeRange = screenRadius * max(InverseScreenSize.x, InverseScreenSize.y);
    fadeRange = clamp(fadeRange, 0.02, 0.1);  // 限制淡化范围在合理区间
    float edgeFade = saturate(edgeMin / fadeRange);
    ao = lerp(1.0, ao, edgeFade);
    
    return float4(ao, ao, ao, 1.0);
}
