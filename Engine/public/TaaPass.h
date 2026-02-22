#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include "BattleFireDirect.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// TAA (Temporal Anti-Aliasing) Pass
// 在 ScreenPass 之后、UIPass 之前执行
// 需要：
// 1. 当前帧颜色缓冲
// 2. 历史帧颜色缓冲
// 3. Motion Vector 缓冲
// 4. 深度缓冲（用于重投影）
// 5. 上一帧的 View-Projection 矩阵

class TaaPass {
public:
    TaaPass() = default;
    ~TaaPass();

    // 初始化 TAA Pass
    // viewportWidth/Height: 视口尺寸
    bool Initialize(int viewportWidth, int viewportHeight);

    // 渲染 TAA
    // currentColorRT: 当前帧颜色（ScreenPass 输出）
    // motionVectorRT: Motion Vector 缓冲
    // depthBuffer: 深度缓冲
    void Render(ID3D12GraphicsCommandList* cmdList,
                ID3D12PipelineState* pso,
                ID3D12RootSignature* rootSig,
                ID3D12Resource* currentColorRT,
                ID3D12Resource* motionVectorRT,
                ID3D12Resource* depthBuffer);

    // 创建 TAA PSO
    ID3D12PipelineState* CreatePSO(ID3D12RootSignature* rootSig,
                                    D3D12_SHADER_BYTECODE vs,
                                    D3D12_SHADER_BYTECODE ps);

    // 分辨率变更
    void Resize(int newWidth, int newHeight);

    // 设置场景常量缓冲区
    void SetSceneConstantBuffer(ID3D12Resource* sceneCB) { m_sceneConstantBuffer = sceneCB; }

    // 获取 TAA 输出纹理（用于最终显示）
    ID3D12Resource* GetOutputTexture() const { return m_outputRT.Get(); }

    // 获取历史缓冲（用于下一帧）
    ID3D12Resource* GetHistoryTexture() const { return m_historyRT.Get(); }

    // 获取中间RT（SkyPass和ScreenPass渲染到此RT）
    ID3D12Resource* GetIntermediateRT() const { return m_intermediateRT.Get(); }

    // 获取中间RT的RTV句柄
    D3D12_CPU_DESCRIPTOR_HANDLE GetIntermediateRTV() const;

    // 渲染TAA结果到交换链（最终输出）
    void RenderToSwapChain(ID3D12GraphicsCommandList* cmdList,
                           ID3D12PipelineState* pso,
                           ID3D12RootSignature* rootSig,
                           ID3D12Resource* motionVectorRT,
                           ID3D12Resource* depthBuffer,
                           D3D12_CPU_DESCRIPTOR_HANDLE swapChainRTV);

    // 将TAA历史缓冲复制到交换链（最终显示）
    void CopyToSwapChain(ID3D12GraphicsCommandList* cmdList,
                         ID3D12PipelineState* copyPso,
                         ID3D12RootSignature* rootSig,
                         D3D12_CPU_DESCRIPTOR_HANDLE swapChainRTV);

    // 创建复制PSO（用于将TAA结果复制到交换链）
    ID3D12PipelineState* CreateCopyPSO(ID3D12RootSignature* rootSig,
                                        D3D12_SHADER_BYTECODE vs,
                                        D3D12_SHADER_BYTECODE ps);

    // 交换历史缓冲（每帧结束时调用）
    void SwapHistoryBuffers();

    // 获取当前帧的 Jitter 偏移（用于投影矩阵）
    XMFLOAT2 GetJitterOffset() const { return m_currentJitter; }

    // 获取当前Jitter索引（用于调试）
    int GetJitterIndex() const { return m_jitterIndex; }

    // 更新 Jitter（每帧开始时调用）
    void UpdateJitter();

    // 获取 Jitter 后的投影矩阵
    XMMATRIX GetJitteredProjectionMatrix(const XMMATRIX& projMatrix) const;

    // TAA 参数
    void SetBlendFactor(float factor) { m_blendFactor = factor; }
    float GetBlendFactor() const { return m_blendFactor; }

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    int GetViewportWidth() const { return m_viewportWidth; }
    int GetViewportHeight() const { return m_viewportHeight; }

private:
    // 创建渲染目标
    void CreateRenderTargets();

    // 创建 SRV 堆
    void CreateSRVHeap();

    // 创建输入 SRV
    void CreateInputSRVs(ID3D12Resource* currentColorRT,
                         ID3D12Resource* motionVectorRT,
                         ID3D12Resource* depthBuffer);

    // 创建全屏四边形顶点缓冲
    void CreateVertexBuffer();

    // 设置viewport和scissor（内部辅助）
    void SetViewportAndScissor(ID3D12GraphicsCommandList* cmdList);

    // 更新TAA常量缓冲区（内部辅助）
    void UpdateTaaConstants();

    // 生成 Halton 序列（用于 Jitter）
    float HaltonSequence(int index, int base);

private:
    // 视口尺寸
    int m_viewportWidth = 0;
    int m_viewportHeight = 0;

    // TAA 输出渲染目标
    ComPtr<ID3D12Resource> m_outputRT;

    // 中间RT（SkyPass和ScreenPass渲染到此RT，TAA从此RT读取）
    ComPtr<ID3D12Resource> m_intermediateRT;

    // 历史帧缓冲（双缓冲）
    ComPtr<ID3D12Resource> m_historyRT;
    ComPtr<ID3D12Resource> m_historyRT2;
    bool m_useHistory2 = false;  // 切换使用哪个历史缓冲

    // RTV 堆（4个RTV：输出 + 中间RT + 2个历史缓冲）
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvDescriptorSize = 0;

    // SRV 堆（存储输入纹理的 SRV）
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize = 0;

    // 场景常量缓冲区
    ID3D12Resource* m_sceneConstantBuffer = nullptr;

    // TAA 常量缓冲区
    ComPtr<ID3D12Resource> m_taaConstantBuffer;

    // Jitter 相关
    XMFLOAT2 m_currentJitter = { 0.0f, 0.0f };
    XMFLOAT2 m_previousJitter = { 0.0f, 0.0f };
    int m_jitterIndex = 0;
    static const int JITTER_SAMPLE_COUNT = 16;  // Halton 序列采样数

    // TAA 参数
    float m_blendFactor = 0.9f;  // 历史帧混合因子（0.9 = 90% 历史 + 10% 当前）
    bool m_enabled = true;

    // 是否是第一帧（第一帧没有历史数据）
    bool m_firstFrame = true;
};

// TAA 常量缓冲区结构
struct TaaConstants {
    XMFLOAT2 jitterOffset;      // 当前帧 Jitter
    XMFLOAT2 previousJitter;    // 上一帧 Jitter
    XMFLOAT2 resolution;        // 分辨率
    float blendFactor;          // 混合因子
    float padding;              // 对齐
};
