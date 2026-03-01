#include "public/ScreenPass.h"
#include <d3dx12.h>
#include <stdexcept>

ScreenPass::~ScreenPass() {
    if (m_srvHeap) m_srvHeap->Release();
}

bool ScreenPass::Initialize(int viewportWidth, int viewportHeight) {
    m_viewportWidth = viewportWidth;
    m_viewportHeight = viewportHeight;
    CreateSRVHeap();
    CreateDefaultWhiteTexture();
    CreateDefaultBlackTexture();
    return true;
}

void ScreenPass::CreateSRVHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 8;  // RT0, RT1, RT2, Depth, SkyCube, ShadowMap, GTAO, SSGI
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("ScreenPass: Failed to create SRV heap");
    }
    m_srvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void ScreenPass::CreateInputSRVs(ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* rt0, ID3D12Resource* rt1, ID3D12Resource* rt2,
    ID3D12Resource* depthBuffer, ComPtr<ID3D12Resource> skyTexture,
    ID3D12Resource* shadowMap, ID3D12Resource* gtaoTexture, ID3D12Resource* ssgiTexture) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    // t0: RT0 (Albedo)
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gD3D12Device->CreateShaderResourceView(rt0, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t1: RT1 (Normal)
    gD3D12Device->CreateShaderResourceView(rt1, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t2: RT2 (ORM)
    gD3D12Device->CreateShaderResourceView(rt2, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t3: Depth buffer
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    gD3D12Device->CreateShaderResourceView(depthBuffer, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t4: SkyCube
    srvDesc.Format = skyTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = skyTexture->GetDesc().MipLevels;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
    gD3D12Device->CreateShaderResourceView(skyTexture.Get(), &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t5: ShadowMap (LightPass输出)
    if (shadowMap) {
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        gD3D12Device->CreateShaderResourceView(shadowMap, &srvDesc, srvHandle);
    } else {
        // shadowmap关闭时使用白色纹理（表示无阴影，完全照亮）
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        gD3D12Device->CreateShaderResourceView(m_defaultWhiteTexture.Get(), &srvDesc, srvHandle);
    }
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t6: GTAO AO纹理
    if (gtaoTexture) {
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        gD3D12Device->CreateShaderResourceView(gtaoTexture, &srvDesc, srvHandle);
    } else {
        // 没有GTAO纹理时使用白色纹理（AO=1，表示无遮蔽）
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        gD3D12Device->CreateShaderResourceView(m_defaultWhiteTexture.Get(), &srvDesc, srvHandle);
    }
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t7: SSGI GI纹理
    if (ssgiTexture) {
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        gD3D12Device->CreateShaderResourceView(ssgiTexture, &srvDesc, srvHandle);
    } else {
        // 没有SSGI纹理时使用黑色纹理（GI=0，无间接光）
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        gD3D12Device->CreateShaderResourceView(m_defaultBlackTexture.Get(), &srvDesc, srvHandle);
    }
}

void ScreenPass::Render(ID3D12GraphicsCommandList* cmdList,
    ID3D12PipelineState* pso, ID3D12RootSignature* rootSig,
    ID3D12Resource* rt0, ID3D12Resource* rt1, ID3D12Resource* rt2,
    ID3D12Resource* depthBuffer, ComPtr<ID3D12Resource> skyTexture,
    ID3D12Resource* shadowMap, ID3D12Resource* gtaoTexture, ID3D12Resource* ssgiTexture) {
    CreateInputSRVs(cmdList, rt0, rt1, rt2, depthBuffer, skyTexture, shadowMap, gtaoTexture, ssgiTexture);

    cmdList->SetGraphicsRootSignature(rootSig);
    cmdList->SetPipelineState(pso);

    if (m_sceneConstantBuffer) {
        cmdList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
    }

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

    // 使用共享全屏四边形VB
    D3D12_VERTEX_BUFFER_VIEW vbv;
    GetSharedFullscreenQuadVB(vbv);
    cmdList->IASetVertexBuffers(0, 1, &vbv);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(6, 1, 0, 0);
}

// 使用共享全屏PSO创建（带Alpha混合）
ID3D12PipelineState* ScreenPass::CreatePSO(ID3D12RootSignature* rootSig,
    D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps) {
    return CreateFullscreenPSO(rootSig, vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM, true);
}

void ScreenPass::CreateDefaultWhiteTexture() {
    // 创建1x1白色纹理，GTAO关闭时使用，AO=1表示无遮蔽
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 1;
    texDesc.Height = 1;
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
        nullptr, IID_PPV_ARGS(&m_defaultWhiteTexture));
    if (FAILED(hr)) return;

    // 创建上传缓冲区
    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(m_defaultWhiteTexture.Get(), 0, 1);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
    hr = gD3D12Device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_defaultWhiteTextureUpload));
    if (FAILED(hr)) return;

    // 白色像素数据 (RGBA = 255,255,255,255)
    uint8_t whitePixel[4] = { 255, 255, 255, 255 };

    // 使用临时命令列表上传
    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;

    gD3D12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    gD3D12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
    gD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    D3D12_SUBRESOURCE_DATA subresourceData = {};
    subresourceData.pData = whitePixel;
    subresourceData.RowPitch = 4;
    subresourceData.SlicePitch = 4;

    UpdateSubresources(cmdList.Get(), m_defaultWhiteTexture.Get(), m_defaultWhiteTextureUpload.Get(), 0, 0, 1, &subresourceData);

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_defaultWhiteTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);
    cmdList->Close();

    ID3D12CommandList* ppCmdLists[] = { cmdList.Get() };
    gCommandQueue->ExecuteCommandLists(1, ppCmdLists);

    // 等待GPU完成
    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    gCommandQueue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    CloseHandle(fenceEvent);
}

void ScreenPass::CreateDefaultBlackTexture() {
    // 创建1x1黑色纹理，SSGI关闭时使用，GI=0表示无间接光
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

    uint16_t blackPixel[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

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

void ScreenPass::Resize(int newWidth, int newHeight) {
    m_viewportWidth = newWidth;
    m_viewportHeight = newHeight;
}