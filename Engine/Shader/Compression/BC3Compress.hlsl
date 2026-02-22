// BC3Compress.hlsl
// BC3 (DXT5) GPU压缩计算着色器
// BC3格式：每个4x4块 = 16字节 (8字节Alpha + 8字节BC1颜色)

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

// 输出BC3纹理 (作为uint4缓冲，每个元素16字节)
RWTexture2D<uint4> outputTexture : register(u0);

// 将float3颜色转换为RGB565
uint ColorToRGB565(float3 color)
{
    color = saturate(color);
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

// 计算颜色距离
float ColorDistance(float3 c1, float3 c2)
{
    float3 diff = c1 - c2;
    float3 weights = float3(0.299, 0.587, 0.114);
    return dot(diff * diff, weights);
}

// 找到颜色范围
void FindMinMaxColors(float3 colors[16], out float3 minColor, out float3 maxColor)
{
    minColor = float3(1, 1, 1);
    maxColor = float3(0, 0, 0);

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

    float3 diff = maxColor - minColor;
    minColor = saturate(minColor - diff * 0.05);
    maxColor = saturate(maxColor + diff * 0.05);
}

// 找到Alpha范围
void FindMinMaxAlpha(float alphas[16], out float minAlpha, out float maxAlpha)
{
    minAlpha = 1.0;
    maxAlpha = 0.0;

    [unroll]
    for (int i = 0; i < 16; i++)
    {
        minAlpha = min(minAlpha, alphas[i]);
        maxAlpha = max(maxAlpha, alphas[i]);
    }

    // 稍微扩展范围
    float range = maxAlpha - minAlpha;
    minAlpha = saturate(minAlpha - range * 0.02);
    maxAlpha = saturate(maxAlpha + range * 0.02);
}

// 计算BC1颜色索引
uint ComputeColorIndices(float3 colors[16], float3 color0, float3 color1)
{
    float3 palette[4];
    palette[0] = color0;
    palette[1] = color1;

    uint c0_565 = ColorToRGB565(color0);
    uint c1_565 = ColorToRGB565(color1);

    if (c0_565 > c1_565)
    {
        palette[2] = (2.0 * color0 + color1) / 3.0;
        palette[3] = (color0 + 2.0 * color1) / 3.0;
    }
    else
    {
        palette[2] = (color0 + color1) * 0.5;
        palette[3] = float3(0, 0, 0);
    }

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

// 计算Alpha块索引 (3位索引，8级插值)
uint2 ComputeAlphaIndices(float alphas[16], float alpha0, float alpha1)
{
    // 计算8个alpha值
    float alphaPalette[8];
    alphaPalette[0] = alpha0;
    alphaPalette[1] = alpha1;

    if (alpha0 > alpha1)
    {
        // 6个插值alpha
        alphaPalette[2] = (6.0 * alpha0 + 1.0 * alpha1) / 7.0;
        alphaPalette[3] = (5.0 * alpha0 + 2.0 * alpha1) / 7.0;
        alphaPalette[4] = (4.0 * alpha0 + 3.0 * alpha1) / 7.0;
        alphaPalette[5] = (3.0 * alpha0 + 4.0 * alpha1) / 7.0;
        alphaPalette[6] = (2.0 * alpha0 + 5.0 * alpha1) / 7.0;
        alphaPalette[7] = (1.0 * alpha0 + 6.0 * alpha1) / 7.0;
    }
    else
    {
        // 4个插值alpha + 0和1
        alphaPalette[2] = (4.0 * alpha0 + 1.0 * alpha1) / 5.0;
        alphaPalette[3] = (3.0 * alpha0 + 2.0 * alpha1) / 5.0;
        alphaPalette[4] = (2.0 * alpha0 + 3.0 * alpha1) / 5.0;
        alphaPalette[5] = (1.0 * alpha0 + 4.0 * alpha1) / 5.0;
        alphaPalette[6] = 0.0;
        alphaPalette[7] = 1.0;
    }

    // 为每个像素找到最佳索引
    uint indices[16];

    [unroll]
    for (int i = 0; i < 16; i++)
    {
        float minDist = 1e10;
        uint bestIdx = 0;

        [unroll]
        for (int j = 0; j < 8; j++)
        {
            float dist = abs(alphas[i] - alphaPalette[j]);
            if (dist < minDist)
            {
                minDist = dist;
                bestIdx = j;
            }
        }

        indices[i] = bestIdx;
    }

    // 打包16个3位索引到48位 (分成两个uint)
    // 低32位: 索引0-10 (33位，实际用32位)
    // 高16位: 索引11-15 (15位)
    uint2 packedIndices = uint2(0, 0);

    // 前16个索引打包到48位
    uint bits = 0;
    [unroll]
    for (int k = 0; k < 16; k++)
    {
        bits |= (indices[k] << (k * 3));
        if (k == 10)
        {
            packedIndices.x = bits;
            bits = indices[k] >> 2; // 第10个索引的高位
        }
    }
    packedIndices.y = bits >> 1;

    return packedIndices;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= blockCountX || dispatchThreadId.y >= blockCountY)
        return;

    uint2 blockStart = dispatchThreadId.xy * 4;

    // 读取4x4块
    float3 colors[16];
    float alphas[16];

    [unroll]
    for (int y = 0; y < 4; y++)
    {
        [unroll]
        for (int x = 0; x < 4; x++)
        {
            uint2 pixelCoord = blockStart + uint2(x, y);
            pixelCoord = min(pixelCoord, uint2(textureWidth - 1, textureHeight - 1));

            float4 pixel = sourceTexture.Load(int3(pixelCoord, mipLevel));

            if (isSRGB)
            {
                pixel.rgb = pow(pixel.rgb, 2.2);
            }

            int idx = y * 4 + x;
            colors[idx] = pixel.rgb;
            alphas[idx] = pixel.a;
        }
    }

    // ===== Alpha块 (8字节) =====
    float minAlpha, maxAlpha;
    FindMinMaxAlpha(alphas, minAlpha, maxAlpha);

    uint a0 = (uint)(maxAlpha * 255.0 + 0.5);
    uint a1 = (uint)(minAlpha * 255.0 + 0.5);

    // 确保a0 > a1使用8级插值
    if (a0 < a1)
    {
        uint temp = a0;
        a0 = a1;
        a1 = temp;
        float tempF = minAlpha;
        minAlpha = maxAlpha;
        maxAlpha = tempF;
    }

    uint2 alphaIndices = ComputeAlphaIndices(alphas, maxAlpha, minAlpha);

    // 打包Alpha块: [alpha0: 8bit][alpha1: 8bit][indices: 48bit]
    uint alphaBlock0 = a0 | (a1 << 8) | ((alphaIndices.x & 0xFFFF) << 16);
    uint alphaBlock1 = (alphaIndices.x >> 16) | (alphaIndices.y << 16);

    // ===== 颜色块 (8字节，同BC1) =====
    float3 minColor, maxColor;
    FindMinMaxColors(colors, minColor, maxColor);

    uint c0 = ColorToRGB565(maxColor);
    uint c1 = ColorToRGB565(minColor);

    if (c0 < c1)
    {
        uint temp = c0;
        c0 = c1;
        c1 = temp;
        float3 tempColor = minColor;
        minColor = maxColor;
        maxColor = tempColor;
    }

    uint colorIndices = ComputeColorIndices(colors, RGB565ToColor(c0), RGB565ToColor(c1));

    uint colorBlock0 = c0 | (c1 << 16);
    uint colorBlock1 = colorIndices;

    // 输出BC3块 (16字节)
    // [Alpha: 8字节][Color: 8字节]
    uint4 bc3Block;
    bc3Block.x = alphaBlock0;
    bc3Block.y = alphaBlock1;
    bc3Block.z = colorBlock0;
    bc3Block.w = colorBlock1;

    outputTexture[dispatchThreadId.xy] = bc3Block;
}
