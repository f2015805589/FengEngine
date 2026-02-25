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
    return true;
}

void ScreenPass::CreateSRVHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 6;  // RT0, RT1, RT2, Depth, SkyCube, ShadowMap
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
    ID3D12Resource* shadowMap) {
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
    }
}

void ScreenPass::Render(ID3D12GraphicsCommandList* cmdList,
    ID3D12PipelineState* pso, ID3D12RootSignature* rootSig,
    ID3D12Resource* rt0, ID3D12Resource* rt1, ID3D12Resource* rt2,
    ID3D12Resource* depthBuffer, ComPtr<ID3D12Resource> skyTexture,
    ID3D12Resource* shadowMap) {
    CreateInputSRVs(cmdList, rt0, rt1, rt2, depthBuffer, skyTexture, shadowMap);

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

void ScreenPass::Resize(int newWidth, int newHeight) {
    m_viewportWidth = newWidth;
    m_viewportHeight = newHeight;
}