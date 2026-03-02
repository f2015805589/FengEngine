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

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_viewportWidth;
    depthDesc.Height = m_viewportHeight;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_RESOURCE_DESC colorDesc = depthDesc;
    colorDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

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
    createRT(m_ssgiRawRT, colorDesc, colorClear, L"SSGI Raw");
    createRT(m_ssgiBlurTempRT, colorDesc, colorClear, L"SSGI Blur Temp");
    createRT(m_ssgiFinalRT, colorDesc, colorClear, L"SSGI Final");
    createRT(m_historyRT1, colorDesc, colorClear, L"SSGI History 1");
    createRT(m_historyRT2, colorDesc, colorClear, L"SSGI History 2");

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
    constants.resolution = XMFLOAT2(static_cast<float>(m_viewportWidth), static_cast<float>(m_viewportHeight));
    constants.inverseResolution = XMFLOAT2(1.0f / m_viewportWidth, 1.0f / m_viewportHeight);
    constants.radius = m_radius;
    constants.intensity = m_intensity;
    constants.stepCount = m_stepCount;
    constants.directionCount = m_directionCount;
    constants.frameCounter = m_frameCounter;
    constants.depthPyramidPasses = m_depthPyramidPasses;
    constants.depthThickness = 0.02f;
    constants.temporalBlend = (m_frameCounter == 0) ? 0.0f : 0.9f;

    UINT8* pData;
    CD3DX12_RANGE readRange(0, 0);
    m_ssgiConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pData));
    memcpy(pData, &constants, sizeof(SsgiConstants));
    m_ssgiConstantBuffer->Unmap(0, nullptr);
}

void SsgiPass::SetViewportAndScissor(ID3D12GraphicsCommandList* cmdList) {
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_viewportWidth);
    viewport.Height = static_cast<float>(m_viewportHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &viewport);

    D3D12_RECT scissorRect = { 0, 0, m_viewportWidth, m_viewportHeight };
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

void SsgiPass::CreateRaymarchInputSRVs(ID3D12Resource* depthMaxTex, ID3D12Resource* baseColorRT, ID3D12Resource* normalRT, ID3D12Resource* sceneDepth, UINT descriptorStartIndex) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), descriptorStartIndex, m_srvDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    srvDesc.Format = DXGI_FORMAT_R32_FLOAT; // depth max
    gD3D12Device->CreateShaderResourceView(depthMaxTex, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // base color
    gD3D12Device->CreateShaderResourceView(baseColorRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // normal
    gD3D12Device->CreateShaderResourceView(normalRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // noise
    gD3D12Device->CreateShaderResourceView(m_noiseTexture.Get(), &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // original depth
    gD3D12Device->CreateShaderResourceView(sceneDepth, &srvDesc, srvHandle);
}

void SsgiPass::CreateTaaInputSRVs(ID3D12Resource* currentRT, ID3D12Resource* historyRT, ID3D12Resource* depthTex) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gD3D12Device->CreateShaderResourceView(currentRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    gD3D12Device->CreateShaderResourceView(historyRT, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    gD3D12Device->CreateShaderResourceView(depthTex, &srvDesc, srvHandle);
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
    ID3D12PipelineState* taaPso,
    ID3D12PipelineState* blurHPso,
    ID3D12PipelineState* blurVPso,
    ID3D12RootSignature* rootSig,
    ID3D12Resource* depthBuffer,
    ID3D12Resource* baseColorRT,
    ID3D12Resource* normalRT) {
    if (m_giType != GIType::SSGI) return;

    (void)depthMaxPso;
    (void)taaPso;

    UpdateConstants();

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(res, before, after);
        cmdList->ResourceBarrier(1, &barrier);
    };

    SetViewportAndScissor(cmdList);

    const UINT kRaymarchSrvStart = 4;

    // 只保留 SSGI Raw Pass
    CreateRaymarchInputSRVs(m_depthMaxPingRT.Get(), baseColorRT, normalRT, depthBuffer, kRaymarchSrvStart);
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

    // ========== 横向模糊 Pass ==========
    // 读取 m_ssgiRawRT(t0) + depthBuffer(t1) → 输出到 m_ssgiBlurTempRT
    // 注意：横向和纵向模糊必须使用不同的 SRV slot，
    // 因为 CPU 写描述符是即时的，但 GPU 执行 draw call 是延迟的，
    // 如果共用同一 slot，纵向模糊的写入会覆盖横向模糊尚未执行完的输入。
    const UINT kBlurHSrvStart = 10;
    const UINT kBlurVSrvStart = 12;
    CreateBlurInputSRV(m_ssgiRawRT.Get(), depthBuffer, kBlurHSrvStart);
    transition(m_ssgiBlurTempRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 3, m_rtvDescriptorSize);
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
    transition(m_ssgiBlurTempRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ========== 纵向模糊 Pass ==========
    // 读取 m_ssgiBlurTempRT(t0) + depthBuffer(t1) → 输出到 m_ssgiFinalRT
    CreateBlurInputSRV(m_ssgiBlurTempRT.Get(), depthBuffer, kBlurVSrvStart);
    transition(m_ssgiFinalRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 4, m_rtvDescriptorSize);
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
    transition(m_ssgiFinalRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

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
