Shader "Screen"
{
    Pass
    {
        Name "DeferredLighting"

        HLSLPROGRAM
        #pragma vertex VS
        #pragma fragment PS

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

        // ==================== UE5 风格 BRDF 函数 ====================

        // 菲涅尔项 - Schlick 近似
        float3 F_Schlick(float3 F0, float VoH)
        {
            float Fc = pow(1.0 - VoH, 5.0);
            return Fc + F0 * (1.0 - Fc);
        }

        // 菲涅尔项 - 带粗糙度的 Schlick 近似（用于 IBL）
        float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
        {
            return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
        }

        // 法线分布函数 (NDF) - GGX / Trowbridge-Reitz
        // UE 使用的版本，a2 = Pow4(Roughness)
        float D_GGX(float a2, float NoH)
        {
            float d = (NoH * a2 - NoH) * NoH + 1.0;
            return a2 / (PI * d * d);
        }

        // 可见性函数 - Smith Joint Approximation (UE 的优化版本)
        // 这个函数已经包含了 1/(4*NoL*NoV) 的除法
        float Vis_SmithJointApprox(float a2, float NoV, float NoL)
        {
            float a = sqrt(a2);
            float Vis_SmithV = NoL * (NoV * (1.0 - a) + a);
            float Vis_SmithL = NoV * (NoL * (1.0 - a) + a);
            return 0.5 / (Vis_SmithV + Vis_SmithL);
        }

        // 漫反射 - Lambert
        float3 Diffuse_Lambert(float3 DiffuseColor)
        {
            return DiffuseColor / PI;
        }

        // 计算 DFG 能量项（用于多重散射能量补偿）
        // 使用 Karis 的近似公式
        float2 ComputeEnvBRDFApprox(float NoV, float Roughness)
        {
            // 拟合曲线，近似预计算的 BRDF LUT
            float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
            float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
            float4 r = Roughness * c0 + c1;
            float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
            return float2(-1.04, 1.04) * a004 + r.zw;
        }

        // 计算 RGB 的亮度值 (UE 使用的公式)
        float Luminance(float3 color)
        {
            return dot(color, float3(0.2126, 0.7152, 0.0722));
        }

        // 计算 GGX 镜面反射能量项 (UE5 风格)
        // E = directional albedo = F0 * scale + bias
        // W = energy compensation = 1 + F0 * (1/E - 1)
        struct FBxDFEnergyTerms
        {
            float3 E;  // 单次散射的方向反照率
            float3 W;  // 多重散射能量补偿
        };

        FBxDFEnergyTerms ComputeGGXSpecEnergyTerms(float Roughness, float NoV, float3 SpecularColor)
        {
            FBxDFEnergyTerms Terms;

            // 获取 EnvBRDF (scale, bias)
            float2 EnvBRDF = ComputeEnvBRDFApprox(NoV, Roughness);

            // E = F0 * scale + bias (单次散射的能量)
            Terms.E = SpecularColor * EnvBRDF.x + EnvBRDF.y;

            // W = 1 + F0 * (1/E - 1) (多重散射能量补偿)
            // 参考: https://bruop.github.io/ibl/
            // 当 E 接近 0 时，避免除零
            float3 Einv = 1.0 / max(Terms.E, 0.001);
            Terms.W = 1.0 + SpecularColor * (Einv - 1.0);

            return Terms;
        }

        // 能量守恒 - 漫反射衰减 (UE5 风格)
        // 使用亮度值而不是完整 RGB，避免颜色偏移
        float ComputeEnergyPreservation(FBxDFEnergyTerms EnergyTerms)
        {
            return 1.0 - Luminance(EnergyTerms.E);
        }

        // 能量补偿 - 镜面反射多重散射 (UE5 风格)
        float3 ComputeEnergyConservation(FBxDFEnergyTerms EnergyTerms)
        {
            return EnergyTerms.W;
        }

        // UE5 风格 DefaultLit BRDF
        float3 DefaultBRDF(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness)
        {
            float3 H = normalize(V + L);

            // 计算各种点积
            float NoL = max(dot(N, L), 0.0);
            float NoV = max(dot(N, V), 0.0);
            float NoH = max(dot(N, H), 0.0);
            float VoH = max(dot(V, H), 0.0);

            // 避免除零 (UE 的做法)
            NoV = saturate(abs(NoV) + 1e-5);

            // 计算 DiffuseColor 和 SpecularColor (UE 的方式)
            float3 DiffuseColor = albedo * (1.0 - metallic);
            float3 SpecularColor = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

            // 粗糙度重映射 (UE 使用 Pow4)
            float a = roughness * roughness;
            float a2 = a * a;

            // ========== 镜面反射 (GGX BRDF) ==========
            float D = D_GGX(a2, NoH);
            float Vis = Vis_SmithJointApprox(a2, NoV, NoL);
            float3 F = F_Schlick(SpecularColor, VoH);

            float3 specular = D * Vis * F;

            // ========== 漫反射 (Lambert) ==========
            float3 diffuse = Diffuse_Lambert(DiffuseColor);

            // ========== 多重散射能量补偿 (UE5 风格) ==========
            FBxDFEnergyTerms EnergyTerms = ComputeGGXSpecEnergyTerms(roughness, NoV, SpecularColor);

            // 漫反射能量守恒（减去被镜面反射的能量，使用亮度值）
            diffuse *= ComputeEnergyPreservation(EnergyTerms);

            // 镜面反射多重散射补偿（补偿高粗糙度时损失的能量）
            specular *= ComputeEnergyConservation(EnergyTerms);

            return (diffuse + specular) * NoL;
        }
        float3 BRDF(int shadingModelID, float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness){
            switch(shadingModelID){
                case 1: return DefaultBRDF(N, V, L, albedo, metallic, roughness);
                //后面的会自动生成
                default: return float3(0, 0, 0);
            }
        }


        float4 PS(PSInput input) : SV_TARGET
        {
            // 从GBuffer采样
            float4 baseColor = BaseColor.Sample(gSamPointWrap, input.uv);
            float4 normal = Normal.Sample(gSamPointWrap, input.uv);
            float4 orm = Orm.Sample(gSamPointWrap, input.uv);  // (AO, Roughness, Metallic, ShadingModelID)
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

            // 解包ORM和ShadingModelID
            float ao = orm.r;
            float roughness = orm.g;
            float metallic = orm.b;
            uint shadingModelID = uint(orm.a * 255.0 + 0.5);  // 解包ShadingModel

            // 计算视线方向和反射方向
            float3 N = normalize(normal.xyz);
            float3 V = normalize(input.cameraPositionWS.xyz - positionWS.xyz);
            float3 R = reflect(-V, N);

            // ==================== 直接光照 (Cook-Torrance BRDF) ====================
            float3 L = normalize(-input.lightDirection);  // 光照方向
            float3 directLighting = BRDF(shadingModelID, N, V, L, baseColor.xyz, metallic, roughness);

            // ==================== Image-Based Lighting (IBL) - UE5 风格 ====================
            // 计算 DiffuseColor 和 SpecularColor (与直接光照一致)
            float3 DiffuseColor = baseColor.xyz * (1.0 - metallic);
            float3 SpecularColor = lerp(float3(0.04, 0.04, 0.04), baseColor.xyz, metallic);

            float NoV = saturate(abs(dot(N, V)) + 1e-5);

            // 采样预过滤环境贴图（根据粗糙度选择 mip level）
            float maxMipLevel = 8.0;
            float mipLevel = roughness * maxMipLevel;
            float3 prefilteredColor = SkyCube.SampleLevel(gSamAnisotropicClamp, R, mipLevel).rgb;

            // 采样辐照度（使用最高 mip 近似漫反射辐照度）
            float3 irradiance = SkyCube.SampleLevel(gSamAnisotropicClamp, N, maxMipLevel).rgb;

            // 计算能量项 (UE5 风格)
            FBxDFEnergyTerms EnergyTerms = ComputeGGXSpecEnergyTerms(roughness, NoV, SpecularColor);

            // ========== 漫反射 IBL ==========
            // 漫反射能量守恒（考虑镜面反射已反射的能量，使用亮度值）
            float3 diffuseIBL = irradiance * DiffuseColor * ComputeEnergyPreservation(EnergyTerms);

            // ========== 镜面反射 IBL ==========
            // Split-sum 近似: SpecularIBL = PrefilteredColor * E
            // E 已经包含了 F0 * scale + bias
            float3 specularIBL = prefilteredColor * EnergyTerms.E;

            // 多重散射能量补偿
            specularIBL *= ComputeEnergyConservation(EnergyTerms);

            // 合并环境光
            float3 ambient = (diffuseIBL + specularIBL) * Skylight * ao;

            // ==================== 组合最终颜色 ====================
            float3 finalColor = float3(0, 0, 0);
            float alpha = 1.0;

            // 检查是否是有效像素
            if (depth < 1.0)
            {
                // 采样阴影图（从LightPass输出）
                float shadow = ShadowMap.Sample(gSamPointWrap, input.uv).r;
                // 直接光照 * 阴影 + 间接光照
                finalColor = directLighting * shadow * 6.0 + ambient;
                alpha = 1.0;
            }
            else
            {
                // 天空盒背景 - 输出透明，让 SkyPass 渲染的天空显示出来
                finalColor = float3(0.0, 0.0, 0.0);
                alpha = 0.0;
            }

            return float4(finalColor, alpha);
        }

        ENDHLSL
    }
}
