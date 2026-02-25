// shadowdepth.hlsl - Shadow Map深度渲染
// 从光源视角渲染场景深度到Shadow Map

// 场景常量缓冲区（与其他Pass共享）
cbuffer SceneConstants : register(b0)
{
    float4x4 ProjectionMatrix;
    float4x4 ViewMatrix;
    float4x4 ModelMatrix;
    float4x4 IT_ModelMatrix;
    float3 LightDirection;
    float _Padding0;
    float3 CameraPositionWS;
    float _Padding1;
    float Skylight;
    float3 _Padding2;
    float4x4 InverseProjectionMatrix;
    float4x4 InverseViewMatrix;
    float3 SkylightColor;
    float _Padding3;
    float4x4 LightViewProjectionMatrix;  // LiSPSM矩阵
};

// 输入顶点格式（与StaticMeshComponent一致）
struct VertexInput
{
    float4 position : POSITION;
    float4 texcoord : TEXCOORD;
    float4 normal : NORMAL;
    float4 tangent : TANGENT;
};

// 输出到像素着色器
struct VSOutput
{
    float4 position : SV_POSITION;  // 光源空间裁剪坐标
};

// 顶点着色器：将顶点变换到光源裁剪空间
VSOutput ShadowDepthVS(VertexInput input)
{
    VSOutput output;

    // 1. 模型空间 -> 世界空间
    float4 positionWS = mul(ModelMatrix, float4(input.position.xyz, 1.0));

    // 2. 世界空间 -> 光源裁剪空间（使用LiSPSM矩阵）
    output.position = mul(LightViewProjectionMatrix, positionWS);

    return output;
}

// 像素着色器：空实现，只需要深度写入
// 深度值由硬件自动写入DSV
void ShadowDepthPS(VSOutput input)
{
    // 不输出任何颜色，深度由光栅化阶段自动写入
}
