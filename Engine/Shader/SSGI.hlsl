// SSGI.hlsl

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

Texture2D<float>  DepthMaxTexture  : register(t0);
Texture2D<float4> BaseColorTexture : register(t1);
Texture2D<float4> NormalTexture    : register(t2);
Texture2D<float4> NoiseTexture     : register(t3);
Texture2D<float>  DepthTexture     : register(t4);

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

// 如果你的 NormalTexture 是 [0,1] 编码，改成: return normalize(n * 2.0 - 1.0);
float3 DecodeWorldNormal(float3 n)
{
    return normalize(n);
}

float3 ReconstructViewPos(float2 uv, float depthNdc)
{
    // uv -> NDC
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;

    float4 clipPos = float4(ndc, depthNdc, 1.0);
    float4 viewPos = mul(clipPos, InverseProjectionMatrix);

    float w = (abs(viewPos.w) < 1e-6) ? ((viewPos.w < 0.0) ? -1e-6 : 1e-6) : viewPos.w;
    return viewPos.xyz / w;
}

float2 ProjectViewToUV(float3 viewPos)
{
    float4 clipPos = mul(float4(viewPos, 1.0), ProjectionMatrix);
    float invW = 1.0 / max(abs(clipPos.w), 1e-6);

    float2 ndc = clipPos.xy * invW;
    float2 uv = ndc * float2(0.5, -0.5) + 0.5;
    return uv;
}

float3 BuildRayDir(float3 normalV, float2 rand2, int dirIndex, int directions, float frameJitter)
{
    float phi = 6.28318530718 * ((float(dirIndex) + rand2.x + frameJitter) / float(directions));
    float z   = saturate(0.15 + rand2.y * 0.85);
    float r   = sqrt(saturate(1.0 - z * z));

    float3 up = (abs(normalV.z) < 0.999) ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 T  = normalize(cross(up, normalV));
    float3 B  = cross(normalV, T);

    return normalize(T * (cos(phi) * r) + B * (sin(phi) * r) + normalV * z);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 uv = input.TexCoord;

    float centerDepthNdc = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
    if (centerDepthNdc >= 1.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 centerNormalW = DecodeWorldNormal(NormalTexture.SampleLevel(PointClampSampler, uv, 0).xyz);
    float3 centerNormalV = normalize(mul(float4(centerNormalW, 0.0), ViewMatrix).xyz);
    float3 centerPosV    = ReconstructViewPos(uv, centerDepthNdc);

    float2 noiseUV = frac(uv * (SSGIResolution / 4.0));
    float2 noise   = NoiseTexture.SampleLevel(PointClampSampler, noiseUV, 0).xy * 2.0 - 1.0;

    int directions = max(SSGIDirectionCount, 1);
    int steps      = max(SSGIStepCount, 1);

    float rayLength = max(SSGIRadius, 0.05);
    float stepSize  = rayLength / float(steps);
    float thickness = max(DepthThickness, 0.001);

    float frameJitter = frac(float(SSGIFrameCounter) * 0.61803398875);

    float3 giSum = 0.0;
    float  hitCount = 0.0;

    [loop]
    for (int dirIndex = 0; dirIndex < directions; ++dirIndex)
    {
        float2 dirJitter = frac(noise + float2(0.75487766 * dirIndex, 0.56984029 * dirIndex));
        float3 rayDirV   = BuildRayDir(centerNormalV, dirJitter, dirIndex, directions, frameJitter);

        // 自相交偏移
        float3 currPosV = centerPosV + rayDirV * (stepSize * 0.5);

        [loop]
        for (int step = 0; step < steps; ++step)
        {
            currPosV += rayDirV * stepSize;

            float2 sampleUV = ProjectViewToUV(currPosV);

            if (sampleUV.x <= 0.0 || sampleUV.x >= 1.0 || sampleUV.y <= 0.0 || sampleUV.y >= 1.0)
            {
                break;
            }

            float sampleDepthNdc = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
            if (sampleDepthNdc >= 1.0)
            {
                continue;
            }

            // 层级深度提前剔除（保持你原接口）
            float maxDepthNdc = DepthMaxTexture.SampleLevel(PointClampSampler, sampleUV, 0);
            float4 rayClip = mul(float4(currPosV, 1.0), ProjectionMatrix);
            float rayDepthNdc = rayClip.z / max(abs(rayClip.w), 1e-6);
            if (rayDepthNdc > maxDepthNdc + 1e-4)
            {
                break;
            }

            float3 scenePosV = ReconstructViewPos(sampleUV, sampleDepthNdc);

            // 穿过几何并在厚度内认为命中（与你项目逻辑一致）
            float dz = currPosV.z - scenePosV.z;
            bool passedSurface = (dz >= 0.0);
            bool withinThickness = (dz <= thickness);

            if (passedSurface && withinThickness)
            {
                float3 sampleNormalW = DecodeWorldNormal(NormalTexture.SampleLevel(PointClampSampler, sampleUV, 0).xyz);

                // 法线一致性过滤，避免穿帮
                if (dot(centerNormalW, sampleNormalW) <= 0.1)
                {
                    continue;
                }

                float3 sampleColor = BaseColorTexture.SampleLevel(LinearClampSampler, sampleUV, 0).rgb;

                float dist = length(scenePosV - centerPosV);
                float att = pow(saturate(1.0 - pow(dist / rayLength, 4.0)), 2.0) / (dist * dist + 1.0);

                giSum += sampleColor * att;
                hitCount += 1.0;
                break;
            }
        }
    }

    float3 indirect = (hitCount > 1e-4) ? (giSum / hitCount) : 0.0;
    indirect *= SSGIIntensity;

    return float4(max(indirect, 0.0), 1.0);
}
