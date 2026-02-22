// SkyPass.cpp
#include "public/SkyPass.h"
#include <d3dx12.h>
#include <DirectXMath.h>
#include <stdexcept>
#include <cmath>

using namespace DirectX;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SkyPass::~SkyPass() {
    if (m_vertexBuffer) m_vertexBuffer.Reset();
    if (m_indexBuffer) m_indexBuffer.Reset();
    if (m_srvHeap) m_srvHeap.Reset();
}

bool SkyPass::Initialize(ID3D12GraphicsCommandList* commandList, float sphereRadius) {
    // 创建SRV堆
    CreateSRVHeap();

    // 生成天空球几何体（反转法线朝内，因为从内部观看）
    GenerateSkySphere(sphereRadius, 32, 16);

    // 创建顶点缓冲
    const UINT vertexBufferSize = static_cast<UINT>(m_vertices.size() * sizeof(SkyVertex));

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_vertexBuffer)
    );
    if (FAILED(hr)) {
        return false;
    }

    // 复制顶点数据
    UINT8* pData;
    CD3DX12_RANGE readRange(0, 0);
    m_vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pData));
    memcpy(pData, m_vertices.data(), vertexBufferSize);
    m_vertexBuffer->Unmap(0, nullptr);

    // 设置顶点缓冲视图
    m_vbv.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbv.StrideInBytes = sizeof(SkyVertex);
    m_vbv.SizeInBytes = vertexBufferSize;

    // 创建索引缓冲
    const UINT indexBufferSize = static_cast<UINT>(m_indices.size() * sizeof(UINT));
    m_indexCount = static_cast<UINT>(m_indices.size());

    bufDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);

    hr = gD3D12Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_indexBuffer)
    );
    if (FAILED(hr)) {
        return false;
    }

    // 复制索引数据
    m_indexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pData));
    memcpy(pData, m_indices.data(), indexBufferSize);
    m_indexBuffer->Unmap(0, nullptr);

    // 设置索引缓冲视图
    m_ibv.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibv.Format = DXGI_FORMAT_R32_UINT;
    m_ibv.SizeInBytes = indexBufferSize;

    return true;
}

void SkyPass::CreateSRVHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;  // 只需要1个SRV（SkyCube）
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        throw std::runtime_error("SkyPass: Failed to create SRV heap");
    }

    m_srvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void SkyPass::CreateSkyCubeSRV(ComPtr<ID3D12Resource> skyTexture) {
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = skyTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = skyTexture->GetDesc().MipLevels;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

    gD3D12Device->CreateShaderResourceView(skyTexture.Get(), &srvDesc, srvHandle);
}

void SkyPass::GenerateSkySphere(float radius, int slices, int stacks) {
    m_vertices.clear();
    m_indices.clear();

    // 生成球体顶点
    for (int stack = 0; stack <= stacks; ++stack) {
        float phi = static_cast<float>(M_PI) * static_cast<float>(stack) / static_cast<float>(stacks);
        float sinPhi = sinf(phi);
        float cosPhi = cosf(phi);

        for (int slice = 0; slice <= slices; ++slice) {
            float theta = 2.0f * static_cast<float>(M_PI) * static_cast<float>(slice) / static_cast<float>(slices);
            float sinTheta = sinf(theta);
            float cosTheta = cosf(theta);

            SkyVertex vertex;
            // 生成局部坐标（相对于球心）
            vertex.position[0] = radius * sinPhi * cosTheta;
            vertex.position[1] = radius * cosPhi;
            vertex.position[2] = radius * sinPhi * sinTheta;

            m_vertices.push_back(vertex);
        }
    }

    // 生成索引（背面剔除关闭，所以顺序不重要，但我们反转三角形顺序以从内部观看）
    for (int stack = 0; stack < stacks; ++stack) {
        for (int slice = 0; slice < slices; ++slice) {
            int first = stack * (slices + 1) + slice;
            int second = first + slices + 1;

            // 第一个三角形（反转顺序以从内部观看）
            m_indices.push_back(first);
            m_indices.push_back(first + 1);
            m_indices.push_back(second);

            // 第二个三角形（反转顺序以从内部观看）
            m_indices.push_back(second);
            m_indices.push_back(first + 1);
            m_indices.push_back(second + 1);
        }
    }
}

void SkyPass::Render(ID3D12GraphicsCommandList* cmdList,
    ID3D12PipelineState* pso,
    ID3D12RootSignature* rootSig,
    ComPtr<ID3D12Resource> skyTexture) {
    // 为天空立方体贴图创建SRV
    CreateSkyCubeSRV(skyTexture);

    // 设置根签名和PSO
    cmdList->SetGraphicsRootSignature(rootSig);
    cmdList->SetPipelineState(pso);

    // 绑定场景常量缓冲区（根参数0，包含矩阵和相机位置）
    if (m_sceneConstantBuffer) {
        cmdList->SetGraphicsRootConstantBufferView(0, m_sceneConstantBuffer->GetGPUVirtualAddress());
    }

    // 绑定SRV堆
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    // 绑定SRV描述符表（根参数1）
    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

    // 设置顶点和索引缓冲
    cmdList->IASetVertexBuffers(0, 1, &m_vbv);
    cmdList->IASetIndexBuffer(&m_ibv);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 绘制天空球
    cmdList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}

ID3D12PipelineState* SkyPass::CreatePSO(ID3D12RootSignature* rootSig,
    D3D12_SHADER_BYTECODE vs,
    D3D12_SHADER_BYTECODE ps) {
    // 输入布局：只有位置
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = rootSig;
    psoDesc.VS = vs;
    psoDesc.PS = ps;

    // 光栅化状态：关闭背面剔除（从球体内部观看）
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // 禁用深度测试（天空球先画，之后 ScreenPass 会用 alpha 混合覆盖场景部分）
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;  // 与交换链格式匹配
    psoDesc.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = gD3D12Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        return nullptr;
    }

    pso->SetName(L"SkyPass_PipelineState");
    return pso;
}
