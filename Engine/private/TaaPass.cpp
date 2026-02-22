#include "public/TaaPass.h"
#include "public/Settings.h"
#include <d3dx12.h>
#include <stdexcept>
#include <iostream>
#include <cmath>

TaaPass::~TaaPass() {
    // ComPtr 会自动释放资源
}

bool TaaPass::Initialize(int viewportWidth, int viewportHeight) {
    m_viewportWidth = viewportWidth;
    m_viewportHeight = viewportHeight;

    m_rtvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateRenderTargets();
    CreateSRVHeap();

    // 创建 TAA 常量缓冲区
    UINT cbSize = (sizeof(TaaConstants) + 255) & ~255;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_taaConstantBuffer));

    if (FAILED(hr)) {
        std::cout << "TaaPass: Failed to create constant buffer" << std::endl;
        return false;
    }

    std::cout << "TaaPass initialized: " << viewportWidth << "x" << viewportHeight << std::endl;
    return true;
}

void TaaPass::CreateRenderTargets() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 4;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("TaaPass: Failed to create RTV heap");
    }

    D3D12_RESOURCE_DESC rtDesc = {};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = m_viewportWidth;
    rtDesc.Height = m_viewportHeight;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.SampleDesc.Quality = 0;
    rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 1.0f;

    CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);

    // 创建4个RT：输出、中间、历史1、历史2
    struct { ComPtr<ID3D12Resource>* target; D3D12_RESOURCE_STATES state; const wchar_t* name; } rts[] = {
        { &m_outputRT,       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"TAA Output RT" },
        { &m_intermediateRT, D3D12_RESOURCE_STATE_RENDER_TARGET,         L"TAA Intermediate RT" },
        { &m_historyRT,      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"TAA History RT 1" },
        { &m_historyRT2,     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"TAA History RT 2" },
    };

    for (auto& rt : rts) {
        hr = gD3D12Device->CreateCommittedResource(
            &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &rtDesc,
            rt.state, &clearValue,
            __uuidof(ID3D12Resource), reinterpret_cast<void**>(rt.target->GetAddressOf()));
        if (FAILED(hr)) {
            char msg[128];
            sprintf_s(msg, "TaaPass: Failed to create %S", rt.name);
            throw std::runtime_error(msg);
        }
        (*rt.target)->SetName(rt.name);
    }

    // 创建 RTV
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    gD3D12Device->CreateRenderTargetView(m_outputRT.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescriptorSize);
    gD3D12Device->CreateRenderTargetView(m_intermediateRT.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescriptorSize);
    gD3D12Device->CreateRenderTargetView(m_historyRT.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescriptorSize);
    gD3D12Device->CreateRenderTargetView(m_historyRT2.Get(), nullptr, rtvHandle);
}

void TaaPass::CreateSRVHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 4;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("TaaPass: Failed to create SRV heap");
    }
}

// 不再需要 CreateVertexBuffer，使用共享全屏VB
void TaaPass::CreateVertexBuffer() {
    // 保留空实现以兼容头文件声明，实际使用 GetSharedFullscreenQuadVB
}

void TaaPass::CreateInputSRVs(ID3D12Resource* currentColorRT,
                               ID3D12Resource* motionVectorRT,
                               ID3D12Resource* depthBuffer) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    // 0: 当前帧颜色
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gD3D12Device->CreateShaderResourceView(currentColorRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // 1: 历史帧颜色
    ID3D12Resource* historyRT = m_useHistory2 ? m_historyRT2.Get() : m_historyRT.Get();
    gD3D12Device->CreateShaderResourceView(historyRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // 2: Motion Vector
    srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    gD3D12Device->CreateShaderResourceView(motionVectorRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // 3: 深度缓冲
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    gD3D12Device->CreateShaderResourceView(depthBuffer, &srvDesc, srvHandle);
}

// ========== 辅助方法 ==========

void TaaPass::SetViewportAndScissor(ID3D12GraphicsCommandList* cmdList) {
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_viewportWidth);
    viewport.Height = static_cast<float>(m_viewportHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &viewport);

    D3D12_RECT scissorRect = { 0, 0, m_viewportWidth, m_viewportHeight };
    cmdList->RSSetScissorRects(1, &scissorRect);
}

void TaaPass::UpdateTaaConstants() {
    TaaConstants taaConstants = {};
    taaConstants.jitterOffset = m_currentJitter;
    taaConstants.previousJitter = m_previousJitter;
    taaConstants.resolution = XMFLOAT2(static_cast<float>(m_viewportWidth),
                                        static_cast<float>(m_viewportHeight));
    taaConstants.blendFactor = m_firstFrame ? 0.0f : m_blendFactor;

    UINT8* pData;
    CD3DX12_RANGE readRange(0, 0);
    m_taaConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pData));
    memcpy(pData, &taaConstants, sizeof(TaaConstants));
    m_taaConstantBuffer->Unmap(0, nullptr);
}

// ========== 绑定共享渲染状态 ==========
static void BindTaaRenderState(ID3D12GraphicsCommandList* cmdList,
    ID3D12PipelineState* pso, ID3D12RootSignature* rootSig,
    ID3D12Resource* sceneCB, ID3D12DescriptorHeap* srvHeap) {
    cmdList->SetGraphicsRootSignature(rootSig);
    cmdList->SetPipelineState(pso);

    if (sceneCB) {
        cmdList->SetGraphicsRootConstantBufferView(0, sceneCB->GetGPUVirtualAddress());
    }

    ID3D12DescriptorHeap* heaps[] = { srvHeap };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

    D3D12_VERTEX_BUFFER_VIEW vbv;
    GetSharedFullscreenQuadVB(vbv);
    cmdList->IASetVertexBuffers(0, 1, &vbv);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// ========== Render ==========

void TaaPass::Render(ID3D12GraphicsCommandList* cmdList,
                     ID3D12PipelineState* pso,
                     ID3D12RootSignature* rootSig,
                     ID3D12Resource* currentColorRT,
                     ID3D12Resource* motionVectorRT,
                     ID3D12Resource* depthBuffer) {
    if (!m_enabled) return;

    UpdateTaaConstants();
    CreateInputSRVs(currentColorRT, motionVectorRT, depthBuffer);

    ID3D12Resource* writeHistoryRT = m_useHistory2 ? m_historyRT.Get() : m_historyRT2.Get();
    int writeHistoryRTVIndex = m_useHistory2 ? 2 : 3;

    // 转换资源状态
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        currentColorRT, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        writeHistoryRT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->ResourceBarrier(2, barriers);

    CD3DX12_CPU_DESCRIPTOR_HANDLE historyRtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                                                    writeHistoryRTVIndex, m_rtvDescriptorSize);
    cmdList->OMSetRenderTargets(1, &historyRtvHandle, FALSE, nullptr);

    SetViewportAndScissor(cmdList);
    BindTaaRenderState(cmdList, pso, rootSig, m_sceneConstantBuffer, m_srvHeap.Get());
    cmdList->DrawInstanced(6, 1, 0, 0);

    // 恢复资源状态
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        currentColorRT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        writeHistoryRT, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(2, barriers);

    m_firstFrame = false;
}

ID3D12PipelineState* TaaPass::CreatePSO(ID3D12RootSignature* rootSig,
                                         D3D12_SHADER_BYTECODE vs,
                                         D3D12_SHADER_BYTECODE ps) {
    return CreateFullscreenPSO(rootSig, vs, ps, DXGI_FORMAT_R16G16B16A16_FLOAT);
}

void TaaPass::Resize(int newWidth, int newHeight) {
    if (newWidth == m_viewportWidth && newHeight == m_viewportHeight) return;

    m_viewportWidth = newWidth;
    m_viewportHeight = newHeight;

    m_outputRT.Reset();
    m_intermediateRT.Reset();
    m_historyRT.Reset();
    m_historyRT2.Reset();
    m_rtvHeap.Reset();

    CreateRenderTargets();
    m_firstFrame = true;

    std::cout << "TaaPass resized: " << newWidth << "x" << newHeight << std::endl;
}

void TaaPass::SwapHistoryBuffers() {
    m_useHistory2 = !m_useHistory2;
}

float TaaPass::HaltonSequence(int index, int base) {
    float result = 0.0f;
    float f = 1.0f / static_cast<float>(base);
    int i = index;
    while (i > 0) {
        result += f * static_cast<float>(i % base);
        i = i / base;
        f = f / static_cast<float>(base);
    }
    return result;
}

void TaaPass::UpdateJitter() {
    m_previousJitter = m_currentJitter;
    m_jitterIndex = (m_jitterIndex + 1) % JITTER_SAMPLE_COUNT;

    float jitterX = HaltonSequence(m_jitterIndex + 1, 2) - 0.5f;
    float jitterY = HaltonSequence(m_jitterIndex + 1, 3) - 0.5f;

    float jitterScale = Settings::GetInstance().GetTaaJitterScale();
    jitterX *= jitterScale;
    jitterY *= jitterScale;

    m_currentJitter.x = jitterX;
    m_currentJitter.y = jitterY;

    static int debugCounter = 0;
    if (debugCounter++ % 16 == 0) {
        char debugMsg[256];
        sprintf_s(debugMsg, "TAA Jitter[%d]: (%.4f, %.4f) NDC: (%.6f, %.6f) Scale: %.2f\n",
            m_jitterIndex, jitterX, jitterY,
            jitterX * 2.0f / m_viewportWidth, jitterY * 2.0f / m_viewportHeight, jitterScale);
        OutputDebugStringA(debugMsg);
    }
}

XMMATRIX TaaPass::GetJitteredProjectionMatrix(const XMMATRIX& projMatrix) const {
    if (!m_enabled) return projMatrix;

    XMMATRIX jitteredProj = projMatrix;
    float jitterOffsetX = m_currentJitter.x * 2.0f / static_cast<float>(m_viewportWidth);
    float jitterOffsetY = m_currentJitter.y * 2.0f / static_cast<float>(m_viewportHeight);
    jitteredProj.r[2].m128_f32[0] += jitterOffsetX;
    jitteredProj.r[2].m128_f32[1] += jitterOffsetY;
    return jitteredProj;
}

D3D12_CPU_DESCRIPTOR_HANDLE TaaPass::GetIntermediateRTV() const {
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_rtvDescriptorSize);
    return rtvHandle;
}

void TaaPass::RenderToSwapChain(ID3D12GraphicsCommandList* cmdList,
                                 ID3D12PipelineState* pso,
                                 ID3D12RootSignature* rootSig,
                                 ID3D12Resource* motionVectorRT,
                                 ID3D12Resource* depthBuffer,
                                 D3D12_CPU_DESCRIPTOR_HANDLE swapChainRTV) {
    if (!m_enabled) return;

    UpdateTaaConstants();
    CreateInputSRVs(m_intermediateRT.Get(), motionVectorRT, depthBuffer);

    ID3D12Resource* writeHistoryRT = m_useHistory2 ? m_historyRT.Get() : m_historyRT2.Get();
    int writeHistoryRTVIndex = m_useHistory2 ? 2 : 3;

    // 转换资源状态
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        m_intermediateRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        writeHistoryRT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->ResourceBarrier(2, barriers);

    CD3DX12_CPU_DESCRIPTOR_HANDLE historyRtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                                                    writeHistoryRTVIndex, m_rtvDescriptorSize);
    cmdList->OMSetRenderTargets(1, &historyRtvHandle, FALSE, nullptr);

    SetViewportAndScissor(cmdList);
    BindTaaRenderState(cmdList, pso, rootSig, m_sceneConstantBuffer, m_srvHeap.Get());
    cmdList->DrawInstanced(6, 1, 0, 0);

    // 历史缓冲转为SRV
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        writeHistoryRT, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, barriers);

    // 中间RT恢复为RT状态
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        m_intermediateRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->ResourceBarrier(1, barriers);

    m_firstFrame = false;
}

void TaaPass::CopyToSwapChain(ID3D12GraphicsCommandList* cmdList,
                               ID3D12PipelineState* copyPso,
                               ID3D12RootSignature* rootSig,
                               D3D12_CPU_DESCRIPTOR_HANDLE swapChainRTV) {
    ID3D12Resource* currentHistoryRT = m_useHistory2 ? m_historyRT.Get() : m_historyRT2.Get();

    // 为复制操作创建SRV
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    gD3D12Device->CreateShaderResourceView(currentHistoryRT, &srvDesc, srvHandle);

    cmdList->OMSetRenderTargets(1, &swapChainRTV, FALSE, nullptr);

    SetViewportAndScissor(cmdList);
    BindTaaRenderState(cmdList, copyPso, rootSig, nullptr, m_srvHeap.Get());
    cmdList->DrawInstanced(6, 1, 0, 0);
}

ID3D12PipelineState* TaaPass::CreateCopyPSO(ID3D12RootSignature* rootSig,
                                             D3D12_SHADER_BYTECODE vs,
                                             D3D12_SHADER_BYTECODE ps) {
    return CreateFullscreenPSO(rootSig, vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM);
}
