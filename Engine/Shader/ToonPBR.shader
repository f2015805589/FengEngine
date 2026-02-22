Shader "ToonPBR"
{
    RenderQueue "Deferred"
    ShadingModel
    {
        shadingmodel=2;
        BRDF=ToonBRDF(N, V, L, albedo, metallic, roughness);
        BRDF
        {
            float3 ToonBRDF(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness)
            {
                float NdotL = max(dot(N, L), 0.0);
                float toonLevels = 4.0;
                float toonShading = floor(NdotL * toonLevels) / toonLevels;
                float NdotV = max(dot(N, V), 0.0);
                float rimLight = pow(1.0 - NdotV, 3.0) * 0.5;
                float3 diffuse = albedo * toonShading;
                float3 rim = float3(1.0, 1.0, 1.0) * rimLight;
                return diffuse + rim;
            }
        }
    }

    Properties
    {
        //# float4 BaseColor {default(1.0, 1.0, 1.0, 1.0), ui(ColorPicker)};
        //# float Roughness {default(0.5), min(0.0), max(1.0), ui(Slider)};
        //# float Metallic {default(0.0), min(0.0), max(1.0), ui(Slider)};
        //# Texture2D BaseColorTex;
        //# Texture2D NormalTex;
        //# Texture2D OrmTex;
    }



    Pass
    {
        Name "GBufferToonPass"

        HLSLPROGRAM
        #pragma vertex MainVS
        #pragma fragment MainPS

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
            float4 currentPositionCS : TEXCOORD8;   // 当前帧裁剪空间位置（用于Motion Vector）
            float4 previousPositionCS : TEXCOORD9;  // 上一帧裁剪空间位置（用于Motion Vector）
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

            // TAA: 计算当前帧和上一帧的裁剪空间位置（用于Motion Vector）
            // 注意：Motion Vector必须使用不带Jitter的投影矩阵，否则会导致重影
            vo.currentPositionCS = mul(CurrentViewProjectionMatrix, positionWS);  // 不带Jitter
            vo.previousPositionCS = mul(PreviousViewProjectionMatrix, positionWS);

            return vo;
        }

        struct GBuffer
        {
            float4 BaseColor : SV_TARGET0;
            float4 Normal : SV_TARGET1;
            float4 ORM : SV_TARGET2;
            float2 MotionVector : SV_TARGET3;  // 屏幕空间速度（用于TAA）
        };

        GBuffer MainPS(VSOut inPSInput)
        {
            GBuffer gbuffer;

            // 采样纹理 - 使用Bindless纹理系统（通过CB中的索引访问全局纹理数组）
            float3 sampledBaseColor = SAMPLE_TEXTURE(BaseColorTexIndex, gSamAnisotropicWarp, inPSInput.texcoord.xy).xyz;
            float4 sampledNormal = SAMPLE_TEXTURE(NormalTexIndex, gSamAnisotropicWarp, inPSInput.texcoord.xy);
            float3 sampledOrm = SAMPLE_TEXTURE(OrmTexIndex, gSamAnisotropicWarp, inPSInput.texcoord.xy).xyz;

            float3 finalBaseColor = sampledBaseColor * BaseColor.xyz;
            float ao = sampledOrm.r;
            float finalRoughness = saturate(sampledOrm.g + Roughness);
            float finalMetallic = saturate(sampledOrm.b + Metallic);

            float3 N = normalize(inPSInput.normal.xyz);
            float4 T = inPSInput.tangent;
            T.xyz = normalize(T.xyz - dot(T.xyz, N) * N);
            float3 B = normalize(cross(N, T.xyz)) * T.w;
            float3x3 TBN = float3x3(T.xyz, B, N);
            TBN = transpose(TBN);

            float3 tangentNormal = sampledNormal.xyz * 2.0 - 1.0;
            float3 normalWS = normalize(mul(TBN, tangentNormal));

            gbuffer.BaseColor = float4(finalBaseColor, 1.0f);
            gbuffer.Normal = float4(normalWS, 1.0f);

            int shadingModelID = 2;
            gbuffer.ORM = float4(ao, finalRoughness, finalMetallic, shadingModelID / 255.0f);

            // TAA: 计算Motion Vector（屏幕空间速度）
            float2 currentNDC = inPSInput.currentPositionCS.xy / inPSInput.currentPositionCS.w;
            float2 previousNDC = inPSInput.previousPositionCS.xy / inPSInput.previousPositionCS.w;
            float2 motionVector = (currentNDC - previousNDC) * 0.5f;
            gbuffer.MotionVector = motionVector;

            return gbuffer;
        }

        ENDHLSL
    }
}
