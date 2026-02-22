// TexturePreview.hlsl
// 纹理预览着色器 - 支持通道可视化

// 常量缓冲
cbuffer PreviewParams : register(b0)
{
    int channelMode;      // 0=RGBA, 1=RGB, 2=R, 3=G, 4=B, 5=A, 6=Normal, 7=Luminance
    float exposure;       // HDR曝光值
    float gamma;          // Gamma校正值
    int mipLevel;         // Mip级别
    float2 uvOffset;      // UV偏移
    float2 uvScale;       // UV缩放
};

// 纹理和采样器
Texture2D<float4> sourceTexture : register(t0);
SamplerState linearSampler : register(s0);

// 顶点输入
struct VSInput
{
    float3 position : POSITION;
    float2 texCoord : TEXCOORD0;
};

// 顶点输出
struct VSOutput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

// 顶点着色器
VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0);
    output.texCoord = input.texCoord * uvScale + uvOffset;
    return output;
}

// 像素着色器
float4 PSMain(VSOutput input) : SV_TARGET
{
    // 采样纹理
    float4 color = sourceTexture.SampleLevel(linearSampler, input.texCoord, mipLevel);

    // 应用HDR曝光
    color.rgb *= exposure;

    float4 result = float4(0, 0, 0, 1);

    // 根据通道模式处理
    switch (channelMode)
    {
        case 0: // RGBA
            result = color;
            break;

        case 1: // RGB (忽略Alpha)
            result = float4(color.rgb, 1.0);
            break;

        case 2: // R通道 (灰度显示)
            result = float4(color.r, color.r, color.r, 1.0);
            break;

        case 3: // G通道 (灰度显示)
            result = float4(color.g, color.g, color.g, 1.0);
            break;

        case 4: // B通道 (灰度显示)
            result = float4(color.b, color.b, color.b, 1.0);
            break;

        case 5: // A通道 (灰度显示)
            result = float4(color.a, color.a, color.a, 1.0);
            break;

        case 6: // Normal可视化
            // 假设法线存储在RG通道 (BC5格式)
            // 重建Z分量: z = sqrt(1 - x^2 - y^2)
            {
                float2 normalXY = color.rg * 2.0 - 1.0;
                float normalZ = sqrt(saturate(1.0 - dot(normalXY, normalXY)));
                float3 normal = float3(normalXY, normalZ);
                // 将法线从[-1,1]映射到[0,1]用于显示
                result = float4(normal * 0.5 + 0.5, 1.0);
            }
            break;

        case 7: // Luminance (亮度)
            {
                // 使用标准亮度公式
                float lum = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
                result = float4(lum, lum, lum, 1.0);
            }
            break;

        default:
            result = color;
            break;
    }

    // 应用Gamma校正
    result.rgb = pow(abs(result.rgb), 1.0 / gamma);

    return result;
}
