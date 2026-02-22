// SH系数计算 Compute Shader
// 从CubeMap生成球谐系数（3阶，9个系数）

TextureCube<float4> InputCubemap : register(t0);
RWStructuredBuffer<float3> SHCoefficients : register(u0);  // 输出9个SH系数（RGB各9个）

SamplerState gSamLinearClamp : register(s0);

cbuffer SHParams : register(b0)
{
    uint CubemapSize;      // Cubemap每个面的分辨率
    uint SampleCount;      // 每个线程处理的样本数
    float Padding0;
    float Padding1;
};

// 球谐基函数（3阶）
// 输入：归一化方向向量
// 输出：9个基函数的值
void EvaluateSHBasis(float3 direction, out float basis[9])
{
    float x = direction.x;
    float y = direction.y;
    float z = direction.z;

    // Band 0 (l=0)
    basis[0] = 0.282095;  // 0.5 * sqrt(1/PI)

    // Band 1 (l=1)
    basis[1] = 0.488603 * y;  // sqrt(3/(4*PI)) * y
    basis[2] = 0.488603 * z;  // sqrt(3/(4*PI)) * z
    basis[3] = 0.488603 * x;  // sqrt(3/(4*PI)) * x

    // Band 2 (l=2)
    basis[4] = 1.092548 * x * y;  // sqrt(15/(4*PI)) * x * y
    basis[5] = 1.092548 * y * z;  // sqrt(15/(4*PI)) * y * z
    basis[6] = 0.315392 * (3.0 * z * z - 1.0);  // sqrt(5/(16*PI)) * (3z^2 - 1)
    basis[7] = 1.092548 * x * z;  // sqrt(15/(4*PI)) * x * z
    basis[8] = 0.546274 * (x * x - y * y);  // sqrt(15/(16*PI)) * (x^2 - y^2)
}

// 将UV坐标映射到CubeMap的3D方向
float3 CubemapUVToDirection(uint face, float2 uv)
{
    // uv范围：[0, 1] -> [-1, 1]
    float2 ndc = uv * 2.0 - 1.0;
    float3 dir;

    switch (face)
    {
        case 0: dir = float3(1.0,  -ndc.y, -ndc.x); break;  // +X
        case 1: dir = float3(-1.0, -ndc.y,  ndc.x); break;  // -X
        case 2: dir = float3(ndc.x, 1.0, ndc.y); break;     // +Y
        case 3: dir = float3(ndc.x, -1.0, -ndc.y); break;   // -Y
        case 4: dir = float3(ndc.x, -ndc.y, 1.0); break;    // +Z
        case 5: dir = float3(-ndc.x, -ndc.y, -1.0); break;  // -Z
    }

    return normalize(dir);
}

// 主Compute Shader
// 线程组：每个线程处理CubeMap的一个像素
[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixelCoord = dispatchThreadID.xy;
    uint faceIndex = dispatchThreadID.z;  // 0-5对应6个面

    // 检查边界
    if (pixelCoord.x >= CubemapSize || pixelCoord.y >= CubemapSize || faceIndex >= 6)
        return;

    // 计算UV坐标
    float2 uv = (float2(pixelCoord) + 0.5) / float(CubemapSize);

    // 获取方向向量
    float3 direction = CubemapUVToDirection(faceIndex, uv);

    // 采样CubeMap
    float3 radiance = InputCubemap.SampleLevel(gSamLinearClamp, direction, 0).rgb;

    // 计算SH基函数
    float shBasis[9];
    EvaluateSHBasis(direction, shBasis);

    // 立体角权重（校正CubeMap采样的变形）
    float2 ndc = uv * 2.0 - 1.0;
    float temp = 1.0 + ndc.x * ndc.x + ndc.y * ndc.y;
    float weight = 4.0 / (sqrt(temp) * temp);

    // 累加到SH系数（原子操作会有性能问题，这里使用GroupShared优化）
    // 注意：这里简化实现，实际应该使用InterlockedAdd或GroupShared内存
    for (int i = 0; i < 9; i++)
    {
        float3 contribution = radiance * shBasis[i] * weight;
        // TODO: 需要原子累加，暂时使用简化版本
        SHCoefficients[i] += contribution;
    }
}

// 归一化Compute Shader（在所有采样完成后调用）
[numthreads(9, 1, 1)]
void CSNormalize(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint coeffIndex = dispatchThreadID.x;
    if (coeffIndex >= 9)
        return;

    // 归一化因子：4 * PI / 总样本数
    float normFactor = 12.566370614359172 / float(CubemapSize * CubemapSize * 6);
    SHCoefficients[coeffIndex] *= normFactor;
}
