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
Texture2D g_Normal : register(t2); // 对应根签名的SRV槽位2
Texture2D g_Orm : register(t3); // 对应根签名的SRV槽位3

SamplerState gSamPointWrap : register(s0);
SamplerState gSamPointClamp : register(s1);
SamplerState gSamLinearWarp : register(s2);
SamplerState gSamLinearClamp : register(s3);
SamplerState gSamAnisotropicWarp : register(s4);
SamplerState gSamAnisotropicClamp : register(s5);

VSOut MainVS(VertexData inVertexData)
{
    VSOut vo;
    vo.IT_ModelMatrix = IT_ModelMatrix;
    float3 tangentWS = normalize(mul(float4(inVertexData.tangent.xyz, 0.0f), ModelMatrix).xyz);
    vo.tangent = float4(tangentWS,inVertexData.tangent.w);
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
    float3 N = normalize(inPSInput.normal.xyz);
    float3 bottomColor = float3(1.0f, 1.0f,1.0f);
    float3 topColor = float3(1.0f, 1.0f, 1.0f);
    float theta = asin(N.y); //-PI/2 ~ PI/2
    theta /= PI; //-0.5~0.5
    theta += 0.5f; //0.0~1.0
    float ambientColorIntensity = 0.2;
    float3 ambientColor = lerp(bottomColor, topColor, theta) * ambientColorIntensity;
    float3 L = normalize(inPSInput.lightDirection);
    float3 cameraPositionWS = float3(inPSInput.cameraPositionWS);
    float3 V = normalize(cameraPositionWS.xyz - inPSInput.positionWS.xyz);

    float diffuseIntensity = max(0.0f, dot(N, L));
    float3 diffuseLightColor = float3(0.1f, 0.4f, 0.6f);
    float3 diffuseColor = diffuseLightColor * diffuseIntensity;
    
    float3 specularColor = float3(0.0f, 0.0f, 0.0f);
    
        //float3 R = normalize(reflect(-L, N));
    float3 R = normalize(L + V);
    
    float3 surfaceColor = ambientColor + diffuseColor + specularColor;
    // 镜面反射计算
    float roughness = 0.7f; // 控制反射的模糊程度，值越小反射越清晰
    float smoothness = 1.0f - roughness;
    // 计算反射向量
    // 修正版镜面反射计算
    
    float3 reflectionVector = normalize(reflect(-V, N));

   
    float4 T = inPSInput.tangent;
    T.xyz = normalize(T.xyz - dot(T.xyz, N) * N);
    float3 B = normalize(cross(N, T.xyz)) *T.w ;
    float3x3 TBN = float3x3(T.xyz, B, N);
    TBN = transpose(TBN);
    float3 RR = normalize(reflect(-V, N));
    
    float3 BaseColor = g_Color.Sample(gSamAnisotropicWarp, inPSInput.texcoord.xy).xyz;
    float4 Normal = g_Normal.Sample(gSamAnisotropicWarp, inPSInput.texcoord.xy);
    float3 Orm = g_Orm.Sample(gSamAnisotropicWarp, inPSInput.texcoord.xy).xyz;
     // 菲涅尔效应计算
    float3 fresnelR0 = float3(0.01f, 0.01f, 0.01f); // 基础反射率
    float fresnelFactor = pow(1.0f - max(0.0f, dot(V, N)), 5.0f);
    float3 fresnel = lerp(fresnelR0, float3(1.0f, 1.0f, 1.0f), fresnelFactor);
    
    
    float3 reflectionColor = g_Cubemap.SampleLevel(gSamAnisotropicClamp, RR, roughness*10).xyz;
     // 混合基础颜色和反射颜色，应用菲涅尔效应
    //float3 finalColor = lerp(surfaceColor * reflectionColor.xyz, reflectionColor.xyz, 0) ;
    
    //reflectionColor = lerp(reflectionColor, reflectionColor * 2, fresnel);
    o.outAlbedo = float4(BaseColor, 1.0f); //BaseColor + reflectionColor*0.1
    //o.outAlbedo = float4(1,1,1, 1.0f); //BaseColor + reflectionColor*0.1
    //o.outAlbedo = float4(RR, 1.0f);
    //Normal.z = sqrt(1 - Normal.x * Normal.x - Normal.y * Normal.y);
    
    Normal *= 2;
    Normal -= 1;
    float3 normalWS = normalize(mul(TBN, Normal.xyz));
    
    float specularIntensity = pow(max(0.0f, dot(normalWS.xyz, R)), 16.0f);
    specularColor = float3(1.0f, 1.0f, 1.0f) * specularIntensity;
    //Normal = mul(inPSInput.IT_ModelMatrix, Normal);
    o.outNormal = float4(normalWS, 1.0f);
    o.outSpecular = float4(Orm.xyz, 1.0f);
    o.position = float4(inPSInput.positionWS.xyz, 1.0f);
    return o;
    //return float4(finalColor, 1.0f);
}