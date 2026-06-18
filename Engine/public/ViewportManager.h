#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

class ViewportManager {
public:
    static ViewportManager& GetInstance() {
        static ViewportManager instance;
        return instance;
    }

    ViewportManager(const ViewportManager&) = delete;
    ViewportManager& operator=(const ViewportManager&) = delete;

    bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* imguiSrvHeap);
    void Shutdown();

    // 根据新的视口尺寸重建所有资源
    bool Resize(int width, int height);

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

    // 颜色缓冲（3D最终输出）
    ID3D12Resource* GetColorTexture() const { return m_colorRT.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetColorRTV() const { return m_colorRTV; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetColorSRV() const { return m_colorSRV; }

private:
    ViewportManager() = default;

    void ReleaseResources();

    ID3D12Device* m_device = nullptr;
    ID3D12DescriptorHeap* m_imguiSrvHeap = nullptr;

    ComPtr<ID3D12Resource> m_colorRT;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;

    D3D12_CPU_DESCRIPTOR_HANDLE m_colorRTV = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_colorSRV = {};

    int m_width = 0;
    int m_height = 0;

    UINT m_rtvDescriptorSize = 0;
    UINT m_srvDescriptorSize = 0;
    UINT m_dsvDescriptorSize = 0;

    // 在 gImGuiDescriptorHeap 中分配的 SRV 偏移
    static constexpr UINT s_viewportSrvOffset = 2;
};