// lightpass.cpp
#include "public/LightPass.h"
#include "public/Scene.h"
#include "public/Actor.h"
#include "public/StaticMeshComponent.h"
#include <DirectXMath.h>
#include <stdexcept>
#include <d3dx12.h>

using namespace DirectX;

LightPass::LightPass(int width, int height, int shadowMapSize)
    : m_width(width), m_height(height), m_shadowMapSize(shadowMapSize)
    , m_rtvDescriptorSize(0), m_srvDescriptorSize(0), m_dsvDescriptorSize(0) {
}

LightPass::~LightPass() {
    // ComPtr自动释放资源
}

bool LightPass::Initialize(ID3D12GraphicsCommandList* commandList) {
    try {
        CreateSRVHeap();
        CreateLightRT(commandList);
        CreateShadowMapResource();
        return true;
    }
    catch (const std::exception& e) {
        OutputDebugStringA("LightPass::Initialize failed: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        return false;
    }
}

void LightPass::CreateSRVHeap() {
    // 创建描述符堆用于2个SRV（Depth + ShadowMap）
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 2;  // t0: Depth, t1: ShadowMap
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
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
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

void LightPass::CreateShadowMapResource() {
    // 创建DSV描述符堆
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create LightPass DSV heap");
    }

    m_dsvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // 创建Shadow Map深度资源
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = m_shadowMapSize;
    texDesc.Height = m_shadowMapSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    hr = gD3D12Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        IID_PPV_ARGS(&m_shadowMap)
    );

    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create Shadow Map resource");
    }

    m_shadowMap->SetName(L"LightPass_ShadowMap");

    // 创建DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

    gD3D12Device->CreateDepthStencilView(
        m_shadowMap.Get(),
        &dsvDesc,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart()
    );
}

void LightPass::CreateInputSRVs(ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* depthBuffer) {

    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    // t0: Depth Buffer
    if (depthBuffer) {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;
        depthSrvDesc.Texture2D.MostDetailedMip = 0;
        DXGI_FORMAT depthFormat = depthBuffer->GetDesc().Format;
        if (depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT || depthFormat == DXGI_FORMAT_R24G8_TYPELESS) {
            depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        } else if (depthFormat == DXGI_FORMAT_D32_FLOAT || depthFormat == DXGI_FORMAT_R32_TYPELESS) {
            depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        } else {
            depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        }
        gD3D12Device->CreateShaderResourceView(depthBuffer, &depthSrvDesc, srvHandle);
    }
    srvHandle.Offset(1, m_srvDescriptorSize);

    // t1: Shadow Map
    D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
    shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowSrvDesc.Texture2D.MipLevels = 1;
    shadowSrvDesc.Texture2D.MostDetailedMip = 0;
    shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    gD3D12Device->CreateShaderResourceView(m_shadowMap.Get(), &shadowSrvDesc, srvHandle);
}

void LightPass::RenderShadowMap(ID3D12GraphicsCommandList* commandList,
    ID3D12PipelineState* pso,
    ID3D12RootSignature* rootSignature,
    Scene* scene) {

    // 设置视口和裁剪矩形
    D3D12_VIEWPORT viewport = {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_shadowMapSize);
    viewport.Height = static_cast<float>(m_shadowMapSize);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissorRect = {};
    scissorRect.left = 0;
    scissorRect.top = 0;
    scissorRect.right = m_shadowMapSize;
    scissorRect.bottom = m_shadowMapSize;
    commandList->RSSetScissorRects(1, &scissorRect);

    // 清除深度缓冲
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // 设置渲染目标（只有深度，无颜色）
    commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    // 设置根签名和PSO
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pso);

    // 绑定场景常量缓冲区
    if (m_sceneConstantBuffer) {
        commandList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
    }

    // 设置图元拓扑
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 渲染所有Actor
    std::vector<Actor*>& actors = scene->GetActors();
    for (Actor* actor : actors) {
        if (!actor) continue;

        StaticMeshComponent* mesh = actor->GetMesh();
        if (!mesh) continue;

        // 绑定Actor的常量缓冲区
        ID3D12Resource* actorCB = actor->GetConstantBuffer();
        if (actorCB) {
            commandList->SetGraphicsRootConstantBufferView(0, actorCB->GetGPUVirtualAddress());
        }

        // 渲染mesh
        mesh->Render(commandList, rootSignature);
    }

    // 转换Shadow Map状态为着色器资源
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_shadowMap.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    commandList->ResourceBarrier(1, &barrier);
}

void LightPass::RenderLighting(ID3D12GraphicsCommandList* commandList,
    ID3D12PipelineState* pso,
    ID3D12RootSignature* rootSignature,
    ID3D12Resource* depthBuffer) {

    // 创建SRV
    CreateInputSRVs(commandList, depthBuffer);

    // 设置根签名和PSO
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pso);

    // 绑定常量缓冲区
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

void LightPass::RenderDirectLight(ID3D12GraphicsCommandList* commandList,
    ID3D12PipelineState* shadowPso,
    ID3D12PipelineState* lightPso,
    ID3D12RootSignature* rootSignature,
    Scene* scene,
    ID3D12Resource* depthBuffer) {

    if (!commandList || !shadowPso || !lightPso || !rootSignature || !scene) {
        return;
    }

    // 子pass 1: 渲染shadow map（从光源视角渲染场景深度）
    RenderShadowMap(commandList, shadowPso, rootSignature, scene);

    // 子pass 2: 计算光照（使用shadow map）
    RenderLighting(commandList, lightPso, rootSignature, depthBuffer);

    // 将Shadow Map状态转回DEPTH_WRITE，为下一帧准备
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_shadowMap.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE
    );
    commandList->ResourceBarrier(1, &barrier);
}

ID3D12PipelineState* LightPass::CreateShadowPSO(ID3D12RootSignature* rootSignature,
    D3D12_SHADER_BYTECODE vertexShader,
    D3D12_SHADER_BYTECODE pixelShader) {

    if (!gD3D12Device || !rootSignature) {
        OutputDebugStringA("LightPass::CreateShadowPSO - Invalid device or root signature\n");
        return nullptr;
    }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = rootSignature;
    psoDesc.VS = vertexShader;
    psoDesc.PS = pixelShader;

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;  // 渲染背面，减少自阴影伪影
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthBias = 5000;
    psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 2.0f;

    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = gD3D12Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        OutputDebugStringA("LightPass::CreateShadowPSO - Failed to create PSO\n");
        return nullptr;
    }

    pso->SetName(L"LightPass_ShadowPSO");
    return pso;
}

ID3D12PipelineState* LightPass::CreateLightPSO(ID3D12RootSignature* rootSignature,
    D3D12_SHADER_BYTECODE vertexShader,
    D3D12_SHADER_BYTECODE pixelShader) {

    if (!gD3D12Device || !rootSignature) {
        OutputDebugStringA("Invalid device or root signature for LightPass PSO creation!\n");
        return nullptr;
    }

    ID3D12PipelineState* pso = CreateFullscreenPSO(rootSignature,
        vertexShader, pixelShader, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (pso) {
        pso->SetName(L"LightPass_LightPSO");
    }
    return pso;
}

bool LightPass::Resize(int newWidth, int newHeight) {
    if (newWidth == m_width && newHeight == m_height) {
        return true;
    }

    WaitForCompletionOfCommandList();

    m_lightRT.Reset();

    m_width = newWidth;
    m_height = newHeight;

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
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clearValue,
        IID_PPV_ARGS(&m_lightRT)
    );
    if (FAILED(hr)) {
        return false;
    }

    m_lightRT->SetName(L"LightPass_RT");

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gD3D12Device->CreateRenderTargetView(m_lightRT.Get(), nullptr, rtvHandle);

    return true;
}

bool LightPass::ResizeShadowMap(int newSize) {
    if (newSize == m_shadowMapSize) {
        return true;
    }

    WaitForCompletionOfCommandList();

    m_shadowMap.Reset();
    m_shadowMapSize = newSize;

    try {
        // 重新创建Shadow Map（不需要重新创建DSV堆）
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Alignment = 0;
        texDesc.Width = m_shadowMapSize;
        texDesc.Height = m_shadowMapSize;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;

        D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

        HRESULT hr = gD3D12Device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            IID_PPV_ARGS(&m_shadowMap)
        );

        if (FAILED(hr)) {
            return false;
        }

        m_shadowMap->SetName(L"LightPass_ShadowMap");

        // 重新创建DSV
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Texture2D.MipSlice = 0;
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

        gD3D12Device->CreateDepthStencilView(
            m_shadowMap.Get(),
            &dsvDesc,
            m_dsvHeap->GetCPUDescriptorHandleForHeapStart()
        );

        return true;
    }
    catch (const std::exception& e) {
        OutputDebugStringA("LightPass::ResizeShadowMap failed: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        return false;
    }
}
