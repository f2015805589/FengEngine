// Standard PBR Shader for FEngine Material System
// 基于 ndctriangle.hlsl，添加了材质系统支持

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
    float4 tangent : TANGENT;
    float4 texcoord : TEXCOORD0;
    float4 positionWS : TEXCOORD1;
    float3 lightDirection : TEXCOORD2;
    float3 cameraPositionWS : TEXCOORD3;
    float4x4 IT_ModelMatrix : TEXCOORD4;
};

static const float PI = 3.141592;

// Scene常量缓冲区 (b0)
cbuffer DefaultVertexCB : register(b0)
{
    float4x4 ProjectionMatrix;          // 0-15
    float4x4 ViewMatrix;                // 16-31
    float4x4 ModelMatrix;               // 32-47
    float4x4 IT_ModelMatrix;            // 48-63
    float3 LightDirection;              // 64-66
    float3 CameraPositionWS;            // 68-70
    float4x4 ReservedMemory[1020];
};

// 材质常量缓冲区 (b1) - 新增
cbuffer MaterialConstants : register(b1)
{
    float4 BaseColor;       // Offset 0-15: 基础颜色
    float Roughness;        // Offset 16: 粗糙度
    float Metallic;         // Offset 20: 金属度
    float2 _Padding;        // Offset 24-31: 填充
    float4 _Reserved[14];   // Offset 32-255: 保留空间
};

// 全局纹理 (t0-t9保留给引擎)
TextureCube g_Cubemap : register(t0);    // 天空盒

// 材质纹理 (t10+由材质系统管理)
Texture2D g_BaseColorTex : register(t10);  // 基础颜色贴图
Texture2D g_NormalTex : register(t11);     // 法线贴图
Texture2D g_OrmTex : register(t12);        // ORM贴图 (Occlusion/Roughness/Metallic)

// 采样器
SamplerState gSamPointWrap : register(s0);
SamplerState gSamPointClamp : register(s1);
SamplerState gSamLinearWarp : register(s2);
SamplerState gSamLinearClamp : register(s3);
SamplerState gSamAnisotropicWarp : register(s4);
SamplerState gSamAnisotropicClamp : register(s5);

// 顶点着色器
VSOut MainVS(VertexData inVertexData)
{
    VSOut vo;
    vo.IT_ModelMatrix = IT_ModelMatrix;
    float3 tangentWS = normalize(mul(float4(inVertexData.tangent.xyz, 0.0f), ModelMatrix).xyz);
    vo.tangent = float4(tangentWS, inVertexData.tangent.w);
    vo.normal = mul(IT_ModelMatrix, inVertexData.normal);
    float3 positionMS = inVertexData.position.xyz;
    float4 positionWS = mul(ModelMatrix, float4(positionMS, 1.0));
    float4 positionVS = mul(ViewMatrix, positionWS);
    vo.position = mul(ProjectionMatrix, positionVS);
    vo.positionWS = positionWS;
    vo.texcoord = inVertexData.texcoord;
    vo.lightDirection = LightDirection;
    vo.cameraPositionWS = CameraPositionWS;

    return vo;
}

// 像素着色器输出结构（延迟渲染的G-Buffer）
struct PSOut
{
    float4 outAlbedo : SV_TARGET0;      // 基础颜色
    float4 outNormal : SV_TARGET1;      // 世界空间法线
    float4 outSpecular : SV_TARGET2;    // ORM (Occlusion/Roughness/Metallic)
    float4 position : SV_TARGET3;       // 世界空间位置
};

// 像素着色器
PSOut MainPS(VSOut inPSInput)
{
    PSOut o;

    // 采样材质纹理
    float3 sampledBaseColor = g_BaseColorTex.Sample(gSamAnisotropicWarp, inPSInput.texcoord.xy).xyz;
    float4 sampledNormal = g_NormalTex.Sample(gSamAnisotropicWarp, inPSInput.texcoord.xy);
    float3 sampledOrm = g_OrmTex.Sample(gSamAnisotropicWarp, inPSInput.texcoord.xy).xyz;

    // 应用材质参数
    float3 finalBaseColor = sampledBaseColor * BaseColor.xyz;

    // 调制ORM值
    float3 finalOrm = sampledOrm;
    finalOrm.g *= Roughness;  // 粗糙度通道
    finalOrm.b *= Metallic;   // 金属度通道

    // 法线处理
    float3 N = normalize(inPSInput.normal.xyz);
    float4 T = inPSInput.tangent;
    T.xyz = normalize(T.xyz - dot(T.xyz, N) * N);
    float3 B = normalize(cross(N, T.xyz)) * T.w;
    float3x3 TBN = float3x3(T.xyz, B, N);
    TBN = transpose(TBN);

    // 将法线从切线空间转换到世界空间
    sampledNormal *= 2;
    sampledNormal -= 1;
    float3 normalWS = normalize(mul(TBN, sampledNormal.xyz));

    // 输出到G-Buffer
    o.outAlbedo = float4(finalBaseColor, 1.0f);
    o.outNormal = float4(normalWS, 1.0f);
    o.outSpecular = float4(finalOrm.xyz, 1.0f);
    o.position = float4(inPSInput.positionWS.xyz, 1.0f);

    return o;
}
