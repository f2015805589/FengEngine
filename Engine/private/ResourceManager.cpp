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

// ---------------------------------------------------------------------------
// 辅助：wstring → string（UTF-8）
// ---------------------------------------------------------------------------
static std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// 辅助：按完整路径在树中查找节点
// ---------------------------------------------------------------------------
FileTreeNode* ResourceManager::FindNodeByPath(FileTreeNode* root, const std::wstring& path) {
    if (!root) return nullptr;
    // 规范化比较：去掉末尾反斜杠
    auto normalize = [](std::wstring p) -> std::wstring {
        if (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
            p.pop_back();
        return p;
    };
    std::wstring target = normalize(path);
    if (normalize(root->fullPath) == target) return root;
    for (auto* child : root->children) {
        if (auto* found = FindNodeByPath(child, path))
            return found;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 查找 target 的父节点
// ---------------------------------------------------------------------------
FileTreeNode* ResourceManager::FindParentNode(FileTreeNode* root, FileTreeNode* target, FileTreeNode* curParent) {
    if (root == target) return curParent;
    for (auto* c : root->children) {
        if (c->isDirectory) {
            if (auto* p = FindParentNode(c, target, root)) return p;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 左侧：文件夹树（仅目录，递归 TreeNode）
// ---------------------------------------------------------------------------
void ResourceManager::RenderFolderTree(FileTreeNode* node) {
    if (!node || !node->isDirectory) return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (node->children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (node == m_leftSelected) flags |= ImGuiTreeNodeFlags_Selected;

    bool open = ImGui::TreeNodeEx(node->name.c_str(), flags);

    // 单击选中 → 右侧跳到该文件夹
    if (ImGui::IsItemClicked()) {
        m_leftSelected = node;
        m_rightCurrent = node;
        m_rightSelected = nullptr;
    }

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", WToUtf8(node->fullPath).c_str());
        ImGui::EndTooltip();
    }

    if (open) {
        for (auto* child : node->children) {
            if (child->isDirectory) RenderFolderTree(child);
        }
        ImGui::TreePop();
    }
}

// ---------------------------------------------------------------------------
// 右侧：平铺视图（显示当前目录下的文件夹+文件）
// ---------------------------------------------------------------------------
void ResourceManager::RenderTileView(FileTreeNode* node) {
    if (!node) return;

    // 面包屑导航
    ImGui::TextDisabled(">");
    ImGui::SameLine();
    ImGui::Text("%s", WToUtf8(node->fullPath).c_str());
    ImGui::Separator();

    // 没有 children
    if (node->children.empty()) {
        ImGui::TextDisabled("(empty)");
        return;
    }

    // 平铺网格：每个项占一定宽度
    float itemW = 120.0f;
    float availW = ImGui::GetContentRegionAvail().x;
    int perRow = (std::max)(1, (int)(availW / itemW));

    for (int i = 0; i < (int)node->children.size(); i++) {
        FileTreeNode* child = node->children[i];

        if (i > 0 && (i % perRow) != 0) ImGui::SameLine();

        // 选中高亮
        bool selected = (child == m_rightSelected);
        ImVec4 bg = selected ? ImVec4(0.3f, 0.5f, 0.8f, 0.6f) : ImVec4(0, 0, 0, 0);
        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 0.4f));

        // 按钮标签：文件夹加 [D]，文件加扩展名缩写
        char label[256];
        if (child->isDirectory) {
            snprintf(label, sizeof(label), "[D] %s", child->name.c_str());
        } else {
            snprintf(label, sizeof(label), "%s", child->name.c_str());
        }

        if (ImGui::Button(label, ImVec2(itemW - 8, 0))) {
            m_rightSelected = child;
        }
        ImGui::PopStyleColor(2);

        // 双击行为
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (child->isDirectory) {
                // 双击文件夹 → 进入该文件夹，左侧同步选中
                m_rightCurrent = child;
                m_rightSelected = nullptr;
                m_leftSelected = child;
            } else {
                // 双击文件 → 纹理则打开预览
                if (IsTextureFile(child->extension)) {
                    TexturePreviewPanel::GetInstance().SetTexturePath(child->fullPath);
                }
            }
        }

        // hover tooltip
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", WToUtf8(child->fullPath).c_str());
            if (!child->isDirectory && !child->extension.empty())
                ImGui::Text("Type: %s", child->extension.c_str());
            if (child->isDirectory)
                ImGui::TextDisabled("Double-click to enter");
            else if (IsTextureFile(child->extension))
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "Double-click to preview");
            ImGui::EndTooltip();
        }
    }
}

// ---------------------------------------------------------------------------
// 主窗口：双面板布局
// ---------------------------------------------------------------------------
void ResourceManager::ShowResourceWindow(bool* open) {
    if (!ImGui::Begin("Resource Manager", open)) {
        ImGui::End();
        return;
    }

    // 默认初始化：首次打开时指向 Content 根
    if (!m_rightCurrent) {
        m_rightCurrent = m_contentRoot;
        m_leftSelected = m_contentRoot;
    }

    // 上方工具栏：返回上级 + 当前路径
    {
        bool canUp = (m_rightCurrent && m_rightCurrent != m_contentRoot && m_rightCurrent != m_engineRoot);
        if (!canUp) { ImGui::BeginDisabled(); }
        if (ImGui::Button(".. (Up)") && m_rightCurrent) {
            // 查找父节点
            FileTreeNode* parent = nullptr;
            parent = FindParentNode(m_contentRoot, m_rightCurrent, nullptr);
            if (!parent) parent = FindParentNode(m_engineRoot, m_rightCurrent, nullptr);
            if (parent) {
                m_rightCurrent = parent;
                m_leftSelected = parent;
                m_rightSelected = nullptr;
            }
        }
        if (!canUp) { ImGui::EndDisabled(); }

        ImGui::SameLine();
        // 根节点切换
        if (ImGui::Button("Content")) {
            m_rightCurrent = m_contentRoot;
            m_leftSelected = m_contentRoot;
            m_rightSelected = nullptr;
        }
        ImGui::SameLine();
        if (ImGui::Button("Engine")) {
            m_rightCurrent = m_engineRoot;
            m_leftSelected = m_engineRoot;
            m_rightSelected = nullptr;
        }
    }

    ImGui::Separator();

    // 双面板：左 30% 文件夹树，右 70% 平铺
    float leftW = ImGui::GetWindowWidth() * 0.30f;

    ImGui::BeginChild("##FolderTree", ImVec2(leftW, 0), true);
    {
        // 左侧：Content 树
        if (m_contentRoot) {
            ImGui::Text("Content");
            ImGui::Separator();
            RenderFolderTree(m_contentRoot);
        }
        ImGui::Spacing();
        // 左侧：Engine 树
        if (m_engineRoot) {
            ImGui::Text("Engine");
            ImGui::Separator();
            RenderFolderTree(m_engineRoot);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##TileView", ImVec2(0, 0), true);
    {
        RenderTileView(m_rightCurrent);
    }
    ImGui::EndChild();

    ImGui::End();
}
