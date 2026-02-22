// shadowdepth.hlsl - Complete rewrite for shadow depth rendering
// Renders scene from light's perspective and outputs depth to R32_FLOAT render target

// Constant buffer with light view-projection matrix
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
    float4x4 LightViewProjectionMatrix;  // Light's view-projection matrix
};

// Input vertex format (matches StaticMeshComponent)
struct VertexInput
{
    float4 position : POSITION;
    float4 texcoord : TEXCOORD;
    float4 normal : NORMAL;
    float4 tangent : TANGENT;
};

// Output to pixel shader
struct VSOutput
{
    float4 position : SV_POSITION;  // Clip space position from light's perspective
    float depth : TEXCOORD0;        // Linear depth for manual output
};

// Vertex Shader: Transform vertices to light clip space
VSOutput ShadowDepthVS(VertexInput input)
{
    VSOutput output;

    // 1. Model space -> World space
    float4 positionWS = mul(ModelMatrix, float4(input.position.xyz, 1.0));

    // 2. World space -> Light clip space
    float4 positionLS = mul(LightViewProjectionMatrix, positionWS);

    // Output clip space position for rasterization
    output.position = positionLS;

    // Store linear depth (Z/W) for pixel shader
    // After perspective divide, this will be in [0,1] range
    output.depth = positionLS.z / positionLS.w;

    return output;
}

// Pixel Shader: Output depth to R32_FLOAT render target
float4 ShadowDepthPS(VSOutput input) : SV_TARGET
{
    // Output linear depth to red channel
    // This depth value is already in [0,1] NDC range after perspective divide
    return float4(input.depth, 0.0, 0.0, 1.0);
}
