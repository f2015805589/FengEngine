#include "public/ViewportManager.h"
#include "public/BattleFireDirect.h"
#include <d3dx12.h>

bool ViewportManager::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* imguiSrvHeap) {
    m_device = device;
    m_imguiSrvHeap = imguiSrvHeap;

    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // 创建独立的 RTV 描述符堆
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)))) return false;

    return true;
}

void ViewportManager::Shutdown() {
    ReleaseResources();
}

void ViewportManager::ReleaseResources() {
    m_colorRT.Reset();
    m_rtvHeap.Reset();

    // gDSRT 交给 ViewportManager 管理
    if (gDSRT) {
        gDSRT->Release();
        gDSRT = nullptr;
    }

    m_width = 0;
    m_height = 0;
}

bool ViewportManager::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (width == m_width && height == m_height) return true;

    // 等待 GPU 完成
    WaitForCompletionOfCommandList();

    m_width = width;
    m_height = height;

    // 同步全局渲染分辨率
    extern int gRenderWidth;
    extern int gRenderHeight;
    gRenderWidth = width;
    gRenderHeight = height;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    // 颜色缓冲：R8G8B8A8_UNORM，允许 RTV + SRV
    D3D12_RESOURCE_DESC colorDesc = {};
    colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    colorDesc.Width = width;
    colorDesc.Height = height;
    colorDesc.DepthOrArraySize = 1;
    colorDesc.MipLevels = 1;
    colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    colorDesc.SampleDesc.Count = 1;
    colorDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE colorClear = {};
    colorClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    colorClear.Color[0] = 0.0f;
    colorClear.Color[1] = 0.0f;
    colorClear.Color[2] = 0.0f;
    colorClear.Color[3] = 1.0f;

    HRESULT hr = m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &colorDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClear,
        IID_PPV_ARGS(&m_colorRT));
    if (FAILED(hr)) return false;

    m_colorRT->SetName(L"ViewportColorRT");

    // 深度缓冲：R24G8_TYPELESS，允许 DSV + SRV
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthClear.DepthStencil.Depth = 1.0f;
    depthClear.DepthStencil.Stencil = 0;

    // 释放旧的 gDSRT
    if (gDSRT) {
        gDSRT->Release();
        gDSRT = nullptr;
    }

    hr = m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
        IID_PPV_ARGS(&gDSRT));
    if (FAILED(hr)) return false;

    gDSRT->SetName(L"ViewportDepthRT");

    // RTV
    m_colorRTV = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(m_colorRT.Get(), nullptr, m_colorRTV);

    // DSV（复用 gSwapChainDSVHeap 的第 0 个 descriptor）
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = gSwapChainDSVHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(gDSRT, &dsvDesc, dsv);

    // SRV（在 ImGui 的 SRV heap 中）
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart = m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu;
    srvCpu.ptr = srvCpuStart.ptr + s_viewportSrvOffset * m_srvDescriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    m_device->CreateShaderResourceView(m_colorRT.Get(), &srvDesc, srvCpu);

    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuStart = m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    m_colorSRV.ptr = srvGpuStart.ptr + s_viewportSrvOffset * m_srvDescriptorSize;

    return true;
}