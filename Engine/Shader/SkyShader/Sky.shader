Shader "Sky"
{
    Pass
    {
        Name "SkyBox"

        HLSLPROGRAM
        #pragma vertex VS
        #pragma fragment PS

        struct VSInput
        {
            float3 pos : POSITION;
        };

        struct PSInput
        {
            float4 pos : SV_POSITION;
            float3 worldDir : TEXCOORD0;
        };

        PSInput VS(VSInput input)
        {
            PSInput output;

            // 天空球顶点就是采样方向（从球心指向顶点的方向）
            // 不需要加相机位置，因为立方体贴图采样只需要方向
            float3 sampleDir = normalize(input.pos);

            // 将顶点位置加上相机位置，使天空球跟随相机移动
            float3 worldPos = input.pos + CameraPositionWS;

            // 变换到裁剪空间（ViewMatrix * worldPos，然后 ProjectionMatrix）
            float4 viewPos = mul(ViewMatrix, float4(worldPos, 1.0f));
            output.pos = mul(ProjectionMatrix, viewPos);

            // 将深度设为最远，确保天空球在最后面
            output.pos.z = output.pos.w * 0.99999f;

            // 传递采样方向给像素着色器
            output.worldDir = sampleDir;

            return output;
        }

        float4 PS(PSInput input) : SV_TARGET
        {
            // 归一化方向向量用于立方体贴图采样
            float3 sampleDir = normalize(input.worldDir);

            // 采样天空立方体贴图
            float3 skyColor = SkyCube.Sample(gSamLinearClamp, sampleDir).rgb;

            return float4(skyColor, 1.0f);
        }

        ENDHLSL
    }
}
