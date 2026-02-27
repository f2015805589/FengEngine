#include "public\BattleFireDirect.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <d3dx12.h>
#include <array>
#include <wrl.h>

ID3D12Device* gD3D12Device = nullptr;
ID3D12CommandQueue* gCommandQueue = nullptr;
IDXGISwapChain3* gSwapChain = nullptr;
ID3D12Resource* gDSRT = nullptr, * gColorRTs[2];
int gCurrentRTIndex = 0;
ID3D12DescriptorHeap* gSwapChainRTVHeap = nullptr;
ID3D12DescriptorHeap* gSwapChainDSVHeap = nullptr;
UINT gRTVDescriptorSize = 0;
UINT gDSVDescriptorSize = 0;
ID3D12CommandAllocator* gCommandAllocator = nullptr;
ID3D12GraphicsCommandList* gCommandList = nullptr;
ID3D12Fence* gFence = nullptr;
HANDLE gFenceEvent = nullptr;
UINT64 gFenceValue = 0;

// 当前渲染分辨率
int gRenderWidth = 1280;
int gRenderHeight = 720;
HWND gMainHWND = nullptr;
//����������
using Microsoft::WRL::ComPtr;
std::array<CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers()
{
    //������POINT,ѰַģʽWRAP�ľ�̬������
    CD3DX12_STATIC_SAMPLER_DESC pointWarp(0,	//��ɫ���Ĵ���
        D3D12_FILTER_MIN_MAG_MIP_POINT,		//����������ΪPOINT(������ֵ)
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//U�����ϵ�ѰַģʽΪWRAP���ظ�Ѱַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//V�����ϵ�ѰַģʽΪWRAP���ظ�Ѱַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);	//W�����ϵ�ѰַģʽΪWRAP���ظ�Ѱַģʽ��

    //������POINT,ѰַģʽCLAMP�ľ�̬������
    CD3DX12_STATIC_SAMPLER_DESC pointClamp(1,	//��ɫ���Ĵ���
        D3D12_FILTER_MIN_MAG_MIP_POINT,		//����������ΪPOINT(������ֵ)
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//U�����ϵ�ѰַģʽΪCLAMP��ǯλѰַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//V�����ϵ�ѰַģʽΪCLAMP��ǯλѰַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);	//W�����ϵ�ѰַģʽΪCLAMP��ǯλѰַģʽ��

    //������LINEAR,ѰַģʽWRAP�ľ�̬������
    CD3DX12_STATIC_SAMPLER_DESC linearWarp(2,	//��ɫ���Ĵ���
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,		//����������ΪLINEAR(���Բ�ֵ)
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//U�����ϵ�ѰַģʽΪWRAP���ظ�Ѱַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//V�����ϵ�ѰַģʽΪWRAP���ظ�Ѱַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);	//W�����ϵ�ѰַģʽΪWRAP���ظ�Ѱַģʽ��

    //������LINEAR,ѰַģʽCLAMP�ľ�̬������
    CD3DX12_STATIC_SAMPLER_DESC linearClamp(3,	//��ɫ���Ĵ���
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,		//����������ΪLINEAR(���Բ�ֵ)
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//U�����ϵ�ѰַģʽΪCLAMP��ǯλѰַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//V�����ϵ�ѰַģʽΪCLAMP��ǯλѰַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);	//W�����ϵ�ѰַģʽΪCLAMP��ǯλѰַģʽ��

    //������ANISOTROPIC,ѰַģʽWRAP�ľ�̬������
    CD3DX12_STATIC_SAMPLER_DESC anisotropicWarp(4,	//��ɫ���Ĵ���
        D3D12_FILTER_ANISOTROPIC,			//����������ΪANISOTROPIC(��������)
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//U�����ϵ�ѰַģʽΪWRAP���ظ�Ѱַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//V�����ϵ�ѰַģʽΪWRAP���ظ�Ѱַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);	//W�����ϵ�ѰַģʽΪWRAP���ظ�Ѱַģʽ��

    //������LINEAR,ѰַģʽCLAMP�ľ�̬������
    CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(5,	//��ɫ���Ĵ���
        D3D12_FILTER_ANISOTROPIC,			//����������ΪANISOTROPIC(��������)
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//U�����ϵ�ѰַģʽΪCLAMP��ǯλѰַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//V�����ϵ�ѰַģʽΪCLAMP��ǯλѰַģʽ��
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);	//W�����ϵ�ѰַģʽΪCLAMP��ǯλѰַģʽ��

    return{ pointWarp, pointClamp, linearWarp, linearClamp, anisotropicWarp, anisotropicClamp };
}
// 创建ImGui的SRV描述符堆
ID3D12DescriptorHeap* gImGuiDescriptorHeap = nullptr;

ID3D12RootSignature* InitRootSignature() {
    // 使用根签名版本1.1以支持Bindless纹理
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(gD3D12Device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData)))) {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    // SRV范围 - 使用版本1.1的描述符范围以支持Bindless
    D3D12_DESCRIPTOR_RANGE1 srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = UINT_MAX;  // 无界数组 - Bindless
    srvRange.BaseShaderRegister = 0;     // 从t0开始
    srvRange.RegisterSpace = 0;
    srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;  // Bindless需要此标志
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // 根参数
    D3D12_ROOT_PARAMETER1 rootParameters[3] = {};

    // Slot 0: Scene constant buffer (b0)
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].Descriptor.RegisterSpace = 0;
    rootParameters[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 1: Descriptor table for textures (Bindless - 无界数组)
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Slot 2: Material constant buffer (b1)
    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[2].Descriptor.ShaderRegister = 1;
    rootParameters[2].Descriptor.RegisterSpace = 0;
    rootParameters[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // 静态采样器
    auto staticSamplers = GetStaticSamplers();

    // 根签名描述 - 版本1.1
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = 3;
    rootSigDesc.Desc_1_1.pParameters = rootParameters;
    rootSigDesc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    rootSigDesc.Desc_1_1.pStaticSamplers = staticSamplers.data();
    rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* signature = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT hResult = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);

    if (FAILED(hResult)) {
        if (error) {
            OutputDebugStringA((char*)error->GetBufferPointer());
            error->Release();
        }
        // 回退到版本1.0
        D3D12_DESCRIPTOR_RANGE srvRange10 = {};
        srvRange10.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange10.NumDescriptors = 1000;  // 版本1.0不支持无界，使用大数量
        srvRange10.BaseShaderRegister = 0;
        srvRange10.RegisterSpace = 0;
        srvRange10.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        CD3DX12_ROOT_PARAMETER rootParams10[3];
        rootParams10[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
        rootParams10[1].InitAsDescriptorTable(1, &srvRange10, D3D12_SHADER_VISIBILITY_PIXEL);
        rootParams10[2].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc10(3, rootParams10,
            static_cast<UINT>(staticSamplers.size()),
            staticSamplers.data(),
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        hResult = D3D12SerializeRootSignature(&rootSigDesc10, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        if (FAILED(hResult)) {
            if (error) {
                OutputDebugStringA((char*)error->GetBufferPointer());
                error->Release();
            }
            return nullptr;
        }
    }

    ID3D12RootSignature* rootSignature = nullptr;
    gD3D12Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&rootSignature));

    if (signature) signature->Release();

    return rootSignature;
}

void CreateShaderFromFile(
    LPCTSTR inShaderFilePath,
    const char* inMainFunctionName,
    const char* inTarget,
    D3D12_SHADER_BYTECODE* inShader) {
    ID3DBlob* shaderBuffer = nullptr;
    ID3DBlob* errorBuffer = nullptr;
    HRESULT hResult = D3DCompileFromFile(inShaderFilePath, nullptr, nullptr,
        inMainFunctionName, inTarget, D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
        0, &shaderBuffer, &errorBuffer);
    if (FAILED(hResult)) {
        printf("CreateShaderFromFile error : [%s][%s]:[%s]\n", inMainFunctionName, inTarget, (char*)errorBuffer->GetBufferPointer());
        errorBuffer->Release();
        return;
    }
    inShader->pShaderBytecode = shaderBuffer->GetBufferPointer();
    inShader->BytecodeLength = shaderBuffer->GetBufferSize();
}

ID3D12Resource* CreateConstantBufferObject(int inDataLen) {
    D3D12_HEAP_PROPERTIES d3dHeapProperties = {};
    d3dHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d3d12ResourceDesc = {};
    d3d12ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d3d12ResourceDesc.Alignment = 0;
    d3d12ResourceDesc.Width = inDataLen;
    d3d12ResourceDesc.Height = 1;
    d3d12ResourceDesc.DepthOrArraySize = 1;
    d3d12ResourceDesc.MipLevels = 1;
    d3d12ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    d3d12ResourceDesc.SampleDesc.Count = 1;
    d3d12ResourceDesc.SampleDesc.Quality = 0;
    d3d12ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d3d12ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* bufferObject = nullptr;
    gD3D12Device->CreateCommittedResource(
        &d3dHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &d3d12ResourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&bufferObject)
    );
    return bufferObject;
}

void UpdateConstantBuffer(ID3D12Resource* inCB, void* inData, int inDataLen) {
    D3D12_RANGE readRange = { 0, 0 }; // 不读取任何数据
    unsigned char* pBuffer = nullptr;
    inCB->Map(0, &readRange, (void**)&pBuffer);
    memcpy(pBuffer, inData, inDataLen);
    inCB->Unmap(0, nullptr); // 使用nullptr表示整个映射范围都被写入
}

ID3D12PipelineState* CreateScenePSO(ID3D12RootSignature* inID3D12RootSignature,
    D3D12_SHADER_BYTECODE inVertexShader, D3D12_SHADER_BYTECODE inPixelShader) {
    // �������벼������
    D3D12_INPUT_ELEMENT_DESC vertexDataElementDesc[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,sizeof(float) * 4,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,sizeof(float) * 8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,sizeof(float) * 12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}
    };

    // ���벼������
    D3D12_INPUT_LAYOUT_DESC vertexDataLayoutDesc = {};
    vertexDataLayoutDesc.NumElements = _countof(vertexDataElementDesc);
    vertexDataLayoutDesc.pInputElementDescs = vertexDataElementDesc;

    // ͼ�ι���״̬����
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = inID3D12RootSignature;
    psoDesc.VS = inVertexShader;
    psoDesc.PS = inPixelShader;

    // 设置4个离屏渲染目标格式（包括Motion Vector RT）
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;  // RT0: Albedo
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;  // RT1: Normal
    psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;  // RT2: ORM (Roughness, Metalness, AO)
    psoDesc.RTVFormats[3] = DXGI_FORMAT_R16G16_FLOAT;        // RT3: Motion Vector (用于TAA)
    psoDesc.NumRenderTargets = 4;  // 渲染目标数量改为4

    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    psoDesc.SampleMask = 0xffffffff;
    psoDesc.InputLayout = vertexDataLayoutDesc;

    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; // ͼԪ��������Ϊ������

    psoDesc.RasterizerState.FrontCounterClockwise = TRUE; // �ؼ���Ĭ��ֵ��˳ʱ��Ϊ����
    // ��դ����״̬����
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;  // 启用深度裁剪

    // 深度模板状态 - 启用深度测试和写入
    psoDesc.DepthStencilState.DepthEnable = TRUE;  // 启用深度测试
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // 混合状态 - GBuffer Pass不需要混合，直接覆盖写入
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;  // 不需要独立混合

    // 禁用所有RT的混合（GBuffer直接写入）
    for (UINT i = 0; i < 4; i++) {
        D3D12_RENDER_TARGET_BLEND_DESC& rtBlend = psoDesc.BlendState.RenderTarget[i];
        rtBlend.BlendEnable = FALSE;  // 禁用混合！
        rtBlend.LogicOpEnable = FALSE;
        rtBlend.SrcBlend = D3D12_BLEND_ONE;
        rtBlend.DestBlend = D3D12_BLEND_ZERO;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    // ����PSO
    ID3D12PipelineState* d3d12PSO = nullptr;
    HRESULT hResult = gD3D12Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&d3d12PSO));
    if (FAILED(hResult)) {
        return nullptr;
    }
    return d3d12PSO;
}

bool InitD3D12(HWND inHWND, int inWidth, int inHeight) {
    gRenderWidth = inWidth;
    gRenderHeight = inHeight;
    gMainHWND = inHWND;

    HRESULT hResult;
    UINT dxgiFactoryFlags = 0;
#ifdef _DEBUG
    {
        ID3D12Debug* debugController = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif
    IDXGIFactory4* dxgiFactory;
    hResult = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hResult)) {
        return false;
    }
    IDXGIAdapter1* adapter;
    int adapterIndex = 0;
    bool adapterFound = false;
    while (dxgiFactory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        hResult = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr);
        if (SUCCEEDED(hResult)) {
            adapterFound = true;
            break;
        }
        adapterIndex++;
    }
    if (false == adapterFound) {
        return false;
    }
    hResult = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gD3D12Device));
    if (FAILED(hResult)) {
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC d3d12CommandQueueDesc = {};
    hResult = gD3D12Device->CreateCommandQueue(&d3d12CommandQueueDesc, IID_PPV_ARGS(&gCommandQueue));
    if (FAILED(hResult)) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc = {};
    swapChainDesc.BufferDesc.Width = inWidth;
    swapChainDesc.BufferDesc.Height = inHeight;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = inHWND;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Windowed = true;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    dxgiFactory->CreateSwapChain(gCommandQueue, &swapChainDesc, &swapChain);
    gSwapChain = static_cast<IDXGISwapChain3*>(swapChain);

    D3D12_HEAP_PROPERTIES d3dHeapProperties = {};
    d3dHeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC d3d12ResourceDesc = {};
    d3d12ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d3d12ResourceDesc.Alignment = 0;
    d3d12ResourceDesc.Width = inWidth;
    d3d12ResourceDesc.Height = inHeight;
    d3d12ResourceDesc.DepthOrArraySize = 1;
    d3d12ResourceDesc.MipLevels = 1;
    d3d12ResourceDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;  // 使用TYPELESS格式以便同时作为DSV和SRV
    d3d12ResourceDesc.SampleDesc.Count = 1;
    d3d12ResourceDesc.SampleDesc.Quality = 0;
    d3d12ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d3d12ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;  // 允许作为深度缓冲和shader资源

    D3D12_CLEAR_VALUE dsClearValue = {};
    dsClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;  // Clear value使用具体格式
    dsClearValue.DepthStencil.Depth = 1.0f;
    dsClearValue.DepthStencil.Stencil = 0;

    gD3D12Device->CreateCommittedResource(&d3dHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &d3d12ResourceDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &dsClearValue,
        IID_PPV_ARGS(&gDSRT)
    );

    D3D12_DESCRIPTOR_HEAP_DESC d3dDescriptorHeapDescRTV = {};
    d3dDescriptorHeapDescRTV.NumDescriptors = 2;
    d3dDescriptorHeapDescRTV.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    gD3D12Device->CreateDescriptorHeap(&d3dDescriptorHeapDescRTV, IID_PPV_ARGS(&gSwapChainRTVHeap));
    gRTVDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC d3dDescriptorHeapDescDSV = {};
    d3dDescriptorHeapDescDSV.NumDescriptors = 1;
    d3dDescriptorHeapDescDSV.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    gD3D12Device->CreateDescriptorHeap(&d3dDescriptorHeapDescDSV, IID_PPV_ARGS(&gSwapChainDSVHeap));
    gDSVDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // ����ImGui��SRV��������
    D3D12_DESCRIPTOR_HEAP_DESC imguiSrvHeapDesc = {};
    imguiSrvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    imguiSrvHeapDesc.NumDescriptors = 100; // �㹻������������
    imguiSrvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    gD3D12Device->CreateDescriptorHeap(&imguiSrvHeapDesc, IID_PPV_ARGS(&gImGuiDescriptorHeap));

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapStart = gSwapChainRTVHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < 2; i++) {
        gSwapChain->GetBuffer(i, IID_PPV_ARGS(&gColorRTs[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE rtvPointer;
        rtvPointer.ptr = rtvHeapStart.ptr + i * gRTVDescriptorSize;
        gD3D12Device->CreateRenderTargetView(gColorRTs[i], nullptr, rtvPointer);
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC d3dDSViewDesc = {};
    d3dDSViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d3dDSViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    gD3D12Device->CreateDepthStencilView(gDSRT, &d3dDSViewDesc, gSwapChainDSVHeap->GetCPUDescriptorHandleForHeapStart());

    gD3D12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gCommandAllocator));
    gD3D12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gCommandAllocator, nullptr, IID_PPV_ARGS(&gCommandList));

    gD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gFence));
    gFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);


    return true;
}


ID3D12CommandAllocator* GetCommandAllocator() {
    return gCommandAllocator;
}

ID3D12GraphicsCommandList* GetCommandList() {
    return gCommandList;
}

void WaitForCompletionOfCommandList() {
    if (gFence->GetCompletedValue() < gFenceValue) {
        gFence->SetEventOnCompletion(gFenceValue, gFenceEvent);
        WaitForSingleObject(gFenceEvent, INFINITE);
    }
}

// 刷新GPU命令队列
void FlushGPU() {
    gFenceValue++;
    gCommandQueue->Signal(gFence, gFenceValue);
    if (gFence->GetCompletedValue() < gFenceValue) {
        gFence->SetEventOnCompletion(gFenceValue, gFenceEvent);
        WaitForSingleObject(gFenceEvent, INFINITE);
    }
}

void EndCommandList() {
    gCommandList->Close();
    ID3D12CommandList* ppCommandLists[] = { gCommandList };
    gCommandQueue->ExecuteCommandLists(1, ppCommandLists);
    gFenceValue += 1;
    gCommandQueue->Signal(gFence, gFenceValue);
}
void BeginOffscreen(ID3D12GraphicsCommandList* commandList) {
    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(gRenderWidth), static_cast<float>(gRenderHeight), 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, gRenderWidth, gRenderHeight };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);
}
void BeginRenderToSwapChain(ID3D12GraphicsCommandList* inCommandList, bool isClear, bool bindDepth) {
    gCurrentRTIndex = gSwapChain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier = InitResourceBarrier(gColorRTs[gCurrentRTIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    inCommandList->ResourceBarrier(1, &barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE colorRT;
    colorRT.ptr = gSwapChainRTVHeap->GetCPUDescriptorHandleForHeapStart().ptr + gCurrentRTIndex * gRTVDescriptorSize;

    // 根据bindDepth参数决定是否绑定深度缓冲
    if (bindDepth) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        dsv.ptr = gSwapChainDSVHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        inCommandList->OMSetRenderTargets(1, &colorRT, FALSE, &dsv);

        // 只有绑定深度缓冲时才可能清除它
        if (isClear) {
            const float clearColor[] = { 0.0f,0.0f,0.0f,1.0f };
            inCommandList->ClearRenderTargetView(colorRT, clearColor, 0, nullptr);
            inCommandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
        }
    } else {
        // 不绑定深度缓冲（用于ScreenPass，因为深度作为SRV读取）
        inCommandList->OMSetRenderTargets(1, &colorRT, FALSE, nullptr);

        if (isClear) {
            const float clearColor[] = { 0.0f,0.0f,0.0f,1.0f };
            inCommandList->ClearRenderTargetView(colorRT, clearColor, 0, nullptr);
        }
    }

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(gRenderWidth), static_cast<float>(gRenderHeight), 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, gRenderWidth, gRenderHeight };
    inCommandList->RSSetViewports(1, &viewport);
    inCommandList->RSSetScissorRects(1, &scissorRect);
}

void EndRenderToSwapChain(ID3D12GraphicsCommandList* inCommandList) {
    D3D12_RESOURCE_BARRIER barrier = InitResourceBarrier(gColorRTs[gCurrentRTIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    inCommandList->ResourceBarrier(1, &barrier);
}

void SwapD3D12Buffers() {
    gSwapChain->Present(0, 0);
}

D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentSwapChainRTV() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    rtvHandle.ptr = gSwapChainRTVHeap->GetCPUDescriptorHandleForHeapStart().ptr + gCurrentRTIndex * gRTVDescriptorSize;
    return rtvHandle;
}

ID3D12Resource* CreateBufferObject(ID3D12GraphicsCommandList* inCommandList,
    void* inData, int inDataLen, D3D12_RESOURCE_STATES inFinalResourceState) {
    D3D12_HEAP_PROPERTIES d3dHeapProperties = {};
    d3dHeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d3d12ResourceDesc = {};
    d3d12ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d3d12ResourceDesc.Alignment = 0;
    d3d12ResourceDesc.Width = inDataLen;
    d3d12ResourceDesc.Height = 1;
    d3d12ResourceDesc.DepthOrArraySize = 1;
    d3d12ResourceDesc.MipLevels = 1;
    d3d12ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    d3d12ResourceDesc.SampleDesc.Count = 1;
    d3d12ResourceDesc.SampleDesc.Quality = 0;
    d3d12ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d3d12ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* bufferObject = nullptr;
    gD3D12Device->CreateCommittedResource(
        &d3dHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &d3d12ResourceDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&bufferObject)
    );
    d3d12ResourceDesc = bufferObject->GetDesc();
    UINT64 memorySizeUsed = 0;
    UINT64 rowSizeInBytes = 0;
    UINT rowUsed = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT subresourceFootprint;
    gD3D12Device->GetCopyableFootprints(&d3d12ResourceDesc, 0, 1, 0,
        &subresourceFootprint, &rowUsed, &rowSizeInBytes, &memorySizeUsed);

    ID3D12Resource* tempBufferObject = nullptr;
    d3dHeapProperties = {};
    d3dHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    gD3D12Device->CreateCommittedResource(
        &d3dHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &d3d12ResourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&tempBufferObject)
    );

    BYTE* pData;
    tempBufferObject->Map(0, nullptr, reinterpret_cast<void**>(&pData));
    BYTE* pDstTempBuffer = reinterpret_cast<BYTE*>(pData + subresourceFootprint.Offset);
    const BYTE* pSrcData = reinterpret_cast<BYTE*>(inData);
    for (UINT i = 0; i < rowUsed; i++) {
        memcpy(pDstTempBuffer + subresourceFootprint.Footprint.RowPitch * i, pSrcData + rowSizeInBytes * i, rowSizeInBytes);
    }
    tempBufferObject->Unmap(0, nullptr);
    inCommandList->CopyBufferRegion(bufferObject, 0, tempBufferObject, 0, subresourceFootprint.Footprint.Width);
    D3D12_RESOURCE_BARRIER barrier = InitResourceBarrier(bufferObject, D3D12_RESOURCE_STATE_COPY_DEST, inFinalResourceState);
    inCommandList->ResourceBarrier(1, &barrier);
    return bufferObject;
}

D3D12_RESOURCE_BARRIER InitResourceBarrier(
    ID3D12Resource* inResource, D3D12_RESOURCE_STATES inPrevState,
    D3D12_RESOURCE_STATES inNextState) {
    D3D12_RESOURCE_BARRIER d3d12ResourceBarrier;
    memset(&d3d12ResourceBarrier, 0, sizeof(d3d12ResourceBarrier));
    d3d12ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    d3d12ResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    d3d12ResourceBarrier.Transition.pResource = inResource;
    d3d12ResourceBarrier.Transition.StateBefore = inPrevState;
    d3d12ResourceBarrier.Transition.StateAfter = inNextState;
    d3d12ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return d3d12ResourceBarrier;
}

// ��ʼ��ImGui
// ImGui��ʼ���������
void InitImGui(HWND hWnd, ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT srvDescriptorSize) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // ��������֧��
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    // ����Ĭ������
    io.Fonts->AddFontDefault();

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX12_Init(
        device,
        2, // ����������������
        DXGI_FORMAT_R8G8B8A8_UNORM,
        srvHeap,
        srvHeap->GetCPUDescriptorHandleForHeapStart(),
        srvHeap->GetGPUDescriptorHandleForHeapStart()
    );

    // ����ImGui�豸����
    ImGui_ImplDX12_CreateDeviceObjects();
}

// ImGui����������
void ShutdownImGui() {
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (gImGuiDescriptorHeap) {
        gImGuiDescriptorHeap->Release();
        gImGuiDescriptorHeap = nullptr;
    }
}

// 分辨率调整函数
bool ResizeSwapChainAndDepthBuffer(int newWidth, int newHeight, bool resizeWindow) {
    if (newWidth == gRenderWidth && newHeight == gRenderHeight) {
        return true; // 没有变化
    }

    if (newWidth <= 0 || newHeight <= 0) {
        OutputDebugStringA("ResizeSwapChainAndDepthBuffer: Invalid dimensions\n");
        return false;
    }

    char debugMsg[256];
    sprintf_s(debugMsg, "ResizeSwapChainAndDepthBuffer: Resizing from %dx%d to %dx%d\n",
              gRenderWidth, gRenderHeight, newWidth, newHeight);
    OutputDebugStringA(debugMsg);

    // 等待所有 GPU 工作完成
    WaitForCompletionOfCommandList();

    // 释放交换链缓冲区引用
    for (int i = 0; i < 2; i++) {
        if (gColorRTs[i]) {
            gColorRTs[i]->Release();
            gColorRTs[i] = nullptr;
        }
    }

    // 释放深度缓冲区
    if (gDSRT) {
        gDSRT->Release();
        gDSRT = nullptr;
    }

    // 调整交换链大小
    HRESULT hr = gSwapChain->ResizeBuffers(
        2,
        newWidth,
        newHeight,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        0
    );
    if (FAILED(hr)) {
        sprintf_s(debugMsg, "ResizeBuffers FAILED with HRESULT: 0x%08X\n", hr);
        OutputDebugStringA(debugMsg);
        return false;
    }
    OutputDebugStringA("ResizeBuffers succeeded\n");

    // 重新获取交换链缓冲区
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapStart = gSwapChainRTVHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < 2; i++) {
        gSwapChain->GetBuffer(i, IID_PPV_ARGS(&gColorRTs[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE rtvPointer;
        rtvPointer.ptr = rtvHeapStart.ptr + i * gRTVDescriptorSize;
        gD3D12Device->CreateRenderTargetView(gColorRTs[i], nullptr, rtvPointer);
    }

    // 重新创建深度缓冲区
    D3D12_HEAP_PROPERTIES d3dHeapProperties = {};
    d3dHeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC d3d12ResourceDesc = {};
    d3d12ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d3d12ResourceDesc.Alignment = 0;
    d3d12ResourceDesc.Width = newWidth;
    d3d12ResourceDesc.Height = newHeight;
    d3d12ResourceDesc.DepthOrArraySize = 1;
    d3d12ResourceDesc.MipLevels = 1;
    d3d12ResourceDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    d3d12ResourceDesc.SampleDesc.Count = 1;
    d3d12ResourceDesc.SampleDesc.Quality = 0;
    d3d12ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d3d12ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE dsClearValue = {};
    dsClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsClearValue.DepthStencil.Depth = 1.0f;
    dsClearValue.DepthStencil.Stencil = 0;

    hr = gD3D12Device->CreateCommittedResource(
        &d3dHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &d3d12ResourceDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &dsClearValue,
        IID_PPV_ARGS(&gDSRT)
    );
    if (FAILED(hr)) {
        return false;
    }

    // 重新创建 DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC d3dDSViewDesc = {};
    d3dDSViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d3dDSViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    gD3D12Device->CreateDepthStencilView(gDSRT, &d3dDSViewDesc, gSwapChainDSVHeap->GetCPUDescriptorHandleForHeapStart());

    // 更新全局分辨率
    gRenderWidth = newWidth;
    gRenderHeight = newHeight;

    // 只有从UI选择分辨率时才调整窗口大小，窗口拖动时不需要
    if (resizeWindow && gMainHWND) {
        RECT rect = { 0, 0, newWidth, newHeight };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(gMainHWND, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }

    return true;
}

int GetRenderWidth() {
    return gRenderWidth;
}

int GetRenderHeight() {
    return gRenderHeight;
}

// ========== 共享全屏四边形 VB ==========
static ID3D12Resource* s_fullscreenQuadVB = nullptr;
static D3D12_VERTEX_BUFFER_VIEW s_fullscreenQuadVBV = {};

ID3D12Resource* GetSharedFullscreenQuadVB(D3D12_VERTEX_BUFFER_VIEW& outVBV) {
    if (s_fullscreenQuadVB) {
        outVBV = s_fullscreenQuadVBV;
        return s_fullscreenQuadVB;
    }

    // Position3 + UV2, 6个顶点（两个三角形）
    float vertices[] = {
        -1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
         1.0f, -1.0f, 0.0f,  1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f,  0.0f, 0.0f,
        -1.0f,  1.0f, 0.0f,  0.0f, 0.0f,
         1.0f, -1.0f, 0.0f,  1.0f, 1.0f,
         1.0f,  1.0f, 0.0f,  1.0f, 0.0f
    };
    const UINT vertexSize = sizeof(vertices);
    const UINT stride = 5 * sizeof(float);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = vertexSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&s_fullscreenQuadVB));
    if (FAILED(hr)) return nullptr;

    UINT8* pData;
    D3D12_RANGE readRange = { 0, 0 };
    s_fullscreenQuadVB->Map(0, &readRange, reinterpret_cast<void**>(&pData));
    memcpy(pData, vertices, vertexSize);
    s_fullscreenQuadVB->Unmap(0, nullptr);

    s_fullscreenQuadVBV.BufferLocation = s_fullscreenQuadVB->GetGPUVirtualAddress();
    s_fullscreenQuadVBV.StrideInBytes = stride;
    s_fullscreenQuadVBV.SizeInBytes = vertexSize;

    outVBV = s_fullscreenQuadVBV;
    return s_fullscreenQuadVB;
}

// ========== 共享全屏 PSO 创建 ==========
ID3D12PipelineState* CreateFullscreenPSO(
    ID3D12RootSignature* rootSig,
    D3D12_SHADER_BYTECODE vs,
    D3D12_SHADER_BYTECODE ps,
    DXGI_FORMAT rtvFormat,
    bool enableAlphaBlend) {

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = rootSig;
    psoDesc.VS = vs;
    psoDesc.PS = ps;

    // 光栅化：禁用剔除
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    psoDesc.RasterizerState.MultisampleEnable = FALSE;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    psoDesc.RasterizerState.ForcedSampleCount = 0;
    psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // 混合状态
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& rtBlend = psoDesc.BlendState.RenderTarget[0];
    if (enableAlphaBlend) {
        rtBlend.BlendEnable = TRUE;
        rtBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    } else {
        rtBlend.BlendEnable = FALSE;
        rtBlend.SrcBlend = D3D12_BLEND_ONE;
        rtBlend.DestBlend = D3D12_BLEND_ZERO;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    rtBlend.LogicOpEnable = FALSE;
    rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // 禁用深度
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtvFormat;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = gD3D12Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        char errorMsg[256];
        sprintf_s(errorMsg, "CreateFullscreenPSO failed: HRESULT 0x%08X\n", hr);
        OutputDebugStringA(errorMsg);
        return nullptr;
    }
    return pso;
}

// ========== 共享 CB 填充 ==========
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
    int shadowMode) {

    using namespace DirectX;

    XMStoreFloat4x4(&out.projMatrix, projMatrix);
    XMStoreFloat4x4(&out.viewMatrix, viewMatrix);
    XMStoreFloat4x4(&out.modelMatrix, modelMatrix);

    // NormalMatrix = transpose(inverse(model))
    XMVECTOR determinant;
    XMMATRIX invModel = XMMatrixInverse(&determinant, modelMatrix);
    if (XMVectorGetX(determinant) != 0.0f) {
        XMStoreFloat4x4(&out.normalMatrix, XMMatrixTranspose(invModel));
    } else {
        XMStoreFloat4x4(&out.normalMatrix, XMMatrixIdentity());
    }

    out.lightDirection = XMFLOAT4(lightDir.x, lightDir.y, lightDir.z, 0.0f);
    out.cameraPosition = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
    out.skylightParams = XMFLOAT4(skylightIntensity, 0.0f, 0.0f, 0.0f);

    XMStoreFloat4x4(&out.invProjMatrix, invProjMatrix);
    XMStoreFloat4x4(&out.invViewMatrix, invViewMatrix);

    out.skylightColor = XMFLOAT4(skylightColor.x, skylightColor.y, skylightColor.z, 0.0f);

    XMStoreFloat4x4(&out.lightViewProjMatrix, lightViewProjMatrix);
    XMStoreFloat4x4(&out.prevViewProjMatrix, previousViewProjMatrix);

    out.jitterParams = XMFLOAT4(jitterOffset.x, jitterOffset.y,
                                 previousJitterOffset.x, previousJitterOffset.y);
    out.screenParams = XMFLOAT4(
        static_cast<float>(viewportWidth),
        static_cast<float>(viewportHeight),
        1.0f / static_cast<float>(viewportWidth),
        1.0f / static_cast<float>(viewportHeight));
    out.nearFarParams = XMFLOAT4(nearPlane, farPlane, 0.0f, 0.0f);

    XMStoreFloat4x4(&out.currentViewProjMatrix, currentViewProjMatrix);

    out.shadowMode = static_cast<float>(shadowMode);
    memset(out.padding, 0, sizeof(out.padding));
}