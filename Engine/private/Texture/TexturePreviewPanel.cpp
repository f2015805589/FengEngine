// TexturePreviewPanel.cpp
// 纹理预览面板实现 - 支持完整的通道可视化

#define NOMINMAX

#include "public/Texture/TexturePreviewPanel.h"
#include "public/Texture/TextureManager.h"
#include "public/Texture/TextureCompressor.h"
#include "public/BattleFireDirect.h"
#include "imgui.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "d3dcompiler.lib")

namespace {
    // wstring转string
    std::string WStringToString(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
        return result;
    }

    // 格式化文件大小
    std::string FormatFileSize(size_t bytes) {
        if (bytes < 1024) {
            return std::to_string(bytes) + " B";
        }
        else if (bytes < 1024 * 1024) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << (bytes / 1024.0) << " KB";
            return oss.str();
        }
        else {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << (bytes / (1024.0 * 1024.0)) << " MB";
            return oss.str();
        }
    }

    // 全屏四边形顶点数据
    struct PreviewVertex {
        float position[3];
        float texCoord[2];
    };

    const PreviewVertex g_quadVertices[] = {
        { {-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f} },  // 左上
        { { 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f} },  // 右上
        { {-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f} },  // 左下
        { { 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f} },  // 右上
        { { 1.0f, -1.0f, 0.0f}, {1.0f, 1.0f} },  // 右下
        { {-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f} },  // 左下
    };
}

// ========== 单例实现 ==========

TexturePreviewPanel& TexturePreviewPanel::GetInstance() {
    static TexturePreviewPanel instance;
    return instance;
}

// ========== 初始化和清理 ==========

bool TexturePreviewPanel::Initialize(ID3D12Device* device) {
    if (!device) {
        std::cout << "TexturePreviewPanel::Initialize - Invalid device" << std::endl;
        return false;
    }

    m_device = device;
    m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 使用ImGui的描述符堆中的固定槽位
    m_imguiSrvSlot = IMGUI_TEXTURE_SLOT_START;

    // 创建预览渲染资源
    if (!CreatePreviewResources()) {
        std::cout << "TexturePreviewPanel: Failed to create preview resources" << std::endl;
        // 继续运行，使用简单的ImGui显示
    }

    std::cout << "TexturePreviewPanel initialized" << std::endl;
    return true;
}

void TexturePreviewPanel::Shutdown() {
    m_currentTexture = nullptr;
    m_previewHeap.Reset();
    m_previewRootSignature.Reset();
    m_previewPSO.Reset();
    m_vsBlob.Reset();
    m_psBlob.Reset();
    m_constantBuffer.Reset();
    m_previewRT.Reset();
    m_rtvHeap.Reset();
    m_quadVB.Reset();
    m_device = nullptr;
    m_imguiSrvSlot = UINT_MAX;
    m_previewResourcesInitialized = false;
    std::cout << "TexturePreviewPanel shutdown" << std::endl;
}

// ========== 预览资源创建 ==========

bool TexturePreviewPanel::CreatePreviewResources() {
    if (!CreatePreviewShaders()) {
        std::cout << "Failed to create preview shaders" << std::endl;
        return false;
    }

    if (!CreatePreviewRootSignature()) {
        std::cout << "Failed to create preview root signature" << std::endl;
        return false;
    }

    if (!CreatePreviewPSO()) {
        std::cout << "Failed to create preview PSO" << std::endl;
        return false;
    }

    // 创建常量缓冲
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(
        (sizeof(PreviewParams) + 255) & ~255);  // 256字节对齐

    HRESULT hr = m_device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_constantBuffer));

    if (FAILED(hr)) {
        std::cout << "Failed to create constant buffer" << std::endl;
        return false;
    }

    // 映射常量缓冲
    CD3DX12_RANGE readRange(0, 0);
    hr = m_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedConstantBuffer));
    if (FAILED(hr)) {
        std::cout << "Failed to map constant buffer" << std::endl;
        return false;
    }

    // 创建RTV堆
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        std::cout << "Failed to create RTV heap" << std::endl;
        return false;
    }

    m_previewResourcesInitialized = true;
    std::cout << "Preview resources created successfully" << std::endl;
    return true;
}

bool TexturePreviewPanel::CreatePreviewShaders() {
    // 内嵌shader代码（避免文件依赖问题）
    const char* shaderCode = R"(
        cbuffer PreviewParams : register(b0)
        {
            int channelMode;
            float exposure;
            float gamma;
            int mipLevel;
            float2 uvOffset;
            float2 uvScale;
        };

        Texture2D<float4> sourceTexture : register(t0);
        SamplerState linearSampler : register(s0);

        struct VSInput
        {
            float3 position : POSITION;
            float2 texCoord : TEXCOORD0;
        };

        struct VSOutput
        {
            float4 position : SV_POSITION;
            float2 texCoord : TEXCOORD0;
        };

        VSOutput VSMain(VSInput input)
        {
            VSOutput output;
            output.position = float4(input.position, 1.0);
            output.texCoord = input.texCoord * uvScale + uvOffset;
            return output;
        }

        float4 PSMain(VSOutput input) : SV_TARGET
        {
            float4 color = sourceTexture.SampleLevel(linearSampler, input.texCoord, mipLevel);
            color.rgb *= exposure;

            float4 result = float4(0, 0, 0, 1);

            if (channelMode == 0) { // RGBA
                result = color;
            }
            else if (channelMode == 1) { // RGB
                result = float4(color.rgb, 1.0);
            }
            else if (channelMode == 2) { // R
                result = float4(color.r, color.r, color.r, 1.0);
            }
            else if (channelMode == 3) { // G
                result = float4(color.g, color.g, color.g, 1.0);
            }
            else if (channelMode == 4) { // B
                result = float4(color.b, color.b, color.b, 1.0);
            }
            else if (channelMode == 5) { // A
                result = float4(color.a, color.a, color.a, 1.0);
            }
            else if (channelMode == 6) { // Normal
                float2 normalXY = color.rg * 2.0 - 1.0;
                float normalZ = sqrt(saturate(1.0 - dot(normalXY, normalXY)));
                float3 normal = float3(normalXY, normalZ);
                result = float4(normal * 0.5 + 0.5, 1.0);
            }
            else if (channelMode == 7) { // Luminance
                float lum = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
                result = float4(lum, lum, lum, 1.0);
            }

            result.rgb = pow(abs(result.rgb), 1.0 / gamma);
            return result;
        }
    )";

    // 编译顶点着色器
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(
        shaderCode, strlen(shaderCode),
        "PreviewShader", nullptr, nullptr,
        "VSMain", "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &m_vsBlob, &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            std::cout << "VS compile error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
        return false;
    }

    // 编译像素着色器
    hr = D3DCompile(
        shaderCode, strlen(shaderCode),
        "PreviewShader", nullptr, nullptr,
        "PSMain", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &m_psBlob, &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            std::cout << "PS compile error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
        return false;
    }

    std::cout << "Preview shaders compiled successfully" << std::endl;
    return true;
}

bool TexturePreviewPanel::CreatePreviewRootSignature() {
    // 根参数
    CD3DX12_ROOT_PARAMETER rootParams[2];

    // CBV (常量缓冲)
    rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);

    // SRV (纹理)
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    rootParams[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    // 静态采样器
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init(2, rootParams, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &signature, &error);

    if (FAILED(hr)) {
        if (error) {
            std::cout << "Root signature serialize error: " << (char*)error->GetBufferPointer() << std::endl;
        }
        return false;
    }

    hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&m_previewRootSignature));

    if (FAILED(hr)) {
        std::cout << "Failed to create root signature" << std::endl;
        return false;
    }

    return true;
}

bool TexturePreviewPanel::CreatePreviewPSO() {
    // 输入布局
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = m_previewRootSignature.Get();
    psoDesc.VS = { m_vsBlob->GetBufferPointer(), m_vsBlob->GetBufferSize() };
    psoDesc.PS = { m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_previewPSO));
    if (FAILED(hr)) {
        std::cout << "Failed to create preview PSO" << std::endl;
        return false;
    }

    return true;
}

void TexturePreviewPanel::CreatePreviewRenderTarget() {
    // 如果尺寸没变，不需要重建
    if (m_previewRT && m_previewRTWidth > 0 && m_previewRTHeight > 0) {
        return;
    }

    // 使用固定的预览尺寸
    m_previewRTWidth = 1024;
    m_previewRTHeight = 1024;

    // 创建渲染目标
    D3D12_RESOURCE_DESC rtDesc = {};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = m_previewRTWidth;
    rtDesc.Height = m_previewRTHeight;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 1.0f;

    CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = m_device->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &rtDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clearValue,
        IID_PPV_ARGS(&m_previewRT));

    if (FAILED(hr)) {
        std::cout << "Failed to create preview render target" << std::endl;
        return;
    }

    // 创建RTV
    m_device->CreateRenderTargetView(m_previewRT.Get(), nullptr,
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    // 在ImGui堆中创建SRV用于显示
    if (gImGuiDescriptorHeap) {
        CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(
            gImGuiDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
            m_imguiSrvSlot,
            m_srvDescriptorSize);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        m_device->CreateShaderResourceView(m_previewRT.Get(), &srvDesc, cpuHandle);

        m_previewSRV = CD3DX12_GPU_DESCRIPTOR_HANDLE(
            gImGuiDescriptorHeap->GetGPUDescriptorHandleForHeapStart(),
            m_imguiSrvSlot,
            m_srvDescriptorSize);
    }
}

void TexturePreviewPanel::RenderTextureToPreviewRT() {
    // 此函数需要在渲染循环中调用，暂时不实现
    // 因为需要访问命令列表，而ImGui渲染时机不太合适
    // 保留简单的ImGui::Image方式
}

// ========== 纹理设置 ==========

void TexturePreviewPanel::SetTexture(TextureAsset* texture) {
    m_currentTexture = texture;

    if (texture) {
        CreatePreviewSRV();
        m_showWindow = true;

        // 重置视图设置
        m_zoomLevel = 1.0f;
        m_panX = 0.0f;
        m_panY = 0.0f;
        m_mipLevel = 0;

        // 同步压缩格式选择
        m_selectedFormat = texture->GetCompressionFormat();
    }
}

void TexturePreviewPanel::SetTexturePath(const std::wstring& path) {
    // 延迟加载：只记录路径，不在渲染帧中间执行纹理加载/压缩
    m_pendingLoadPath = path;
    m_hasPendingLoad = true;
    m_showWindow = true;  // 先显示窗口（会显示"Loading..."）
}

void TexturePreviewPanel::ProcessPendingLoad() {
    if (!m_hasPendingLoad) return;
    m_hasPendingLoad = false;

    m_currentTexturePath = m_pendingLoadPath;

    // 确保TextureManager有commandList
    extern ID3D12GraphicsCommandList* gCommandList;
    if (gCommandList) {
        TextureManager::GetInstance().SetCommandList(gCommandList);
    }

    // 通过TextureManager加载
    TextureAsset* texture = TextureManager::GetInstance().LoadTexture(m_pendingLoadPath);
    SetTexture(texture);
}

// ========== 预览SRV创建 ==========

void TexturePreviewPanel::CreatePreviewSRV() {
    if (!m_currentTexture || !m_device) return;

    ID3D12Resource* resource = m_currentTexture->GetResource();
    if (!resource) {
        std::cout << "TexturePreviewPanel::CreatePreviewSRV - No resource" << std::endl;
        return;
    }

    // 使用ImGui的描述符堆
    if (!gImGuiDescriptorHeap) {
        std::cout << "TexturePreviewPanel::CreatePreviewSRV - ImGui descriptor heap not available" << std::endl;
        return;
    }

    // 计算在ImGui堆中的位置
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(
        gImGuiDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
        m_imguiSrvSlot,
        m_srvDescriptorSize);

    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(
        gImGuiDescriptorHeap->GetGPUDescriptorHandleForHeapStart(),
        m_imguiSrvSlot,
        m_srvDescriptorSize);

    // 创建SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = resource->GetDesc().Format;

    TextureType type = m_currentTexture->GetType();
    if (type == TextureType::TextureCube) {
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MipLevels = resource->GetDesc().MipLevels;
        srvDesc.TextureCube.MostDetailedMip = 0;
    }
    else {
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = resource->GetDesc().MipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
    }

    m_device->CreateShaderResourceView(resource, &srvDesc, cpuHandle);

    // 保存GPU句柄用于ImGui
    m_previewSRV = gpuHandle;

    std::cout << "TexturePreviewPanel: Created SRV in ImGui heap at slot " << m_imguiSrvSlot
              << ", GPU handle: " << gpuHandle.ptr << std::endl;
}

// ========== UI渲染 ==========

void TexturePreviewPanel::RenderUI() {
    if (!m_showWindow) return;

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);

    std::string windowTitle = "Texture Preview";
    if (m_currentTexture) {
        windowTitle += " - " + m_currentTexture->GetName();
    }
    windowTitle += "###TexturePreview";

    if (ImGui::Begin(windowTitle.c_str(), &m_showWindow, ImGuiWindowFlags_MenuBar)) {
        // 菜单栏
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Info Panel", nullptr, &m_showInfoPanel);
                ImGui::MenuItem("Compression Settings", nullptr, &m_showCompressionPanel);
                ImGui::Separator();
                if (ImGui::MenuItem("Fit to Window", nullptr, m_fitToWindow)) {
                    m_fitToWindow = !m_fitToWindow;
                }
                if (ImGui::MenuItem("Reset View")) {
                    m_zoomLevel = 1.0f;
                    m_panX = 0.0f;
                    m_panY = 0.0f;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Channel")) {
                if (ImGui::MenuItem("RGBA", nullptr, m_channelMode == PreviewChannelMode::RGBA))
                    m_channelMode = PreviewChannelMode::RGBA;
                if (ImGui::MenuItem("RGB", nullptr, m_channelMode == PreviewChannelMode::RGB))
                    m_channelMode = PreviewChannelMode::RGB;
                if (ImGui::MenuItem("R", nullptr, m_channelMode == PreviewChannelMode::R))
                    m_channelMode = PreviewChannelMode::R;
                if (ImGui::MenuItem("G", nullptr, m_channelMode == PreviewChannelMode::G))
                    m_channelMode = PreviewChannelMode::G;
                if (ImGui::MenuItem("B", nullptr, m_channelMode == PreviewChannelMode::B))
                    m_channelMode = PreviewChannelMode::B;
                if (ImGui::MenuItem("A", nullptr, m_channelMode == PreviewChannelMode::A))
                    m_channelMode = PreviewChannelMode::A;
                ImGui::Separator();
                if (ImGui::MenuItem("Normal", nullptr, m_channelMode == PreviewChannelMode::Normal))
                    m_channelMode = PreviewChannelMode::Normal;
                if (ImGui::MenuItem("Luminance", nullptr, m_channelMode == PreviewChannelMode::Luminance))
                    m_channelMode = PreviewChannelMode::Luminance;
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // 工具栏
        RenderToolbar();

        ImGui::Separator();

        // 主内容区域
        float infoWidth = m_showInfoPanel ? 250.0f : 0.0f;
        float compressionWidth = m_showCompressionPanel ? 200.0f : 0.0f;

        // 左侧：纹理预览
        ImGui::BeginChild("TextureView", ImVec2(-(infoWidth + compressionWidth), 0), true);
        RenderTextureView();
        ImGui::EndChild();

        // 右侧面板
        if (m_showInfoPanel || m_showCompressionPanel) {
            ImGui::SameLine();
            ImGui::BeginChild("RightPanel", ImVec2(0, 0), false);

            if (m_showInfoPanel) {
                RenderInfoPanel();
            }

            if (m_showCompressionPanel) {
                ImGui::Separator();
                RenderCompressionSettings();
            }

            ImGui::EndChild();
        }
    }
    ImGui::End();
}

void TexturePreviewPanel::RenderToolbar() {
    // 通道模式选择
    const char* channelModes[] = { "RGBA", "RGB", "R", "G", "B", "A", "Normal", "Luminance" };
    int currentMode = static_cast<int>(m_channelMode);
    ImGui::SetNextItemWidth(100);
    if (ImGui::Combo("Channel", &currentMode, channelModes, IM_ARRAYSIZE(channelModes))) {
        m_channelMode = static_cast<PreviewChannelMode>(currentMode);
    }

    ImGui::SameLine();

    // Mip级别选择
    if (m_currentTexture) {
        int maxMip = m_currentTexture->GetMipLevels() - 1;
        if (maxMip < 0) maxMip = 0;
        ImGui::SetNextItemWidth(80);
        ImGui::SliderInt("Mip", &m_mipLevel, 0, maxMip);
    }

    ImGui::SameLine();

    // 缩放控制
    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("Zoom", &m_zoomLevel, 0.1f, 10.0f, "%.1fx");

    ImGui::SameLine();

    // HDR曝光
    if (m_currentTexture) {
        TextureCompressionFormat format = m_currentTexture->GetCompressionFormat();
        if (format == TextureCompressionFormat::BC6H) {
            ImGui::SetNextItemWidth(100);
            ImGui::SliderFloat("Exposure", &m_exposure, 0.1f, 10.0f, "%.1f");
            ImGui::SameLine();
        }
    }

    // Alpha棋盘格
    ImGui::Checkbox("Alpha BG", &m_showAlphaCheckerboard);
}

void TexturePreviewPanel::RenderTextureView() {
    if (m_hasPendingLoad) {
        ImGui::Text("Loading texture...");
        return;
    }
    if (!m_currentTexture) {
        ImGui::Text("No texture loaded");
        ImGui::Text("Double-click a texture in Resource Manager to preview");
        return;
    }

    // 获取纹理尺寸
    UINT texWidth = m_currentTexture->GetWidth();
    UINT texHeight = m_currentTexture->GetHeight();

    if (texWidth == 0 || texHeight == 0) {
        ImGui::Text("Invalid texture dimensions");
        return;
    }

    // 计算显示尺寸
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    float displayWidth, displayHeight;

    if (m_fitToWindow) {
        float scaleX = availSize.x / texWidth;
        float scaleY = availSize.y / texHeight;
        float scale = std::min(scaleX, scaleY) * m_zoomLevel;
        displayWidth = texWidth * scale;
        displayHeight = texHeight * scale;
    }
    else {
        displayWidth = texWidth * m_zoomLevel;
        displayHeight = texHeight * m_zoomLevel;
    }

    // 处理鼠标交互
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            m_zoomLevel *= (1.0f + wheel * 0.1f);
            m_zoomLevel = std::max(0.1f, std::min(10.0f, m_zoomLevel));
        }

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
            m_panX += delta.x;
            m_panY += delta.y;
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
        }
    }

    // 居中显示
    float offsetX = (availSize.x - displayWidth) * 0.5f + m_panX;
    float offsetY = (availSize.y - displayHeight) * 0.5f + m_panY;

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + offsetX,
                                ImGui::GetCursorPosY() + offsetY));

    // 渲染纹理
    if (m_previewSRV.ptr != 0) {
        // 根据通道模式设置tint颜色
        ImVec4 tintColor = ImVec4(1, 1, 1, 1);
        switch (m_channelMode) {
        case PreviewChannelMode::RGBA:
        case PreviewChannelMode::RGB:
            tintColor = ImVec4(1, 1, 1, 1);
            break;
        case PreviewChannelMode::R:
            tintColor = ImVec4(1, 0, 0, 1);
            break;
        case PreviewChannelMode::G:
            tintColor = ImVec4(0, 1, 0, 1);
            break;
        case PreviewChannelMode::B:
            tintColor = ImVec4(0, 0, 1, 1);
            break;
        case PreviewChannelMode::A:
        case PreviewChannelMode::Normal:
        case PreviewChannelMode::Luminance:
            tintColor = ImVec4(1, 1, 1, 1);
            break;
        }

        ImGui::Image((ImTextureID)m_previewSRV.ptr, ImVec2(displayWidth, displayHeight),
                     ImVec2(0, 0), ImVec2(1, 1), tintColor, ImVec4(0, 0, 0, 0));

        // 显示像素信息
        if (ImGui::IsItemHovered()) {
            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 itemPos = ImGui::GetItemRectMin();
            float relX = (mousePos.x - itemPos.x) / displayWidth;
            float relY = (mousePos.y - itemPos.y) / displayHeight;

            if (relX >= 0 && relX <= 1 && relY >= 0 && relY <= 1) {
                int pixelX = static_cast<int>(relX * texWidth);
                int pixelY = static_cast<int>(relY * texHeight);

                ImGui::BeginTooltip();
                ImGui::Text("Pixel: (%d, %d)", pixelX, pixelY);
                ImGui::Text("UV: (%.3f, %.3f)", relX, relY);
                ImGui::EndTooltip();
            }
        }
    } else {
        ImGui::BeginChild("TexturePreviewImage", ImVec2(displayWidth, displayHeight), true);
        ImGui::Text("Texture: %s", m_currentTexture->GetName().c_str());
        ImGui::Text("Size: %u x %u", texWidth, texHeight);
        ImGui::Text("Format: %s", TextureAsset::GetFormatName(m_currentTexture->GetCompressionFormat()));
        ImGui::Text("Mips: %u", m_currentTexture->GetMipLevels());
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "[SRV not available]");
        ImGui::EndChild();
    }
}

void TexturePreviewPanel::RenderInfoPanel() {
    ImGui::Text("Texture Info");
    ImGui::Separator();

    if (!m_currentTexture) {
        ImGui::Text("No texture selected");
        return;
    }

    ImGui::Text("Name: %s", m_currentTexture->GetName().c_str());
    ImGui::Spacing();

    ImGui::Text("Dimensions:");
    ImGui::Indent();
    ImGui::Text("Width: %u", m_currentTexture->GetWidth());
    ImGui::Text("Height: %u", m_currentTexture->GetHeight());
    ImGui::Text("Mip Levels: %u", m_currentTexture->GetMipLevels());
    ImGui::Unindent();

    ImGui::Spacing();

    ImGui::Text("Format:");
    ImGui::Indent();
    ImGui::Text("Compression: %s", TextureAsset::GetFormatName(m_currentTexture->GetCompressionFormat()));
    ImGui::Text("Type: %s", TextureAsset::GetTypeName(m_currentTexture->GetType()));
    ImGui::Text("sRGB: %s", m_currentTexture->IsSRGB() ? "Yes" : "No");
    ImGui::Unindent();

    ImGui::Spacing();

    ImGui::Text("Memory:");
    ImGui::Indent();
    size_t memSize = m_currentTexture->GetMemorySize();
    ImGui::Text("GPU: %s", FormatFileSize(memSize).c_str());
    ImGui::Unindent();

    ImGui::Spacing();

    ImGui::Text("Cache:");
    ImGui::Indent();
    ImGui::Text("Valid: %s", m_currentTexture->IsCacheValid() ? "Yes" : "No");
    ImGui::Unindent();
}

void TexturePreviewPanel::RenderCompressionSettings() {
    ImGui::Text("Compression Settings");
    ImGui::Separator();

    if (!m_currentTexture) {
        ImGui::Text("No texture selected");
        return;
    }

    // NVTT 开关
    bool useNVTT = TextureAsset::GetUseNVTT();
    if (ImGui::Checkbox("Use NVIDIA Texture Tools", &useNVTT)) {
        TextureAsset::SetUseNVTT(useNVTT);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Use NVIDIA Texture Tools for high-quality compression.\nFalls back to DirectXTex if NVTT is unavailable.");
    }

    // 显示NVTT状态
    if (useNVTT) {
        if (TextureCompressor::GetInstance().IsNVTTAvailable()) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "NVTT: Available");

            // NVTT质量选择
            const char* qualityNames[] = { "Fastest", "Normal", "Production", "Highest" };
            int currentQuality = static_cast<int>(TextureCompressor::GetInstance().GetNVTTQuality());
            if (ImGui::Combo("Quality", &currentQuality, qualityNames, IM_ARRAYSIZE(qualityNames))) {
                TextureCompressor::GetInstance().SetNVTTQuality(
                    static_cast<TextureCompressor::NVTTQuality>(currentQuality));
            }

            // 质量说明
            const char* qualityDesc = "";
            switch (static_cast<TextureCompressor::NVTTQuality>(currentQuality)) {
            case TextureCompressor::NVTTQuality::Fastest:    qualityDesc = "Fast compression, lower quality"; break;
            case TextureCompressor::NVTTQuality::Normal:     qualityDesc = "Balanced speed and quality"; break;
            case TextureCompressor::NVTTQuality::Production: qualityDesc = "High quality (recommended)"; break;
            case TextureCompressor::NVTTQuality::Highest:    qualityDesc = "Best quality, slowest"; break;
            }
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", qualityDesc);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "NVTT: Not Found (using DirectXTex)");
        }
    } else {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Using DirectXTex");
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Current: %s",
        TextureAsset::GetFormatName(m_currentTexture->GetCompressionFormat()));

    ImGui::Spacing();

    const char* formatNames[] = { "None", "BC1", "BC3", "BC5", "BC7", "BC6H (HDR)" };
    int currentFormat = static_cast<int>(m_selectedFormat);
    if (ImGui::Combo("Format", &currentFormat, formatNames, IM_ARRAYSIZE(formatNames))) {
        m_selectedFormat = static_cast<TextureCompressionFormat>(currentFormat);
    }

    // 格式说明
    const char* formatDesc = "";
    switch (m_selectedFormat) {
    case TextureCompressionFormat::None: formatDesc = "Uncompressed RGBA (largest)"; break;
    case TextureCompressionFormat::BC1: formatDesc = "RGB, 4bpp, no alpha (smallest)"; break;
    case TextureCompressionFormat::BC3: formatDesc = "RGBA, 8bpp, full alpha"; break;
    case TextureCompressionFormat::BC5: formatDesc = "RG, 8bpp, for normal maps"; break;
    case TextureCompressionFormat::BC7: formatDesc = "RGBA, 8bpp, high quality"; break;
    case TextureCompressionFormat::BC6H: formatDesc = "RGB HDR, 8bpp, for HDR textures"; break;
    }
    ImGui::TextWrapped("%s", formatDesc);

    ImGui::Spacing();

    ImGui::Checkbox("Generate Mipmaps", &m_generateMips);
    ImGui::Checkbox("sRGB Color Space", &m_sRGB);

    ImGui::Spacing();
    ImGui::Separator();

    bool hasChanges = (m_selectedFormat != m_currentTexture->GetCompressionFormat());

    if (hasChanges) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
    }

    if (ImGui::Button("Apply", ImVec2(-1, 30))) {
        ID3D12Device* device = TextureManager::GetInstance().GetDevice();
        ID3D12GraphicsCommandList* cmdList = TextureManager::GetInstance().GetCommandList();

        if (device && cmdList) {
            m_currentTexture->SetGenerateMips(m_generateMips);
            m_currentTexture->SetSRGB(m_sRGB);

            if (m_currentTexture->ApplyCompression(m_selectedFormat, device, cmdList)) {
                std::cout << "Compression applied: " << formatNames[currentFormat] << std::endl;
                CreatePreviewSRV();
            } else {
                std::cout << "Failed to apply compression!" << std::endl;
            }
        }
    }

    if (hasChanges) {
        ImGui::PopStyleColor(2);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Unsaved changes");
    }

    ImGui::Spacing();

    if (ImGui::Button("Save Asset File", ImVec2(-1, 0))) {
        if (!m_currentTexture->GetSourcePath().empty()) {
            std::wstring assetPath = m_currentTexture->GetSourcePath();
            size_t dotPos = assetPath.rfind(L'.');
            if (dotPos != std::wstring::npos) {
                assetPath = assetPath.substr(0, dotPos);
            }
            assetPath += L".texture.ast";
            m_currentTexture->SaveAssetFile(assetPath);
            std::cout << "Asset file saved" << std::endl;
        }
    }
}
