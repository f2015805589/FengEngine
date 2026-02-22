// BC1Compress.hlsl
// BC1 (DXT1) GPU压缩计算着色器
// BC1格式：每个4x4块 = 8字节 (2个16位颜色 + 4x4的2位索引)

// 常量缓冲
cbuffer CompressionParams : register(b0)
{
    uint textureWidth;
    uint textureHeight;
    uint blockCountX;
    uint blockCountY;
    uint mipLevel;
    uint isSRGB;
    float2 padding;
};

// 源纹理
Texture2D<float4> sourceTexture : register(t0);
SamplerState linearSampler : register(s0);

// 输出BC1纹理 (作为uint2缓冲，每个元素8字节)
RWTexture2D<uint2> outputTexture : register(u0);

// 将float3颜色转换为RGB565
uint ColorToRGB565(float3 color)
{
    // 限制到[0,1]范围
    color = saturate(color);

    // 转换为5-6-5位
    uint r = (uint)(color.r * 31.0 + 0.5);
    uint g = (uint)(color.g * 63.0 + 0.5);
    uint b = (uint)(color.b * 31.0 + 0.5);

    return (r << 11) | (g << 5) | b;
}

// 将RGB565转换为float3颜色
float3 RGB565ToColor(uint rgb565)
{
    float r = (float)((rgb565 >> 11) & 0x1F) / 31.0;
    float g = (float)((rgb565 >> 5) & 0x3F) / 63.0;
    float b = (float)(rgb565 & 0x1F) / 31.0;
    return float3(r, g, b);
}

// 计算两个颜色之间的距离（使用亮度加权）
float ColorDistance(float3 c1, float3 c2)
{
    float3 diff = c1 - c2;
    // 亮度加权
    float3 weights = float3(0.299, 0.587, 0.114);
    return dot(diff * diff, weights);
}

// 找到4x4块中的最小和最大颜色
void FindMinMaxColors(float3 colors[16], out float3 minColor, out float3 maxColor)
{
    minColor = float3(1, 1, 1);
    maxColor = float3(0, 0, 0);

    // 计算亮度并找到亮度范围
    float minLum = 1.0;
    float maxLum = 0.0;
    int minIdx = 0;
    int maxIdx = 0;

    [unroll]
    for (int i = 0; i < 16; i++)
    {
        float lum = dot(colors[i], float3(0.299, 0.587, 0.114));
        if (lum < minLum)
        {
            minLum = lum;
            minIdx = i;
        }
        if (lum > maxLum)
        {
            maxLum = lum;
            maxIdx = i;
        }
    }

    minColor = colors[minIdx];
    maxColor = colors[maxIdx];

    // 稍微向外扩展颜色范围以改善压缩质量
    float3 diff = maxColor - minColor;
    minColor = saturate(minColor - diff * 0.05);
    maxColor = saturate(maxColor + diff * 0.05);
}

// 计算BC1索引
uint ComputeBC1Indices(float3 colors[16], float3 color0, float3 color1)
{
    // 计算4个调色板颜色
    float3 palette[4];
    palette[0] = color0;
    palette[1] = color1;

    uint c0_565 = ColorToRGB565(color0);
    uint c1_565 = ColorToRGB565(color1);

    if (c0_565 > c1_565)
    {
        // 4色模式
        palette[2] = (2.0 * color0 + color1) / 3.0;
        palette[3] = (color0 + 2.0 * color1) / 3.0;
    }
    else
    {
        // 3色+透明模式（BC1不透明时我们用4色）
        palette[2] = (color0 + color1) * 0.5;
        palette[3] = float3(0, 0, 0); // 透明
    }

    // 为每个像素找到最近的调色板颜色
    uint indices = 0;

    [unroll]
    for (int i = 0; i < 16; i++)
    {
        float minDist = 1e10;
        uint bestIdx = 0;

        [unroll]
        for (int j = 0; j < 4; j++)
        {
            float dist = ColorDistance(colors[i], palette[j]);
            if (dist < minDist)
            {
                minDist = dist;
                bestIdx = j;
            }
        }

        indices |= (bestIdx << (i * 2));
    }

    return indices;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // 检查是否超出范围
    if (dispatchThreadId.x >= blockCountX || dispatchThreadId.y >= blockCountY)
        return;

    // 计算4x4块的起始像素坐标
    uint2 blockStart = dispatchThreadId.xy * 4;

    // 读取4x4块的颜色
    float3 colors[16];

    [unroll]
    for (int y = 0; y < 4; y++)
    {
        [unroll]
        for (int x = 0; x < 4; x++)
        {
            uint2 pixelCoord = blockStart + uint2(x, y);

            // 处理边界情况
            pixelCoord = min(pixelCoord, uint2(textureWidth - 1, textureHeight - 1));

            // 使用Load而不是Sample以获得精确像素
            float4 pixel = sourceTexture.Load(int3(pixelCoord, mipLevel));

            // sRGB转线性（如果需要）
            if (isSRGB)
            {
                pixel.rgb = pow(pixel.rgb, 2.2);
            }

            colors[y * 4 + x] = pixel.rgb;
        }
    }

    // 找到最小和最大颜色
    float3 minColor, maxColor;
    FindMinMaxColors(colors, minColor, maxColor);

    // 转换为RGB565
    uint c0 = ColorToRGB565(maxColor);
    uint c1 = ColorToRGB565(minColor);

    // 确保c0 > c1以使用4色模式
    if (c0 < c1)
    {
        uint temp = c0;
        c0 = c1;
        c1 = temp;
        float3 tempColor = minColor;
        minColor = maxColor;
        maxColor = tempColor;
    }

    // 计算索引
    uint indices = ComputeBC1Indices(colors, RGB565ToColor(c0), RGB565ToColor(c1));

    // 打包BC1块
    // BC1格式: [color0: 16bit][color1: 16bit][indices: 32bit]
    uint2 bc1Block;
    bc1Block.x = c0 | (c1 << 16);  // 两个颜色
    bc1Block.y = indices;           // 4x4索引

    // 写入输出
    outputTexture[dispatchThreadId.xy] = bc1Block;
}
