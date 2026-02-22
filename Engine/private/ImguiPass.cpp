#include "public\ImguiPass.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <d3dx12.h>
#include <array>
#include <wrl.h>

ID3D12PipelineState* CreateUiPSO(ID3D12RootSignature* inID3D12RootSignature,
    D3D12_SHADER_BYTECODE inVertexShader, D3D12_SHADER_BYTECODE inPixelShader) {
    // 顶点数据元素描述数组
    D3D12_INPUT_ELEMENT_DESC vertexDataElementDesc[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,sizeof(float) * 4,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,sizeof(float) * 8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,sizeof(float) * 12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}
    };

    // 顶点数据布局描述
    D3D12_INPUT_LAYOUT_DESC vertexDataLayoutDesc = {};
    vertexDataLayoutDesc.NumElements = 4;
    vertexDataLayoutDesc.pInputElementDescs = vertexDataElementDesc;

    // 图形管线状态描述
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = inID3D12RootSignature;
    psoDesc.VS = inVertexShader;
    psoDesc.PS = inPixelShader;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    psoDesc.SampleMask = 0xffffffff;
    psoDesc.InputLayout = vertexDataLayoutDesc;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; // 图元拓扑类型为三角形

    // 光栅化器状态设置
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // 深度模板状态设置
    psoDesc.DepthStencilState.DepthEnable = false;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // 混合状态设置
    psoDesc.BlendState = { 0 };
    D3D12_RENDER_TARGET_BLEND_DESC rtBlendDesc = {
        TRUE,FALSE,
        D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD, // Alpha混合
        D3D12_BLEND_ONE,D3D12_BLEND_ZERO,D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL,
    };
    for (int i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        psoDesc.BlendState.RenderTarget[i] = rtBlendDesc;
    psoDesc.NumRenderTargets = 1;

    // 创建管线状态对象
    ID3D12PipelineState* d3d12PSO = nullptr;
    HRESULT hResult = gD3D12Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&d3d12PSO));
    if (FAILED(hResult)) {
        return nullptr;
    }
    return d3d12PSO;
}