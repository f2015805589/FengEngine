cbuffer DefaultVertexCB : register(b0)
{
    float4x4 ProjectionMatrix;
    float4x4 ViewMatrix;
    float4x4 ModelMatrix;
    float4x4 IT_ModelMatrix;
    float3 LightDirection;
    float3 CameraPositionWS;
    float Skylight;
    float3 _Padding0;
    float4x4 ReservedMemory[1020];
};

// Auto-generated Material Constants
cbuffer MaterialConstants : register(b1) {
    float4 BaseColor;  // Offset: 0
    float Roughness;  // Offset: 16
    float Metallic;  // Offset: 20
    float4 _Padding[14];  // Padding to 256 bytes
};


TextureCube g_Cubemap : register(t0);
Texture2D g_Color : register(t1);
Texture2D g_Normal : register(t2);
Texture2D g_Orm : register(t3);

// Auto-generated Texture Declarations
Texture2D BaseColorTex : register(t10);
Texture2D NormalTex : register(t11);
Texture2D OrmTex : register(t12);


SamplerState gSamPointWrap : register(s0);
SamplerState gSamPointClamp : register(s1);
SamplerState gSamLinearWarp : register(s2);
SamplerState gSamLinearClamp : register(s3);
SamplerState gSamAnisotropicWarp : register(s4);
SamplerState gSamAnisotropicClamp : register(s5);


                
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

        struct PSOut
        {
            float4 outAlbedo : SV_TARGET0;
            float4 outNormal : SV_TARGET1;
            float4 outSpecular : SV_TARGET2;
        };

        PSOut MainPS(VSOut inPSInput)
        {
            PSOut o;

            // 采样纹理 - 使用Properties定义的纹理
            float3 sampledBaseColor = BaseColorTex.Sample(gSamAnisotropicWarp, inPSInput.texcoord.xy).xyz;
            float4 sampledNormal = NormalTex.Sample(gSamAnisotropicWarp, inPSInput.texcoord.xy);
            float3 sampledOrm = OrmTex.Sample(gSamAnisotropicWarp, inPSInput.texcoord.xy).xyz;

            // 应用材质参数
            float3 finalBaseColor = sampledBaseColor * BaseColor.xyz;

            // 调整ORM：在纹理基础上加上材质参数
            float ao = sampledOrm.r;  // AO保持不变
            float finalRoughness = saturate(sampledOrm.g + Roughness);  // 粗糙度相加
            float finalMetallic = saturate(sampledOrm.b + Metallic);    // 金属度相加

            // 计算TBN矩阵
            float3 N = normalize(inPSInput.normal.xyz);
            float4 T = inPSInput.tangent;
            T.xyz = normalize(T.xyz - dot(T.xyz, N) * N);
            float3 B = normalize(cross(N, T.xyz)) * T.w;
            float3x3 TBN = float3x3(T.xyz, B, N);
            TBN = transpose(TBN);

            // 法线贴图处理
            float3 tangentNormal = sampledNormal.xyz * 2.0 - 1.0;
            float3 normalWS = normalize(mul(TBN, tangentNormal));

            // 输出到GBuffer（不再输出position，使用深度重构）
            o.outAlbedo = float4(finalBaseColor, 1.0f);
            o.outNormal = float4(normalWS, 1.0f);
            o.outSpecular = float4(ao, finalRoughness, finalMetallic, 1.0f);

            return o;
        }

        