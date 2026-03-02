#include "public/SsgiPass.h"
#include <d3dx12.h>
#include <stdexcept>
#include <vector>
#include <cstdlib>

SsgiPass::~SsgiPass() {
}

bool SsgiPass::Initialize(int viewportWidth, int viewportHeight) {
    m_viewportWidth = viewportWidth;
    m_viewportHeight = viewportHeight;

    // 计算SSGI渲染分辨率
    int scale = 1 << static_cast<int>(m_resolutionScale);
    m_ssgiWidth = m_viewportWidth / scale;
    m_ssgiHeight = m_viewportHeight / scale;

    m_rtvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateRenderTargets();
    CreateSRVHeap();
    CreateConstantBuffer();
    CreateDefaultBlackTexture();
    CreateNoiseTexture();

    return true;
}

void SsgiPass::CreateRenderTargets() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 7; // depth ping/pong, raw, blur temp, final, history1, history2
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("SsgiPass: Failed to create RTV heap");
    }

    // SSGI在低分辨率渲染
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_ssgiWidth;
    depthDesc.Height = m_ssgiHeight;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_RESOURCE_DESC colorDesc = depthDesc;
    colorDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // 升采样和模糊在全分辨率
    D3D12_RESOURCE_DESC fullResDesc = colorDesc;
    fullResDesc.Width = m_viewportWidth;
    fullResDesc.Height = m_viewportHeight;

    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = DXGI_FORMAT_R32_FLOAT;
    depthClear.Color[0] = 1.0f;

    D3D12_CLEAR_VALUE colorClear = {};
    colorClear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    colorClear.Color[0] = 0.0f;
    colorClear.Color[1] = 0.0f;
    colorClear.Color[2] = 0.0f;
    colorClear.Color[3] = 1.0f;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    auto createRT = [&](ComPtr<ID3D12Resource>& target, const D3D12_RESOURCE_DESC& desc, const D3D12_CLEAR_VALUE& clear, const wchar_t* name) {
        HRESULT ret = gD3D12Device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
            IID_PPV_ARGS(&target));
        if (FAILED(ret)) {
            throw std::runtime_error("SsgiPass: Failed to create RT");
        }
        target->SetName(name);
    };

    createRT(m_depthMaxPingRT, depthDesc, depthClear, L"SSGI Depth Max Ping");
    createRT(m_depthMaxPongRT, depthDesc, depthClear, L"SSGI Depth Max Pong");
    createRT(m_ssgiRawRT, colorDesc, colorClear, L"SSGI Raw");           // 低分辨率
    createRT(m_historyRT1, colorDesc, colorClear, L"SSGI History 1");    // 低分辨率
    createRT(m_historyRT2, colorDesc, colorClear, L"SSGI History 2");    // 低分辨率
    createRT(m_ssgiBlurTempRT, fullResDesc, colorClear, L"SSGI Blur Temp"); // 全分辨率
    createRT(m_ssgiFinalRT, fullResDesc, colorClear, L"SSGI Final");        // 全分辨率

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    gD3D12Device->CreateRenderTargetView(m_depthMaxPingRT.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescriptorSize);
    gD3D12Device->CreateRenderTargetView(m_depthMaxPongRT.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescriptorSize);
    gD3D12Device->CreateRenderTargetView(m_ssgiRawRT.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescriptorSize);
    gD3D12Device->CreateRenderTargetView(m_ssgiBlurTempRT.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescriptorSize);
    gD3D12Device->CreateRenderTargetView(m_ssgiFinalRT.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescriptorSize);
    gD3D12Device->CreateRenderTargetView(m_historyRT1.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescriptorSize);
    gD3D12Device->CreateRenderTargetView(m_historyRT2.Get(), nullptr, rtvHandle);
}

void SsgiPass::CreateSRVHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 32;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("SsgiPass: Failed to create SRV heap");
    }
}

void SsgiPass::CreateConstantBuffer() {
    UINT cbSize = (sizeof(SsgiConstants) + 255) & ~255;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_ssgiConstantBuffer));

    if (FAILED(hr)) {
        throw std::runtime_error("SsgiPass: Failed to create constant buffer");
    }
}

void SsgiPass::CreateDefaultBlackTexture() {
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&m_defaultBlackTexture));
    if (FAILED(hr)) return;

    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(m_defaultBlackTexture.Get(), 0, 1);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
    hr = gD3D12Device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_defaultBlackTextureUpload));
    if (FAILED(hr)) return;

    uint16_t blackPixel[8] = { 0,0,0,0,0,0,0,0 };

    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;

    gD3D12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    gD3D12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
    gD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    D3D12_SUBRESOURCE_DATA subresourceData = {};
    subresourceData.pData = blackPixel;
    subresourceData.RowPitch = sizeof(blackPixel);
    subresourceData.SlicePitch = sizeof(blackPixel);

    UpdateSubresources(cmdList.Get(), m_defaultBlackTexture.Get(), m_defaultBlackTextureUpload.Get(), 0, 0, 1, &subresourceData);

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_defaultBlackTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);
    cmdList->Close();

    ID3D12CommandList* ppCmdLists[] = { cmdList.Get() };
    gCommandQueue->ExecuteCommandLists(1, ppCmdLists);

    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    gCommandQueue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    CloseHandle(fenceEvent);
}

void SsgiPass::CreateNoiseTexture() {
    const UINT noiseSize = 4;
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = noiseSize;
    texDesc.Height = noiseSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&m_noiseTexture));
    if (FAILED(hr)) return;

    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(m_noiseTexture.Get(), 0, 1);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
    hr = gD3D12Device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_noiseTextureUpload));
    if (FAILED(hr)) return;

    std::vector<unsigned char> noise(noiseSize * noiseSize * 4);
    for (size_t i = 0; i < noise.size(); i += 4) {
        noise[i + 0] = static_cast<unsigned char>(rand() % 256);
        noise[i + 1] = static_cast<unsigned char>(rand() % 256);
        noise[i + 2] = 0;
        noise[i + 3] = 255;
    }

    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;

    gD3D12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    gD3D12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
    gD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    D3D12_SUBRESOURCE_DATA subresourceData = {};
    subresourceData.pData = noise.data();
    subresourceData.RowPitch = noiseSize * 4;
    subresourceData.SlicePitch = noiseSize * noiseSize * 4;

    UpdateSubresources(cmdList.Get(), m_noiseTexture.Get(), m_noiseTextureUpload.Get(), 0, 0, 1, &subresourceData);

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_noiseTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);
    cmdList->Close();

    ID3D12CommandList* ppCmdLists[] = { cmdList.Get() };
    gCommandQueue->ExecuteCommandLists(1, ppCmdLists);

    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    gCommandQueue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    CloseHandle(fenceEvent);
}

void SsgiPass::UpdateConstants() {
    SsgiConstants constants = {};
    constants.resolution = XMFLOAT2(static_cast<float>(m_ssgiWidth), static_cast<float>(m_ssgiHeight));
    constants.inverseResolution = XMFLOAT2(1.0f / m_ssgiWidth, 1.0f / m_ssgiHeight);
    constants.radius = m_radius;
    constants.intensity = m_intensity;
    constants.stepCount = m_stepCount;
    constants.directionCount = m_directionCount;
    constants.frameCounter = m_frameCounter;
    constants.depthPyramidPasses = m_depthPyramidPasses;
    constants.depthThickness = 0.02f;
    constants.temporalBlend = (m_frameCounter == 0) ? 0.0f : 0.95f;  // 提高到0.95，配合更大的抖动

    UINT8* pData;
    CD3DX12_RANGE readRange(0, 0);
    m_ssgiConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pData));
    memcpy(pData, &constants, sizeof(SsgiConstants));
    m_ssgiConstantBuffer->Unmap(0, nullptr);
}

void SsgiPass::SetViewportAndScissor(ID3D12GraphicsCommandList* cmdList) {
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_ssgiWidth);
    viewport.Height = static_cast<float>(m_ssgiHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &viewport);

    D3D12_RECT scissorRect = { 0, 0, m_ssgiWidth, m_ssgiHeight };
    cmdList->RSSetScissorRects(1, &scissorRect);
}

void SsgiPass::CreateDepthInputSRV(ID3D12Resource* sourceDepth, DXGI_FORMAT format, UINT descriptorIndex) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, m_srvDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    srvDesc.Format = format;
    gD3D12Device->CreateShaderResourceView(sourceDepth, &srvDesc, srvHandle);
}

void SsgiPass::CreateRaymarchInputSRVs(ID3D12Resource* depthMaxTex, ID3D12Resource* baseColorRT, ID3D12Resource* normalRT, ID3D12Resource* sceneDepth, ID3D12Resource* historyRT, ID3D12Resource* velocityRT, UINT descriptorStartIndex) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), descriptorStartIndex, m_srvDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    // t0: depth max
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    gD3D12Device->CreateShaderResourceView(depthMaxTex, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t1: base color
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gD3D12Device->CreateShaderResourceView(baseColorRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t2: normal
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gD3D12Device->CreateShaderResourceView(normalRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t3: noise
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    gD3D12Device->CreateShaderResourceView(m_noiseTexture.Get(), &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t4: original depth
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    gD3D12Device->CreateShaderResourceView(sceneDepth, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t5: history SSGI
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gD3D12Device->CreateShaderResourceView(historyRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t6: velocity (motion vector)
    srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    gD3D12Device->CreateShaderResourceView(velocityRT, &srvDesc, srvHandle);
}

void SsgiPass::CreateBlurInputSRV(ID3D12Resource* sourceRT, ID3D12Resource* sceneDepth, UINT descriptorStartIndex) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), descriptorStartIndex, m_srvDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gD3D12Device->CreateShaderResourceView(sourceRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    gD3D12Device->CreateShaderResourceView(sceneDepth, &srvDesc, srvHandle);
}

void SsgiPass::Render(ID3D12GraphicsCommandList* cmdList,
    ID3D12PipelineState* depthMaxPso,
    ID3D12PipelineState* ssgiPso,
    ID3D12PipelineState* upsamplePso,
    ID3D12PipelineState* blurHPso,
    ID3D12PipelineState* blurVPso,
    ID3D12RootSignature* rootSig,
    ID3D12Resource* depthBuffer,
    ID3D12Resource* baseColorRT,
    ID3D12Resource* normalRT,
    ID3D12Resource* velocityRT) {
    if (m_giType != GIType::SSGI) return;

    (void)depthMaxPso;

    UpdateConstants();

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(res, before, after);
        cmdList->ResourceBarrier(1, &barrier);
    };

    // 选择history buffer（ping-pong）
    ID3D12Resource* historyRT = m_useHistory2 ? m_historyRT2.Get() : m_historyRT1.Get();
    ID3D12Resource* outputHistoryRT = m_useHistory2 ? m_historyRT1.Get() : m_historyRT2.Get();

    // ========== SSGI Raw Pass (低分辨率，带temporal accumulation) ==========
    SetViewportAndScissor(cmdList); // 设置低分辨率viewport

    const UINT kRaymarchSrvStart = 0;
    CreateRaymarchInputSRVs(m_depthMaxPingRT.Get(), baseColorRT, normalRT, depthBuffer, historyRT, velocityRT, kRaymarchSrvStart);
    transition(m_ssgiRawRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 2, m_rtvDescriptorSize);
        float clearColor[4] = { 0, 0, 0, 1 };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        cmdList->SetGraphicsRootSignature(rootSig);
        cmdList->SetPipelineState(ssgiPso);
        if (m_sceneConstantBuffer) cmdList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
        cmdList->SetGraphicsRootConstantBufferView(2, m_ssgiConstantBuffer->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), kRaymarchSrvStart, m_srvDescriptorSize);
        cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

        D3D12_VERTEX_BUFFER_VIEW vbv;
        GetSharedFullscreenQuadVB(vbv);
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(6, 1, 0, 0);
    }
    transition(m_ssgiRawRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 保存当前帧到history（低分辨率）
    transition(outputHistoryRT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(m_ssgiRawRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(outputHistoryRT, m_ssgiRawRT.Get());
    transition(outputHistoryRT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(m_ssgiRawRT.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_useHistory2 = !m_useHistory2;

    // ========== Upsample Pass (升采样到全分辨率) ==========
    // 切换到全分辨率viewport
    D3D12_VIEWPORT fullViewport = {};
    fullViewport.Width = static_cast<float>(m_viewportWidth);
    fullViewport.Height = static_cast<float>(m_viewportHeight);
    fullViewport.MinDepth = 0.0f;
    fullViewport.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &fullViewport);
    D3D12_RECT fullScissor = { 0, 0, m_viewportWidth, m_viewportHeight };
    cmdList->RSSetScissorRects(1, &fullScissor);

    const UINT kUpsampleSrvStart = 11;
    CreateBlurInputSRV(m_ssgiRawRT.Get(), depthBuffer, kUpsampleSrvStart);
    transition(m_ssgiBlurTempRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 3, m_rtvDescriptorSize);
        float clearColor[4] = { 0, 0, 0, 1 };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        cmdList->SetGraphicsRootSignature(rootSig);
        cmdList->SetPipelineState(upsamplePso);
        if (m_sceneConstantBuffer) cmdList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
        cmdList->SetGraphicsRootConstantBufferView(2, m_ssgiConstantBuffer->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), kUpsampleSrvStart, m_srvDescriptorSize);
        cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

        D3D12_VERTEX_BUFFER_VIEW vbv;
        GetSharedFullscreenQuadVB(vbv);
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(6, 1, 0, 0);
    }
    transition(m_ssgiBlurTempRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ========== 横向模糊 Pass (全分辨率) ==========
    const UINT kBlurHSrvStart = 7;
    const UINT kBlurVSrvStart = 9;
    CreateBlurInputSRV(m_ssgiBlurTempRT.Get(), depthBuffer, kBlurHSrvStart);
    transition(m_ssgiFinalRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 4, m_rtvDescriptorSize);
        float clearColor[4] = { 0, 0, 0, 1 };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        cmdList->SetGraphicsRootSignature(rootSig);
        cmdList->SetPipelineState(blurHPso);
        if (m_sceneConstantBuffer) cmdList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
        cmdList->SetGraphicsRootConstantBufferView(2, m_ssgiConstantBuffer->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), kBlurHSrvStart, m_srvDescriptorSize);
        cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

        D3D12_VERTEX_BUFFER_VIEW vbv;
        GetSharedFullscreenQuadVB(vbv);
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(6, 1, 0, 0);
    }
    transition(m_ssgiFinalRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ========== 纵向模糊 Pass (全分辨率) ==========
    CreateBlurInputSRV(m_ssgiFinalRT.Get(), depthBuffer, kBlurVSrvStart);
    transition(m_ssgiBlurTempRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 3, m_rtvDescriptorSize);
        float clearColor[4] = { 0, 0, 0, 1 };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        cmdList->SetGraphicsRootSignature(rootSig);
        cmdList->SetPipelineState(blurVPso);
        if (m_sceneConstantBuffer) cmdList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
        cmdList->SetGraphicsRootConstantBufferView(2, m_ssgiConstantBuffer->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), kBlurVSrvStart, m_srvDescriptorSize);
        cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

        D3D12_VERTEX_BUFFER_VIEW vbv;
        GetSharedFullscreenQuadVB(vbv);
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(6, 1, 0, 0);
    }
    transition(m_ssgiBlurTempRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 最终输出在 m_ssgiBlurTempRT，需要复制到 m_ssgiFinalRT
    transition(m_ssgiFinalRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(m_ssgiBlurTempRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(m_ssgiFinalRT.Get(), m_ssgiBlurTempRT.Get());
    transition(m_ssgiFinalRT.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(m_ssgiBlurTempRT.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_frameCounter++;
}

ID3D12PipelineState* SsgiPass::CreateDepthPSO(ID3D12RootSignature* rootSig,
    D3D12_SHADER_BYTECODE vs,
    D3D12_SHADER_BYTECODE ps) {
    return CreateFullscreenPSO(rootSig, vs, ps, DXGI_FORMAT_R32_FLOAT);
}

ID3D12PipelineState* SsgiPass::CreateColorPSO(ID3D12RootSignature* rootSig,
    D3D12_SHADER_BYTECODE vs,
    D3D12_SHADER_BYTECODE ps) {
    return CreateFullscreenPSO(rootSig, vs, ps, DXGI_FORMAT_R16G16B16A16_FLOAT);
}

void SsgiPass::Resize(int newWidth, int newHeight) {
    if (newWidth == m_viewportWidth && newHeight == m_viewportHeight) return;

    m_viewportWidth = newWidth;
    m_viewportHeight = newHeight;

    // 重新计算SSGI渲染分辨率
    int scale = 1 << static_cast<int>(m_resolutionScale);
    m_ssgiWidth = m_viewportWidth / scale;
    m_ssgiHeight = m_viewportHeight / scale;

    m_depthMaxPingRT.Reset();
    m_depthMaxPongRT.Reset();
    m_ssgiRawRT.Reset();
    m_ssgiBlurTempRT.Reset();
    m_ssgiFinalRT.Reset();
    m_historyRT1.Reset();
    m_historyRT2.Reset();
    m_rtvHeap.Reset();

    CreateRenderTargets();
    m_frameCounter = 0;
    m_useHistory2 = false;
}

void SsgiPass::SetResolutionScale(int value) {
    SSGIResolutionScale newScale = static_cast<SSGIResolutionScale>(value);
    if (newScale == m_resolutionScale) return;

    m_resolutionScale = newScale;

    // 重新计算SSGI渲染分辨率
    int scale = 1 << static_cast<int>(m_resolutionScale);
    m_ssgiWidth = m_viewportWidth / scale;
    m_ssgiHeight = m_viewportHeight / scale;

    // 重建渲染目标
    m_depthMaxPingRT.Reset();
    m_depthMaxPongRT.Reset();
    m_ssgiRawRT.Reset();
    m_ssgiBlurTempRT.Reset();
    m_ssgiFinalRT.Reset();
    m_historyRT1.Reset();
    m_historyRT2.Reset();
    m_rtvHeap.Reset();

    CreateRenderTargets();
    m_frameCounter = 0;
    m_useHistory2 = false;
}

