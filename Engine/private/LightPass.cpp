// lightpass.cpp
#include "public/LightPass.h"
#include <DirectXMath.h>
#include <stdexcept>
#include <d3dx12.h>

using namespace DirectX;

LightPass::LightPass(int width, int height)
    : m_width(width), m_height(height), m_rtvDescriptorSize(0), m_srvDescriptorSize(0) {
}

LightPass::~LightPass() {
    // ComPtr自动释放资源
}

bool LightPass::Initialize(ID3D12GraphicsCommandList* commandList) {
    CreateSRVHeap();
    CreateLightRT(commandList);
    return true;
}

void LightPass::CreateSRVHeap() {
    // 创建描述符堆用于5个SRV（3个GBuffer + 深度 + ShadowMap）
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 5;  // t0-t4: Albedo, Normal, ORM, Depth, ShadowMap
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create LightPass SRV heap");
    }

    m_srvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void LightPass::CreateLightRT(ID3D12GraphicsCommandList* commandList) {
    // 创建Light Pass输出RT（光照结果）
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 1.0f;

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,  // 初始状态为SRV，与Render函数一致
        &clearValue,
        IID_PPV_ARGS(&m_lightRT)
    );
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create light RT resource");
    }

    m_lightRT->SetName(L"LightPass_RT");

    // 创建RTV堆
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    hr = gD3D12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create light RT RTV heap");
    }

    m_rtvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // 创建RTV
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gD3D12Device->CreateRenderTargetView(m_lightRT.Get(), nullptr, rtvHandle);
}

void LightPass::CreateInputSRVs(ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* rt0,
    ID3D12Resource* rt1,
    ID3D12Resource* rt2,
    ID3D12Resource* depthBuffer,
    ID3D12Resource* shadowMap) {

    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    // t0: Albedo (RT0)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    if (rt0) {
        srvDesc.Format = rt0->GetDesc().Format;
        gD3D12Device->CreateShaderResourceView(rt0, &srvDesc, srvHandle);
    }
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t1: Normal (RT1)
    if (rt1) {
        srvDesc.Format = rt1->GetDesc().Format;
        gD3D12Device->CreateShaderResourceView(rt1, &srvDesc, srvHandle);
    }
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t2: MetallicRoughness (RT2)
    if (rt2) {
        srvDesc.Format = rt2->GetDesc().Format;
        gD3D12Device->CreateShaderResourceView(rt2, &srvDesc, srvHandle);
    }
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t3: Depth Buffer
    if (depthBuffer) {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;
        depthSrvDesc.Texture2D.MostDetailedMip = 0;
        // 深度缓冲格式转换：D24_UNORM_S8_UINT -> R24_UNORM_X8_TYPELESS 或 D32_FLOAT -> R32_FLOAT
        DXGI_FORMAT depthFormat = depthBuffer->GetDesc().Format;
        if (depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT || depthFormat == DXGI_FORMAT_R24G8_TYPELESS) {
            depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        } else if (depthFormat == DXGI_FORMAT_D32_FLOAT || depthFormat == DXGI_FORMAT_R32_TYPELESS) {
            depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        } else {
            depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;  // 默认
        }
        gD3D12Device->CreateShaderResourceView(depthBuffer, &depthSrvDesc, srvHandle);
    }
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t4: Shadow Map
    if (shadowMap) {
        D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
        shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadowSrvDesc.Texture2D.MipLevels = 1;
        shadowSrvDesc.Texture2D.MostDetailedMip = 0;
        // Shadow Map格式：R32_TYPELESS -> R32_FLOAT
        DXGI_FORMAT shadowFormat = shadowMap->GetDesc().Format;
        if (shadowFormat == DXGI_FORMAT_R32_TYPELESS) {
            shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        } else if (shadowFormat == DXGI_FORMAT_D32_FLOAT) {
            shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        } else {
            shadowSrvDesc.Format = shadowFormat;
        }
        gD3D12Device->CreateShaderResourceView(shadowMap, &shadowSrvDesc, srvHandle);
    }
}

// 新版本Render（带深度和Shadow Map）
void LightPass::Render(ID3D12GraphicsCommandList* commandList,
    ID3D12PipelineState* pso,
    ID3D12RootSignature* rootSignature,
    ID3D12Resource* rt0,
    ID3D12Resource* rt1,
    ID3D12Resource* rt2,
    ID3D12Resource* depthBuffer,
    ID3D12Resource* shadowMap) {

    // 为输入的RT创建SRV
    CreateInputSRVs(commandList, rt0, rt1, rt2, depthBuffer, shadowMap);

    // 设置根签名和PSO
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pso);

    // 绑定Scene的常量缓冲区
    if (m_sceneConstantBuffer) {
        commandList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
    }

    // 绑定SRV描述符堆
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);

    // 绑定SRV描述符表
    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

    // Light RT状态转换到渲染目标
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_lightRT.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET
    );
    commandList->ResourceBarrier(1, &barrier);

    // 设置渲染目标
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // 清除RT
    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // 设置视口和裁剪矩形
    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, m_width, m_height };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);

    // 绘制全屏四边形
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv;
    GetSharedFullscreenQuadVB(vbv);
    commandList->IASetVertexBuffers(0, 1, &vbv);
    commandList->DrawInstanced(6, 1, 0, 0);

    // Light RT状态转换回着色器资源
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_lightRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    commandList->ResourceBarrier(1, &barrier);
}

// 旧版本兼容（不带Shadow Map）
void LightPass::Render(ID3D12GraphicsCommandList* commandList,
    ID3D12PipelineState* pso,
    ID3D12RootSignature* rootSignature,
    ID3D12Resource* rt0,
    ID3D12Resource* rt1,
    ID3D12Resource* rt2) {
    // 调用新版本，深度和Shadow Map传nullptr
    Render(commandList, pso, rootSignature, rt0, rt1, rt2, nullptr, nullptr);
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

// 分辨率变更时重新创建资源
bool LightPass::Resize(int newWidth, int newHeight) {
    if (newWidth == m_width && newHeight == m_height) {
        return true;
    }

    WaitForCompletionOfCommandList();

    m_lightRT.Reset();

    m_width = newWidth;
    m_height = newHeight;

    // 重新创建Light RT
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,  // 初始状态为SRV，与Render函数一致
        &clearValue,
        IID_PPV_ARGS(&m_lightRT)
    );
    if (FAILED(hr)) {
        return false;
    }

    m_lightRT->SetName(L"LightPass_RT");

    // 重新创建RTV
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gD3D12Device->CreateRenderTargetView(m_lightRT.Get(), nullptr, rtvHandle);

    return true;
}
