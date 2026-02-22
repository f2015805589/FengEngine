// TextureAsset.cpp
// 纹理资产实现

// 禁用Windows宏以避免与std::max/min冲突
#define NOMINMAX

#include "public/Texture/TextureAsset.h"
#include "public/Texture/TextureManager.h"
#include "public/Texture/TextureCompressor.h"
#include "public/BattleFireDirect.h"
#include <d3dx12.h>
#include <DirectXTex/DirectXTex.h>
#include <comdef.h>
#include <msxml6.h>
#include <shlwapi.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "msxml6.lib")
#pragma comment(lib, "shlwapi.lib")

// MD5哈希计算（简化版，用于缓存验证）
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

// 静态变量初始化：默认使用NVTT压缩
bool TextureAsset::s_useNVTT = true;

namespace {
    // BSTR转std::string辅助函数
    std::string BSTRToString(BSTR bstr) {
        if (!bstr) return "";
        int len = WideCharToMultiByte(CP_UTF8, 0, bstr, -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, bstr, -1, &result[0], len, nullptr, nullptr);
        return result;
    }

    // std::string转BSTR
    BSTR StringToBSTR(const std::string& str) {
        int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        std::wstring wstr(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
        return SysAllocString(wstr.c_str());
    }

    // wstring转string
    std::string WStringToString(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
        return result;
    }

    // string转wstring
    std::wstring StringToWString(const std::string& str) {
        if (str.empty()) return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        std::wstring result(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], len);
        return result;
    }

    // 计算文件MD5哈希
    std::string CalculateFileMD5(const std::wstring& filePath) {
        HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return "";

        HCRYPTPROV hProv = 0;
        HCRYPTHASH hHash = 0;
        std::string result;

        if (CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            if (CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
                BYTE buffer[8192];
                DWORD bytesRead;
                while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
                    CryptHashData(hHash, buffer, bytesRead, 0);
                }

                BYTE hash[16];
                DWORD hashLen = 16;
                if (CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
                    std::ostringstream oss;
                    for (int i = 0; i < 16; i++) {
                        oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
                    }
                    result = oss.str();
                }
                CryptDestroyHash(hHash);
            }
            CryptReleaseContext(hProv, 0);
        }

        CloseHandle(hFile);
        return result;
    }
}

// ========== 构造和析构 ==========

TextureAsset::TextureAsset(const std::string& name)
    : m_name(name) {
}

TextureAsset::~TextureAsset() {
    UnloadFromGPU();
}

// ========== 静态辅助函数 ==========

DXGI_FORMAT TextureAsset::GetDXGIFormat(TextureCompressionFormat format, bool sRGB) {
    switch (format) {
    case TextureCompressionFormat::None:
        return sRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureCompressionFormat::BC1:
        return sRGB ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    case TextureCompressionFormat::BC3:
        return sRGB ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    case TextureCompressionFormat::BC5:
        return DXGI_FORMAT_BC5_UNORM;  // BC5不支持sRGB
    case TextureCompressionFormat::BC7:
        return sRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    case TextureCompressionFormat::BC6H:
        return DXGI_FORMAT_BC6H_UF16;  // HDR，无sRGB
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

const char* TextureAsset::GetFormatName(TextureCompressionFormat format) {
    switch (format) {
    case TextureCompressionFormat::None: return "None";
    case TextureCompressionFormat::BC1: return "BC1";
    case TextureCompressionFormat::BC3: return "BC3";
    case TextureCompressionFormat::BC5: return "BC5";
    case TextureCompressionFormat::BC7: return "BC7";
    case TextureCompressionFormat::BC6H: return "BC6H";
    default: return "Unknown";
    }
}

const char* TextureAsset::GetTypeName(TextureType type) {
    switch (type) {
    case TextureType::Texture2D: return "Texture2D";
    case TextureType::TextureCube: return "TextureCube";
    case TextureType::Texture2DArray: return "Texture2DArray";
    default: return "Unknown";
    }
}

size_t TextureAsset::CalculateMemorySize(UINT width, UINT height, UINT mipLevels,
                                          TextureCompressionFormat format) {
    size_t totalSize = 0;
    UINT w = width, h = height;

    for (UINT mip = 0; mip < mipLevels; mip++) {
        size_t mipSize;
        switch (format) {
        case TextureCompressionFormat::None:
            mipSize = w * h * 4;  // RGBA8
            break;
        case TextureCompressionFormat::BC1:
            mipSize = ((w + 3) / 4) * ((h + 3) / 4) * 8;
            break;
        case TextureCompressionFormat::BC3:
        case TextureCompressionFormat::BC5:
        case TextureCompressionFormat::BC7:
        case TextureCompressionFormat::BC6H:
            mipSize = ((w + 3) / 4) * ((h + 3) / 4) * 16;
            break;
        default:
            mipSize = w * h * 4;
        }
        totalSize += mipSize;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return totalSize;
}

// ========== 文件操作 ==========

bool TextureAsset::LoadFromAssetFile(const std::wstring& assetPath) {
    std::cout << "TextureAsset::LoadFromAssetFile - START: " << WStringToString(assetPath) << std::endl;
    m_assetPath = assetPath;
    bool result = ParseAssetXML(assetPath);
    std::cout << "TextureAsset::LoadFromAssetFile - END: " << (result ? "SUCCESS" : "FAILED") << std::endl;
    return result;
}

bool TextureAsset::SaveAssetFile(const std::wstring& assetPath) {
    m_assetPath = assetPath;
    return WriteAssetXML(assetPath);
}

bool TextureAsset::ImportFromSource(const std::wstring& sourcePath, const TextureAssetDesc& desc) {
    m_sourcePath = sourcePath;
    m_desc = desc;
    m_sourceHash = CalculateSourceHash(sourcePath);
    m_cacheDdsPath = GenerateCachePath();
    m_cacheValid = false;

    // 提取文件名作为资产名（如果未设置）
    if (m_name.empty()) {
        wchar_t fileName[MAX_PATH];
        wcscpy_s(fileName, sourcePath.c_str());
        PathStripPathW(fileName);
        PathRemoveExtensionW(fileName);
        m_name = WStringToString(fileName);
    }

    return true;
}

// ========== GPU资源管理 ==========

bool TextureAsset::LoadToGPU(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) {
    if (m_isLoaded) return true;

    // 优先从缓存加载
    if (m_cacheValid && PathFileExistsW(m_cacheDdsPath.c_str())) {
        if (LoadDDSFromCache(device, commandList)) {
            m_isLoaded = true;
            return true;
        }
    }

    // 从源文件加载并压缩
    if (!m_sourcePath.empty() && PathFileExistsW(m_sourcePath.c_str())) {
        if (LoadAndCompressSource(device, commandList)) {
            m_isLoaded = true;
            return true;
        }
        std::cout << "LoadAndCompressSource failed for: " << m_name << std::endl;
    } else {
        std::cout << "Source path empty or not exists: " << WStringToString(m_sourcePath) << std::endl;
    }

    return false;
}

void TextureAsset::UnloadFromGPU() {
    // 释放SRV槽位
    if (m_srvIndex != UINT_MAX) {
        TextureManager::GetInstance().FreeSRVIndex(m_srvIndex);
        m_srvIndex = UINT_MAX;
    }

    m_resource.Reset();
    m_uploadHeap.Reset();
    m_srvCPU = {};
    m_srvGPU = {};
    m_isLoaded = false;
}

bool TextureAsset::LoadSourceToGPU(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) {
    if (m_isLoaded) return true;

    // 直接从源文件加载原图（不压缩）
    if (m_sourcePath.empty() || !PathFileExistsW(m_sourcePath.c_str())) {
        std::cout << "Source path empty or not exists" << std::endl;
        return false;
    }

    DirectX::ScratchImage sourceImage;
    DirectX::TexMetadata metadata;
    HRESULT hr;

    // 获取文件扩展名
    std::wstring ext = PathFindExtensionW(m_sourcePath.c_str());
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    if (ext == L".dds") {
        hr = DirectX::LoadFromDDSFile(m_sourcePath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, sourceImage);
    }
    else if (ext == L".hdr") {
        hr = DirectX::LoadFromHDRFile(m_sourcePath.c_str(), &metadata, sourceImage);
    }
    else {
        hr = DirectX::LoadFromWICFile(m_sourcePath.c_str(), DirectX::WIC_FLAGS_NONE, &metadata, sourceImage);
    }

    if (FAILED(hr)) {
        std::cout << "Failed to load source file" << std::endl;
        return false;
    }

    // 创建纹理资源（不压缩，使用原始格式）
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = static_cast<UINT64>(metadata.width);
    texDesc.Height = static_cast<UINT>(metadata.height);
    texDesc.DepthOrArraySize = static_cast<UINT16>(metadata.arraySize);
    texDesc.MipLevels = 1;  // 预览时只用1级mip
    texDesc.Format = metadata.format;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
    hr = device->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_resource));

    if (FAILED(hr)) {
        std::cout << "Failed to create texture resource" << std::endl;
        return false;
    }

    // 准备子资源数据（只用第一个mip）
    D3D12_SUBRESOURCE_DATA subresource = {};
    const DirectX::Image* img = sourceImage.GetImage(0, 0, 0);
    subresource.pData = img->pixels;
    subresource.RowPitch = static_cast<LONG_PTR>(img->rowPitch);
    subresource.SlicePitch = static_cast<LONG_PTR>(img->slicePitch);

    // 计算上传堆大小
    UINT64 uploadHeapSize = GetRequiredIntermediateSize(m_resource.Get(), 0, 1);

    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC uploadResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadHeapSize);

    hr = device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadResourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_uploadHeap));

    if (FAILED(hr)) {
        std::cout << "Failed to create upload heap" << std::endl;
        return false;
    }

    UpdateSubresources(commandList, m_resource.Get(), m_uploadHeap.Get(), 0, 0, 1, &subresource);

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);

    // 更新运行时信息
    m_runtimeInfo.width = static_cast<UINT>(metadata.width);
    m_runtimeInfo.height = static_cast<UINT>(metadata.height);
    m_runtimeInfo.mipLevels = 1;
    m_runtimeInfo.format = metadata.format;
    m_runtimeInfo.arraySize = static_cast<UINT>(metadata.arraySize);
    m_runtimeInfo.memorySize = img->slicePitch;

    CreateSRV(device);
    m_isLoaded = true;

    std::cout << "Loaded source texture: " << m_name
              << " (" << m_runtimeInfo.width << "x" << m_runtimeInfo.height << ")" << std::endl;
    return true;
}

bool TextureAsset::ApplyCompression(TextureCompressionFormat format,
                                     ID3D12Device* device,
                                     ID3D12GraphicsCommandList* commandList) {
    // 设置新格式
    m_desc.format = format;
    m_cacheValid = false;
    m_cacheDdsPath = GenerateCachePath();

    // 卸载当前资源
    UnloadFromGPU();

    // 如果选择不压缩，直接加载原图
    if (format == TextureCompressionFormat::None) {
        return LoadSourceToGPU(device, commandList);
    }

    // 执行压缩并加载
    return LoadAndCompressSource(device, commandList);
}

bool TextureAsset::Recompress(TextureCompressionFormat newFormat,
                               ID3D12Device* device,
                               ID3D12GraphicsCommandList* commandList) {
    // 更新格式
    m_desc.format = newFormat;
    m_cacheValid = false;

    // 卸载并重新加载
    UnloadFromGPU();
    return LoadToGPU(device, commandList);
}

// ========== 内部实现 ==========

std::string TextureAsset::CalculateSourceHash(const std::wstring& path) {
    return CalculateFileMD5(path);
}

std::wstring TextureAsset::GenerateCachePath() {
    if (m_sourceHash.empty()) {
        m_sourceHash = CalculateSourceHash(m_sourcePath);
    }

    // 在源图片同目录下创建TextureCache文件夹
    // 获取源文件所在目录
    std::wstring sourceDir = m_sourcePath;
    size_t lastSlash = sourceDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        sourceDir = sourceDir.substr(0, lastSlash + 1);
    }

    // 获取源文件名（不含扩展名）
    std::wstring fileName = m_sourcePath;
    if (lastSlash != std::wstring::npos) {
        fileName = fileName.substr(lastSlash + 1);
    }
    size_t dotPos = fileName.find_last_of(L'.');
    if (dotPos != std::wstring::npos) {
        fileName = fileName.substr(0, dotPos);
    }

    // 创建TextureCache目录路径
    std::wstring cacheDir = sourceDir + L"TextureCache\\";

    // 确保目录存在
    CreateDirectoryW(cacheDir.c_str(), nullptr);

    // 返回缓存文件路径：源目录/TextureCache/原文件名.dds
    return cacheDir + fileName + L".dds";
}

bool TextureAsset::LoadDDSFromCache(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) {
    // 使用DirectXTex加载DDS
    DirectX::TexMetadata metadata;
    DirectX::ScratchImage scratchImage;

    HRESULT hr = DirectX::LoadFromDDSFile(
        m_cacheDdsPath.c_str(),
        DirectX::DDS_FLAGS_NONE,
        &metadata,
        scratchImage
    );

    if (FAILED(hr)) {
        std::cout << "Failed to load DDS from cache: " << WStringToString(m_cacheDdsPath) << std::endl;
        return false;
    }

    bool bIsCube = (metadata.miscFlags & DirectX::TEX_MISC_TEXTURECUBE) != 0;

    // 手动创建纹理资源
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = static_cast<UINT64>(metadata.width);
    texDesc.Height = static_cast<UINT>(metadata.height);
    texDesc.DepthOrArraySize = static_cast<UINT16>(metadata.arraySize);
    texDesc.MipLevels = static_cast<UINT16>(metadata.mipLevels);
    texDesc.Format = metadata.format;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
    hr = device->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_resource)
    );

    if (FAILED(hr)) {
        std::cout << "Failed to create texture resource" << std::endl;
        return false;
    }

    // 手动准备子资源数据
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    subresources.reserve(scratchImage.GetImageCount());

    const DirectX::Image* images = scratchImage.GetImages();
    for (size_t i = 0; i < scratchImage.GetImageCount(); ++i) {
        D3D12_SUBRESOURCE_DATA subresource = {};
        subresource.pData = images[i].pixels;
        subresource.RowPitch = static_cast<LONG_PTR>(images[i].rowPitch);
        subresource.SlicePitch = static_cast<LONG_PTR>(images[i].slicePitch);
        subresources.push_back(subresource);
    }

    // 计算上传堆大小
    UINT64 uploadHeapSize = GetRequiredIntermediateSize(
        m_resource.Get(), 0, static_cast<UINT>(subresources.size()));

    // 创建上传堆
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC uploadResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadHeapSize);

    hr = device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadResourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_uploadHeap)
    );

    if (FAILED(hr)) {
        std::cout << "Failed to create upload heap" << std::endl;
        return false;
    }

    // 上传数据
    UpdateSubresources(
        commandList,
        m_resource.Get(),
        m_uploadHeap.Get(),
        0, 0,
        static_cast<UINT>(subresources.size()),
        subresources.data()
    );

    // 资源状态转换
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    commandList->ResourceBarrier(1, &barrier);

    // 更新运行时信息
    m_runtimeInfo.width = static_cast<UINT>(metadata.width);
    m_runtimeInfo.height = static_cast<UINT>(metadata.height);
    m_runtimeInfo.mipLevels = static_cast<UINT>(metadata.mipLevels);
    m_runtimeInfo.format = metadata.format;
    m_runtimeInfo.arraySize = static_cast<UINT>(metadata.arraySize);
    m_runtimeInfo.memorySize = CalculateMemorySize(
        m_runtimeInfo.width, m_runtimeInfo.height,
        m_runtimeInfo.mipLevels, m_desc.format);

    // 创建SRV
    CreateSRV(device);

    std::cout << "Loaded texture from cache: " << m_name
              << " (" << m_runtimeInfo.width << "x" << m_runtimeInfo.height << ")" << std::endl;

    return true;
}

bool TextureAsset::LoadAndCompressSource(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) {
    // Debug output
    std::cout << "LoadAndCompressSource called:" << std::endl;
    std::cout << "  s_useNVTT = " << (s_useNVTT ? "true" : "false") << std::endl;
    std::cout << "  format = " << GetFormatName(m_desc.format) << std::endl;
    std::cout << "  sourcePath = " << WStringToString(m_sourcePath) << std::endl;

    // 如果启用NVTT压缩且需要压缩，优先使用NVTT
    if (s_useNVTT && m_desc.format != TextureCompressionFormat::None) {
        TextureCompressor& compressor = TextureCompressor::GetInstance();
        std::cout << "  NVTT available = " << (compressor.IsNVTTAvailable() ? "true" : "false") << std::endl;
        if (compressor.IsNVTTAvailable()) {
            std::cout << "Using NVIDIA Texture Tools for compression (Quality: "
                      << TextureCompressor::GetNVTTQualityName(compressor.GetNVTTQuality()) << ")..." << std::endl;

            // 确保缓存目录存在
            std::wstring cacheDir = m_cacheDdsPath.substr(0, m_cacheDdsPath.find_last_of(L"\\/"));
            CreateDirectoryW(cacheDir.c_str(), nullptr);

            // 使用NVTT压缩，传入当前质量设置
            if (compressor.CompressWithNVTT(m_sourcePath, m_cacheDdsPath,
                                            m_desc.format, m_desc.generateMips, m_desc.sRGB,
                                            compressor.GetNVTTQuality())) {
                m_cacheValid = true;
                std::cout << "NVTT compression successful, loading from cache..." << std::endl;
                return LoadDDSFromCache(device, commandList);
            }
            std::cout << "NVTT compression failed, falling back to DirectXTex..." << std::endl;
        }
    }

    // 使用DirectXTex加载源文件（回退方案或不需要压缩时）
    DirectX::ScratchImage sourceImage;
    DirectX::TexMetadata metadata;
    HRESULT hr;

    // 获取文件扩展名
    std::wstring ext = PathFindExtensionW(m_sourcePath.c_str());
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    if (ext == L".dds") {
        hr = DirectX::LoadFromDDSFile(m_sourcePath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, sourceImage);
    }
    else if (ext == L".hdr") {
        hr = DirectX::LoadFromHDRFile(m_sourcePath.c_str(), &metadata, sourceImage);
    }
    else {
        // PNG, JPG, BMP等使用WIC
        hr = DirectX::LoadFromWICFile(m_sourcePath.c_str(), DirectX::WIC_FLAGS_NONE, &metadata, sourceImage);
    }

    if (FAILED(hr)) {
        std::cout << "Failed to load source file: " << WStringToString(m_sourcePath) << " HRESULT: " << hr << std::endl;
        return false;
    }

    // 生成Mipmap（如果需要）
    DirectX::ScratchImage mipChain;
    if (m_desc.generateMips && metadata.mipLevels == 1) {
        hr = DirectX::GenerateMipMaps(
            sourceImage.GetImages(), sourceImage.GetImageCount(), sourceImage.GetMetadata(),
            DirectX::TEX_FILTER_DEFAULT, 0, mipChain);
        if (SUCCEEDED(hr)) {
            sourceImage = std::move(mipChain);
            metadata = sourceImage.GetMetadata();
        }
    }

    // 压缩（如果需要）
    DirectX::ScratchImage compressedImage;
    DXGI_FORMAT targetFormat = GetDXGIFormat(m_desc.format, m_desc.sRGB);

    if (m_desc.format != TextureCompressionFormat::None &&
        !DirectX::IsCompressed(metadata.format)) {
        hr = DirectX::Compress(
            sourceImage.GetImages(), sourceImage.GetImageCount(), sourceImage.GetMetadata(),
            targetFormat,
            DirectX::TEX_COMPRESS_DEFAULT | DirectX::TEX_COMPRESS_PARALLEL,
            DirectX::TEX_THRESHOLD_DEFAULT,
            compressedImage
        );
        if (SUCCEEDED(hr)) {
            sourceImage = std::move(compressedImage);
            metadata = sourceImage.GetMetadata();
        }
        else {
            std::cout << "Compression failed, using uncompressed. HRESULT: " << hr << std::endl;
        }
    }

    // 保存到缓存DDS
    hr = DirectX::SaveToDDSFile(
        sourceImage.GetImages(), sourceImage.GetImageCount(), sourceImage.GetMetadata(),
        DirectX::DDS_FLAGS_NONE, m_cacheDdsPath.c_str()
    );

    if (SUCCEEDED(hr)) {
        m_cacheValid = true;
        std::cout << "Cache saved: " << WStringToString(m_cacheDdsPath) << std::endl;
        // 从新缓存加载
        return LoadDDSFromCache(device, commandList);
    }
    else {
        std::cout << "Failed to save DDS cache. HRESULT: " << hr << std::endl;
        return false;
    }
}

void TextureAsset::CreateSRV(ID3D12Device* device) {
    // 分配SRV槽位
    m_srvIndex = TextureManager::GetInstance().AllocateSRVIndex();
    if (m_srvIndex == UINT_MAX) {
        std::cout << "Failed to allocate SRV index" << std::endl;
        return;
    }

    // 获取句柄
    m_srvCPU = TextureManager::GetInstance().GetSRVCPUHandle(m_srvIndex);
    m_srvGPU = TextureManager::GetInstance().GetSRVGPUHandle(m_srvIndex);

    // 创建SRV描述
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = m_runtimeInfo.format;

    switch (m_desc.type) {
    case TextureType::Texture2D:
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = m_runtimeInfo.mipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        break;

    case TextureType::TextureCube:
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MipLevels = m_runtimeInfo.mipLevels;
        srvDesc.TextureCube.MostDetailedMip = 0;
        break;

    case TextureType::Texture2DArray:
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MipLevels = m_runtimeInfo.mipLevels;
        srvDesc.Texture2DArray.ArraySize = m_runtimeInfo.arraySize;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        break;
    }

    device->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_srvCPU);
}

// ========== XML解析 ==========

bool TextureAsset::ParseAssetXML(const std::wstring& xmlPath) {
    HRESULT hrInit = CoInitialize(nullptr);
    bool comInitialized = SUCCEEDED(hrInit);

    IXMLDOMDocument* pDoc = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IXMLDOMDocument, (void**)&pDoc);
    if (FAILED(hr) || !pDoc) {
        std::cout << "Failed to create XML document" << std::endl;
        if (comInitialized) CoUninitialize();
        return false;
    }

    VARIANT_BOOL success;
    hr = pDoc->load(_variant_t(xmlPath.c_str()), &success);
    if (FAILED(hr) || success != VARIANT_TRUE) {
        pDoc->Release();
        std::cout << "Failed to load XML: " << WStringToString(xmlPath) << std::endl;
        if (comInitialized) CoUninitialize();
        return false;
    }

    IXMLDOMElement* pRoot = nullptr;
    pDoc->get_documentElement(&pRoot);
    if (!pRoot) {
        pDoc->Release();
        if (comInitialized) CoUninitialize();
        return false;
    }

    // 解析name属性
    IXMLDOMNamedNodeMap* pRootAttrs = nullptr;
    pRoot->get_attributes(&pRootAttrs);
    if (pRootAttrs) {
        IXMLDOMNode* pNameAttr = nullptr;
        pRootAttrs->getNamedItem(_bstr_t("name"), &pNameAttr);
        if (pNameAttr) {
            BSTR val = nullptr;
            pNameAttr->get_text(&val);
            if (val) {
                m_name = BSTRToString(val);
                SysFreeString(val);
            }
            pNameAttr->Release();
        }
        pRootAttrs->Release();
    }

    // 解析Source节点
    IXMLDOMNodeList* pSourceList = nullptr;
    pRoot->getElementsByTagName(_bstr_t("Source"), &pSourceList);
    if (pSourceList) {
        IXMLDOMNode* pSource = nullptr;
        pSourceList->get_item(0, &pSource);
        if (pSource) {
            // 安全获取IXMLDOMElement接口
            IXMLDOMElement* pSourceElem = nullptr;
            hr = pSource->QueryInterface(IID_IXMLDOMElement, (void**)&pSourceElem);
            if (SUCCEEDED(hr) && pSourceElem) {
                // Path
                IXMLDOMNodeList* pPathList = nullptr;
                pSourceElem->getElementsByTagName(_bstr_t("Path"), &pPathList);
                if (pPathList) {
                    IXMLDOMNode* pPath = nullptr;
                    pPathList->get_item(0, &pPath);
                    if (pPath) {
                        BSTR val = nullptr;
                        pPath->get_text(&val);
                        if (val) {
                            m_sourcePath = val;
                            SysFreeString(val);
                        }
                        pPath->Release();
                    }
                    pPathList->Release();
                }

                // SourceHash
                IXMLDOMNodeList* pHashList = nullptr;
                pSourceElem->getElementsByTagName(_bstr_t("SourceHash"), &pHashList);
                if (pHashList) {
                    IXMLDOMNode* pHash = nullptr;
                    pHashList->get_item(0, &pHash);
                    if (pHash) {
                        BSTR val = nullptr;
                        pHash->get_text(&val);
                        if (val) {
                            m_sourceHash = BSTRToString(val);
                            SysFreeString(val);
                        }
                        pHash->Release();
                    }
                    pHashList->Release();
                }
                pSourceElem->Release();
            }
            pSource->Release();
        }
        pSourceList->Release();
    }

    // 解析Compression节点
    IXMLDOMNodeList* pCompList = nullptr;
    pRoot->getElementsByTagName(_bstr_t("Compression"), &pCompList);
    if (pCompList) {
        IXMLDOMNode* pComp = nullptr;
        pCompList->get_item(0, &pComp);
        if (pComp) {
            // 安全获取IXMLDOMElement接口
            IXMLDOMElement* pCompElem = nullptr;
            hr = pComp->QueryInterface(IID_IXMLDOMElement, (void**)&pCompElem);
            if (SUCCEEDED(hr) && pCompElem) {
                // Format
                IXMLDOMNodeList* pFormatList = nullptr;
                pCompElem->getElementsByTagName(_bstr_t("Format"), &pFormatList);
                if (pFormatList) {
                    IXMLDOMNode* pFormat = nullptr;
                    pFormatList->get_item(0, &pFormat);
                    if (pFormat) {
                        BSTR val = nullptr;
                        pFormat->get_text(&val);
                        if (val) {
                            std::string formatStr = BSTRToString(val);
                            if (formatStr == "BC1") m_desc.format = TextureCompressionFormat::BC1;
                            else if (formatStr == "BC3") m_desc.format = TextureCompressionFormat::BC3;
                            else if (formatStr == "BC5") m_desc.format = TextureCompressionFormat::BC5;
                            else if (formatStr == "BC7") m_desc.format = TextureCompressionFormat::BC7;
                            else if (formatStr == "BC6H") m_desc.format = TextureCompressionFormat::BC6H;
                            else m_desc.format = TextureCompressionFormat::None;
                            SysFreeString(val);
                        }
                        pFormat->Release();
                    }
                    pFormatList->Release();
                }

                // GenerateMips
                IXMLDOMNodeList* pMipsList = nullptr;
                pCompElem->getElementsByTagName(_bstr_t("GenerateMips"), &pMipsList);
                if (pMipsList) {
                    IXMLDOMNode* pMips = nullptr;
                    pMipsList->get_item(0, &pMips);
                    if (pMips) {
                        BSTR val = nullptr;
                        pMips->get_text(&val);
                        if (val) {
                            m_desc.generateMips = (BSTRToString(val) == "true");
                            SysFreeString(val);
                        }
                        pMips->Release();
                    }
                    pMipsList->Release();
                }

                // sRGB
                IXMLDOMNodeList* pSRGBList = nullptr;
                pCompElem->getElementsByTagName(_bstr_t("sRGB"), &pSRGBList);
                if (pSRGBList) {
                    IXMLDOMNode* pSRGB = nullptr;
                    pSRGBList->get_item(0, &pSRGB);
                    if (pSRGB) {
                        BSTR val = nullptr;
                        pSRGB->get_text(&val);
                        if (val) {
                            m_desc.sRGB = (BSTRToString(val) == "true");
                            SysFreeString(val);
                        }
                        pSRGB->Release();
                    }
                    pSRGBList->Release();
                }
                pCompElem->Release();
            }
            pComp->Release();
        }
        pCompList->Release();
    }

    // 解析Cache节点
    IXMLDOMNodeList* pCacheList = nullptr;
    pRoot->getElementsByTagName(_bstr_t("Cache"), &pCacheList);
    if (pCacheList) {
        IXMLDOMNode* pCache = nullptr;
        pCacheList->get_item(0, &pCache);
        if (pCache) {
            // 安全获取IXMLDOMElement接口
            IXMLDOMElement* pCacheElem = nullptr;
            hr = pCache->QueryInterface(IID_IXMLDOMElement, (void**)&pCacheElem);
            if (SUCCEEDED(hr) && pCacheElem) {
                // DdsPath
                IXMLDOMNodeList* pDdsPathList = nullptr;
                pCacheElem->getElementsByTagName(_bstr_t("DdsPath"), &pDdsPathList);
                if (pDdsPathList) {
                    IXMLDOMNode* pDdsPath = nullptr;
                    pDdsPathList->get_item(0, &pDdsPath);
                    if (pDdsPath) {
                        BSTR val = nullptr;
                        pDdsPath->get_text(&val);
                        if (val) {
                            m_cacheDdsPath = val;
                            SysFreeString(val);
                        }
                        pDdsPath->Release();
                    }
                    pDdsPathList->Release();
                }

                // CacheValid
                IXMLDOMNodeList* pValidList = nullptr;
                pCacheElem->getElementsByTagName(_bstr_t("CacheValid"), &pValidList);
                if (pValidList) {
                    IXMLDOMNode* pValid = nullptr;
                    pValidList->get_item(0, &pValid);
                    if (pValid) {
                        BSTR val = nullptr;
                        pValid->get_text(&val);
                        if (val) {
                            m_cacheValid = (BSTRToString(val) == "true");
                            SysFreeString(val);
                        }
                        pValid->Release();
                    }
                    pValidList->Release();
                }
                pCacheElem->Release();
            }
            pCache->Release();
        }
        pCacheList->Release();
    }

    // 解析TextureType
    IXMLDOMNodeList* pTypeList = nullptr;
    pRoot->getElementsByTagName(_bstr_t("TextureType"), &pTypeList);
    if (pTypeList) {
        IXMLDOMNode* pType = nullptr;
        pTypeList->get_item(0, &pType);
        if (pType) {
            BSTR val = nullptr;
            pType->get_text(&val);
            if (val) {
                std::string typeStr = BSTRToString(val);
                if (typeStr == "TextureCube") m_desc.type = TextureType::TextureCube;
                else if (typeStr == "Texture2DArray") m_desc.type = TextureType::Texture2DArray;
                else m_desc.type = TextureType::Texture2D;
                SysFreeString(val);
            }
            pType->Release();
        }
        pTypeList->Release();
    }

    pRoot->Release();
    pDoc->Release();
    if (comInitialized) CoUninitialize();

    // 验证缓存
    if (m_cacheValid && !m_sourceHash.empty() && !m_sourcePath.empty()) {
        std::string currentHash = CalculateSourceHash(m_sourcePath);
        if (currentHash != m_sourceHash) {
            m_cacheValid = false;  // 源文件已更改
            std::cout << "Cache invalidated: source file changed" << std::endl;
        }
    }

    return true;
}

bool TextureAsset::WriteAssetXML(const std::wstring& xmlPath) {
    std::wofstream file(xmlPath);
    if (!file.is_open()) {
        std::cout << "Failed to open file for writing: " << WStringToString(xmlPath) << std::endl;
        return false;
    }

    file << L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    file << L"<TextureAsset name=\"" << StringToWString(m_name) << L"\" version=\"1.0\">\n";

    // Source
    file << L"  <Source>\n";
    file << L"    <Path>" << m_sourcePath << L"</Path>\n";
    file << L"    <SourceHash>" << StringToWString(m_sourceHash) << L"</SourceHash>\n";
    file << L"  </Source>\n";

    // TextureType
    file << L"  <TextureType>" << StringToWString(GetTypeName(m_desc.type)) << L"</TextureType>\n";

    // Compression
    file << L"  <Compression>\n";
    file << L"    <Format>" << StringToWString(GetFormatName(m_desc.format)) << L"</Format>\n";
    file << L"    <GenerateMips>" << (m_desc.generateMips ? L"true" : L"false") << L"</GenerateMips>\n";
    file << L"    <sRGB>" << (m_desc.sRGB ? L"true" : L"false") << L"</sRGB>\n";
    file << L"  </Compression>\n";

    // Cache
    file << L"  <Cache>\n";
    file << L"    <DdsPath>" << m_cacheDdsPath << L"</DdsPath>\n";
    file << L"    <CacheValid>" << (m_cacheValid ? L"true" : L"false") << L"</CacheValid>\n";
    file << L"  </Cache>\n";

    // RuntimeInfo
    if (m_isLoaded) {
        file << L"  <RuntimeInfo>\n";
        file << L"    <Width>" << m_runtimeInfo.width << L"</Width>\n";
        file << L"    <Height>" << m_runtimeInfo.height << L"</Height>\n";
        file << L"    <MipLevels>" << m_runtimeInfo.mipLevels << L"</MipLevels>\n";
        file << L"    <MemorySize>" << m_runtimeInfo.memorySize << L"</MemorySize>\n";
        file << L"  </RuntimeInfo>\n";
    }

    file << L"</TextureAsset>\n";
    file.close();

    return true;
}
