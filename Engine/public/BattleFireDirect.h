#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <stdio.h>

extern ID3D12Device* gD3D12Device;
extern ID3D12CommandQueue* gCommandQueue;

// 交换链RTV堆和相关变量
extern ID3D12DescriptorHeap* gSwapChainRTVHeap;
extern UINT gRTVDescriptorSize;
extern int gCurrentRTIndex;

// 深度缓冲资源（Depth Stencil Resource）
extern ID3D12Resource* gDSRT;
extern ID3D12DescriptorHeap* gSwapChainDSVHeap;

// ImGui使用的描述符堆，用于着色器资源视图(SRV)
extern ID3D12DescriptorHeap* gImGuiDescriptorHeap;
extern void ShutdownImGui();

// 初始化资源屏障，用于资源状态转换
D3D12_RESOURCE_BARRIER InitResourceBarrier(
    ID3D12Resource* inResource, D3D12_RESOURCE_STATES inPrevState,
    D3D12_RESOURCE_STATES inNextState);

// 初始化根签名，定义GPU可访问的资源
ID3D12RootSignature* InitRootSignature();

// 从文件创建着色器
// inShaderFilePath: 着色器文件路径
// inMainFunctionName: 着色器入口函数名
// inTarget: 着色器目标版本，如"vs_5_0","ps_5_0","vs_4_0"
// inShader: 输出的着色器字节码
void CreateShaderFromFile(
    LPCTSTR inShaderFilePath,
    const char* inMainFunctionName,
    const char* inTarget,
    D3D12_SHADER_BYTECODE* inShader);

// 创建常量缓冲区对象
// inDataLen: 缓冲区数据长度
ID3D12Resource* CreateConstantBufferObject(int inDataLen);

// 更新常量缓冲区数据
// inCB: 常量缓冲区
// inData: 要更新的数据
// inDataLen: 数据长度
void UpdateConstantBuffer(ID3D12Resource* inCB, void* inData, int inDataLen);

// 创建缓冲区对象并复制数据
// inCommandList: 命令列表
// inData: 要复制的数据
// inDataLen: 数据长度
// inFinalResourceState: 最终资源状态
ID3D12Resource* CreateBufferObject(ID3D12GraphicsCommandList* inCommandList,
    void* inData, int inDataLen, D3D12_RESOURCE_STATES inFinalResourceState);

// 创建渲染管线状态对象
// inID3D12RootSignature: 根签名
// inVertexShader: 顶点着色器字节码
// inPixelShader: 像素着色器字节码
ID3D12PipelineState* CreateScenePSO(ID3D12RootSignature* inID3D12RootSignature,
    D3D12_SHADER_BYTECODE inVertexShader, D3D12_SHADER_BYTECODE inPixelShader);

// 初始化D3D12设备和相关组件
// inHWND: 窗口句柄
// inWidth: 渲染宽度
// inHeight: 渲染高度
bool InitD3D12(HWND inHWND, int inWidth, int inHeight);

// 获取命令列表
ID3D12GraphicsCommandList* GetCommandList();

// 获取命令分配器
ID3D12CommandAllocator* GetCommandAllocator();

// 等待命令列表执行完成
void WaitForCompletionOfCommandList();

// 刷新GPU命令队列（用于分辨率变更等需要完全同步的场景）
void FlushGPU();

// 结束命令列表并执行
void EndCommandList();

void BeginOffscreen(ID3D12GraphicsCommandList* commandList);

// 开始渲染到交换链
// inCommandList: 命令列表
// isClear: 是否清除渲染目标
// bindDepth: 是否绑定深度缓冲（默认true）
void BeginRenderToSwapChain(ID3D12GraphicsCommandList* inCommandList, bool isClear, bool bindDepth = true);

// 结束渲染到交换链
// inCommandList: 命令列表
void EndRenderToSwapChain(ID3D12GraphicsCommandList* inCommandList);

// 交换缓冲区，显示渲染结果
void SwapD3D12Buffers();

// 初始化ImGui，需要在D3D12初始化之后调用
// hWnd: 窗口句柄
// device: D3D12设备
// srvHeap: SRV描述符堆
// srvDescriptorSize: SRV描述符大小
void InitImGui(HWND hWnd, ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT srvDescriptorSize);

// 分辨率调整函数
// 调整交换链和深度缓冲区大小
// resizeWindow: 是否同时调整窗口大小（从UI选择分辨率时为true，窗口拖动时为false）
// 返回 true 表示成功，false 表示失败
bool ResizeSwapChainAndDepthBuffer(int newWidth, int newHeight, bool resizeWindow = true);

// 获取当前渲染分辨率
int GetRenderWidth();
int GetRenderHeight();

// 获取当前交换链RTV句柄
D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentSwapChainRTV();

// ========== 共享工具函数 ==========

// 获取共享的全屏四边形顶点缓冲（Position3 + UV2，6个顶点）
// 全局只创建一次，所有全屏Pass共享
ID3D12Resource* GetSharedFullscreenQuadVB(D3D12_VERTEX_BUFFER_VIEW& outVBV);

// 创建全屏Pass通用PSO（禁用深度、CullNone、Position3+UV2输入布局）
ID3D12PipelineState* CreateFullscreenPSO(
    ID3D12RootSignature* rootSig,
    D3D12_SHADER_BYTECODE vs,
    D3D12_SHADER_BYTECODE ps,
    DXGI_FORMAT rtvFormat,
    bool enableAlphaBlend = false);

// ========== 共享 CB 结构体 ==========

// 场景常量缓冲区数据布局（176 floats = 704 bytes）
// Scene::Update() 和 Actor::UpdateConstantBuffer() 共用
struct SceneCBData {
    DirectX::XMFLOAT4X4 projMatrix;           // [0-15]
    DirectX::XMFLOAT4X4 viewMatrix;           // [16-31]
    DirectX::XMFLOAT4X4 modelMatrix;          // [32-47]
    DirectX::XMFLOAT4X4 normalMatrix;         // [48-63]
    DirectX::XMFLOAT4   lightDirection;       // [64-67]
    DirectX::XMFLOAT4   cameraPosition;       // [68-71]
    DirectX::XMFLOAT4   skylightParams;       // [72-75] x=intensity, yzw=padding
    DirectX::XMFLOAT4X4 invProjMatrix;        // [76-91]
    DirectX::XMFLOAT4X4 invViewMatrix;        // [92-107]
    DirectX::XMFLOAT4   skylightColor;        // [108-111] xyz=color, w=padding
    DirectX::XMFLOAT4X4 lightViewProjMatrix;  // [112-127]
    DirectX::XMFLOAT4X4 prevViewProjMatrix;   // [128-143]
    DirectX::XMFLOAT4   jitterParams;         // [144-147] xy=current, zw=previous
    DirectX::XMFLOAT4   screenParams;         // [148-151] xy=size, zw=invSize
    DirectX::XMFLOAT4   nearFarParams;        // [152-155] x=near, y=far, zw=padding
    DirectX::XMFLOAT4X4 currentViewProjMatrix;// [156-171]
    float shadowMode;                          // [172] 0=Hard, 1=PCF, 2=PCSS
    float padding[3];                          // [173-175]
};

// 填充 SceneCBData（共享逻辑，Scene和Actor都调用）
void FillSceneCBData(SceneCBData& out,
    const DirectX::XMMATRIX& viewMatrix,
    const DirectX::XMMATRIX& projMatrix,
    const DirectX::XMMATRIX& modelMatrix,
    const DirectX::XMFLOAT3& lightDir,
    const DirectX::XMFLOAT3& cameraPos,
    float skylightIntensity,
    const DirectX::XMFLOAT3& skylightColor,
    const DirectX::XMMATRIX& invProjMatrix,
    const DirectX::XMMATRIX& invViewMatrix,
    const DirectX::XMMATRIX& lightViewProjMatrix,
    const DirectX::XMMATRIX& previousViewProjMatrix,
    const DirectX::XMFLOAT2& jitterOffset,
    const DirectX::XMFLOAT2& previousJitterOffset,
    int viewportWidth, int viewportHeight,
    float nearPlane, float farPlane,
    const DirectX::XMMATRIX& currentViewProjMatrix,
    int shadowMode = 2,
    int giType = 0);
