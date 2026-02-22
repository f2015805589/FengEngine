#include "public/ResourceManager.h"
#include "public/Material/MaterialManager.h"
#include "public/Material/Shader.h"
#include "public/Texture/TexturePreviewPanel.h"
#include "public/Texture/TextureManager.h"
#include "public/PathUtils.h"
#include <iostream>
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include "imgui.h"

#pragma comment(lib, "shlwapi.lib")

namespace {
    // 检查是否是纹理文件
    bool IsTextureFile(const std::string& extension) {
        return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
               extension == ".dds" || extension == ".bmp" || extension == ".tga" ||
               extension == ".hdr" || extension == ".ast";
    }
}

ResourceManager& ResourceManager::GetInstance() {
    static ResourceManager instance;
    return instance;
}

void ResourceManager::Initialize(ID3D12Device* device, ID3D12RootSignature* rootSig) {
    m_device = device;
    m_rootSignature = rootSig;

    // 动态设置路径（基于exe位置）
    m_contentPath = GetContentPath();
    m_enginePath = GetEnginePath();

    std::cout << "========== ResourceManager Initialized ==========" << std::endl;
    std::cout << "Content Path: ";
    std::wcout << m_contentPath << std::endl;
    std::cout << "Engine Path: ";
    std::wcout << m_enginePath << std::endl;
}

void ResourceManager::ScanAndLoadAllResources() {
    std::cout << "\n========== Scanning All Resources ==========" << std::endl;

    // 清空之前的资源列表
    m_shaderResources.clear();
    m_materialResources.clear();

    // 清空之前的文件树
    delete m_contentRoot;
    delete m_engineRoot;
    m_contentRoot = nullptr;
    m_engineRoot = nullptr;

    // 1. 扫描 Engine 层级的 shaders
    std::cout << "\n[Engine Layer] Scanning shaders..." << std::endl;
    ScanShaders(m_enginePath + L"Shader/", ResourceLayer::Engine);

    // 2. 扫描 Content 层级的 shaders
    std::cout << "\n[Content Layer] Scanning shaders..." << std::endl;
    ScanShaders(m_contentPath + L"Shaders/", ResourceLayer::Content);

    // 3. 扫描 Engine 层级的 materials
    std::cout << "\n[Engine Layer] Scanning materials..." << std::endl;
    ScanMaterials(m_enginePath + L"Shader/", ResourceLayer::Engine);

    // 4. 扫描 Content 层级的 materials
    std::cout << "\n[Content Layer] Scanning materials..." << std::endl;
    ScanMaterials(m_contentPath + L"Materials/", ResourceLayer::Content);

    // 5. 扫描完整的目录树（所有文件和文件夹）
    std::cout << "\n[Scanning Complete Directory Trees]" << std::endl;

    // 创建根节点
    m_contentRoot = new FileTreeNode();
    m_contentRoot->name = "Content";
    m_contentRoot->fullPath = m_contentPath;
    m_contentRoot->isDirectory = true;
    ScanCompleteDirectory(m_contentPath, m_contentRoot);

    m_engineRoot = new FileTreeNode();
    m_engineRoot->name = "Engine";
    m_engineRoot->fullPath = m_enginePath;
    m_engineRoot->isDirectory = true;
    ScanCompleteDirectory(m_enginePath, m_engineRoot);

    std::cout << "\n========== Scan Complete ==========" << std::endl;
    std::cout << "Total shaders found: " << m_shaderResources.size() << std::endl;
    std::cout << "Total materials found: " << m_materialResources.size() << std::endl;
    std::cout << "Shaders need to be loaded manually in main.cpp" << std::endl;
}

void ResourceManager::ScanShaders(const std::wstring& directory, ResourceLayer layer) {
    ScanDirectory(directory, L".shader", layer, m_shaderResources);
}

void ResourceManager::ScanMaterials(const std::wstring& directory, ResourceLayer layer) {
    ScanDirectory(directory, L".material", layer, m_materialResources);
}

void ResourceManager::ScanCompleteDirectory(const std::wstring& directory, FileTreeNode* parentNode) {
    // 检查目录是否存在
    DWORD attribs = GetFileAttributesW(directory.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES || !(attribs & FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }

    // 搜索所有文件和文件夹
    std::wstring searchPath = directory + L"*";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        std::wstring fileName = findData.cFileName;

        // 跳过 "." 和 ".."
        if (fileName == L"." || fileName == L"..") {
            continue;
        }

        std::wstring fullPath = directory + fileName;

        // 创建新节点
        FileTreeNode* newNode = new FileTreeNode();

        // 转换名称为 std::string
        int len = WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            newNode->name.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, &newNode->name[0], len, nullptr, nullptr);
        }

        newNode->fullPath = fullPath;

        // 如果是目录
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            newNode->isDirectory = true;
            parentNode->children.push_back(newNode);
            // 递归扫描子目录
            ScanCompleteDirectory(fullPath + L"/", newNode);
        }
        // 如果是文件
        else {
            newNode->isDirectory = false;

            // 提取扩展名
            size_t dotPos = fileName.find_last_of(L".");
            if (dotPos != std::wstring::npos) {
                std::wstring extW = fileName.substr(dotPos);
                int extLen = WideCharToMultiByte(CP_UTF8, 0, extW.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (extLen > 0) {
                    newNode->extension.resize(extLen - 1);
                    WideCharToMultiByte(CP_UTF8, 0, extW.c_str(), -1, &newNode->extension[0], extLen, nullptr, nullptr);
                }
            }

            parentNode->children.push_back(newNode);
        }
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);

    // 排序子节点：文件夹在前，文件在后
    std::sort(parentNode->children.begin(), parentNode->children.end(),
        [](FileTreeNode* a, FileTreeNode* b) {
            // 如果一个是文件夹，一个是文件，文件夹优先
            if (a->isDirectory && !b->isDirectory) return true;
            if (!a->isDirectory && b->isDirectory) return false;
            // 如果都是文件夹或都是文件，按名称排序
            return a->name < b->name;
        });
}

void ResourceManager::ScanDirectory(const std::wstring& directory, const std::wstring& extension, ResourceLayer layer, std::vector<ResourceInfo>& outResources) {
    // 检查目录是否存在
    DWORD attribs = GetFileAttributesW(directory.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES || !(attribs & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wcout << L"  Directory not found: " << directory << std::endl;
        return;
    }

    // 搜索所有文件
    std::wstring searchPath = directory + L"*";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        std::wcout << L"  Failed to search directory: " << directory << std::endl;
        return;
    }

    do {
        std::wstring fileName = findData.cFileName;

        // 跳过 "." 和 ".."
        if (fileName == L"." || fileName == L"..") {
            continue;
        }

        std::wstring fullPath = directory + fileName;

        // 如果是目录，递归扫描
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDirectory(fullPath + L"/", extension, layer, outResources);
        }
        // 如果是目标文件
        else if (fileName.size() >= extension.size() &&
                 fileName.substr(fileName.size() - extension.size()) == extension) {

            // 排除 screen.shader 和 sky.shader（不需要预编译，仅用于特殊Pass）
            if (fileName == L"screen.shader" || fileName == L"Screen.shader" ||
                fileName == L"sky.shader" || fileName == L"Sky.shader") {
                std::cout << "  Skipped (excluded): " << ExtractResourceName(fullPath) << std::endl;
                continue;
            }

            ResourceInfo resInfo;
            resInfo.name = ExtractResourceName(fullPath);
            resInfo.filePath = fullPath;
            resInfo.layer = layer;
            resInfo.type = "shader";
            resInfo.isLoaded = false;

            outResources.push_back(resInfo);

            std::cout << "  Found: " << resInfo.name << std::endl;
        }
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
}

std::string ResourceManager::ExtractResourceName(const std::wstring& filePath) {
    // 提取文件名（不含扩展名）
    size_t lastSlash = filePath.find_last_of(L"/\\");
    size_t lastDot = filePath.find_last_of(L".");

    std::wstring fileName;
    if (lastSlash != std::wstring::npos) {
        if (lastDot != std::wstring::npos && lastDot > lastSlash) {
            fileName = filePath.substr(lastSlash + 1, lastDot - lastSlash - 1);
        } else {
            fileName = filePath.substr(lastSlash + 1);
        }
    } else {
        if (lastDot != std::wstring::npos) {
            fileName = filePath.substr(0, lastDot);
        } else {
            fileName = filePath;
        }
    }

    // 转换为 std::string
    int len = WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result;
    if (len > 0) {
        result.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, &result[0], len, nullptr, nullptr);
    }

    return result;
}

int ResourceManager::GetLoadedShaderCount() const {
    int count = 0;
    for (const auto& res : m_shaderResources) {
        if (res.isLoaded) count++;
    }
    return count;
}

int ResourceManager::GetLoadedMaterialCount() const {
    int count = 0;
    for (const auto& res : m_materialResources) {
        if (res.isLoaded) count++;
    }
    return count;
}

void ResourceManager::RenderFileTreeNode(FileTreeNode* node) {
    if (!node) return;

    if (node->isDirectory) {
        // 文件夹节点
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (node->children.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }

        bool nodeOpen = ImGui::TreeNodeEx(node->name.c_str(), flags);

        // 显示路径提示
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            std::string pathStr;
            int len = WideCharToMultiByte(CP_UTF8, 0, node->fullPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                pathStr.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, node->fullPath.c_str(), -1, &pathStr[0], len, nullptr, nullptr);
            }
            ImGui::Text("%s", pathStr.c_str());
            ImGui::EndTooltip();
        }

        if (nodeOpen) {
            // 递归渲染子节点
            for (auto* child : node->children) {
                RenderFileTreeNode(child);
            }
            ImGui::TreePop();
        }
    } else {
        // 文件节点
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

        // 根据文件类型显示不同颜色
        ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        if (node->extension == ".shader") {
            color = ImVec4(0.5f, 1.0f, 0.5f, 1.0f);  // 绿色 - shader文件
        } else if (node->extension == ".hlsl") {
            color = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);  // 蓝色 - hlsl文件
        } else if (node->extension == ".mesh") {
            color = ImVec4(1.0f, 0.8f, 0.5f, 1.0f);  // 橙色 - mesh文件
        } else if (node->extension == ".level") {
            color = ImVec4(1.0f, 0.5f, 1.0f, 1.0f);  // 粉色 - level文件
        } else if (node->extension == ".png" || node->extension == ".jpg" ||
                   node->extension == ".dds" || node->extension == ".hdr" ||
                   node->extension == ".ast") {
            color = ImVec4(1.0f, 1.0f, 0.5f, 1.0f);  // 黄色 - 纹理文件
        } else if (node->extension == ".material") {
            color = ImVec4(0.8f, 0.5f, 1.0f, 1.0f);  // 紫色 - 材质文件
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TreeNodeEx(node->name.c_str(), flags);
        ImGui::PopStyleColor();

        // 双击纹理文件打开预览
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (IsTextureFile(node->extension)) {
                // 打开纹理预览面板
                TexturePreviewPanel::GetInstance().SetTexturePath(node->fullPath);
                std::cout << "Opening texture preview: " << node->name << std::endl;
            }
        }

        // 显示路径提示
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            std::string pathStr;
            int len = WideCharToMultiByte(CP_UTF8, 0, node->fullPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                pathStr.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, node->fullPath.c_str(), -1, &pathStr[0], len, nullptr, nullptr);
            }
            ImGui::Text("%s", pathStr.c_str());
            ImGui::Text("Extension: %s", node->extension.c_str());

            // 纹理文件提示
            if (IsTextureFile(node->extension)) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Double-click to preview");
            }

            ImGui::EndTooltip();
        }
    }
}

void ResourceManager::ShowResourceWindow(bool* open) {
    if (!ImGui::Begin("Resource Manager", open)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Resource Manager");
    ImGui::Separator();

    // 统计信息
    ImGui::Text("Total Shaders Found: %d", GetTotalShaderCount());
    ImGui::Text("Total Materials Found: %d", GetTotalMaterialCount());
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Note: Resources are loaded automatically at startup");
    ImGui::Separator();

    // 文件树显示
    if (ImGui::CollapsingHeader("File Browser", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();

        // Content 层级完整目录树
        if (m_contentRoot) {
            RenderFileTreeNode(m_contentRoot);
        }

        ImGui::Spacing();

        // Engine 层级完整目录树
        if (m_engineRoot) {
            RenderFileTreeNode(m_engineRoot);
        }

        ImGui::Unindent();
    }

    ImGui::End();
}
