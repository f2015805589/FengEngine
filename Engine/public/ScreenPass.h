#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include "BattleFireDirect.h"

using Microsoft::WRL::ComPtr;



class ScreenPass {
public:
    ScreenPass() = default;
    ~ScreenPass();

    // ���������ó�����������������BasePass������
    void SetSceneConstantBuffer(ID3D12Resource* sceneCB) { m_sceneConstantBuffer = sceneCB; }

    // 设置材质常量缓冲区（用于匹配root signature）
    void SetMaterialConstantBuffer(ID3D12Resource* materialCB) { m_materialConstantBuffer = materialCB; }

    // ��ʼ��ʱ��ָ���ӿڴ�С�����ڴ���SRV��
    bool Initialize(int viewportWidth, int viewportHeight);
    void Render(ID3D12GraphicsCommandList* cmdList,
        ID3D12PipelineState* pso,
        ID3D12RootSignature* rootSig,
        ID3D12Resource* rt0,    // 离屏RT0 (Albedo)
        ID3D12Resource* rt1,    // 离屏RT1 (Normal)
        ID3D12Resource* rt2,    // 离屏RT2 (ORM)
        ID3D12Resource* depthBuffer,  // 深度缓冲（用于位置重构）
        ComPtr<ID3D12Resource> skyTexture

    );

    ID3D12PipelineState* CreatePSO(ID3D12RootSignature* rootSig,
        D3D12_SHADER_BYTECODE vs,
        D3D12_SHADER_BYTECODE ps);

    // 分辨率变更时更新视口尺寸
    void Resize(int newWidth, int newHeight);
    int GetViewportWidth() const { return m_viewportWidth; }
    int GetViewportHeight() const { return m_viewportHeight; }

private:
    // ����SRV�ѣ����ڴ洢4����Դ��SRV��3������RT + 1��LightRT��
    void CreateSRVHeap();
    // 为输入资源创建SRV
    void CreateInputSRVs(ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* rt0,
        ID3D12Resource* rt1,
        ID3D12Resource* rt2,
        ID3D12Resource* depthBuffer,  // 深度缓冲
        ComPtr<ID3D12Resource> skyTexture);

    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize = 0;  // SRV��������С
    int m_viewportWidth = 0;
    int m_viewportHeight = 0;

    // �洢BasePass�ĳ������������������ݣ�
    ID3D12Resource* m_sceneConstantBuffer = nullptr;

    // 材质常量缓冲区（用于匹配root signature）
    ID3D12Resource* m_materialConstantBuffer = nullptr;
};