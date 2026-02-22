struct VertexData
{
    float4 position : POSITION;
    float4 texcoord : TEXCOORD0;
    float4 normal : NORMAL;
    float4 tangent : TANGENT;
};

struct VSOut
{
    float4 position : SV_POSITION;
    float4 normal : NORMAL;
    float4 texcoord : TEXCOORD0;
    float4 positionWS : TEXCOORD1;
    float3 lightDirection : TEXCOORD2;
    float3 cameraPositionWS : TEXCOORD3;
};

static const float PI = 3.141592;
/*
cbuffer globalConstants : register(b1)
{
    float4 misc;
};
*/
cbuffer DefaultVertexCB : register(b0)
{
    float4x4 ProjectionMatrix; // 0-15
    float4x4 ViewMatrix; // 16-31
    float4x4 ModelMatrix; // 32-47
    float4x4 IT_ModelMatrix; // 48-63
    float3 LightDirection; // 64-66（实际占用64-67，含填充）
    float3 CameraPositionWS; // 68-70（实际占用68-71，含填充）
    float4x4 ReservedMemory[1020];
};

TextureCube g_Cubemap : register(t0); // 对应根签名的SRV槽位0
Texture2D g_Color : register(t1); // 对应根签名的SRV槽位1
Texture2D g_Normal : register(t2); // 对应根签名的SRV槽位1
Texture2D g_Orm : register(t3); // 对应根签名的SRV槽位1

SamplerState gSamPointWrap : register(s0);
SamplerState gSamPointClamp : register(s1);
SamplerState gSamLinearWarp : register(s2);
SamplerState gSamLinearClamp : register(s3);
SamplerState gSamAnisotropicWarp : register(s4);
SamplerState gSamAnisotropicClamp : register(s5);

VSOut MainVS(VertexData inVertexData)
{
    VSOut vo;
    vo.normal = mul(IT_ModelMatrix, inVertexData.normal);
    float3 positionMS = inVertexData.position.xyz;
    float4 positionWS = mul(ModelMatrix, float4(positionMS, 1.0));
    float4 positionVS = mul(ViewMatrix, positionWS);
    vo.position = mul(ProjectionMatrix, positionVS);
    vo.positionWS = positionWS;
    vo.texcoord = inVertexData.texcoord;
    vo.lightDirection = LightDirection;
     // 计算并传递相机世界坐标（从视图矩阵的逆矩阵获取）
    vo.cameraPositionWS = CameraPositionWS; // 视图逆矩阵的第四行前三个元素是相机位置
    
    return vo;
}


struct PSOut
{
    float4 outAlbedo : SV_TARGET0;
    float4 outNormal : SV_TARGET1;
    float4 outSpecular : SV_TARGET2;
    float4 position : SV_TARGET3;
};

PSOut MainPS(VSOut inPSInput)  //: SV_TARGET0
{
   
    PSOut o;
    o.outAlbedo = float4(1, 1, 1, 1);
    o.outNormal = float4(1, 1, 1, 1);
    o.outNormal = float4(1, 1, 1, 1);
    o.position = float4(1, 1, 1, 1);
    return o;
    //return float4(finalColor, 1.0f);
}