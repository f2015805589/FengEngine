#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <stdio.h>

extern ID3D12Device* gD3D12Device;

// 创建渲染管线状态对象
// inID3D12RootSignature: 根签名
// inVertexShader: 顶点着色器字节码
// inPixelShader: 像素着色器字节码
ID3D12PipelineState* CreateUiPSO(ID3D12RootSignature* inID3D12RootSignature,
    D3D12_SHADER_BYTECODE inVertexShader, D3D12_SHADER_BYTECODE inPixelShader);