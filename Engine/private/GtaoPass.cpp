#include "public/GtaoPass.h"
#include <d3dx12.h>
#include <stdexcept>
#include <iostream>

GtaoPass::~GtaoPass() {
    // ComPtr 会自动释放资源
}

bool GtaoPass::Initialize(int viewportWidth, int viewportHeight) {
    m_viewportWidth = viewportWidth;
    m_viewportHeight = viewportHeight;

    m_rtvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateRenderTargets();
    CreateSRVHeap();
    CreateConstantBuffer();

    std::cout << "GtaoPass initialized: " << viewportWidth << "x" << viewportHeight << std::endl;
    return true;
}

void GtaoPass::CreateRenderTargets() {
    // RTV堆：2个RTV（raw AO + blurred AO）
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 2;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("GtaoPass: Failed to create RTV heap");
    }

    // RT描述（R8_UNORM单通道，节省带宽）
    D3D12_RESOURCE_DESC rtDesc = {};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = m_viewportWidth;
    rtDesc.Height = m_viewportHeight;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // 使用RGBA8方便调试，实际可以用R8
    rtDesc.SampleDesc.Count = 1;
    rtDesc.SampleDesc.Quality = 0;
    rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearValue.Color[0] = 1.0f;  // 默认AO = 1（无遮蔽）
    clearValue.Color[1] = 1.0f;
    clearValue.Color[2] = 1.0f;
    clearValue.Color[3] = 1.0f;

    CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);

    // 创建Raw AO RT
    hr = gD3D12Device->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
        IID_PPV_ARGS(&m_aoRawRT));
    if (FAILED(hr)) {
        throw std::runtime_error("GtaoPass: Failed to create raw AO RT");
    }
    m_aoRawRT->SetName(L"GTAO Raw AO RT");

    // 创建Blurred AO RT
    hr = gD3D12Device->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &rtDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
        IID_PPV_ARGS(&m_aoBlurredRT));
    if (FAILED(hr)) {
        throw std::runtime_error("GtaoPass: Failed to create blurred AO RT");
    }
    m_aoBlurredRT->SetName(L"GTAO Blurred AO RT");

    // 创建RTV
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    gD3D12Device->CreateRenderTargetView(m_aoRawRT.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, m_rtvDescriptorSize);
    gD3D12Device->CreateRenderTargetView(m_aoBlurredRT.Get(), nullptr, rtvHandle);
}

void GtaoPass::CreateSRVHeap() {
    // AO计算阶段SRV堆：2个SRV（Depth + Normal）
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 2;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_aoSrvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("GtaoPass: Failed to create AO SRV heap");
    }

    // Blur阶段SRV堆：2个SRV（RawAO + Depth）
    hr = gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_blurSrvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("GtaoPass: Failed to create Blur SRV heap");
    }
}

void GtaoPass::CreateConstantBuffer() {
    UINT cbSize = (sizeof(GtaoConstants) + 255) & ~255;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_gtaoConstantBuffer));

    if (FAILED(hr)) {
        std::cout << "GtaoPass: Failed to create constant buffer" << std::endl;
    }
}

void GtaoPass::UpdateConstants() {
    GtaoConstants constants = {};
    constants.resolution = XMFLOAT2(static_cast<float>(m_viewportWidth),
                                     static_cast<float>(m_viewportHeight));
    constants.inverseResolution = XMFLOAT2(1.0f / m_viewportWidth,
                                            1.0f / m_viewportHeight);
    constants.aoRadius = m_radius;
    constants.aoIntensity = m_intensity;
    constants.sliceCount = m_sliceCount;
    constants.stepsPerSlice = m_stepsPerSlice;
    constants.frameCounter = m_frameCounter;
    constants.falloffStart = m_radius * 0.6f;   // 衰减开始 = 半径的60%（削弱一半范围）
    constants.falloffEnd = m_radius;              // 衰减结束 = 半径
    constants.padding = 0.0f;

    UINT8* pData;
    CD3DX12_RANGE readRange(0, 0);
    m_gtaoConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pData));
    memcpy(pData, &constants, sizeof(GtaoConstants));
    m_gtaoConstantBuffer->Unmap(0, nullptr);
}

void GtaoPass::CreateAOInputSRVs(ID3D12Resource* depthBuffer, ID3D12Resource* normalRT) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_aoSrvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    // t0: 深度缓冲
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    gD3D12Device->CreateShaderResourceView(depthBuffer, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t1: 法线纹理
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gD3D12Device->CreateShaderResourceView(normalRT, &srvDesc, srvHandle);
}

void GtaoPass::CreateBlurInputSRVs(ID3D12Resource* depthBuffer) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_blurSrvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    // t0: Raw AO纹理
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    gD3D12Device->CreateShaderResourceView(m_aoRawRT.Get(), &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t1: 深度缓冲（用于边缘保持）
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    gD3D12Device->CreateShaderResourceView(depthBuffer, &srvDesc, srvHandle);
}

void GtaoPass::SetViewportAndScissor(ID3D12GraphicsCommandList* cmdList) {
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_viewportWidth);
    viewport.Height = static_cast<float>(m_viewportHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &viewport);

    D3D12_RECT scissorRect = { 0, 0, m_viewportWidth, m_viewportHeight };
    cmdList->RSSetScissorRects(1, &scissorRect);
}

void GtaoPass::Render(ID3D12GraphicsCommandList* cmdList,
                       ID3D12PipelineState* gtaoPso,
                       ID3D12PipelineState* blurPso,
                       ID3D12RootSignature* rootSig,
                       ID3D12Resource* depthBuffer,
                       ID3D12Resource* normalRT) {
    if (!m_enabled) return;

    // 更新常量缓冲区
    UpdateConstants();
    m_frameCounter++;

    // ========== Pass 1: GTAO 计算 ==========
    {
        // 创建AO输入SRV
        CreateAOInputSRVs(depthBuffer, normalRT);

        // 转换Raw AO RT为渲染目标状态
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_aoRawRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ResourceBarrier(1, &barrier);

        // 设置渲染目标
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
        float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        SetViewportAndScissor(cmdList);

        // 设置渲染状态
        cmdList->SetGraphicsRootSignature(rootSig);
        cmdList->SetPipelineState(gtaoPso);

        // 绑定场景常量缓冲区（b0）
        if (m_sceneConstantBuffer) {
            cmdList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
        }

        // 绑定GTAO常量缓冲区（b1，使用root parameter index 2）
        // 注意：root signature中 index 0 = b0(scene CB), index 1 = SRV table, index 2 = b1(material CB)
        cmdList->SetGraphicsRootConstantBufferView(2, m_gtaoConstantBuffer->GetGPUVirtualAddress());

        // 绑定SRV堆
        ID3D12DescriptorHeap* heaps[] = { m_aoSrvHeap.Get() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_aoSrvHeap->GetGPUDescriptorHandleForHeapStart());
        cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

        // 绘制全屏四边形
        D3D12_VERTEX_BUFFER_VIEW vbv;
        GetSharedFullscreenQuadVB(vbv);
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(6, 1, 0, 0);

        // Raw AO RT转为SRV状态
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_aoRawRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
    }

    // ========== Pass 2: 空间模糊 ==========
    {
        // 创建Blur输入SRV
        CreateBlurInputSRVs(depthBuffer);

        // 转换Blurred AO RT为渲染目标状态
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_aoBlurredRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ResourceBarrier(1, &barrier);

        // 设置渲染目标
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_rtvDescriptorSize);
        float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        SetViewportAndScissor(cmdList);

        // 设置渲染状态
        cmdList->SetGraphicsRootSignature(rootSig);
        cmdList->SetPipelineState(blurPso);

        // 绑定场景常量缓冲区（b0）- Blur也需要分辨率信息
        if (m_sceneConstantBuffer) {
            cmdList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
        }

        // 绑定SRV堆
        ID3D12DescriptorHeap* heaps[] = { m_blurSrvHeap.Get() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_blurSrvHeap->GetGPUDescriptorHandleForHeapStart());
        cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

        // 绘制全屏四边形
        D3D12_VERTEX_BUFFER_VIEW vbv;
        GetSharedFullscreenQuadVB(vbv);
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(6, 1, 0, 0);

        // Blurred AO RT转为SRV状态
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_aoBlurredRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
    }
}

ID3D12PipelineState* GtaoPass::CreateGtaoPSO(ID3D12RootSignature* rootSig,
                                               D3D12_SHADER_BYTECODE vs,
                                               D3D12_SHADER_BYTECODE ps) {
    return CreateFullscreenPSO(rootSig, vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM);
}

ID3D12PipelineState* GtaoPass::CreateBlurPSO(ID3D12RootSignature* rootSig,
                                              D3D12_SHADER_BYTECODE vs,
                                              D3D12_SHADER_BYTECODE ps) {
    return CreateFullscreenPSO(rootSig, vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM);
}

void GtaoPass::Resize(int newWidth, int newHeight) {
    if (newWidth == m_viewportWidth && newHeight == m_viewportHeight) return;

    m_viewportWidth = newWidth;
    m_viewportHeight = newHeight;

    // 释放旧资源
    m_aoRawRT.Reset();
    m_aoBlurredRT.Reset();
    m_rtvHeap.Reset();

    // 重新创建
    CreateRenderTargets();

    std::cout << "GtaoPass resized: " << newWidth << "x" << newHeight << std::endl;
}
