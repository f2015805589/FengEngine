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
    float AORadius;           // 推荐默认值: 2.5
    float AOIntensity;        // 推荐默认值: 5.0
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
    
    // ========== 纯粹的 SSAO 实现 ==========
    float totalAO = 0.0;
    const int sampleCount = 16;  // 固定采样数量

    for (int i = 0; i < sampleCount; i++)
    {
        // 生成随机采样方向（在法线半球内）
        float angle1 = (float(i) / float(sampleCount) + InterleavedGradientNoise(input.Position.xy) * 0.1) * 2.0 * PI;
        float angle2 = sqrt(float(i) / float(sampleCount)) * HALF_PI;  // 使用平方根分布，更均匀

        // 球面坐标转换为笛卡尔坐标
        float3 sampleDir;
        sampleDir.x = sin(angle2) * cos(angle1);
        sampleDir.y = sin(angle2) * sin(angle1);
        sampleDir.z = cos(angle2);

        // 确保采样方向在法线半球内
        if (dot(sampleDir, viewNormal) < 0.0)
        {
            sampleDir = -sampleDir;
        }

        // 采样距离（使用渐进分布）
        float sampleRadius = AORadius * (0.1 + 0.9 * float(i) / float(sampleCount));

        // 计算采样点的视图空间位置
        float3 samplePos = viewPos + sampleDir * sampleRadius;

        // 投影到屏幕空间
        float4 sampleClipPos = mul(ProjectionMatrix, float4(samplePos, 1.0));
        sampleClipPos.xy /= sampleClipPos.w;
        float2 sampleUV = sampleClipPos.xy * 0.5 + 0.5;
        sampleUV.y = 1.0 - sampleUV.y;

        // 检查是否在屏幕内
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0)
        {
            continue;
        }

        // 采样深度
        float sampleDepth = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
        if (sampleDepth >= 1.0)
        {
            continue;
        }

        // 重建采样点的实际视图空间位置
        float3 actualSamplePos = ReconstructViewPosition(sampleUV, sampleDepth);

        // 比较深度
        float depthDiff = samplePos.z - actualSamplePos.z;

        // 范围检查：只考虑合理范围内的遮挡
        if (depthDiff > 0.01 && depthDiff < AORadius)
        {
            // 距离衰减
            float distance = length(actualSamplePos - viewPos);
            float falloff = saturate(1.0 - distance / AORadius);

            // 遮挡权重
            float occlusionWeight = saturate(depthDiff / AORadius);

            totalAO += falloff * occlusionWeight;
        }
    }

    // 归一化并反转
    totalAO = 1.0 - saturate(totalAO / float(sampleCount) * AOIntensity);

    return float4(totalAO, totalAO, totalAO, 1.0);
}
