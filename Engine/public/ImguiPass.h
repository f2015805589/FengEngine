#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <stdio.h>

extern ID3D12Device* gD3D12Device;

// ������Ⱦ����״̬����
// inID3D12RootSignature: ��ǩ��
// inVertexShader: ������ɫ���ֽ���
// inPixelShader: ������ɫ���ֽ���
ID3D12PipelineState* CreateUiPSO(ID3D12RootSignature* inID3D12RootSignature,
    D3D12_SHADER_BYTECODE inVertexShader, D3D12_SHADER_BYTECODE inPixelShader);