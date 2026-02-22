// TextureCompressor.cpp
// GPU纹理压缩器实现

#define NOMINMAX

#include "public/Texture/TextureCompressor.h"
#include "public/BattleFireDirect.h"
#include "public/PathUtils.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <iostream>
#include <algorithm>

// ========== 单例实现 ==========

TextureCompressor& TextureCompressor::GetInstance() {
    static TextureCompressor instance;
    return instance;
}

// ========== 静态辅助函数 ==========

UINT TextureCompressor::GetBlockSize(TextureCompressionFormat format) {
    switch (format) {
    case TextureCompressionFormat::BC1:
        return 8;   // 8字节/4x4块
    case TextureCompressionFormat::BC3:
    case TextureCompressionFormat::BC5:
    case TextureCompressionFormat::BC7:
    case TextureCompressionFormat::BC6H:
        return 16;  // 16字节/4x4块
    default:
        return 0;
    }
}

DXGI_FORMAT TextureCompressor::GetCompressedFormat(TextureCompressionFormat format, bool sRGB) {
    switch (format) {
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
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool TextureCompressor::SupportsGPUCompression(TextureCompressionFormat format) {
    // BC1, BC3, BC5支持GPU压缩
    // BC7和BC6H算法复杂，优先使用CPU压缩
    switch (format) {
    case TextureCompressionFormat::BC1:
    case TextureCompressionFormat::BC3:
    case TextureCompressionFormat::BC5:
        return true;
    default:
        return false;
    }
}

// ========== 初始化和清理 ==========

bool TextureCompressor::Initialize(ID3D12Device* device) {
    if (!device) {
        std::cout << "TextureCompressor::Initialize - Invalid device" << std::endl;
        return false;
    }

    m_device = device;

    // 设置NVTT路径（在项目根目录的 Tools/ 下查找）
    if (m_nvttPath.empty()) {
        m_nvttPath = GetProjectRoot() + L"Tools\\NVIDIA Texture Tools\\nvtt_export.exe";
    }

    // 编译计算着色器
    if (!CompileComputeShaders()) {
        std::cout << "TextureCompressor: Failed to compile compute shaders" << std::endl;
        // 不返回false，因为可以回退到CPU压缩
    }

    // 创建根签名
    if (!CreateComputeRootSignature()) {
        std::cout << "TextureCompressor: Failed to create root signature" << std::endl;
    }

    // 创建描述符堆
    CreateDescriptorHeaps();

    // 创建常量缓冲
    CreateConstantBuffer();

    std::cout << "TextureCompressor initialized" << std::endl;
    return true;
}

void TextureCompressor::Shutdown() {
    // 取消常量缓冲映射
    if (m_constantBuffer && m_mappedConstantBuffer) {
        m_constantBuffer->Unmap(0, nullptr);
        m_mappedConstantBuffer = nullptr;
    }

    // ComPtr会自动释放资源
    m_bc1PSO.Reset();
    m_bc3PSO.Reset();
    m_bc5PSO.Reset();
    m_bc7PSO.Reset();
    m_bc6hPSO.Reset();
    m_computeRootSignature.Reset();
    m_srvHeap.Reset();
    m_uavHeap.Reset();
    m_samplerHeap.Reset();
    m_constantBuffer.Reset();

    m_device = nullptr;
    std::cout << "TextureCompressor shutdown" << std::endl;
}

// ========== 内部初始化 ==========

bool TextureCompressor::CompileComputeShaders() {
    UINT compileFlags = 0;
#ifdef _DEBUG
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr;

    // 编译BC1压缩着色器
    hr = D3DCompileFromFile(
        L"Engine/Shader/Compression/BC1Compress.hlsl",
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "CSMain", "cs_5_0",
        compileFlags, 0,
        &m_bc1ShaderBlob, &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cout << "BC1 shader compile error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
        std::cout << "BC1 shader not found, GPU compression will use CPU fallback" << std::endl;
    }

    // 编译BC3压缩着色器
    hr = D3DCompileFromFile(
        L"Engine/Shader/Compression/BC3Compress.hlsl",
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "CSMain", "cs_5_0",
        compileFlags, 0,
        &m_bc3ShaderBlob, &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cout << "BC3 shader compile error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
    }

    // 编译BC5压缩着色器
    hr = D3DCompileFromFile(
        L"Engine/Shader/Compression/BC5Compress.hlsl",
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "CSMain", "cs_5_0",
        compileFlags, 0,
        &m_bc5ShaderBlob, &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cout << "BC5 shader compile error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
    }

    return true;  // 即使部分失败也返回true，可以回退到CPU
}

bool TextureCompressor::CreateComputeRootSignature() {
    // 根参数：
    // 0: CBV (压缩参数)
    // 1: SRV (源纹理)
    // 2: UAV (输出压缩纹理)
    // 3: Sampler (采样器)

    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE1 uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE1 samplerRange;
    samplerRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);

    CD3DX12_ROOT_PARAMETER1 rootParams[4];
    rootParams[0].InitAsConstantBufferView(0);              // b0: 常量缓冲
    rootParams[1].InitAsDescriptorTable(1, &srvRange);      // t0: 源纹理
    rootParams[2].InitAsDescriptorTable(1, &uavRange);      // u0: 输出纹理
    rootParams[3].InitAsDescriptorTable(1, &samplerRange);  // s0: 采样器

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init_1_1(_countof(rootParams), rootParams, 0, nullptr,
                         D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rootSigDesc,
                                                        D3D_ROOT_SIGNATURE_VERSION_1_1,
                                                        &signature, &error);
    if (FAILED(hr)) {
        if (error) {
            std::cout << "Root signature serialize error: " << (char*)error->GetBufferPointer() << std::endl;
        }
        return false;
    }

    hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(),
                                        signature->GetBufferSize(),
                                        IID_PPV_ARGS(&m_computeRootSignature));
    if (FAILED(hr)) {
        std::cout << "Failed to create compute root signature" << std::endl;
        return false;
    }

    // 创建PSO（仅当着色器编译成功时）
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_computeRootSignature.Get();

    if (m_bc1ShaderBlob) {
        psoDesc.CS = { m_bc1ShaderBlob->GetBufferPointer(), m_bc1ShaderBlob->GetBufferSize() };
        hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_bc1PSO));
        if (FAILED(hr)) {
            std::cout << "Failed to create BC1 PSO" << std::endl;
        }
    }

    if (m_bc3ShaderBlob) {
        psoDesc.CS = { m_bc3ShaderBlob->GetBufferPointer(), m_bc3ShaderBlob->GetBufferSize() };
        hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_bc3PSO));
        if (FAILED(hr)) {
            std::cout << "Failed to create BC3 PSO" << std::endl;
        }
    }

    if (m_bc5ShaderBlob) {
        psoDesc.CS = { m_bc5ShaderBlob->GetBufferPointer(), m_bc5ShaderBlob->GetBufferSize() };
        hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_bc5PSO));
        if (FAILED(hr)) {
            std::cout << "Failed to create BC5 PSO" << std::endl;
        }
    }

    return true;
}

void TextureCompressor::CreateDescriptorHeaps() {
    // SRV堆
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 16;  // 支持多个mip级别
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));

    // UAV堆
    D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
    uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uavHeapDesc.NumDescriptors = 16;
    uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    m_device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&m_uavHeap));

    // Sampler堆
    D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
    samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplerHeapDesc.NumDescriptors = 1;
    samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    m_device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&m_samplerHeap));

    // 创建采样器
    D3D12_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.MipLODBias = 0;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;

    m_device->CreateSampler(&samplerDesc, m_samplerHeap->GetCPUDescriptorHandleForHeapStart());

    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void TextureCompressor::CreateConstantBuffer() {
    // 创建常量缓冲（持久映射）
    UINT bufferSize = (sizeof(CompressionParams) + 255) & ~255;  // 256字节对齐

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

    m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_constantBuffer)
    );

    // 持久映射
    CD3DX12_RANGE readRange(0, 0);
    m_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedConstantBuffer));
}

// ========== GPU压缩 ==========

bool TextureCompressor::CompressGPU(ID3D12GraphicsCommandList* commandList,
                                     ID3D12Resource* sourceTexture,
                                     TextureCompressionFormat format,
                                     bool sRGB,
                                     ComPtr<ID3D12Resource>& outCompressedTexture) {
    // 检查是否支持GPU压缩
    if (!SupportsGPUCompression(format)) {
        std::cout << "Format does not support GPU compression, use CPU fallback" << std::endl;
        return false;
    }

    // 获取对应的PSO
    ID3D12PipelineState* pso = nullptr;
    switch (format) {
    case TextureCompressionFormat::BC1:
        pso = m_bc1PSO.Get();
        break;
    case TextureCompressionFormat::BC3:
        pso = m_bc3PSO.Get();
        break;
    case TextureCompressionFormat::BC5:
        pso = m_bc5PSO.Get();
        break;
    default:
        break;
    }

    if (!pso) {
        std::cout << "GPU compression PSO not available" << std::endl;
        return false;
    }

    // 获取源纹理信息
    D3D12_RESOURCE_DESC srcDesc = sourceTexture->GetDesc();
    UINT width = static_cast<UINT>(srcDesc.Width);
    UINT height = srcDesc.Height;

    // 计算BC块数量
    UINT blockCountX = (width + 3) / 4;
    UINT blockCountY = (height + 3) / 4;

    // 创建输出纹理
    DXGI_FORMAT compressedFormat = GetCompressedFormat(format, sRGB);
    D3D12_RESOURCE_DESC destDesc = {};
    destDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    destDesc.Width = width;
    destDesc.Height = height;
    destDesc.DepthOrArraySize = 1;
    destDesc.MipLevels = 1;
    destDesc.Format = compressedFormat;
    destDesc.SampleDesc.Count = 1;
    destDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &destDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&outCompressedTexture)
    );

    if (FAILED(hr)) {
        std::cout << "Failed to create compressed texture" << std::endl;
        return false;
    }

    // 执行压缩
    return CompressMipLevel(commandList, sourceTexture, outCompressedTexture.Get(),
                            0, width, height, format, sRGB);
}

bool TextureCompressor::CompressMipLevel(ID3D12GraphicsCommandList* commandList,
                                          ID3D12Resource* sourceTexture,
                                          ID3D12Resource* destTexture,
                                          UINT mipLevel,
                                          UINT width,
                                          UINT height,
                                          TextureCompressionFormat format,
                                          bool sRGB) {
    // 获取PSO
    ID3D12PipelineState* pso = nullptr;
    switch (format) {
    case TextureCompressionFormat::BC1:
        pso = m_bc1PSO.Get();
        break;
    case TextureCompressionFormat::BC3:
        pso = m_bc3PSO.Get();
        break;
    case TextureCompressionFormat::BC5:
        pso = m_bc5PSO.Get();
        break;
    default:
        return false;
    }

    if (!pso) return false;

    // 更新常量缓冲
    UINT blockCountX = (width + 3) / 4;
    UINT blockCountY = (height + 3) / 4;

    m_mappedConstantBuffer->textureWidth = width;
    m_mappedConstantBuffer->textureHeight = height;
    m_mappedConstantBuffer->blockCountX = blockCountX;
    m_mappedConstantBuffer->blockCountY = blockCountY;
    m_mappedConstantBuffer->mipLevel = mipLevel;
    m_mappedConstantBuffer->isSRGB = sRGB ? 1 : 0;

    // 创建源纹理SRV
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = sourceTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = mipLevel;
    m_device->CreateShaderResourceView(sourceTexture, &srvDesc, srvHandle);

    // 创建输出纹理UAV
    CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle(m_uavHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = destTexture->GetDesc().Format;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = mipLevel;
    m_device->CreateUnorderedAccessView(destTexture, nullptr, &uavDesc, uavHandle);

    // 设置计算着色器
    commandList->SetPipelineState(pso);
    commandList->SetComputeRootSignature(m_computeRootSignature.Get());

    // 设置描述符堆
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get(), m_samplerHeap.Get() };
    commandList->SetDescriptorHeaps(2, heaps);

    // 绑定资源
    commandList->SetComputeRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootDescriptorTable(1, srvGpuHandle);

    // 切换到UAV堆
    ID3D12DescriptorHeap* uavHeaps[] = { m_uavHeap.Get() };
    commandList->SetDescriptorHeaps(1, uavHeaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE uavGpuHandle(m_uavHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootDescriptorTable(2, uavGpuHandle);

    // 切换到Sampler堆
    ID3D12DescriptorHeap* samplerHeaps[] = { m_samplerHeap.Get() };
    commandList->SetDescriptorHeaps(1, samplerHeaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE samplerGpuHandle(m_samplerHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootDescriptorTable(3, samplerGpuHandle);

    // 分发计算（每个线程组处理8x8个BC块）
    UINT dispatchX = (blockCountX + THREAD_GROUP_SIZE - 1) / THREAD_GROUP_SIZE;
    UINT dispatchY = (blockCountY + THREAD_GROUP_SIZE - 1) / THREAD_GROUP_SIZE;
    commandList->Dispatch(dispatchX, dispatchY, 1);

    return true;
}

bool TextureCompressor::CompressGPUWithMips(ID3D12GraphicsCommandList* commandList,
                                             ID3D12Resource* sourceTexture,
                                             UINT mipLevels,
                                             TextureCompressionFormat format,
                                             bool sRGB,
                                             ComPtr<ID3D12Resource>& outCompressedTexture) {
    // TODO: 实现带mipmap的GPU压缩
    // 需要为每个mip级别分别执行压缩
    return false;
}

// ========== CPU压缩 ==========

bool TextureCompressor::CompressCPU(const DirectX::ScratchImage& sourceImage,
                                     TextureCompressionFormat format,
                                     bool sRGB,
                                     TextureCompressionQuality quality,
                                     DirectX::ScratchImage& outCompressedImage) {
    if (format == TextureCompressionFormat::None) {
        // 无需压缩，直接复制
        HRESULT hr = outCompressedImage.InitializeFromImage(*sourceImage.GetImage(0, 0, 0));
        return SUCCEEDED(hr);
    }

    DXGI_FORMAT targetFormat = GetCompressedFormat(format, sRGB);
    if (targetFormat == DXGI_FORMAT_UNKNOWN) {
        std::cout << "Unknown compression format" << std::endl;
        return false;
    }

    // 设置压缩标志
    DirectX::TEX_COMPRESS_FLAGS compressFlags = DirectX::TEX_COMPRESS_PARALLEL;

    switch (quality) {
    case TextureCompressionQuality::Fast:
        // 使用默认快速压缩
        break;
    case TextureCompressionQuality::Normal:
        compressFlags |= DirectX::TEX_COMPRESS_DEFAULT;
        break;
    case TextureCompressionQuality::High:
        // BC7使用高质量模式
        if (format == TextureCompressionFormat::BC7) {
            compressFlags |= DirectX::TEX_COMPRESS_BC7_QUICK;
        }
        break;
    }

    // 执行压缩
    HRESULT hr = DirectX::Compress(
        sourceImage.GetImages(),
        sourceImage.GetImageCount(),
        sourceImage.GetMetadata(),
        targetFormat,
        compressFlags,
        DirectX::TEX_THRESHOLD_DEFAULT,
        outCompressedImage
    );

    if (FAILED(hr)) {
        std::cout << "DirectXTex compression failed: " << std::hex << hr << std::endl;
        return false;
    }

    return true;
}

bool TextureCompressor::CompressAndSaveDDS(const std::wstring& sourcePath,
                                            const std::wstring& outputDdsPath,
                                            TextureCompressionFormat format,
                                            bool generateMips,
                                            bool sRGB,
                                            TextureCompressionQuality quality) {
    // 加载源文件
    DirectX::ScratchImage sourceImage;
    DirectX::TexMetadata metadata;
    HRESULT hr;

    // 获取文件扩展名
    std::wstring ext = sourcePath.substr(sourcePath.find_last_of(L'.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    if (ext == L".dds") {
        hr = DirectX::LoadFromDDSFile(sourcePath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, sourceImage);
    }
    else if (ext == L".hdr") {
        hr = DirectX::LoadFromHDRFile(sourcePath.c_str(), &metadata, sourceImage);
    }
    else {
        hr = DirectX::LoadFromWICFile(sourcePath.c_str(), DirectX::WIC_FLAGS_NONE, &metadata, sourceImage);
    }

    if (FAILED(hr)) {
        std::cout << "Failed to load source image" << std::endl;
        return false;
    }

    // 生成mipmap（如果需要）
    DirectX::ScratchImage mipChain;
    if (generateMips && metadata.mipLevels == 1) {
        hr = DirectX::GenerateMipMaps(
            sourceImage.GetImages(),
            sourceImage.GetImageCount(),
            sourceImage.GetMetadata(),
            DirectX::TEX_FILTER_DEFAULT,
            0,  // 生成完整mipchain
            mipChain
        );

        if (SUCCEEDED(hr)) {
            sourceImage = std::move(mipChain);
            metadata = sourceImage.GetMetadata();
        }
    }

    // 压缩
    DirectX::ScratchImage compressedImage;
    if (format != TextureCompressionFormat::None) {
        if (!CompressCPU(sourceImage, format, sRGB, quality, compressedImage)) {
            std::cout << "Compression failed" << std::endl;
            return false;
        }
    }
    else {
        compressedImage = std::move(sourceImage);
    }

    // 保存为DDS
    hr = DirectX::SaveToDDSFile(
        compressedImage.GetImages(),
        compressedImage.GetImageCount(),
        compressedImage.GetMetadata(),
        DirectX::DDS_FLAGS_NONE,
        outputDdsPath.c_str()
    );

    if (FAILED(hr)) {
        std::cout << "Failed to save DDS file" << std::endl;
        return false;
    }

    std::wcout << L"Compressed and saved: " << outputDdsPath << std::endl;
    return true;
}

// ========== NVIDIA Texture Tools 压缩 ==========

const char* TextureCompressor::GetNVTTQualityName(NVTTQuality quality) {
    switch (quality) {
    case NVTTQuality::Fastest:    return "Fastest";
    case NVTTQuality::Normal:     return "Normal";
    case NVTTQuality::Production: return "Production";
    case NVTTQuality::Highest:    return "Highest";
    default:                      return "Unknown";
    }
}

bool TextureCompressor::IsNVTTAvailable() const {
    // 检查nvtt_export.exe是否存在
    DWORD attrs = GetFileAttributesW(m_nvttPath.c_str());
    bool available = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));

    // Debug output
    std::wcout << L"IsNVTTAvailable: checking path: " << m_nvttPath << std::endl;
    std::cout << "  File attributes: " << attrs << " (INVALID=" << INVALID_FILE_ATTRIBUTES << ")" << std::endl;
    std::cout << "  Result: " << (available ? "available" : "not available") << std::endl;

    return available;
}

bool TextureCompressor::CompressWithNVTT(const std::wstring& sourcePath,
                                          const std::wstring& outputDdsPath,
                                          TextureCompressionFormat format,
                                          bool generateMips,
                                          bool sRGB,
                                          NVTTQuality quality) {
    // 检查NVTT是否可用
    if (!IsNVTTAvailable()) {
        std::cout << "NVTT not available, falling back to DirectXTex" << std::endl;
        return CompressAndSaveDDS(sourcePath, outputDdsPath, format, generateMips, sRGB,
                                  TextureCompressionQuality::High);
    }

    // 构建命令行参数
    // nvtt_export.exe 命令行格式:
    // nvtt_export.exe -o output.dds --format bc3 [--mips] [--srgb] input.png

    std::wstring formatArg;
    switch (format) {
    case TextureCompressionFormat::BC1:
        formatArg = L"bc1";
        break;
    case TextureCompressionFormat::BC3:
        formatArg = L"bc3";
        break;
    case TextureCompressionFormat::BC5:
        formatArg = L"bc5";
        break;
    case TextureCompressionFormat::BC7:
        formatArg = L"bc7";
        break;
    case TextureCompressionFormat::BC6H:
        formatArg = L"bc6h";
        break;
    default:
        formatArg = L"bc3";  // 默认BC3
        break;
    }

    // 构建完整命令行
    std::wstring cmdLine = L"\"" + m_nvttPath + L"\" ";
    cmdLine += L"-o \"" + outputDdsPath + L"\" ";
    cmdLine += L"--format " + formatArg + L" ";

    if (generateMips) {
        cmdLine += L"--mips ";
    }

    if (sRGB) {
        cmdLine += L"--srgb ";
    }

    // 使用传入的质量参数
    cmdLine += L"--quality " + std::to_wstring(static_cast<int>(quality)) + L" ";

    cmdLine += L"\"" + sourcePath + L"\"";

    std::wcout << L"Running NVTT: " << cmdLine << std::endl;

    // 创建进程
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;  // 隐藏窗口

    // 需要可修改的命令行缓冲区
    std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
    cmdBuffer.push_back(L'\0');

    BOOL success = CreateProcessW(
        nullptr,           // 应用程序名（使用命令行）
        cmdBuffer.data(),  // 命令行
        nullptr,           // 进程安全属性
        nullptr,           // 线程安全属性
        FALSE,             // 不继承句柄
        CREATE_NO_WINDOW,  // 不创建窗口
        nullptr,           // 使用父进程环境
        nullptr,           // 使用父进程目录
        &si,
        &pi
    );

    if (!success) {
        DWORD error = GetLastError();
        std::cout << "Failed to start NVTT process, error: " << error << std::endl;
        std::cout << "Falling back to DirectXTex" << std::endl;
        return CompressAndSaveDDS(sourcePath, outputDdsPath, format, generateMips, sRGB,
                                  TextureCompressionQuality::High);
    }

    // 等待进程完成（最多60秒）
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 60000);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (waitResult == WAIT_TIMEOUT) {
        std::cout << "NVTT process timed out" << std::endl;
        return false;
    }

    if (exitCode != 0) {
        std::cout << "NVTT process failed with exit code: " << exitCode << std::endl;
        std::cout << "Falling back to DirectXTex" << std::endl;
        return CompressAndSaveDDS(sourcePath, outputDdsPath, format, generateMips, sRGB,
                                  TextureCompressionQuality::High);
    }

    // 验证输出文件是否存在
    DWORD attrs = GetFileAttributesW(outputDdsPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        std::cout << "NVTT output file not found" << std::endl;
        return false;
    }

    std::wcout << L"NVTT compression successful: " << outputDdsPath << std::endl;
    return true;
}
