// ShadowPass.cpp
// Shadow Map 深度渲染实现
#include "public/ShadowPass.h"
#include "public/Scene.h"
#include "public/Actor.h"
#include "public/StaticMeshComponent.h"
#include <d3dx12.h>
#include <stdexcept>

ShadowPass::ShadowPass(int shadowMapSize)
    : m_shadowMapSize(shadowMapSize)
    , m_dsvDescriptorSize(0)
    , m_srvDescriptorSize(0) {
}

ShadowPass::~ShadowPass() {
    // ComPtr自动释放资源
}

bool ShadowPass::Initialize() {
    try {
        CreateDescriptorHeaps();
        CreateShadowMapResource();
        return true;
    }
    catch (const std::exception& e) {
        OutputDebugStringA("ShadowPass::Initialize failed: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        return false;
    }
}

void ShadowPass::CreateDescriptorHeaps() {
    // 创建DSV描述符堆（用于深度写入）
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create ShadowPass DSV heap");
    }

    m_dsvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // 创建SRV描述符堆（用于后续采样，需要SHADER_VISIBLE）
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    hr = gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create ShadowPass SRV heap");
    }

    m_srvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void ShadowPass::CreateShadowMapResource() {
    // 创建Shadow Map深度资源
    // 使用D24_UNORM_S8_UINT格式，可以同时作为DSV和SRV使用
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = m_shadowMapSize;
    texDesc.Height = m_shadowMapSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;  // Typeless以便同时创建DSV和SRV
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
        throw std::runtime_error("Failed to create Shadow Map resource");
    }

    m_shadowMap->SetName(L"ShadowMap");

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

    // 创建SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;  // 作为SRV时使用R32_FLOAT
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    gD3D12Device->CreateShaderResourceView(
        m_shadowMap.Get(),
        &srvDesc,
        m_srvHeap->GetCPUDescriptorHandleForHeapStart()
    );
}

D3D12_GPU_DESCRIPTOR_HANDLE ShadowPass::GetShadowMapSRV() const {
    return m_srvHeap->GetGPUDescriptorHandleForHeapStart();
}

ID3D12PipelineState* ShadowPass::CreateShadowPSO(ID3D12RootSignature* rootSignature,
                                                   D3D12_SHADER_BYTECODE vertexShader,
                                                   D3D12_SHADER_BYTECODE pixelShader) {
    if (!gD3D12Device || !rootSignature) {
        OutputDebugStringA("ShadowPass::CreateShadowPSO - Invalid device or root signature\n");
        return nullptr;
    }

    // 输入布局（与场景mesh一致）
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

    // 光栅化状态 - 关键：设置深度偏移防止Shadow Acne
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    // 深度偏移参数（防止Shadow Acne）
    psoDesc.RasterizerState.DepthBias = 100000;           // 固定偏移量
    psoDesc.RasterizerState.DepthBiasClamp = 0.0f;        // 最大深度偏移量（0表示无限制）
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;  // 根据斜率缩放的偏移

    // 混合状态
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // 深度模板状态
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // 关键：Shadow Map只输出深度，不输出颜色
    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = gD3D12Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        OutputDebugStringA("ShadowPass::CreateShadowPSO - Failed to create PSO\n");
        return nullptr;
    }

    pso->SetName(L"ShadowPass_PSO");
    return pso;
}

void ShadowPass::Render(ID3D12GraphicsCommandList* commandList,
                        ID3D12PipelineState* pso,
                        ID3D12RootSignature* rootSignature,
                        Scene* scene) {
    if (!commandList || !pso || !rootSignature || !scene) {
        return;
    }

    // 1. 设置视口和裁剪矩形（Shadow Map尺寸）
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

    // 2. 清除深度缓冲（Shadow Map已经在DEPTH_WRITE状态）
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // 3. 设置渲染目标（只有深度，无颜色）
    commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    // 4. 设置根签名和PSO
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pso);

    // 5. 绑定场景常量缓冲区（包含LightViewProjectionMatrix）
    if (m_sceneConstantBuffer) {
        commandList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
    }

    // 6. 设置图元拓扑
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 7. 渲染所有Actor
    std::vector<Actor*>& actors = scene->GetActors();
    for (Actor* actor : actors) {
        if (!actor) continue;

        StaticMeshComponent* mesh = actor->GetMesh();
        if (!mesh) continue;

        // 绑定Actor的常量缓冲区（包含ModelMatrix）
        ID3D12Resource* actorCB = actor->GetConstantBuffer();
        if (actorCB) {
            commandList->SetGraphicsRootConstantBufferView(0, actorCB->GetGPUVirtualAddress());
        }

        // 渲染mesh
        mesh->Render(commandList, rootSignature);
    }

    // 8. 转换Shadow Map状态为着色器资源（供后续Pass采样）
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_shadowMap.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    commandList->ResourceBarrier(1, &barrier);
}

bool ShadowPass::Resize(int newSize) {
    if (newSize == m_shadowMapSize) {
        return true;
    }

    m_shadowMapSize = newSize;

    // 等待GPU完成
    WaitForCompletionOfCommandList();

    // 释放旧资源
    m_shadowMap.Reset();

    // 重新创建
    try {
        CreateShadowMapResource();
        return true;
    }
    catch (const std::exception& e) {
        OutputDebugStringA("ShadowPass::Resize failed: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        return false;
    }
}
