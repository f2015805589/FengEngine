// BC5Compress.hlsl
// BC5 (ATI2/3Dc) GPU压缩计算着色器
// BC5格式：每个4x4块 = 16字节 (8字节R通道 + 8字节G通道)
// 主要用于法线贴图的RG通道

// 常量缓冲
cbuffer CompressionParams : register(b0)
{
    uint textureWidth;
    uint textureHeight;
    uint blockCountX;
    uint blockCountY;
    uint mipLevel;
    uint isSRGB;      // BC5通常不使用sRGB
    float2 padding;
};

// 源纹理
Texture2D<float4> sourceTexture : register(t0);
SamplerState linearSampler : register(s0);

// 输出BC5纹理 (作为uint4缓冲，每个元素16字节)
RWTexture2D<uint4> outputTexture : register(u0);

// 找到通道的最小最大值
void FindMinMaxChannel(float values[16], out float minVal, out float maxVal)
{
    minVal = 1.0;
    maxVal = 0.0;

    [unroll]
    for (int i = 0; i < 16; i++)
    {
        minVal = min(minVal, values[i]);
        maxVal = max(maxVal, values[i]);
    }

    // 稍微扩展范围以改善压缩质量
    float range = maxVal - minVal;
    minVal = saturate(minVal - range * 0.02);
    maxVal = saturate(maxVal + range * 0.02);
}

// 计算单通道的索引 (3位索引，8级插值)
// 返回48位索引，打包成两个uint（低32位 + 高16位）
uint2 ComputeChannelIndices(float values[16], float val0, float val1)
{
    // 计算8个插值
    float palette[8];
    palette[0] = val0;
    palette[1] = val1;

    if (val0 > val1)
    {
        // 6个插值
        palette[2] = (6.0 * val0 + 1.0 * val1) / 7.0;
        palette[3] = (5.0 * val0 + 2.0 * val1) / 7.0;
        palette[4] = (4.0 * val0 + 3.0 * val1) / 7.0;
        palette[5] = (3.0 * val0 + 4.0 * val1) / 7.0;
        palette[6] = (2.0 * val0 + 5.0 * val1) / 7.0;
        palette[7] = (1.0 * val0 + 6.0 * val1) / 7.0;
    }
    else
    {
        // 4个插值 + 极值
        palette[2] = (4.0 * val0 + 1.0 * val1) / 5.0;
        palette[3] = (3.0 * val0 + 2.0 * val1) / 5.0;
        palette[4] = (2.0 * val0 + 3.0 * val1) / 5.0;
        palette[5] = (1.0 * val0 + 4.0 * val1) / 5.0;
        palette[6] = 0.0;
        palette[7] = 1.0;
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
            float dist = abs(values[i] - palette[j]);
            if (dist < minDist)
            {
                minDist = dist;
                bestIdx = j;
            }
        }

        indices[i] = bestIdx;
    }

    // 打包16个3位索引到48位
    // 索引布局: i0(3) i1(3) i2(3) ... i15(3) = 48位
    uint2 packed = uint2(0, 0);

    // 构建48位索引
    // 低32位包含索引0-10的部分
    // 高16位包含剩余

    uint lowBits = 0;
    uint highBits = 0;

    [unroll]
    for (int k = 0; k < 16; k++)
    {
        uint bitPos = k * 3;
        if (bitPos < 32)
        {
            lowBits |= (indices[k] << bitPos);
            // 检查是否跨越32位边界
            if (bitPos > 29)
            {
                highBits |= (indices[k] >> (32 - bitPos));
            }
        }
        else
        {
            highBits |= (indices[k] << (bitPos - 32));
        }
    }

    packed.x = lowBits;
    packed.y = highBits;

    return packed;
}

// 打包单个通道块 (8字节)
uint2 PackChannelBlock(float values[16])
{
    float minVal, maxVal;
    FindMinMaxChannel(values, minVal, maxVal);

    uint v0 = (uint)(maxVal * 255.0 + 0.5);
    uint v1 = (uint)(minVal * 255.0 + 0.5);

    // 确保v0 > v1使用8级插值模式
    float actualV0 = maxVal;
    float actualV1 = minVal;
    if (v0 < v1)
    {
        uint temp = v0;
        v0 = v1;
        v1 = temp;
        actualV0 = minVal;
        actualV1 = maxVal;
    }

    uint2 indices = ComputeChannelIndices(values, actualV0, actualV1);

    // 格式: [val0: 8bit][val1: 8bit][indices: 48bit]
    uint2 block;
    block.x = v0 | (v1 << 8) | ((indices.x & 0xFFFF) << 16);
    block.y = (indices.x >> 16) | ((indices.y & 0xFFFF) << 16);

    return block;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= blockCountX || dispatchThreadId.y >= blockCountY)
        return;

    uint2 blockStart = dispatchThreadId.xy * 4;

    // 读取4x4块的R和G通道
    float redChannel[16];
    float greenChannel[16];

    [unroll]
    for (int y = 0; y < 4; y++)
    {
        [unroll]
        for (int x = 0; x < 4; x++)
        {
            uint2 pixelCoord = blockStart + uint2(x, y);
            pixelCoord = min(pixelCoord, uint2(textureWidth - 1, textureHeight - 1));

            float4 pixel = sourceTexture.Load(int3(pixelCoord, mipLevel));

            // 法线贴图通常存储在线性空间，不需要sRGB转换
            int idx = y * 4 + x;
            redChannel[idx] = pixel.r;
            greenChannel[idx] = pixel.g;
        }
    }

    // 压缩R通道 (8字节)
    uint2 redBlock = PackChannelBlock(redChannel);

    // 压缩G通道 (8字节)
    uint2 greenBlock = PackChannelBlock(greenChannel);

    // 输出BC5块 (16字节)
    // [Red: 8字节][Green: 8字节]
    uint4 bc5Block;
    bc5Block.x = redBlock.x;
    bc5Block.y = redBlock.y;
    bc5Block.z = greenBlock.x;
    bc5Block.w = greenBlock.y;

    outputTexture[dispatchThreadId.xy] = bc5Block;
}
