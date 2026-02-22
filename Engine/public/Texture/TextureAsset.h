#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <DirectXTex/DirectXTex.h>

using Microsoft::WRL::ComPtr;

// 纹理类型
enum class TextureType {
    Texture2D,          // 2D纹理
    TextureCube,        // 立方体贴图
    Texture2DArray      // 2D纹理数组
};

// 压缩格式
enum class TextureCompressionFormat {
    None,       // 不压缩 (R8G8B8A8)
    BC1,        // RGB, 无Alpha (DXT1) - 8字节/4x4块
    BC3,        // RGBA (DXT5) - 16字节/4x4块
    BC5,        // 法线贴图 (2通道RG) - 16字节/4x4块
    BC7,        // 高质量RGBA - 16字节/4x4块
    BC6H        // HDR (半精度浮点) - 16字节/4x4块
};

// 压缩质量
enum class TextureCompressionQuality {
    Fast,       // GPU压缩，最快
    Normal,     // GPU压缩，平衡
    High,       // GPU压缩，高质量
    Ultra       // CPU压缩(DirectXTex)，最高质量
};

// 纹理过滤模式
enum class TextureFilterMode {
    Point,          // 最近邻
    Bilinear,       // 双线性
    Trilinear,      // 三线性（带Mipmap）
    Anisotropic     // 各向异性
};

// 纹理寻址模式
enum class TextureAddressMode {
    Wrap,       // 重复
    Clamp,      // 夹取
    Mirror,     // 镜像
    Border      // 边框颜色
};

// 纹理资产描述
struct TextureAssetDesc {
    std::wstring sourcePath;            // 源文件路径
    TextureType type = TextureType::Texture2D;
    TextureCompressionFormat format = TextureCompressionFormat::BC3;
    TextureCompressionQuality quality = TextureCompressionQuality::Normal;
    bool generateMips = true;
    bool sRGB = true;

    // 采样设置
    TextureFilterMode filter = TextureFilterMode::Trilinear;
    TextureAddressMode addressU = TextureAddressMode::Wrap;
    TextureAddressMode addressV = TextureAddressMode::Wrap;
    TextureAddressMode addressW = TextureAddressMode::Wrap;
    int maxAnisotropy = 16;
};

// 纹理运行时信息
struct TextureRuntimeInfo {
    UINT width = 0;
    UINT height = 0;
    UINT depth = 1;             // 用于3D纹理或数组
    UINT mipLevels = 0;
    UINT arraySize = 1;         // 用于纹理数组或Cubemap(6)
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    size_t memorySize = 0;      // GPU内存占用（字节）
};

class TextureAsset {
public:
    TextureAsset(const std::string& name);
    ~TextureAsset();

    // 从资产文件加载 (.texture.ast)
    bool LoadFromAssetFile(const std::wstring& assetPath);

    // 从源文件导入（创建新资产）
    bool ImportFromSource(const std::wstring& sourcePath,
                          const TextureAssetDesc& desc);

    // 保存资产描述文件
    bool SaveAssetFile(const std::wstring& assetPath);

    // 加载到GPU（从缓存的DDS或源文件）
    bool LoadToGPU(ID3D12Device* device,
                   ID3D12GraphicsCommandList* commandList);

    // 仅加载原图到GPU（不压缩，用于预览）
    bool LoadSourceToGPU(ID3D12Device* device,
                         ID3D12GraphicsCommandList* commandList);

    // 应用压缩设置（UE风格：在预览面板中选择格式后点击Apply）
    bool ApplyCompression(TextureCompressionFormat format,
                          ID3D12Device* device,
                          ID3D12GraphicsCommandList* commandList);

    // 卸载GPU资源
    void UnloadFromGPU();

    // 重新压缩（格式变更时）
    bool Recompress(TextureCompressionFormat newFormat,
                    ID3D12Device* device,
                    ID3D12GraphicsCommandList* commandList);

    // 设置压缩格式（不立即应用）
    void SetCompressionFormat(TextureCompressionFormat format) { m_desc.format = format; }
    void SetGenerateMips(bool generate) { m_desc.generateMips = generate; }
    void SetSRGB(bool sRGB) { m_desc.sRGB = sRGB; }

    // 检查是否已压缩
    bool IsCompressed() const { return m_desc.format != TextureCompressionFormat::None && m_cacheValid; }

    // ========== Getter ==========
    const std::string& GetName() const { return m_name; }
    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRV() const { return m_srvGPU; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPU() const { return m_srvCPU; }
    UINT GetSRVIndex() const { return m_srvIndex; }

    UINT GetWidth() const { return m_runtimeInfo.width; }
    UINT GetHeight() const { return m_runtimeInfo.height; }
    UINT GetMipLevels() const { return m_runtimeInfo.mipLevels; }
    DXGI_FORMAT GetFormat() const { return m_runtimeInfo.format; }
    size_t GetMemorySize() const { return m_runtimeInfo.memorySize; }

    TextureType GetType() const { return m_desc.type; }
    TextureCompressionFormat GetCompressionFormat() const { return m_desc.format; }
    bool IsSRGB() const { return m_desc.sRGB; }
    bool IsLoaded() const { return m_isLoaded; }
    bool IsCacheValid() const { return m_cacheValid; }

    const TextureAssetDesc& GetDesc() const { return m_desc; }
    const TextureRuntimeInfo& GetRuntimeInfo() const { return m_runtimeInfo; }
    const std::wstring& GetSourcePath() const { return m_sourcePath; }
    const std::wstring& GetCachePath() const { return m_cacheDdsPath; }
    const std::wstring& GetAssetPath() const { return m_assetPath; }

    // ========== Setter ==========
    void SetSRVIndex(UINT index) { m_srvIndex = index; }
    void SetSRVHandles(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
        m_srvCPU = cpu;
        m_srvGPU = gpu;
    }

    // ========== 静态辅助函数 ==========
    static DXGI_FORMAT GetDXGIFormat(TextureCompressionFormat format, bool sRGB);
    static const char* GetFormatName(TextureCompressionFormat format);
    static const char* GetTypeName(TextureType type);
    static size_t CalculateMemorySize(UINT width, UINT height, UINT mipLevels,
                                      TextureCompressionFormat format);

    // ========== NVTT压缩开关 ==========
    // 是否使用NVIDIA Texture Tools进行压缩（默认开启）
    static bool s_useNVTT;
    static void SetUseNVTT(bool use) { s_useNVTT = use; }
    static bool GetUseNVTT() { return s_useNVTT; }

private:
    std::string m_name;
    TextureAssetDesc m_desc;
    TextureRuntimeInfo m_runtimeInfo;

    // 文件路径
    std::wstring m_assetPath;       // .texture.ast文件路径
    std::wstring m_sourcePath;      // 源文件路径 (png/jpg/hdr等)
    std::string m_sourceHash;       // 源文件哈希（用于缓存验证）

    // 缓存信息
    std::wstring m_cacheDdsPath;    // 缓存的DDS文件路径
    bool m_cacheValid = false;

    // GPU资源
    ComPtr<ID3D12Resource> m_resource;
    ComPtr<ID3D12Resource> m_uploadHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_srvCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGPU = {};
    UINT m_srvIndex = UINT_MAX;     // 在TextureManager SRV堆中的索引

    bool m_isLoaded = false;

    // ========== 内部方法 ==========
    // 计算源文件哈希
    std::string CalculateSourceHash(const std::wstring& path);

    // 从缓存的DDS加载
    bool LoadDDSFromCache(ID3D12Device* device,
                          ID3D12GraphicsCommandList* commandList);

    // 从源文件加载并压缩
    bool LoadAndCompressSource(ID3D12Device* device,
                               ID3D12GraphicsCommandList* commandList);

    // 创建SRV
    void CreateSRV(ID3D12Device* device);

    // XML解析辅助
    bool ParseAssetXML(const std::wstring& xmlPath);
    bool WriteAssetXML(const std::wstring& xmlPath);

    // 生成缓存路径
    std::wstring GenerateCachePath();
};
