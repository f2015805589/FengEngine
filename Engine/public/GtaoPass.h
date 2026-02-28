#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include "BattleFireDirect.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// AO类型枚举
enum class AOType {
    Off = 0,    // 关闭
    SSAO = 1,   // Screen Space Ambient Occlusion
    GTAO = 2    // Ground Truth Ambient Occlusion
};

// GTAO (Ground Truth Ambient Occlusion) Pass
// 在 LightPass 之后、ScreenPass 之前执行
// 输入：
//   1. 深度缓冲（用于重建世界坐标和射线求交）
//   2. 法线图（用于在法线半球内进行视线方向积分）
// 输出：
//   AO 贴图（单通道，用于延迟光照的环境遮蔽）
// 
// 流程：
//   Pass 1: GTAO 计算 - 从深度重建位置，在法线半球视线方向积分得到AO
//   Pass 2: 空间模糊（Cross-Bilateral Blur）- 边缘保持的模糊降噪

class GtaoPass {
public:
    GtaoPass() = default;
    ~GtaoPass();

    // 初始化
    bool Initialize(int viewportWidth, int viewportHeight);

    // 设置场景常量缓冲区
    void SetSceneConstantBuffer(ID3D12Resource* sceneCB) { m_sceneConstantBuffer = sceneCB; }

    // 渲染GTAO（包括AO计算 + 空间模糊）
    void Render(ID3D12GraphicsCommandList* cmdList,
                ID3D12PipelineState* gtaoPso,
                ID3D12PipelineState* blurPso,
                ID3D12RootSignature* rootSig,
                ID3D12Resource* depthBuffer,
                ID3D12Resource* normalRT);

    // 创建GTAO PSO（AO计算）
    ID3D12PipelineState* CreateGtaoPSO(ID3D12RootSignature* rootSig,
                                        D3D12_SHADER_BYTECODE vs,
                                        D3D12_SHADER_BYTECODE ps);

    // 创建Blur PSO（空间模糊）
    ID3D12PipelineState* CreateBlurPSO(ID3D12RootSignature* rootSig,
                                       D3D12_SHADER_BYTECODE vs,
                                       D3D12_SHADER_BYTECODE ps);

    // 分辨率变更
    void Resize(int newWidth, int newHeight);

    // 获取最终AO输出纹理（供ScreenPass/延迟光照使用）
    // 当AO关闭时返回默认白色纹理（AO=1，无遮蔽）
    ID3D12Resource* GetAOTexture() const {
        if (m_aoType != AOType::Off) return m_aoBlurredRT.Get();
        return m_defaultWhiteTexture.Get();
    }

    // 获取原始AO纹理（模糊前，用于调试）
    ID3D12Resource* GetRawAOTexture() const { return m_aoRawRT.Get(); }

    // AO类型设置
    void SetAOType(int type) { m_aoType = static_cast<AOType>(type); }
    int GetAOType() const { return static_cast<int>(m_aoType); }

    // 启用/禁用（保留兼容性）
    void SetEnabled(bool enabled) { m_aoType = enabled ? AOType::SSAO : AOType::Off; }
    bool IsEnabled() const { return m_aoType != AOType::Off; }

    // GTAO 参数
    void SetRadius(float radius) {
        if (m_aoType == AOType::SSAO) {
            m_ssaoRadius = radius;
        } else if (m_aoType == AOType::GTAO) {
            m_gtaoRadius = radius;
        }
    }
    float GetRadius() const {
        return (m_aoType == AOType::SSAO) ? m_ssaoRadius : m_gtaoRadius;
    }

    void SetIntensity(float intensity) {
        if (m_aoType == AOType::SSAO) {
            m_ssaoIntensity = intensity;
        } else if (m_aoType == AOType::GTAO) {
            m_gtaoIntensity = intensity;
        }
    }
    float GetIntensity() const {
        return (m_aoType == AOType::SSAO) ? m_ssaoIntensity : m_gtaoIntensity;
    }

    void SetSliceCount(int count) { m_sliceCount = count; }
    int GetSliceCount() const { return m_sliceCount; }

    void SetStepsPerSlice(int steps) { m_stepsPerSlice = steps; }
    int GetStepsPerSlice() const { return m_stepsPerSlice; }

    int GetViewportWidth() const { return m_viewportWidth; }
    int GetViewportHeight() const { return m_viewportHeight; }

private:
    // 创建渲染目标
    void CreateRenderTargets();

    // 创建SRV堆
    void CreateSRVHeap();

    // 创建GTAO常量缓冲区
    void CreateConstantBuffer();

    // 创建默认白色纹理（GTAO关闭时使用，AO=1表示无遮蔽）
    void CreateDefaultWhiteTexture();

    // 更新GTAO常量缓冲区
    void UpdateConstants();

    // 为AO计算Pass创建输入SRV
    void CreateAOInputSRVs(ID3D12Resource* depthBuffer, ID3D12Resource* normalRT);

    // 为Blur Pass创建输入SRV
    void CreateBlurInputSRVs(ID3D12Resource* depthBuffer);

    // 设置viewport和scissor
    void SetViewportAndScissor(ID3D12GraphicsCommandList* cmdList);

private:
    int m_viewportWidth = 0;
    int m_viewportHeight = 0;

    // AO原始输出RT（GTAO计算结果，模糊前）
    ComPtr<ID3D12Resource> m_aoRawRT;

    // AO模糊后输出RT（最终AO结果）
    ComPtr<ID3D12Resource> m_aoBlurredRT;

    // RTV堆（2个RTV: raw + blurred）
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvDescriptorSize = 0;

    // SRV堆 - AO计算阶段（2个SRV: Depth + Normal）
    ComPtr<ID3D12DescriptorHeap> m_aoSrvHeap;

    // SRV堆 - Blur阶段（2个SRV: RawAO + Depth）
    ComPtr<ID3D12DescriptorHeap> m_blurSrvHeap;

    UINT m_srvDescriptorSize = 0;

    // 场景常量缓冲区
    ID3D12Resource* m_sceneConstantBuffer = nullptr;

    // GTAO 常量缓冲区
    ComPtr<ID3D12Resource> m_gtaoConstantBuffer;

    // 默认白色纹理（GTAO关闭时作为fallback，AO=1无遮蔽）
    ComPtr<ID3D12Resource> m_defaultWhiteTexture;

    // 参数
    // SSAO参数
    float m_ssaoRadius = 2.5f;       // SSAO采样半径
    float m_ssaoIntensity = 5.0f;    // SSAO强度

    // GTAO参数
    float m_gtaoRadius = 1.0f;       // GTAO采样半径
    float m_gtaoIntensity = 1.0f;    // GTAO强度
    int m_sliceCount = 8;            // 方向切片数（GTAO专用）
    int m_stepsPerSlice = 8;         // 每个切片的步进数（GTAO专用）

    AOType m_aoType = AOType::SSAO;  // AO类型（默认SSAO）

    // 帧计数器（用于时域噪声旋转）
    int m_frameCounter = 0;
};

// GTAO 常量缓冲区结构（与HLSL对齐）
struct GtaoConstants {
    XMFLOAT2 resolution;         // 分辨率
    XMFLOAT2 inverseResolution;  // 1/分辨率
    float aoRadius;              // AO半径（世界空间）
    float aoIntensity;           // AO强度
    int sliceCount;              // 方向切片数
    int stepsPerSlice;           // 每个切片步进数
    int frameCounter;            // 帧计数器（时域旋转）
    int aoType;                  // AO类型 (0=Off, 1=SSAO, 2=GTAO)
    float falloffStart;          // 衰减开始距离
    float falloffEnd;            // 衰减结束距离
};
