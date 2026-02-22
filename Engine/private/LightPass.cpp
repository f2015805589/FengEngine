// lightpass.cpp
#include "public/LightPass.h"
#include <DirectXMath.h>
#include <stdexcept>
#include <d3dx12.h>

using namespace DirectX;

LightPass::LightPass(int width, int height)
    : m_width(width), m_height(height), m_rtvDescriptorSize(0), m_srvDescriptorSize(0) {
    // 移除原LightConstants初始化
}

LightPass::~LightPass() {
    // 不需要释放自己的常量缓冲区（Scene负责）
    if (m_shadowMap) m_shadowMap->Release();
    if (m_rtvHeap) m_rtvHeap->Release();
    if (m_srvHeap) m_srvHeap->Release();
}

bool LightPass::Initialize(ID3D12GraphicsCommandList* commandList) {
    CreateSRVHeap();
    CreateShadowMap(commandList);
    return true;
}


// 其他辅助函数CreateSRVHeap/CreateShadowMap/CreateInputSRVs的实现部分

void LightPass::Render(ID3D12GraphicsCommandList* commandList,
    ID3D12PipelineState* pso,
    ID3D12RootSignature* rootSignature,
    ID3D12Resource* rt0,
    ID3D12Resource* rt1,
    ID3D12Resource* rt2) {
    // 为输入的RT创建SRV
    CreateInputSRVs(commandList, rt0, rt1, rt2);

    // 设置根签名和PSO
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pso);

    // 绑定Scene的常量缓冲区（与ScenePSO使用相同的CBV）
    // 注意：根参数0与Scene的根签名保持一致，对应b0）
    commandList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());

    // 绑定SRV描述符堆
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);

    // 绑定SRV描述符表（根参数1，与Scene根签名一致）
    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

    // 阴影图状态转换到渲染目标（这里可能不需要，视实际逻辑调整）
    D3D12_RESOURCE_BARRIER barrier = InitResourceBarrier(
        m_shadowMap.Get(),
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_RENDER_TARGET
    );
    commandList->ResourceBarrier(1, &barrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);


    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // 使用共享全屏四边形VB
    D3D12_VERTEX_BUFFER_VIEW vbv;
    GetSharedFullscreenQuadVB(vbv);
    commandList->IASetVertexBuffers(0, 1, &vbv);
    commandList->DrawInstanced(6, 1, 0, 0);

    barrier = InitResourceBarrier(
        m_shadowMap.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COMMON
    );
    commandList->ResourceBarrier(1, &barrier);
}


// 使用共享全屏PSO创建
ID3D12PipelineState* LightPass::CreateLightPSO(ID3D12RootSignature* inID3D12RootSignature,
    D3D12_SHADER_BYTECODE inVertexShader,
    D3D12_SHADER_BYTECODE inPixelShader) {
    if (!gD3D12Device || !inID3D12RootSignature) {
        OutputDebugStringA("Invalid device or root signature for LightPass PSO creation!\n");
        return nullptr;
    }

    ID3D12PipelineState* pso = CreateFullscreenPSO(inID3D12RootSignature,
        inVertexShader, inPixelShader, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (pso) {
        pso->SetName(L"LightPass_PipelineState");
    }
    return pso;
}
void LightPass::CreateSRVHeap() {
    // 创建描述符堆用于3个SRV（输入纹理）
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 3; // 3个输入RT
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create LightPass SRV heap");
    }

    m_srvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void LightPass::CreateShadowMap(ID3D12GraphicsCommandList* commandList) {
    // 创建阴影图资源
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_FLOAT; // 单通道阴影图
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr,
        IID_PPV_ARGS(&m_shadowMap)
    );
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create shadow map resource");
    }

    // 创建RTV堆
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    hr = gD3D12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create shadow map RTV heap");
    }

    m_rtvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // 创建RTV
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gD3D12Device->CreateRenderTargetView(m_shadowMap.Get(), nullptr, rtvHandle);
}

void LightPass::CreateInputSRVs(ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* rt0,
    ID3D12Resource* rt1,
    ID3D12Resource* rt2) {
    // 为三个输入RT创建SRV
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    // RT0 SRV (可能是位置或颜色)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    gD3D12Device->CreateShaderResourceView(rt0, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // RT1 SRV (可能是法线或材质)
    gD3D12Device->CreateShaderResourceView(rt1, &srvDesc, srvHandle);
    srvHandle.Offset(1, m_srvDescriptorSize);

    // RT2 SRV (可能是Albedo或其他)
    gD3D12Device->CreateShaderResourceView(rt2, &srvDesc, srvHandle);
}

// 分辨率变更时重新创建资源
bool LightPass::Resize(int newWidth, int newHeight) {
    if (newWidth == m_width && newHeight == m_height) {
        return true; // 没有变化
    }

    // 等待GPU完成所有工作
    WaitForCompletionOfCommandList();

    // 释放旧的阴影贴图
    if (m_shadowMap) {
        m_shadowMap.Reset();
    }

    // 更新尺寸
    m_width = newWidth;
    m_height = newHeight;

    // 重新创建阴影贴图资源
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr,
        IID_PPV_ARGS(&m_shadowMap)
    );
    if (FAILED(hr)) {
        return false;
    }

    // 重新创建RTV
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gD3D12Device->CreateRenderTargetView(m_shadowMap.Get(), nullptr, rtvHandle);

    return true;
}
