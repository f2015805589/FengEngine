// GTAOBlur.hlsl
// GTAO 空间模糊（Cross-Bilateral Blur）
// 
// 边缘保持的高斯模糊：
// 使用深度差异作为权重，在保持边缘的同时平滑 AO 噪声
// 参考 GTAO 论文中的时域/空间混合滤波策略

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

// 输入纹理
Texture2D<float4> AOTexture : register(t0);      // GTAO原始输出
Texture2D<float> DepthTexture : register(t1);     // 深度缓冲（用于边缘保持）

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

// 获取线性深度
float GetLinearDepth(float rawDepth)
{
    // 使用投影矩阵参数反推线性深度
    // 对于标准透视投影：linearZ = NearPlane * FarPlane / (FarPlane - rawDepth * (FarPlane - NearPlane))
    float4 clipPos = float4(0, 0, rawDepth, 1.0);
    float4 viewPos = mul(InverseProjectionMatrix, clipPos);
    return viewPos.z / viewPos.w;
}

// Cross-Bilateral 模糊
// 使用 4x4 高斯核 + 深度感知权重
float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 uv = input.TexCoord;
    
    // 中心像素
    float centerAO = AOTexture.SampleLevel(PointClampSampler, uv, 0).r;
    float centerDepth = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
    
    // 天空像素直接返回白色
    if (centerDepth >= 1.0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    
    float centerLinearDepth = GetLinearDepth(centerDepth);
    
    // 高斯权重（3x3简化版，更快）
    // 权重: 1 2 1
    //       2 4 2
    //       1 2 1  (归一化后总和=16)
    static const float gaussWeights[9] = {
        1.0, 2.0, 1.0,
        2.0, 4.0, 2.0,
        1.0, 2.0, 1.0
    };
    
    static const int2 offsets[9] = {
        int2(-1, -1), int2(0, -1), int2(1, -1),
        int2(-1,  0), int2(0,  0), int2(1,  0),
        int2(-1,  1), int2(0,  1), int2(1,  1)
    };
    
    float totalAO = 0.0;
    float totalWeight = 0.0;
    
    // 深度敏感度（控制边缘保持的强度）
    // 使用相对深度差异（除以中心深度），这样远近物体都能正确保持边缘
    float depthThreshold = 0.02; // 相对深度差异阈值（2%）
    
    [unroll]
    for (int i = 0; i < 9; i++)
    {
        float2 sampleUV = uv + offsets[i] * InverseScreenSize;
        
        // 边界检查：超出屏幕的像素用中心AO值代替（而非跳过）
        // 这样避免边缘处权重不均衡导致的黑边
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0)
        {
            float weight = gaussWeights[i];
            totalAO += centerAO * weight;
            totalWeight += weight;
            continue;
        }
        
        float sampleAO = AOTexture.SampleLevel(PointClampSampler, sampleUV, 0).r;
        float sampleDepth = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
        
        // 天空像素：使用AO=1（无遮蔽）参与模糊，但给予极低权重
        if (sampleDepth >= 1.0)
        {
            float weight = gaussWeights[i] * 0.01; // 天空像素权重极低
            totalAO += 1.0 * weight;
            totalWeight += weight;
            continue;
        }
        
        float sampleLinearDepth = GetLinearDepth(sampleDepth);
        
        // 深度权重：使用相对深度差异，边缘处权重快速衰减
        float depthDiff = abs(centerLinearDepth - sampleLinearDepth) / (abs(centerLinearDepth) + 1e-6);
        float depthWeight = (depthDiff < depthThreshold) ? 1.0 : exp(-pow(depthDiff / depthThreshold, 2.0) * 10.0);
        
        // 最终权重 = 高斯权重 × 深度权重
        float weight = gaussWeights[i] * depthWeight;
        
        totalAO += sampleAO * weight;
        totalWeight += weight;
    }
    
    // 归一化
    float blurredAO = (totalWeight > 0.0) ? (totalAO / totalWeight) : centerAO;
    
    return float4(blurredAO, blurredAO, blurredAO, 1.0);
}
