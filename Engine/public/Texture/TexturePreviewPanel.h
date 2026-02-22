// TexturePreviewPanel.h
// 纹理预览面板

#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include "TextureAsset.h"

using Microsoft::WRL::ComPtr;

// 预览参数常量缓冲
struct PreviewParams {
    int channelMode;      // 0=RGBA, 1=RGB, 2=R, 3=G, 4=B, 5=A, 6=Normal, 7=Luminance
    float exposure;       // HDR曝光值
    float gamma;          // Gamma校正值
    int mipLevel;         // Mip级别
    float uvOffsetX;      // UV偏移X
    float uvOffsetY;      // UV偏移Y
    float uvScaleX;       // UV缩放X
    float uvScaleY;       // UV缩放Y
};

// 预览通道模式
enum class PreviewChannelMode {
    RGBA,           // 完整RGBA
    RGB,            // 仅RGB（忽略Alpha）
    R,              // 仅红色通道
    G,              // 仅绿色通道
    B,              // 仅蓝色通道
    A,              // 仅Alpha通道（灰度显示）
    Normal,         // 法线可视化（将RG映射为法线方向）
    Luminance       // 亮度
};

// Cubemap展开模式
enum class CubemapUnfoldMode {
    Cross,          // 十字展开
    Horizontal,     // 水平展开（6格并排）
    Vertical,       // 垂直展开
    Sphere          // 球形预览（3D旋转）
};

class TexturePreviewPanel {
public:
    static TexturePreviewPanel& GetInstance();

    // 初始化（需要在ImGui初始化后调用）
    bool Initialize(ID3D12Device* device);
    void Shutdown();

    // 渲染UI
    void RenderUI();

    // 窗口控制
    void Show() { m_showWindow = true; }
    void Hide() { m_showWindow = false; }
    bool IsVisible() const { return m_showWindow; }

    // 设置要预览的纹理
    void SetTexture(TextureAsset* texture);
    void SetTexturePath(const std::wstring& path);

    // 延迟加载：在渲染帧之间的安全时机调用
    bool HasPendingLoad() const { return m_hasPendingLoad; }
    void ProcessPendingLoad();

    // 获取当前纹理
    TextureAsset* GetCurrentTexture() const { return m_currentTexture; }

private:
    TexturePreviewPanel() = default;
    ~TexturePreviewPanel() = default;
    TexturePreviewPanel(const TexturePreviewPanel&) = delete;
    TexturePreviewPanel& operator=(const TexturePreviewPanel&) = delete;

    // 渲染不同部分
    void RenderToolbar();
    void RenderTextureView();
    void RenderInfoPanel();
    void RenderCompressionSettings();

    // 创建预览用的SRV
    void CreatePreviewSRV();

    // 创建预览渲染资源
    bool CreatePreviewResources();
    bool CreatePreviewShaders();
    bool CreatePreviewRootSignature();
    bool CreatePreviewPSO();
    void CreatePreviewRenderTarget();

    // 渲染纹理到预览RT
    void RenderTextureToPreviewRT();

    // 设备引用
    ID3D12Device* m_device = nullptr;

    // 窗口状态
    bool m_showWindow = false;
    bool m_showInfoPanel = true;
    bool m_showCompressionPanel = false;

    // 当前预览的纹理
    TextureAsset* m_currentTexture = nullptr;
    std::wstring m_currentTexturePath;

    // 预览设置
    PreviewChannelMode m_channelMode = PreviewChannelMode::RGBA;
    CubemapUnfoldMode m_cubemapMode = CubemapUnfoldMode::Cross;
    float m_zoomLevel = 1.0f;
    int m_mipLevel = 0;
    float m_exposure = 1.0f;         // HDR曝光
    float m_gamma = 2.2f;            // Gamma值
    bool m_showAlphaCheckerboard = true;  // Alpha棋盘格背景
    bool m_fitToWindow = true;       // 自适应窗口大小

    // 平移
    float m_panX = 0.0f;
    float m_panY = 0.0f;
    bool m_isDragging = false;
    float m_dragStartX = 0.0f;
    float m_dragStartY = 0.0f;

    // 压缩设置（用于重新压缩）
    TextureCompressionFormat m_selectedFormat = TextureCompressionFormat::BC3;
    TextureCompressionQuality m_selectedQuality = TextureCompressionQuality::Normal;
    bool m_generateMips = true;
    bool m_sRGB = true;

    // ImGui纹理ID（用于预览显示）
    D3D12_GPU_DESCRIPTOR_HANDLE m_previewSRV = {};

    // 预览用描述符堆
    ComPtr<ID3D12DescriptorHeap> m_previewHeap;
    UINT m_srvDescriptorSize = 0;

    // 在ImGui描述符堆中的槽位索引
    UINT m_imguiSrvSlot = UINT_MAX;
    static const UINT IMGUI_TEXTURE_SLOT_START = 10;

    // 预览渲染资源
    ComPtr<ID3D12RootSignature> m_previewRootSignature;
    ComPtr<ID3D12PipelineState> m_previewPSO;
    ComPtr<ID3DBlob> m_vsBlob;
    ComPtr<ID3DBlob> m_psBlob;

    // 常量缓冲
    ComPtr<ID3D12Resource> m_constantBuffer;
    PreviewParams* m_mappedConstantBuffer = nullptr;

    // 预览渲染目标
    ComPtr<ID3D12Resource> m_previewRT;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_previewRTWidth = 0;
    UINT m_previewRTHeight = 0;

    // 顶点缓冲（全屏四边形）
    ComPtr<ID3D12Resource> m_quadVB;

    // 是否已初始化预览资源
    bool m_previewResourcesInitialized = false;

    // 延迟加载状态
    bool m_hasPendingLoad = false;
    std::wstring m_pendingLoadPath;
};

