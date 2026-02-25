cbuffer DefaultVertexCB : register(b0)
{
    float4x4 ProjectionMatrix;
    float4x4 ViewMatrix;
    float4x4 ModelMatrix;
    float4x4 IT_ModelMatrix;
    float3 LightDirection;
    float _LightPadding;
    float3 CameraPositionWS;
    float _CameraPadding;
    float Skylight;
    float3 _Padding0;
    float4x4 InverseProjectionMatrix;
    float4x4 InverseViewMatrix;
    float3 SkylightColor;
    float _Padding1;
    float4x4 ReservedMemory[1020];
};

Texture2D BaseColor : register(t0);
Texture2D Normal : register(t1);
Texture2D Orm : register(t2);
Texture2D DepthTexture : register(t3);
TextureCube SkyCube : register(t4);
Texture2D ShadowMap : register(t5);  // LightPass输出的阴影图

SamplerState gSamPointWrap : register(s0);
SamplerState gSamPointClamp : register(s1);
SamplerState gSamLinearWarp : register(s2);
SamplerState gSamLinearClamp : register(s3);
SamplerState gSamAnisotropicWarp : register(s4);
SamplerState gSamAnisotropicClamp : register(s5);


                
        struct VSInput
        {
            float3 pos : POSITION;
            float2 uv : TEXCOORD;
        };

        struct PSInput
        {
            float4 pos : SV_POSITION;
            float2 uv : TEXCOORD;
            float3 lightDirection : TEXCOORD1;
            float3 cameraPositionWS : TEXCOORD2;
        };

        PSInput VS(VSInput input)
        {
            PSInput output;
            output.pos = float4(input.pos, 1.0f);
            output.uv = input.uv;
            output.lightDirection = LightDirection;
            output.cameraPositionWS = CameraPositionWS;
            return output;
        }

        #define PI 3.14159265359

        // 菲涅尔项 - Schlick 近似
        float3 FresnelSchlick(float cosTheta, float3 F0)
        {
            return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
        }

        // 法线分布函数 (NDF, GGX / Trowbridge-Reitz)
        float DistributionGGX(float3 N, float3 H, float roughness)
        {
            float a = roughness * roughness;
            float a2 = a * a;
            float NdotH = max(dot(N, H), 0.0);
            float NdotH2 = NdotH * NdotH;

            float denom = (NdotH2 * (a2 - 1.0) + 1.0);
            return a2 / (PI * denom * denom);
        }

        // 几何遮蔽函数 (Schlick-GGX)
        float GeometrySchlickGGX(float NdotV, float roughness)
        {
            float r = (roughness + 1.0);
            float k = (r * r) / 8.0;
            return NdotV / (NdotV * (1.0 - k) + k);
        }

        // 几何函数 Smith
        float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
        {
            float NdotV = max(dot(N, V), 0.0);
            float NdotL = max(dot(N, L), 0.0);
            float ggx1 = GeometrySchlickGGX(NdotV, roughness);
            float ggx2 = GeometrySchlickGGX(NdotL, roughness);
            return ggx1 * ggx2;
        }

        // Cook-Torrance BRDF
        float3 BRDF(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness)
        {
            float3 H = normalize(V + L);

            float3 F0 = float3(0.04, 0.04, 0.04);
            F0 = lerp(F0, albedo, metallic);

            float NDF = DistributionGGX(N, H, roughness);
            float G = GeometrySmith(N, V, L, roughness);
            float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

            float3 numerator = NDF * G * F;
            float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;
            float3 specular = numerator / denominator;

            float3 kS = F;
            float3 kD = (1.0 - kS) * (1.0 - metallic);

            float NdotL = max(dot(N, L), 0.0);
            float3 diffuse = kD * albedo / PI;

            return (diffuse + specular) * NdotL;
        }

        float4 PS(PSInput input) : SV_TARGET
        {
            // 从GBuffer采样
            float4 baseColor = BaseColor.Sample(gSamPointWrap, input.uv);
            float4 normal = Normal.Sample(gSamPointWrap, input.uv);
            float4 orm = Orm.Sample(gSamPointWrap, input.uv);  // (AO, Roughness, Metallic, 1.0)
            float depth = DepthTexture.Sample(gSamPointWrap, input.uv).r;  // 采样深度

            // 从深度重构世界坐标
            // 1. UV转换到NDC空间 (范围-1到1)
            float2 ndcXY = input.uv * 2.0 - 1.0;
            ndcXY.y = -ndcXY.y;  // Y轴翻转（DX坐标系）

            // 2. 构建裁剪空间坐标 (NDC + Depth)
            float4 clipSpacePos = float4(ndcXY, depth, 1.0);

            // 3. 逆投影到视图空间
            float4 viewSpacePos = mul(InverseProjectionMatrix, clipSpacePos);
            viewSpacePos /= viewSpacePos.w;  // 透视除法

            // 4. 逆视图变换到世界空间
            float4 positionWS = mul(InverseViewMatrix, viewSpacePos);

            // 解包ORM
            float ao = orm.r;
            float roughness = orm.g;
            float metallic = orm.b;

            // 计算视线方向和反射方向
            float3 V = normalize(input.cameraPositionWS.xyz - positionWS.xyz);
            float3 R = reflect(-V, normal.xyz);

            // ==================== 直接光照 (Cook-Torrance BRDF) ====================
            float3 L = normalize(-input.lightDirection);  // 光照方向
            float3 directLighting = BRDF(normal.xyz, V, L, baseColor.xyz, metallic, roughness);

            // ==================== Image-Based Lighting (IBL) ====================
            // 1. 菲涅尔项
            float3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor.xyz, metallic);
            float NdotV = max(dot(normal.xyz, V), 0.0);
            float3 F = FresnelSchlick(NdotV, F0);

            // 2. 环境镜面反射（根据粗糙度采样预过滤的环境贴图）
            float maxMipLevel = 10.0;
            float mipLevel = roughness * maxMipLevel;
            float3 prefilteredColor = SkyCube.SampleLevel(gSamAnisotropicClamp, R, mipLevel).xyz;

            // 3. 环境漫反射（使用法线采样辐照度贴图）
            float3 irradiance = SkyCube.SampleLevel(gSamAnisotropicClamp, normal.xyz, maxMipLevel).xyz;

            // 4. 计算IBL贡献
            float3 kS = F;
            float3 kD = (1.0 - kS) * (1.0 - metallic);
            float3 diffuseIBL = kD * irradiance * baseColor.xyz;
            float3 specularIBL = prefilteredColor * F;

            float3 ambient = (diffuseIBL + specularIBL) * Skylight * SkylightColor * ao;

            // ==================== 组合最终颜色 ====================
            float3 finalColor = float3(0, 0, 0);

            // 检查是否是有效像素
            if (depth < 1.0)
            {
                // 采样阴影图
                float shadow = ShadowMap.Sample(gSamPointWrap, input.uv).r;
                finalColor = directLighting * shadow * 6.0 + ambient;
            }
            else
            {
                // 天空盒背景
                //float3 skyColor = SkyCube.SampleLevel(gSamAnisotropicClamp, V, 0.0).xyz;
                finalColor = float3(0.0,0.0,0.0);//skyColor * Skylight * SkylightColor;
            }

            return float4(finalColor, 1.0);
        }

        