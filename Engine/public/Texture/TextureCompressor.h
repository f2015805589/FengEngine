// TextureCompressor.h
// GPU纹理压缩器

#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXTex/DirectXTex.h>
#include <string>
#include <memory>
#include "TextureAsset.h"

using Microsoft::WRL::ComPtr;

// 压缩参数常量缓冲
struct CompressionParams {
    UINT textureWidth;
    UINT textureHeight;
    UINT blockCountX;       // 水平方向BC块数量
    UINT blockCountY;       // 垂直方向BC块数量
    UINT mipLevel;          // 当前mip级别
    UINT isSRGB;            // 是否sRGB空间
    float padding[2];       // 对齐到16字节
};

class TextureCompressor {
public:
    static TextureCompressor& GetInstance();

    // 初始化压缩器
    bool Initialize(ID3D12Device* device);
    void Shutdown();

    // ========== GPU压缩（快速，用于运行时） ==========

    // 压缩单个纹理
    // 输入：源纹理资源（RGBA格式）
    // 输出：压缩后的纹理资源
    bool CompressGPU(ID3D12GraphicsCommandList* commandList,
                     ID3D12Resource* sourceTexture,
                     TextureCompressionFormat format,
                     bool sRGB,
                     ComPtr<ID3D12Resource>& outCompressedTexture);

    // 批量压缩（用于mipmap链）
    bool CompressGPUWithMips(ID3D12GraphicsCommandList* commandList,
                             ID3D12Resource* sourceTexture,
                             UINT mipLevels,
                             TextureCompressionFormat format,
                             bool sRGB,
                             ComPtr<ID3D12Resource>& outCompressedTexture);

    // ========== CPU压缩（高质量，用于导入时） ==========

    // 使用DirectXTex进行CPU压缩
    bool CompressCPU(const DirectX::ScratchImage& sourceImage,
                     TextureCompressionFormat format,
                     bool sRGB,
                     TextureCompressionQuality quality,
                     DirectX::ScratchImage& outCompressedImage);

    // 压缩并保存为DDS
    bool CompressAndSaveDDS(const std::wstring& sourcePath,
                            const std::wstring& outputDdsPath,
                            TextureCompressionFormat format,
                            bool generateMips,
                            bool sRGB,
                            TextureCompressionQuality quality);

    // ========== NVIDIA Texture Tools 压缩（高质量） ==========

    // NVTT压缩质量等级
    enum class NVTTQuality {
        Fastest = 0,    // 最快
        Normal = 1,     // 普通
        Production = 2, // 生产级（默认）
        Highest = 3     // 最高质量
    };

    // 使用NVIDIA Texture Tools压缩
    // 如果NVTT可用，优先使用NVTT；否则回退到DirectXTex
    bool CompressWithNVTT(const std::wstring& sourcePath,
                          const std::wstring& outputDdsPath,
                          TextureCompressionFormat format,
                          bool generateMips,
                          bool sRGB,
                          NVTTQuality quality = NVTTQuality::Production);

    // 设置NVIDIA Texture Tools路径
    void SetNVTTPath(const std::wstring& nvttExePath) { m_nvttPath = nvttExePath; }
    const std::wstring& GetNVTTPath() const { return m_nvttPath; }

    // 设置/获取NVTT压缩质量
    void SetNVTTQuality(NVTTQuality quality) { m_nvttQuality = quality; }
    NVTTQuality GetNVTTQuality() const { return m_nvttQuality; }

    // 检查NVTT是否可用
    bool IsNVTTAvailable() const;

    // 获取质量名称
    static const char* GetNVTTQualityName(NVTTQuality quality);

    // ========== 辅助函数 ==========

    // 获取压缩格式的块大小（字节）
    static UINT GetBlockSize(TextureCompressionFormat format);

    // 获取压缩格式对应的DXGI格式
    static DXGI_FORMAT GetCompressedFormat(TextureCompressionFormat format, bool sRGB);

    // 检查格式是否支持GPU压缩
    static bool SupportsGPUCompression(TextureCompressionFormat format);

    // 析构函数需要public以供unique_ptr使用
    ~TextureCompressor() = default;

private:
    TextureCompressor() = default;
    TextureCompressor(const TextureCompressor&) = delete;
    TextureCompressor& operator=(const TextureCompressor&) = delete;

    // 编译压缩计算着色器
    bool CompileComputeShaders();

    // 创建压缩专用根签名
    bool CreateComputeRootSignature();

    // 创建描述符堆
    void CreateDescriptorHeaps();

    // 创建常量缓冲
    void CreateConstantBuffer();

    // 执行单个mip级别的压缩
    bool CompressMipLevel(ID3D12GraphicsCommandList* commandList,
                          ID3D12Resource* sourceTexture,
                          ID3D12Resource* destTexture,
                          UINT mipLevel,
                          UINT width,
                          UINT height,
                          TextureCompressionFormat format,
                          bool sRGB);

    // 设备引用
    ID3D12Device* m_device = nullptr;

    // 计算着色器
    ComPtr<ID3DBlob> m_bc1ShaderBlob;
    ComPtr<ID3DBlob> m_bc3ShaderBlob;
    ComPtr<ID3DBlob> m_bc5ShaderBlob;
    ComPtr<ID3DBlob> m_bc7ShaderBlob;   // 可选：BC7复杂度高
    ComPtr<ID3DBlob> m_bc6hShaderBlob;  // 可选：BC6H用于HDR

    // PSO
    ComPtr<ID3D12PipelineState> m_bc1PSO;
    ComPtr<ID3D12PipelineState> m_bc3PSO;
    ComPtr<ID3D12PipelineState> m_bc5PSO;
    ComPtr<ID3D12PipelineState> m_bc7PSO;
    ComPtr<ID3D12PipelineState> m_bc6hPSO;

    // 根签名
    ComPtr<ID3D12RootSignature> m_computeRootSignature;

    // 描述符堆
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;     // 源纹理SRV
    ComPtr<ID3D12DescriptorHeap> m_uavHeap;     // 输出纹理UAV
    ComPtr<ID3D12DescriptorHeap> m_samplerHeap; // 采样器
    UINT m_srvDescriptorSize = 0;

    // 常量缓冲
    ComPtr<ID3D12Resource> m_constantBuffer;
    CompressionParams* m_mappedConstantBuffer = nullptr;

    // NVIDIA Texture Tools 路径（可通过SetNVTTPath设置，默认在项目根目录的上级查找）
    std::wstring m_nvttPath;

    // NVTT压缩质量
    NVTTQuality m_nvttQuality = NVTTQuality::Production;

    // 线程组大小（每个线程处理一个4x4块）
    static const UINT THREAD_GROUP_SIZE = 8;  // 8x8线程组
};
