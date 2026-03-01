#include "public\Scene.h"
#include "public\BattleFireDirect.h"
#include "public/Material.h"
#include "public/Material/MaterialInstance.h"
#include "public/Material/MaterialManager.h"
#include "public/Material/Shader.h"
#include <DirectXMath.h>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <DirectXTex\DirectXTex.h>
#include <wincodec.h>
#include <comdef.h>
#include <shlwapi.h>
#include <wrl.h>
#include <stdexcept>
#include <string>
#include <DDSTextureLoader\DDSTextureLoader12.h>
#include <d3d12.h>
#include <d3dx12.h>
#include "public/PathUtils.h"

#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;
ID3D12DescriptorHeap* srvHeap = nullptr;

Scene::Scene(int viewportWidth, int viewportHeight)
    : m_viewportWidth(viewportWidth),
    m_viewportHeight(viewportHeight),
    m_camera(DirectX::XMConvertToRadians(45.0f), (float)viewportWidth / (float)viewportHeight, 0.1f, 1000.0f),
    m_lightRotation(0.0f, 0.0f, 0.0f),
    m_lightDirection(-1.0f, -1.0f, 1.0f), m_textureLoaded(false), m_textureLoadSuccess(false){
    m_constantBuffer = nullptr;
    m_texBuffer = nullptr;

    m_constantBuffer = CreateConstantBufferObject(sizeof(SceneCBData));  // 使用共享CB结构体大小
    // 初始化时映射一次（持久映射）
    if (m_constantBuffer) {
        D3D12_RANGE readRange = { 0, 0 };
        m_constantBuffer->Map(0, &readRange, &m_mappedConstantBuffer);
    }
}

Scene::~Scene() {
    // 只在缓冲区存在且已映射时才执行Unmap
    if ( m_mappedConstantBuffer) {
        m_constantBuffer->Unmap(0, nullptr);
        m_mappedConstantBuffer = nullptr;
    }

    if (m_constantBuffer) {
        m_constantBuffer->Release();
        m_constantBuffer = nullptr;
    }
    for (auto rt : m_offscreenRTs) {
        if (rt) rt->Release();
    }
    if (m_rtvHeap) m_rtvHeap->Release();
}




// 创建离屏渲染目标(pso创建在battlefiredirect里面）
std::vector<ID3D12Resource*> Scene::CreateOffscreenRTs(int width, int height, ID3D12DescriptorHeap* rtvHeap, UINT rtvDescriptorSize) {
    std::vector<ID3D12Resource*> rts;

    // 创建4个RT：Albedo, Normal, ORM, Motion Vector
    DXGI_FORMAT formats[4] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,  // RT0: 颜色缓冲区 (Albedo)
        DXGI_FORMAT_R16G16B16A16_FLOAT,  // RT1: 法线缓冲区 (Normal)
        DXGI_FORMAT_R16G16B16A16_FLOAT,  // RT2: ORM (AO, Roughness, Metallic)
        DXGI_FORMAT_R16G16_FLOAT         // RT3: Motion Vector (屏幕空间速度)
    };

    for (int i = 0; i < 4; i++) {
        // 资源描述
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Alignment = 0;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = formats[i];
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        // 堆属性
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        // 创建资源
        ID3D12Resource* rt;
        HRESULT hr = gD3D12Device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,  // 修正：初始状态改为PIXEL_SHADER_RESOURCE，与每帧开始的转换匹配
            nullptr,
            IID_PPV_ARGS(&rt)
        );
        if (FAILED(hr)) {
            // 错误处理
            for (auto resource : rts) {
                resource->Release();
            }
            return {};
        }

        // 创建RTV描述符（明确指定格式）
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += i * rtvDescriptorSize;

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = formats[i];
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;

        gD3D12Device->CreateRenderTargetView(rt, &rtvDesc, rtvHandle);

        rts.push_back(rt);
    }

    return rts;
}

std::unordered_map<std::string, std::unique_ptr<Texture>> textures; // 示例纹理存储

HRESULT InitCOM() {
    return CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
}
// 加载PNG并保存为临时DDS
HRESULT ConvertPNGToDDS(const wchar_t* pngPath, Texture* Tex) {
    DirectX::ScratchImage image;
    HRESULT hr;
    
    wchar_t ddsPath[MAX_PATH] = { 0 };

    // 生成DDS文件路径
    wcscpy_s(ddsPath, MAX_PATH, pngPath);
    PathRemoveExtensionW(ddsPath);
    wcscat_s(ddsPath, MAX_PATH, L".dds");
    
    // 1. 加载PNG图像
    hr = DirectX::LoadFromWICFile(pngPath, DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) {
        _com_error err(hr);
        OutputDebugStringW(err.ErrorMessage());
        return hr;
    }

    DirectX::ScratchImage mipChain;
    // 2. 生成Mipmap链
    hr = DirectX::GenerateMipMaps(
        image.GetImages(),         // 输入图像
        image.GetImageCount(),     // 图像数量
        image.GetMetadata(),       // 元数据
        DirectX::TEX_FILTER_DEFAULT, // 默认滤波算法
        0,                         // 0表示生成完整Mipmap链
        mipChain                   // 输出Mipmap链
    );
    if (FAILED(hr)) {
        _com_error err(hr);
        OutputDebugStringW(err.ErrorMessage());
        return hr;
    }

    DirectX::ScratchImage compressedImage;
    // 3. 压缩格式
    hr = DirectX::Compress(
        mipChain.GetImages(),      // 输入Mipmap链图像
        mipChain.GetImageCount(),  // 图像数量
        mipChain.GetMetadata(),    // 元数据
        DXGI_FORMAT_BC3_UNORM_SRGB,     // 压缩格式
        DirectX::TEX_COMPRESS_DEFAULT, // 默认压缩设置
        0.5f,                      // BC1阈值(BC7中未使用，但需提供)
        compressedImage            // 输出压缩后的图像
    );
    if (FAILED(hr)) {
        _com_error err(hr);
        OutputDebugStringW(err.ErrorMessage());
        return hr;
    }

    // 4. 保存为DDS文件 (自动添加DX10头部)
    hr = DirectX::SaveToDDSFile(
        compressedImage.GetImages(),
        compressedImage.GetImageCount(),
        compressedImage.GetMetadata(),
        DirectX::DDS_FLAGS_FORCE_DX10_EXT, // DirectX::DDS_FLAGS_NONE为BC1-5
        ddsPath
    );

    // 保存路径到Texture结构
    wcscpy_s(Tex->fileName, MAX_PATH, ddsPath);
    return hr;
}

// ========== 通用纹理加载函数（替代原来4个独立函数） ==========
bool Scene::LoadAndUploadTexture(const wchar_t* pngPath, const char* textureName, bool isCubemap) {
    auto texture = std::make_unique<Texture>();
    texture->name = textureName;

    // 构造DDS文件路径
    wchar_t ddsPath[MAX_PATH] = { 0 };
    wcscpy_s(ddsPath, MAX_PATH, pngPath);
    PathRemoveExtensionW(ddsPath);
    wcscat_s(ddsPath, MAX_PATH, L".dds");

    HRESULT hr;
    if (!PathFileExistsW(ddsPath)) {
        MessageBoxA(NULL, "路径无dds，进行cook操作\n", "Cook", MB_OK | MB_ICONERROR);
        hr = ConvertPNGToDDS(pngPath, texture.get());
        if (FAILED(hr)) {
            MessageBoxA(NULL, "PNG转换为DDS失败\n", "File Error", MB_OK | MB_ICONERROR);
            return false;
        }
    } else {
        wcscpy_s(texture->fileName, MAX_PATH, ddsPath);
    }

    // 加载DDS
    ID3D12Device* device = gD3D12Device;
    ID3D12GraphicsCommandList* commandList = GetCommandList();

    std::unique_ptr<uint8_t[]> ddsData;
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    bool bIsCube = isCubemap;
    DirectX::DDS_ALPHA_MODE emAlphaMode = DirectX::DDS_ALPHA_MODE_UNKNOWN;

    if (!PathFileExistsW(texture->fileName)) {
        char msg[256];
        sprintf_s(msg, "没有读取到 %s 的DDS文件", textureName);
        MessageBoxA(NULL, msg, "File Error", MB_OK | MB_ICONERROR);
        return false;
    }

    hr = DirectX::LoadDDSTextureFromFile(device,
        texture->fileName,
        texture->resource.GetAddressOf(), ddsData, subresources,
        0, &emAlphaMode, &bIsCube);
    if (FAILED(hr)) {
        char msg[256];
        sprintf_s(msg, "%s DDS文件加载失败", textureName);
        MessageBoxA(NULL, msg, "Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // 创建上传堆并上传
    UINT64 uploadHeapSize = GetRequiredIntermediateSize(
        texture->resource.Get(), 0, static_cast<UINT>(subresources.size()));

    D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC uploadResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadHeapSize);

    hr = gD3D12Device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadResourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&texture->uploadHeap));
    if (FAILED(hr)) {
        MessageBoxA(NULL, "创建上传资源失败", "错误", MB_OK | MB_ICONERROR);
        return false;
    }

    UpdateSubresources(commandList,
        texture->resource.Get(), texture->uploadHeap.Get(),
        0, 0, static_cast<UINT>(subresources.size()), subresources.data());

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        texture->resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);

    texture->ddsData = std::move(ddsData);
    texture->subresources = std::move(subresources);
    textures.emplace(textureName, std::move(texture));
    return true;
}

// 加载所有纹理
bool Scene::LoadTextures() {
    ID3D12GraphicsCommandList* commandList = GetCommandList();

    if (!LoadAndUploadTexture((GetContentPath() + L"Texture\\color.png").c_str(), "CreateTex", false))
        return false;
    m_textureLoadSuccess = true;

    if (!LoadAndUploadTexture((GetContentPath() + L"Cubemap\\cubemap.png").c_str(), "SkyTexture", true))
        return false;

    if (!LoadAndUploadTexture((GetContentPath() + L"Texture\\normal.png").c_str(), "NormalTexture", false))
        return false;

    if (!LoadAndUploadTexture((GetContentPath() + L"Texture\\orm.png").c_str(), "OrmTexture", false))
        return false;

    // 释放之前的SRV堆并创建新的
    if (srvHeap) {
        srvHeap->Release();
        srvHeap = nullptr;
    }
    CreateSRVHeap();
    if (!CreateTextureSRV(commandList)) {
        MessageBoxA(NULL, "创建纹理SRV失败", "错误", MB_OK | MB_ICONERROR);
    }

    return true;
}





std::string GetCurrentWorkingDirectory() {
    char buffer[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, buffer);
    return std::string(buffer);
}

void Scene::CreateSRVHeap() {
    // 获取设备指针（假设全局设备为gD3D12Device）
    ID3D12Device* device = gD3D12Device;
    //创建SRV堆 - Bindless纹理系统需要更大的堆
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    srvHeapDesc.NumDescriptors = 1000;  // Bindless: 支持最多1000个纹理
    srvHeapDesc.NodeMask = 0;
    gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap));

}

// 2. 为加载的纹理创建SRV
bool Scene::CreateTextureSRV(ID3D12GraphicsCommandList* commandList) {
    if (!srvHeap) {
        OutputDebugString(L"SRV heap not initialized\n");
        return false;
    }

    ID3D12Device* device = gD3D12Device;

    // SkyTexture（Cubemap）
    auto skyTexture = textures["SkyTexture"]->resource;
    m_skyTexture = textures["SkyTexture"]->resource;

    // BaseColorTexture (CreateTex)
    auto baseColorTexture = textures["CreateTex"]->resource;
    
    // NormalTexture (NormalTexture)
    auto normalTexture = textures["NormalTexture"]->resource;
    // OrmTexture (OrmTexture)
    auto ormTexture = textures["OrmTexture"]->resource;
    
    // 创建描述符句柄
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(srvHeap->GetCPUDescriptorHandleForHeapStart());

    // 1. SkyTexture (Cubemap) 绑定到 t0
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = skyTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = skyTexture->GetDesc().MipLevels;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(skyTexture.Get(), &srvDesc, handle);

    handle.Offset(1, gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));

    // 2. BaseColorTexture (CreateTex) 绑定到 t1
    srvDesc.Format = baseColorTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = baseColorTexture->GetDesc().MipLevels;
    device->CreateShaderResourceView(baseColorTexture.Get(), &srvDesc, handle);

    handle.Offset(1, gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    
    // 3. NormalTexture (NormalTexture) 绑定到 t2
    srvDesc.Format = normalTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = normalTexture->GetDesc().MipLevels;
    device->CreateShaderResourceView(normalTexture.Get(), &srvDesc, handle);

    handle.Offset(1, gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));

    // 4. OrmTexture 绑定到 t3
    srvDesc.Format = ormTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = ormTexture->GetDesc().MipLevels;
    device->CreateShaderResourceView(ormTexture.Get(), &srvDesc, handle);

    // 跳过 t4-t9，为材质纹理预留槽位
    handle.Offset(7, gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));

    // 材质纹理绑定到 t10-t12（供材质shader使用）
    // t10: BaseColorTex
    srvDesc.Format = baseColorTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = baseColorTexture->GetDesc().MipLevels;
    device->CreateShaderResourceView(baseColorTexture.Get(), &srvDesc, handle);

    handle.Offset(1, gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));

    // t11: NormalTex
    srvDesc.Format = normalTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = normalTexture->GetDesc().MipLevels;
    device->CreateShaderResourceView(normalTexture.Get(), &srvDesc, handle);

    handle.Offset(1, gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));

    // t12: OrmTex
    srvDesc.Format = ormTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = ormTexture->GetDesc().MipLevels;
    device->CreateShaderResourceView(ormTexture.Get(), &srvDesc, handle);

    handle.ptr += gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);


    return true;
}

// 动态更新纹理SRV（用于材质系统）
void Scene::UpdateTextureSRV(UINT slotIndex, ID3D12Resource* textureResource) {
    if (!srvHeap || !textureResource || !gD3D12Device) {
        return;
    }

    // 计算目标槽位的CPU句柄
    UINT descriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(srvHeap->GetCPUDescriptorHandleForHeapStart());
    handle.Offset(slotIndex, descriptorSize);

    // 获取纹理描述
    D3D12_RESOURCE_DESC texDesc = textureResource->GetDesc();

    // 创建SRV描述
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    // 创建新的SRV
    gD3D12Device->CreateShaderResourceView(textureResource, &srvDesc, handle);

    std::cout << "Scene::UpdateTextureSRV - Updated slot " << slotIndex << std::endl;
}

// ========== Bindless纹理系统 ==========
// 静态变量：跟踪已分配的SRV槽位
static std::vector<bool> s_bindlessSRVSlotUsed;
static UINT s_nextFreeBindlessSlot = 10;  // 从t10开始（t0-t9保留给场景纹理）
static const UINT BINDLESS_START_SLOT = 10;
static const UINT BINDLESS_MAX_SLOTS = 990;  // 1000 - 10 = 990个可用槽位

UINT Scene::AllocateBindlessSRVSlot() {
    // 初始化槽位跟踪数组
    if (s_bindlessSRVSlotUsed.empty()) {
        s_bindlessSRVSlotUsed.resize(BINDLESS_MAX_SLOTS, false);
    }

    // 从上次分配位置开始查找
    for (UINT i = 0; i < BINDLESS_MAX_SLOTS; i++) {
        UINT localIndex = (s_nextFreeBindlessSlot - BINDLESS_START_SLOT + i) % BINDLESS_MAX_SLOTS;
        if (!s_bindlessSRVSlotUsed[localIndex]) {
            s_bindlessSRVSlotUsed[localIndex] = true;
            UINT globalSlot = BINDLESS_START_SLOT + localIndex;
            s_nextFreeBindlessSlot = globalSlot + 1;
            std::cout << "Scene::AllocateBindlessSRVSlot - Allocated slot " << globalSlot << std::endl;
            return globalSlot;
        }
    }

    std::cout << "Scene::AllocateBindlessSRVSlot - No free slots available!" << std::endl;
    return UINT_MAX;
}

void Scene::FreeBindlessSRVSlot(UINT slotIndex) {
    if (slotIndex < BINDLESS_START_SLOT || slotIndex >= BINDLESS_START_SLOT + BINDLESS_MAX_SLOTS) {
        return;
    }

    UINT localIndex = slotIndex - BINDLESS_START_SLOT;
    if (localIndex < s_bindlessSRVSlotUsed.size()) {
        s_bindlessSRVSlotUsed[localIndex] = false;
        std::cout << "Scene::FreeBindlessSRVSlot - Freed slot " << slotIndex << std::endl;
    }
}

void Scene::CreateBindlessTextureSRV(UINT slotIndex, ID3D12Resource* textureResource) {
    if (!srvHeap || !textureResource || !gD3D12Device) {
        std::cout << "Scene::CreateBindlessTextureSRV - Invalid parameters" << std::endl;
        return;
    }

    // 计算目标槽位的CPU句柄
    UINT descriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(srvHeap->GetCPUDescriptorHandleForHeapStart());
    handle.Offset(slotIndex, descriptorSize);

    // 获取纹理描述
    D3D12_RESOURCE_DESC texDesc = textureResource->GetDesc();

    // 创建SRV描述
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    // 创建SRV
    gD3D12Device->CreateShaderResourceView(textureResource, &srvDesc, handle);

    std::cout << "Scene::CreateBindlessTextureSRV - Created SRV at slot " << slotIndex << std::endl;
}


bool Scene::Initialize(ID3D12GraphicsCommandList* commandList) {
    // 常量缓冲区已在构造函数中映射，不需要再次Map

    const char* modelPath = "Content/Models/sphere1.fbx";

    std::string currentDir = GetCurrentWorkingDirectory();
    std::cout << "Current working directory: " << currentDir << std::endl;
    std::cout << "Looking for model at: " << currentDir << "\\" << modelPath << std::endl;

    if (GetFileAttributesA(modelPath) == INVALID_FILE_ATTRIBUTES) {
        std::string alternativePath = WToA(GetContentPath() + L"Model\\sphere1.fbx");
        std::cout << "Trying alternative path: " << alternativePath << std::endl;

        if (GetFileAttributesA(alternativePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string errorMsg = "Model file not found:\n"
                + std::string(modelPath) + "\n"
                + "Alternative path failed:\n"
                + alternativePath + "\n"
                + "Current directory: " + currentDir;
            MessageBoxA(NULL, errorMsg.c_str(), "File Error", MB_OK | MB_ICONERROR);
            return false;
        }
        else {
            m_staticMesh.InitFromFile(commandList, alternativePath.c_str());
        }
    }
    else {
        m_staticMesh.InitFromFile(commandList, modelPath);
    }

    // 常量缓冲区已在构造函数中创建和映射，这里只需检查是否有效
    if (!m_constantBuffer || !m_mappedConstantBuffer) {
        MessageBoxA(NULL, "常量缓冲区未正确初始化", "错误", MB_OK | MB_ICONERROR);
        return false;
    }

    // 1. 创建RTV描述符堆（用于4个离屏RT）
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 4; // 4个离屏RT
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(gD3D12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)))) {
        MessageBoxA(NULL, "创建RTV描述符堆失败", "错误", MB_OK | MB_ICONERROR);
        return false;
    }
    m_rtvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // 2. 创建离屏RT（尺寸与视口一致）
    m_offscreenRTs = CreateOffscreenRTs(m_viewportWidth, m_viewportHeight, m_rtvHeap, m_rtvDescriptorSize);
    if (m_offscreenRTs.empty()) {
        return false;
    }

    return true;
}
//纹理异步加载

bool Scene::AsyncLoadTextures() {
    // 使用 std::async 异步加载
    m_textureLoadFuture = std::async(std::launch::async, &Scene::LoadTextures, this);
    return true;
}

void Scene::Update(float deltaTime) {

    //异步加载
    if (!m_textureLoadSuccess) {
        AsyncLoadTextures();  // 只在未开始加载时启动一次
    }
    // 检查异步加载是否完成
    else if (m_textureLoadSuccess && !srvHeap) {

        MessageBoxA(NULL, "update异步失败", "File Error", MB_OK | MB_ICONERROR);

        }


    m_camera.Update(deltaTime);

    // 计算光照方向
    DirectX::XMMATRIX rotationMatrix = DirectX::XMMatrixRotationRollPitchYaw(
        m_lightRotation.x, m_lightRotation.y, m_lightRotation.z);
    DirectX::XMVECTOR initialDir = DirectX::XMVectorSet(-1.0f, -1.0f, 1.0f, 0.0f);
    DirectX::XMVECTOR rotatedDir = DirectX::XMVector3TransformNormal(initialDir, rotationMatrix);
    DirectX::XMStoreFloat3(&m_lightDirection, rotatedDir);

    // 归一化光照方向
    DirectX::XMVECTOR lightDirVec = DirectX::XMLoadFloat3(&m_lightDirection);
    lightDirVec = DirectX::XMVector3Normalize(lightDirVec);
    DirectX::XMFLOAT3 normalizedLightDir;
    DirectX::XMStoreFloat3(&normalizedLightDir, lightDirVec);

    // 获取投影矩阵并应用TAA Jitter
    DirectX::XMMATRIX projectionMatrix = m_camera.GetProjectionMatrix();
    if (m_jitterOffset.x != 0.0f || m_jitterOffset.y != 0.0f) {
        float jitterOffsetX = m_jitterOffset.x * 2.0f / static_cast<float>(m_viewportWidth);
        float jitterOffsetY = m_jitterOffset.y * 2.0f / static_cast<float>(m_viewportHeight);
        projectionMatrix.r[2].m128_f32[0] += jitterOffsetX;
        projectionMatrix.r[2].m128_f32[1] += jitterOffsetY;
    }

    DirectX::XMMATRIX viewMatrix = m_camera.GetViewMatrix();
    DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();

    // 计算逆矩阵
    DirectX::XMVECTOR projDet, viewDet;
    DirectX::XMMATRIX invProjMatrix = DirectX::XMMatrixInverse(&projDet, projectionMatrix);
    DirectX::XMMATRIX invViewMatrix = DirectX::XMMatrixInverse(&viewDet, viewMatrix);

    // 计算LiSPSM矩阵（阴影用不带Jitter的原始投影矩阵）
    DirectX::XMMATRIX originalProjMatrix = m_camera.GetProjectionMatrix();
    DirectX::XMMATRIX lightViewProjMatrix = CalculateLiSPSMMatrix(lightDirVec, viewMatrix, originalProjMatrix);

    // 当前帧VP矩阵（不带Jitter，用于Motion Vector）
    DirectX::XMMATRIX currentViewProjMatrix = viewMatrix * originalProjMatrix;

    // 使用共享函数填充CB
    FillSceneCBData(m_cbData,
        viewMatrix, projectionMatrix, modelMatrix,
        normalizedLightDir, m_camera.GetPosition(),
        m_skylightIntensity, m_skylightColor,
        invProjMatrix, invViewMatrix,
        lightViewProjMatrix, m_previousViewProjectionMatrix,
        m_jitterOffset, m_previousJitterOffset,
        m_viewportWidth, m_viewportHeight,
        m_camera.GetNearPlane(), m_camera.GetFarPlane(),
        currentViewProjMatrix, m_shadowMode, m_giType);

    // 使用持久映射，直接memcpy到映射内存
    if (m_mappedConstantBuffer) {
        memcpy(m_mappedConstantBuffer, &m_cbData, sizeof(SceneCBData));
    }

    // 更新所有Actor的CB（确保在任何Pass之前CB已准备好）
    for (Actor* actor : m_actors) {
        if (!actor) continue;
        actor->UpdateConstantBuffer(viewMatrix, projectionMatrix,
            normalizedLightDir, m_camera.GetPosition(),
            m_skylightIntensity, m_skylightColor,
            invProjMatrix, invViewMatrix,
            lightViewProjMatrix, m_previousViewProjectionMatrix,
            m_jitterOffset, m_previousJitterOffset,
            m_viewportWidth, m_viewportHeight,
            m_camera.GetNearPlane(), m_camera.GetFarPlane(),
            currentViewProjMatrix, m_shadowMode, m_giType);
    }
}

// TAA: 更新上一帧的 ViewProjection 矩阵（在帧结束时调用）
void Scene::UpdatePreviousViewProjectionMatrix() {
    DirectX::XMMATRIX viewMatrix = m_camera.GetViewMatrix();
    DirectX::XMMATRIX projectionMatrix = m_camera.GetProjectionMatrix();
    m_previousViewProjectionMatrix = viewMatrix * projectionMatrix;
}

// 在Scene::Render函数中修改描述符堆的设置
void Scene::Render(ID3D12GraphicsCommandList* commandList, ID3D12PipelineState* pso, ID3D12RootSignature* rootSignature) {
    // 获取4个离屏RT的RTV句柄（包括Motion Vector RT）
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandles[4];
    rtvHandles[0] = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_rtvDescriptorSize);
    rtvHandles[1] = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_rtvDescriptorSize);
    rtvHandles[2] = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 2, m_rtvDescriptorSize);
    rtvHandles[3] = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 3, m_rtvDescriptorSize);

    // 转换离屏RT状态为渲染目标状态
    // 注意：上一帧结束时RT被转换为PIXEL_SHADER_RESOURCE状态
    for (auto rt : m_offscreenRTs) {
        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            rt,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET
        );
        commandList->ResourceBarrier(1, &barrier);
    }

    // 注意：深度缓冲不需要状态转换，因为：
    // - 第一帧：初始状态是 DEPTH_WRITE
    // - 后续帧：ScreenPass后转回 DEPTH_WRITE，BeginRenderToSwapChain会使用并保持 DEPTH_WRITE
    // 所以深度缓冲始终处于 DEPTH_WRITE 状态进入这个Pass

    // 获取深度缓冲的DSV句柄
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = gSwapChainDSVHeap->GetCPUDescriptorHandleForHeapStart();

    //  绑定离屏RT为渲染目标，同时绑定深度缓冲
    commandList->OMSetRenderTargets(
        4,                  // 数量与PSO的NumRenderTargets一致（改为4，包括Motion Vector）
        rtvHandles,  // 4个RTV句柄
        FALSE,
        &dsvHandle          // 绑定深度模板视图
    );


    commandList->SetGraphicsRootSignature(rootSignature);

    // 注意：不在这里绑定常量缓冲区，而是在每个Actor渲染前绑定
    // 这样可以确保每次绑定时GPU读取的是最新的CB数据

    // 根据SRV堆状态决定设置哪些堆

    CD3DX12_GPU_DESCRIPTOR_HANDLE texHandle(srvHeap->GetGPUDescriptorHandleForHeapStart());

    commandList->SetGraphicsRootDescriptorTable(1, texHandle);

    //  清除离屏RT和深度缓冲

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (const auto& handle : rtvHandles) {
        commandList->ClearRenderTargetView(handle, clearColor, 0, nullptr);
    }

    // 清除深度缓冲
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    //  绘制逻辑（使用当前PSO和根签名）
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 【新增】多Actor支持：遍历所有Actor，每个Actor使用其对应的Material进行渲染
    // 如果没有Actor，则回退到旧的单Mesh渲染方式
    if (!m_actors.empty()) {
        // 准备CB参数（所有Actor共享的场景级参数）
        DirectX::XMMATRIX viewMatrix = m_camera.GetViewMatrix();
        DirectX::XMMATRIX projMatrix = m_camera.GetProjectionMatrix();

        // TAA: 应用Jitter到投影矩阵（与UpdateSceneCB中相同的逻辑）
        if (m_jitterOffset.x != 0.0f || m_jitterOffset.y != 0.0f) {
            float jitterOffsetX = m_jitterOffset.x * 2.0f / static_cast<float>(m_viewportWidth);
            float jitterOffsetY = m_jitterOffset.y * 2.0f / static_cast<float>(m_viewportHeight);
            // DirectXMath使用行主序，投影矩阵的[2,0]和[2,1]元素控制NDC的x和y偏移
            projMatrix.r[2].m128_f32[0] += jitterOffsetX;
            projMatrix.r[2].m128_f32[1] += jitterOffsetY;
        }

        DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();

        // 归一化光照方向
        DirectX::XMVECTOR lightDirVec = DirectX::XMLoadFloat3(&m_lightDirection);
        lightDirVec = DirectX::XMVector3Normalize(lightDirVec);
        DirectX::XMFLOAT3 normalizedLightDir;
        DirectX::XMStoreFloat3(&normalizedLightDir, lightDirVec);

        // 计算逆矩阵
        DirectX::XMVECTOR projDeterminant, viewDeterminant;
        DirectX::XMMATRIX invProjMatrix = DirectX::XMMatrixInverse(&projDeterminant, projMatrix);
        DirectX::XMMATRIX invViewMatrix = DirectX::XMMatrixInverse(&viewDeterminant, viewMatrix);

        // 计算LightViewProjMatrix（LiSPSM）
        DirectX::XMMATRIX lightViewProjMatrix = CalculateLiSPSMMatrix(lightDirVec, viewMatrix, projMatrix);

        // TAA: 计算当前帧的ViewProjectionMatrix（不带Jitter，用于Motion Vector）
        DirectX::XMMATRIX currentViewProjMatrix = m_camera.GetViewMatrix() * m_camera.GetProjectionMatrix();

        // 获取相机的近远平面
        float nearPlane = m_camera.GetNearPlane();
        float farPlane = m_camera.GetFarPlane();

        for (Actor* actor : m_actors) {
            if (!actor) continue;

            StaticMeshComponent* mesh = actor->GetMesh();
            if (!mesh) continue;

            // 调试输出：显示当前Actor的Transform
            DirectX::XMFLOAT3 pos = actor->GetPosition();
            char debugMsg[256];
            sprintf_s(debugMsg, "Rendering Actor '%s' at position (%.2f, %.2f, %.2f)\n",
                     actor->GetName().c_str(), pos.x, pos.y, pos.z);
            OutputDebugStringA(debugMsg);

            // CRITICAL: 获取Actor的Material并设置对应的PSO
            // 每个Actor可能使用不同的Material（不同的Shader），因此需要切换PSO
            MaterialInstance* material = actor->GetMaterial();
            if (material && material->GetShader()) {
                Shader* shader = material->GetShader();

                // 获取该Shader的Pass 0的PSO（GBuffer Pass）
                ID3D12PipelineState* actorPSO = shader->GetPSO(0);
                if (actorPSO) {
                    commandList->SetPipelineState(actorPSO);
                } else {
                    // 如果Actor的Shader没有编译PSO，使用传入的默认PSO
                    commandList->SetPipelineState(pso);
                }

                // 绑定Material的常量缓冲区（b1，材质参数）
                // 首先检查是否有待加载的纹理
                if (material->HasPendingTextures()) {
                    material->LoadTexturesFromPaths(commandList);
                }
                material->Bind(commandList, rootSignature, 2);

                // Bindless纹理系统：不再需要每帧重新绑定纹理到固定槽位
                // 纹理索引已经通过MaterialInstance的CB传递给Shader
                // Shader使用动态索引从全局纹理数组中采样
                // 注意：全局SRV堆（TextureManager的堆）需要在渲染开始前设置
            } else {
                // 如果Actor没有Material，使用传入的默认PSO
                commandList->SetPipelineState(pso);
            }

            // SOLUTION B: 更新Actor独立的CB并绑定（包含TAA参数）
            actor->UpdateConstantBuffer(viewMatrix, projMatrix,
                                       normalizedLightDir, cameraPos,
                                       m_skylightIntensity, m_skylightColor,
                                       invProjMatrix, invViewMatrix,
                                       lightViewProjMatrix,
                                       m_previousViewProjectionMatrix,
                                       m_jitterOffset, m_previousJitterOffset,
                                       m_viewportWidth, m_viewportHeight,
                                       nearPlane, farPlane,
                                       currentViewProjMatrix, m_shadowMode, m_giType);

            // 调试：输出CB的GPU地址，确认每个Actor使用不同的CB
            char cbMsg[256];
            sprintf_s(cbMsg, "  Actor CB GPU Address: 0x%llX\n",
                     actor->GetConstantBuffer()->GetGPUVirtualAddress());
            OutputDebugStringA(cbMsg);

            // 绑定Actor的CB（b0）
            commandList->SetGraphicsRootConstantBufferView(0, actor->GetConstantBuffer()->GetGPUVirtualAddress());

            // 渲染当前Actor的Mesh（一个DrawCall）
            mesh->Render(commandList, rootSignature);
        }
    } else {
        // 旧的单Mesh渲染方式（向后兼容）
        commandList->SetPipelineState(pso);
        m_staticMesh.Render(commandList, rootSignature);
    }

    // 6. 渲染完成后，转换离屏RT状态为着色器资源（如需后续读取）
    for (auto rt : m_offscreenRTs) {
        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            rt,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        );
        commandList->ResourceBarrier(1, &barrier);
    }

    // 转换深度缓冲状态（从DEPTH_WRITE到PIXEL_SHADER_RESOURCE，供后续Pass使用）
    CD3DX12_RESOURCE_BARRIER depthBarrierToSRV = CD3DX12_RESOURCE_BARRIER::Transition(
        gDSRT,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    commandList->ResourceBarrier(1, &depthBarrierToSRV);


}

void Scene::HandleInput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    m_camera.HandleInput(hWnd, msg, wParam, lParam);
}

// 计算平行光阴影矩阵（固定阴影范围 + 标准正交投影）
DirectX::XMMATRIX Scene::CalculateLiSPSMMatrix(const DirectX::XMVECTOR& lightDir,
                                                const DirectX::XMMATRIX& cameraView,
                                                const DirectX::XMMATRIX& cameraProj) {
    return CalculateStandardShadowMatrix(lightDir);
}

// 辅助函数：计算标准正交阴影矩阵（用于退化情况）
DirectX::XMMATRIX Scene::CalculateStandardShadowMatrix(const DirectX::XMVECTOR& lightDir) {
    using namespace DirectX;

    XMVECTOR L = XMVector3Normalize(-lightDir);

    // 以摄像机为中心的固定阴影范围（替代场景AABB）
    float shadowSize = m_shadowOrthoSize;
    XMFLOAT3 camPosF = m_camera.GetPosition();

    // 对齐到纹素网格，消除摄像机移动时的阴影抖动
    const float shadowMapResolution = 2048.0f;  // TODO: 从LightPass获取实际分辨率
    float texelSize = (shadowSize * 2.0f) / shadowMapResolution;
    float snappedX = floorf(camPosF.x / texelSize) * texelSize;
    float snappedZ = floorf(camPosF.z / texelSize) * texelSize;

    XMFLOAT3 minBounds(snappedX - shadowSize, camPosF.y - shadowSize, snappedZ - shadowSize);
    XMFLOAT3 maxBounds(snappedX + shadowSize, camPosF.y + shadowSize, snappedZ + shadowSize);

    XMVECTOR sceneCenter = XMVectorSet(snappedX, camPosF.y, snappedZ, 1.0f);

    float lightDistance = shadowSize * 4.0f;  // 确保光源足够远，覆盖整个阴影范围
    XMVECTOR lightPos = sceneCenter + L * lightDistance;

    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    float dotUp = abs(XMVectorGetX(XMVector3Dot(L, up)));
    if (dotUp > 0.99f) {
        up = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    }

    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, sceneCenter, up);

    // 在光源空间计算AABB
    float minX = FLT_MAX, maxX = -FLT_MAX;
    float minY = FLT_MAX, maxY = -FLT_MAX;
    float minZ = FLT_MAX, maxZ = -FLT_MAX;

    XMFLOAT3 corners[8] = {
        {minBounds.x, minBounds.y, minBounds.z},
        {maxBounds.x, minBounds.y, minBounds.z},
        {minBounds.x, maxBounds.y, minBounds.z},
        {maxBounds.x, maxBounds.y, minBounds.z},
        {minBounds.x, minBounds.y, maxBounds.z},
        {maxBounds.x, minBounds.y, maxBounds.z},
        {minBounds.x, maxBounds.y, maxBounds.z},
        {maxBounds.x, maxBounds.y, maxBounds.z}
    };

    for (int i = 0; i < 8; ++i) {
        XMVECTOR corner = XMVectorSet(corners[i].x, corners[i].y, corners[i].z, 1.0f);
        XMVECTOR lsCorner = XMVector3Transform(corner, lightView);

        minX = min(minX, XMVectorGetX(lsCorner));
        maxX = max(maxX, XMVectorGetX(lsCorner));
        minY = min(minY, XMVectorGetY(lsCorner));
        maxY = max(maxY, XMVectorGetY(lsCorner));
        minZ = min(minZ, XMVectorGetZ(lsCorner));
        maxZ = max(maxZ, XMVectorGetZ(lsCorner));
    }

    float padding = 5.0f;
    // 确保near平面不为负（光源后方），同时保留足够深度范围
    float nearZ = max(0.1f, minZ - padding);
    float farZ = maxZ + padding;
    XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
        minX - padding, maxX + padding,
        minY - padding, maxY + padding,
        nearZ, farZ
    );

    return XMMatrixMultiply(lightView, lightProj);
}

// ============ Actor管理函数实现 ============

Actor* Scene::CreateActor(const std::string& name) {
    Actor* actor = new Actor(name);
    m_actors.push_back(actor);
    return actor;
}

bool Scene::LoadActorFromMeshFile(const std::wstring& meshFilePath, ID3D12GraphicsCommandList* commandList) {
    OutputDebugStringA("Scene::LoadActorFromMeshFile - Start\n");
    char msg[512];
    sprintf_s(msg, "  Mesh file path: %S\n", meshFilePath.c_str());
    OutputDebugStringA(msg);

    // 创建Actor
    Actor* actor = new Actor("Sphere");
    actor->CreateConstantBuffer(gD3D12Device);  // Create independent CB

    // 加载.mesh文件
    if (!actor->LoadFromMeshFile(meshFilePath)) {
        OutputDebugStringA("Scene::LoadActorFromMeshFile - Failed to load mesh file\n");
        delete actor;
        return false;
    }

    // 从.mesh文件信息中加载FBX
    const MeshAssetInfo& meshInfo = actor->GetMeshAssetInfo();

    // 将相对路径转换为绝对路径
    std::wstring fbxPath = GetEnginePath();
    std::string fbxPathStr = meshInfo.fbxPath;
    fbxPath += std::wstring(fbxPathStr.begin(), fbxPathStr.end());

    sprintf_s(msg, "  FBX path: %S\n", fbxPath.c_str());
    OutputDebugStringA(msg);

    // 将wstring转换为string (FBX加载需要char*)
    std::string fbxPathAnsi(fbxPath.begin(), fbxPath.end());

    sprintf_s(msg, "  FBX path (ANSI): %s\n", fbxPathAnsi.c_str());
    OutputDebugStringA(msg);

    // 加载FBX文件到StaticMeshComponent
    StaticMeshComponent* mesh = new StaticMeshComponent();

    OutputDebugStringA("  Calling InitFromFile...\n");
    mesh->InitFromFile(commandList, fbxPathAnsi.c_str());
    OutputDebugStringA("  InitFromFile completed\n");

    actor->SetMesh(mesh);
    m_actors.push_back(actor);

    OutputDebugStringA("Scene::LoadActorFromMeshFile - Success\n");
    return true;
}

void Scene::RemoveActor(Actor* actor) {
    auto it = std::find(m_actors.begin(), m_actors.end(), actor);
    if (it != m_actors.end()) {
        delete *it;
        m_actors.erase(it);
    }
}

Actor* Scene::GetActorByName(const std::string& name) {
    for (Actor* actor : m_actors) {
        if (actor->GetName() == name) {
            return actor;
        }
    }
    return nullptr;
}

// ============ Level管理函数实现 ============

// ========== LoadLevel 辅助函数 ==========

static DirectX::XMFLOAT3 ParseFloat3(const std::string& value, const DirectX::XMFLOAT3& defaultVal) {
    std::stringstream ss(value);
    std::string token;
    std::vector<float> values;
    while (std::getline(ss, token, ',')) {
        values.push_back(std::stof(token));
    }
    if (values.size() == 3) {
        return DirectX::XMFLOAT3(values[0], values[1], values[2]);
    }
    return defaultVal;
}

static bool FinalizeAndAddActor(Actor* actor, const std::string& actorName,
    const std::string& meshAssetPath, const std::string& materialName,
    const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& rotation, const DirectX::XMFLOAT3& scale,
    ID3D12GraphicsCommandList* commandList, std::vector<Actor*>& actors) {

    actor->SetPosition(position);
    actor->SetRotation(rotation);
    actor->SetScale(scale);

    std::wstring meshAssetPathW(meshAssetPath.begin(), meshAssetPath.end());
    std::wstring absoluteMeshPath = GetEnginePath() + meshAssetPathW;

    if (!actor->LoadFromMeshFile(absoluteMeshPath)) {
        char msg[256];
        sprintf_s(msg, "Failed to load mesh file for actor '%s'\n", actorName.c_str());
        OutputDebugStringA(msg);
        delete actor;
        return false;
    }

    const MeshAssetInfo& meshInfo = actor->GetMeshAssetInfo();
    std::wstring fbxPath = GetEnginePath();
    fbxPath += std::wstring(meshInfo.fbxPath.begin(), meshInfo.fbxPath.end());
    std::string fbxPathAnsi(fbxPath.begin(), fbxPath.end());

    StaticMeshComponent* mesh = new StaticMeshComponent();
    mesh->InitFromFile(commandList, fbxPathAnsi.c_str());
    actor->SetMesh(mesh);

    if (!materialName.empty()) {
        MaterialInstance* material = MaterialManager::GetInstance().GetMaterial(materialName);
        if (material) {
            actor->SetMaterial(material);
        } else {
            char msg[256];
            sprintf_s(msg, "Warning: Material '%s' not found for actor '%s'\n",
                     materialName.c_str(), actorName.c_str());
            OutputDebugStringA(msg);
        }
    }

    actors.push_back(actor);
    return true;
}

bool Scene::LoadLevel(const std::wstring& levelFilePath, ID3D12GraphicsCommandList* commandList) {
    OutputDebugStringA("Scene::LoadLevel - Start\n");
    char msg[512];
    sprintf_s(msg, "  Level file path: %S\n", levelFilePath.c_str());
    OutputDebugStringA(msg);

    std::ifstream file(levelFilePath);
    if (!file.is_open()) {
        OutputDebugStringA("Scene::LoadLevel - Failed to open level file\n");
        MessageBoxA(NULL, "Failed to open level file", "Error", MB_OK);
        return false;
    }

    // Clear existing actors
    for (Actor* actor : m_actors) {
        delete actor;
    }
    m_actors.clear();

    std::string line;
    std::string currentSection;
    Actor* currentActor = nullptr;
    std::string actorName;
    std::string meshAssetPath;
    std::string materialName;
    DirectX::XMFLOAT3 position(0, 0, 0);
    DirectX::XMFLOAT3 rotation(0, 0, 0);
    DirectX::XMFLOAT3 scale(1, 1, 1);

    while (std::getline(file, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line[0] == '#') continue;

        // Section header
        if (line[0] == '[' && line[line.length() - 1] == ']') {
            // Finalize previous actor
            if (currentActor) {
                FinalizeAndAddActor(currentActor, actorName, meshAssetPath, materialName,
                    position, rotation, scale, commandList, m_actors);
                currentActor = nullptr;
            }

            currentSection = line.substr(1, line.length() - 2);

            if (currentSection.find("Actor_") == 0) {
                actorName = "Actor";
                meshAssetPath.clear();
                materialName.clear();
                position = DirectX::XMFLOAT3(0, 0, 0);
                rotation = DirectX::XMFLOAT3(0, 0, 0);
                scale = DirectX::XMFLOAT3(1, 1, 1);
            }
            continue;
        }

        // Key-value pairs
        size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) continue;

        std::string key = line.substr(0, equalPos);
        std::string value = line.substr(equalPos + 1);

        if (currentSection.find("Actor_") == 0) {
            if (key == "Name") {
                actorName = value;
                currentActor = new Actor(actorName);
                currentActor->CreateConstantBuffer(gD3D12Device);
            } else if (key == "MeshAsset") {
                meshAssetPath = value;
            } else if (key == "Material") {
                materialName = value;
            } else if (key == "Position") {
                position = ParseFloat3(value, DirectX::XMFLOAT3(0, 0, 0));
            } else if (key == "Rotation") {
                rotation = ParseFloat3(value, DirectX::XMFLOAT3(0, 0, 0));
            } else if (key == "Scale") {
                scale = ParseFloat3(value, DirectX::XMFLOAT3(1, 1, 1));
            }
        }
    }

    // Don't forget the last actor
    if (currentActor) {
        FinalizeAndAddActor(currentActor, actorName, meshAssetPath, materialName,
            position, rotation, scale, commandList, m_actors);
    }

    file.close();

    sprintf_s(msg, "Scene::LoadLevel - Loaded %d actors\n", (int)m_actors.size());
    OutputDebugStringA(msg);

    return true;
}

bool Scene::SaveLevel(const std::wstring& levelFilePath) {
    std::ofstream file(levelFilePath);
    if (!file.is_open()) {
        return false;
    }

    file << "[Level]\n";
    file << "Name=Default\n";
    file << "Description=Saved level\n\n";

    for (size_t i = 0; i < m_actors.size(); ++i) {
        Actor* actor = m_actors[i];
        file << "[Actor_" << i << "]\n";
        file << "Name=" << actor->GetName() << "\n";

        const MeshAssetInfo& meshInfo = actor->GetMeshAssetInfo();
        file << "MeshAsset=" << meshInfo.fbxPath << "\n";  // Store relative path
        file << "Material=" << meshInfo.defaultMaterial << "\n";

        DirectX::XMFLOAT3 pos = actor->GetPosition();
        DirectX::XMFLOAT3 rot = actor->GetRotation();
        DirectX::XMFLOAT3 scale = actor->GetScale();

        file << "Position=" << pos.x << "," << pos.y << "," << pos.z << "\n";
        file << "Rotation=" << rot.x << "," << rot.y << "," << rot.z << "\n";
        file << "Scale=" << scale.x << "," << scale.y << "," << scale.z << "\n\n";
    }

    file.close();
    return true;
}

// 分辨率变更时重新创建RTs
bool Scene::ResizeRenderTargets(int newWidth, int newHeight) {
    if (newWidth == m_viewportWidth && newHeight == m_viewportHeight) {
        return true; // 没有变化
    }

    // 等待GPU完成所有工作
    WaitForCompletionOfCommandList();

    // 释放旧的离屏RT
    for (auto rt : m_offscreenRTs) {
        if (rt) {
            rt->Release();
        }
    }
    m_offscreenRTs.clear();

    // 更新视口尺寸
    m_viewportWidth = newWidth;
    m_viewportHeight = newHeight;

    // 更新相机宽高比
    m_camera.SetAspectRatio(static_cast<float>(newWidth) / static_cast<float>(newHeight));

    // 重新创建离屏RT
    m_offscreenRTs = CreateOffscreenRTs(m_viewportWidth, m_viewportHeight, m_rtvHeap, m_rtvDescriptorSize);
    if (m_offscreenRTs.empty()) {
        return false;
    }

    return true;
}