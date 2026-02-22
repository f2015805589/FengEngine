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
cbuffer DefaultVertexCB : register(b0)
{
    float4x4 ProjectionMatrix; // 0-15
    float4x4 ViewMatrix; // 16-31
    float4x4 ModelMatrix; // 32-47
    float4x4 IT_ModelMatrix; // 48-63
    float3 LightDirection; // 64-66��ʵ��ռ��64-67������䣩
    float3 CameraPositionWS; // 68-70��ʵ��ռ��68-71������䣩
    float4x4 ReservedMemory[1020];
};
PSInput VS(VSInput input)
{
    PSInput output;
    output.pos = float4(input.pos, 1.0f);
    output.uv = input.uv;

    output.lightDirection = LightDirection;
     // ���㲢��������������꣨����ͼ�����������ȡ��
    output.cameraPositionWS = CameraPositionWS; // ��ͼ�����ĵ�����ǰ����Ԫ�������λ��
    return output;
}
// ����������������ӦScenePass��4��RT��LightPass��1��RT��
Texture2D BaseColor : register(t0);
Texture2D Normal : register(t1);
Texture2D Orm : register(t2);
Texture2D position : register(t3);
Texture2D lightRT : register(t4);
TextureCube SkyCube : register(t5);

SamplerState gSamPointWrap : register(s0);
SamplerState gSamPointClamp : register(s1);
SamplerState gSamLinearWarp : register(s2);
SamplerState gSamLinearClamp : register(s3);
SamplerState gSamAnisotropicWarp : register(s4);
SamplerState gSamAnisotropicClamp : register(s5);


#define PI 3.14159265359

// ������ - Schlick ����
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// ���߷ֲ����� (NDF, GGX / Trowbridge-Reitz)
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom);
}

// �����ڱκ��� (Schlick-GGX)
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0; // k = (��+1)^2 / 8
    return NdotV / (NdotV * (1.0 - k) + k);
}

// ���κ��� Smith
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

    // ������ F0���ǽ���Ĭ��0.04��������albedo
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
//IBL����

// Fresnel - Schlick �ֲڶȱ���
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(1.0 - cosTheta, 5.0);
}

// IBL ��������
float3 ComputeIBL(
    float3 baseColor, // ���� BaseColor
    float3 normal, // ���ߣ�����ռ䣩
    float3 V, // �ӽǷ���
    float3 orm, // (R=AO, G=Roughness, B=Metallic)
    TextureCube skyTex, // �����ͼ
    SamplerState sam // ������
)
{
    float ao = orm.r;
    float roughness = orm.g;
    float metallic = orm.b;

    float3 N = normalize(normal);
    float3 R = reflect(-V, N);

    // F0 ����������
    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, baseColor, metallic);

    float NdotV = max(dot(N, V), 0.0);

    // ���� diffuse���򵥽��ƣ�
    float3 diffuse = baseColor * ao;

    // ���� specular��ֱ���� mip level ģ�� prefilter��
    float mip = roughness * 10.0;
    float3 prefiltered = skyTex.SampleLevel(sam, R, mip).rgb;
    float3 F = FresnelSchlickRoughness(NdotV, F0, roughness);

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);

    // ��������
    float3 ambient = kD * diffuse / PI + prefiltered * kS;

    return ambient;
}


float4 PS(PSInput input) : SV_TARGET
{
    
    // �򵥺ϳɣ�ʹ��LightPass�����Ϊ�������
    float4 baseColor = BaseColor.Sample(gSamPointWrap, input.uv);
    float4 normal = Normal.Sample(gSamPointWrap, input.uv);
    float4 orm = Orm.Sample(gSamPointWrap, input.uv);
    float4 positionWS = position.Sample(gSamPointWrap, input.uv);
    float4 shadow = lightRT.Sample(gSamPointWrap, input.uv);
    
    float3 V = normalize(input.cameraPositionWS.xyz - positionWS.xyz);
    float3 R = normalize(reflect(-V, normal.xyz));
    
    float roughness = 0.7;
    float3 reflectionColor = SkyCube.SampleLevel(gSamAnisotropicClamp, R, (1 - orm.y) * 10).xyz;
    
    float4 fin = float4(0, 0, 0, 0);
    
    //float specularIntensity = pow(R1, 100.0f);
    //���ռ���(BRDF)
    orm.y =  orm.y;
    orm.z =  1 - orm.z;
    float lightPow = 2.0f;
    float skyPow = 1.0f;
    fin.xyz = BRDF(normal.xyz, V, input.lightDirection, baseColor.xyz, orm.z, orm.y) * lightPow;
    if (positionWS.x * positionWS.y * positionWS.z != 0)
    {
        fin.xyz += reflectionColor * orm.z * skyPow;

    }
    else
    {
        fin.xyz += float3(0.3, 0.3, 0.4);
    }
    return fin;
}