#include "public/Material/MaterialEditorPanel.h"
#include "public/Material/MaterialManager.h"
#include "public/Material/MaterialInstance.h"
#include "public/Material/Shader.h"
#include "public/Material/ShaderParameter.h"
#include "public/Texture/TextureManager.h"
#include "public/Texture/TextureAsset.h"
#include "public/Scene.h"
#include "public/Actor.h"
#include "imgui.h"
#include <Windows.h>
#include <commdlg.h>
#include <DirectXMath.h>
#include <fstream>
#include <iostream>

using namespace DirectX;

MaterialEditorPanel::MaterialEditorPanel()
    : m_showWindow(false)  // 默认关闭
    , m_showCreateDialog(false)
    , m_scene(nullptr)
    , m_targetActor(nullptr)
{
    memset(m_newMaterialName, 0, sizeof(m_newMaterialName));
    memset(m_savePath, 0, sizeof(m_savePath));
    memset(m_loadPath, 0, sizeof(m_loadPath));
}

void MaterialEditorPanel::RenderUI() {
    if (!m_showWindow) return;

    ImGui::Begin("Material Editor", &m_showWindow, ImGuiWindowFlags_AlwaysAutoResize);

    RenderMaterialSelector();
    ImGui::Separator();

    MaterialInstance* currentMaterial = MaterialManager::GetInstance()
        .GetMaterial(m_selectedMaterialName);

    if (currentMaterial) {
        RenderParameterEditors();
        ImGui::Separator();
        RenderTextureSlots();
        ImGui::Separator();
        RenderControlButtons();
    } else {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "No material selected");
    }

    ImGui::End();

    // 创建新材质对话框
    if (m_showCreateDialog) {
        ImGui::Begin("Create New Material", &m_showCreateDialog);
        ImGui::InputText("Material Name", m_newMaterialName, sizeof(m_newMaterialName));
        if (ImGui::Button("Create")) {
            OnCreateNewMaterial();
            m_showCreateDialog = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_showCreateDialog = false;
        }
        ImGui::End();
    }
}

void MaterialEditorPanel::RenderMaterialSelector() {
    auto materialNames = MaterialManager::GetInstance().GetAllMaterialNames();

    ImGui::Text("Current Material:");
    if (ImGui::BeginCombo("##MaterialCombo", m_selectedMaterialName.c_str())) {
        for (const auto& name : materialNames) {
            bool isSelected = (m_selectedMaterialName == name);
            if (ImGui::Selectable(name.c_str(), isSelected)) {
                m_selectedMaterialName = name;
                // 自动应用材质到mesh
                OnApplyToMesh();
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    MaterialInstance* mat = MaterialManager::GetInstance().GetMaterial(m_selectedMaterialName);
    if (mat && mat->GetShader()) {
        ImGui::Text("Shader: %s", mat->GetShader()->GetName().c_str());
    }
}

void MaterialEditorPanel::RenderParameterEditors() {
    MaterialInstance* material = MaterialManager::GetInstance()
        .GetMaterial(m_selectedMaterialName);
    if (!material || !material->GetShader()) return;

    ImGui::Text("Parameters:");

    const auto& parameters = material->GetShader()->GetParameters();
    for (const auto& param : parameters) {
        // 跳过纹理参数（在单独区域显示）
        if (param.type == ShaderParameterType::Texture2D ||
            param.type == ShaderParameterType::TextureCube) {
            continue;
        }

        switch (param.type) {
            case ShaderParameterType::Float:
                RenderFloatParameter(param, material);
                break;
            case ShaderParameterType::Vector4:
            case ShaderParameterType::Vector3:
                RenderVectorParameter(param, material);
                break;
            case ShaderParameterType::Int:
                RenderIntParameter(param, material);
                break;
            case ShaderParameterType::Bool:
                RenderBoolParameter(param, material);
                break;
            default:
                break;
        }
    }
}

void MaterialEditorPanel::RenderFloatParameter(const ShaderParameter& param,
                                               MaterialInstance* material) {
    float value = material->GetFloat(param.name);
    std::string label = param.name;

    if (param.uiWidget == "Slider") {
        if (ImGui::SliderFloat(label.c_str(), &value, param.minValue, param.maxValue)) {
            material->SetFloat(param.name, value);
        }
    } else {
        if (ImGui::InputFloat(label.c_str(), &value)) {
            material->SetFloat(param.name, value);
        }
    }
}

void MaterialEditorPanel::RenderVectorParameter(const ShaderParameter& param,
                                                MaterialInstance* material) {
    std::string label = param.name;

    if (param.type == ShaderParameterType::Vector4) {
        XMFLOAT4 value = material->GetVector(param.name);
        float color[4] = { value.x, value.y, value.z, value.w };

        if (param.uiWidget == "ColorPicker") {
            if (ImGui::ColorEdit4(label.c_str(), color)) {
                material->SetVector(param.name, XMFLOAT4(color[0], color[1], color[2], color[3]));
            }
        } else {
            if (ImGui::InputFloat4(label.c_str(), color)) {
                material->SetVector(param.name, XMFLOAT4(color[0], color[1], color[2], color[3]));
            }
        }
    } else if (param.type == ShaderParameterType::Vector3) {
        XMFLOAT3 value = material->GetVector3(param.name);
        float vec[3] = { value.x, value.y, value.z };

        if (param.uiWidget == "ColorPicker") {
            if (ImGui::ColorEdit3(label.c_str(), vec)) {
                material->SetVector3(param.name, XMFLOAT3(vec[0], vec[1], vec[2]));
            }
        } else {
            if (ImGui::InputFloat3(label.c_str(), vec)) {
                material->SetVector3(param.name, XMFLOAT3(vec[0], vec[1], vec[2]));
            }
        }
    }
}

void MaterialEditorPanel::RenderIntParameter(const ShaderParameter& param,
                                             MaterialInstance* material) {
    int value = material->GetInt(param.name);
    std::string label = param.name;

    if (ImGui::InputInt(label.c_str(), &value)) {
        material->SetInt(param.name, value);
    }
}

void MaterialEditorPanel::RenderBoolParameter(const ShaderParameter& param,
                                              MaterialInstance* material) {
    bool value = material->GetBool(param.name);
    std::string label = param.name;

    if (ImGui::Checkbox(label.c_str(), &value)) {
        material->SetBool(param.name, value);
    }
}

void MaterialEditorPanel::RenderTextureParameter(const ShaderParameter& param,
                                                 MaterialInstance* material) {
    std::wstring texPath = material->GetTexture(param.name);

    // 转换为string显示
    int len = WideCharToMultiByte(CP_UTF8, 0, texPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string texPathStr;
    if (len > 0) {
        texPathStr.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, texPath.c_str(), -1, &texPathStr[0], len, nullptr, nullptr);
    }

    // 显示当前纹理的SRV索引（Bindless）
    UINT currentSRVIndex = material->GetTextureSRVIndex(param.name);
    ImGui::Text("%s (SRV: %u):", param.name.c_str(), currentSRVIndex);
    ImGui::SameLine();
    ImGui::TextWrapped("%s", texPathStr.c_str());

    if (ImGui::Button(("Browse##" + param.name).c_str())) {
        // 打开文件对话框
        OPENFILENAMEW ofn;
        wchar_t fileName[MAX_PATH] = L"";
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = L"Texture Files\\0*.png;*.dds;*.jpg;*.tga;*.texture.ast\\0Texture Asset\\0*.texture.ast\\0All Files\\0*.*\\0\\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        ofn.lpstrDefExt = L"texture.ast";

        if (GetOpenFileNameW(&ofn)) {
            // 设置纹理路径到材质
            material->SetTexture(param.name, fileName);

            // 确保TextureManager有commandList（从全局获取）
            extern ID3D12GraphicsCommandList* gCommandList;
            if (gCommandList) {
                TextureManager::GetInstance().SetCommandList(gCommandList);
            }

            // 加载纹理资源 - 使用TextureManager加载纹理
            TextureAsset* textureAsset = TextureManager::GetInstance().LoadTexture(fileName);
            if (textureAsset && textureAsset->IsLoaded()) {
                // Bindless模式：在Scene的全局SRV堆中分配槽位
                UINT srvIndex = Scene::AllocateBindlessSRVSlot();
                if (srvIndex != UINT_MAX) {
                    // 在Scene的SRV堆中创建纹理的SRV
                    Scene::CreateBindlessTextureSRV(srvIndex, textureAsset->GetResource());

                    // 将SRV索引存储到材质实例中（会写入CB）
                    // 注意：shader中g_BindlessTextures从t10开始，所以需要存储相对偏移量
                    // srvIndex是绝对索引（10, 11, 12...），需要减去10得到相对偏移量（0, 1, 2...）
                    UINT relativeIndex = srvIndex - 10;  // t10对应偏移0
                    material->SetTextureSRVIndex(param.name, relativeIndex);

                    // 保留旧的资源引用（用于兼容）
                    material->SetTextureResource(param.name, textureAsset->GetResource(), param.registerSlot);

                    std::cout << "MaterialEditorPanel: Texture '" << param.name
                              << "' loaded at SRV slot " << srvIndex
                              << " (relative index = " << relativeIndex << ")" << std::endl;
                } else {
                    std::cout << "MaterialEditorPanel: Failed to allocate SRV slot" << std::endl;
                }
            } else {
                std::cout << "MaterialEditorPanel: Failed to load texture" << std::endl;
            }
        }
    }
}

void MaterialEditorPanel::RenderTextureSlots() {
    MaterialInstance* material = MaterialManager::GetInstance()
        .GetMaterial(m_selectedMaterialName);
    if (!material || !material->GetShader()) return;

    const auto& parameters = material->GetShader()->GetParameters();

    // 调试：显示参数总数和纹理参数数量
    int textureCount = 0;
    for (const auto& param : parameters) {
        if (param.type == ShaderParameterType::Texture2D ||
            param.type == ShaderParameterType::TextureCube) {
            textureCount++;
        }
    }
    ImGui::Text("Textures: (%d total params, %d textures)", (int)parameters.size(), textureCount);

    for (const auto& param : parameters) {
        if (param.type == ShaderParameterType::Texture2D ||
            param.type == ShaderParameterType::TextureCube) {
            RenderTextureParameter(param, material);
        }
    }
}

void MaterialEditorPanel::RenderControlButtons() {
    if (ImGui::Button("Save Material")) {
        OnSaveMaterial();
    }

    ImGui::SameLine();
    if (ImGui::Button("Load Material")) {
        OnLoadMaterial();
    }

    // 临时测试按钮：直接加载ToonMaterial
    ImGui::SameLine();
    if (ImGui::Button("Test Load Toon")) {
        std::ofstream logFile("Engine/Shader/Shader_Cache/log/ui_debug.txt", std::ios::app);
        if (logFile.is_open()) {
            logFile << "Test Load Toon button clicked" << std::endl;
            logFile.close();
        }

        MaterialInstance* mat = MaterialManager::GetInstance().LoadMaterial(L"Content/Materials/toonmaterial.material");

        std::ofstream logFile2("Engine/Shader/Shader_Cache/log/ui_debug.txt", std::ios::app);
        if (logFile2.is_open()) {
            logFile2 << "LoadMaterial returned: " << (mat ? "SUCCESS" : "NULL") << std::endl;
            logFile2.close();
        }

        if (mat) {
            m_selectedMaterialName = mat->GetName();
            OnApplyToMesh();
        }
    }

    // 显示最后加载的材质名称（用于调试）
    static std::string lastLoadedMaterial = "None";
    ImGui::SameLine();
    ImGui::TextDisabled("Last loaded: %s", lastLoadedMaterial.c_str());

    ImGui::SameLine();
    if (ImGui::Button("Create New")) {
        m_showCreateDialog = true;
    }

    // 提示：材质会在选择/加载时自动应用到mesh
    ImGui::Separator();
    ImGui::TextDisabled("Tip: Materials auto-apply to mesh when selected");
}

void MaterialEditorPanel::OnSaveMaterial() {
    MaterialInstance* material = MaterialManager::GetInstance()
        .GetMaterial(m_selectedMaterialName);
    if (!material) return;

    // 打开保存文件对话框
    OPENFILENAMEW ofn;
    wchar_t fileName[MAX_PATH] = L"";
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"Material Files (*.material)\0*.material\0Legacy Material (*.ast)\0*.ast\0All Files\0*.*\0\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = L"material";

    if (GetSaveFileNameW(&ofn)) {
        if (MaterialManager::GetInstance().SaveMaterial(material, fileName)) {
            // 保存成功
        }
    }
}

void MaterialEditorPanel::OnLoadMaterial() {
    // 写入日志文件用于调试
    std::ofstream logFile("Engine/Shader/Shader_Cache/log/ui_debug.txt", std::ios::app);
    if (logFile.is_open()) {
        logFile << "OnLoadMaterial: Function called" << std::endl;
        logFile.close();
    }

    OutputDebugStringA("OnLoadMaterial: Starting file dialog...\n");

    // 获取活动窗口句柄
    HWND hwnd = GetActiveWindow();
    if (!hwnd) {
        hwnd = GetForegroundWindow();
    }

    std::ofstream logFile1("Engine/Shader/Shader_Cache/log/ui_debug.txt", std::ios::app);
    if (logFile1.is_open()) {
        logFile1 << "Window handle: " << (hwnd ? "VALID" : "NULL") << std::endl;
        logFile1.close();
    }

    // 打开文件对话框
    OPENFILENAMEW ofn;
    wchar_t fileName[MAX_PATH] = L"";
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;  // 使用实际窗口句柄
    ofn.lpstrFilter = L"Material Files (*.material)\0*.material\0Legacy Material (*.ast)\0*.ast\0All Files\0*.*\0\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"material";

    std::ofstream logFile1b("Engine/Shader/Shader_Cache/log/ui_debug.txt", std::ios::app);
    if (logFile1b.is_open()) {
        logFile1b << "About to call GetOpenFileNameW..." << std::endl;
        logFile1b.close();
    }

    BOOL dialogResult = GetOpenFileNameW(&ofn);

    std::ofstream logFile2("Engine/Shader/Shader_Cache/log/ui_debug.txt", std::ios::app);
    if (logFile2.is_open()) {
        logFile2 << "Dialog result: " << (dialogResult ? "SUCCESS" : "FAILED") << std::endl;
        if (dialogResult) {
            std::wstring ws(fileName);
            std::string debugPath(ws.begin(), ws.end());
            logFile2 << "Selected file: " << debugPath << std::endl;
        }
        logFile2.close();
    }

    if (dialogResult) {
        // 转换为string用于调试
        std::wstring ws(fileName);
        std::string debugPath(ws.begin(), ws.end());
        OutputDebugStringA(("OnLoadMaterial: Selected file: " + debugPath + "\n").c_str());

        std::ofstream logFile3("Engine/Shader/Shader_Cache/log/ui_debug.txt", std::ios::app);
        if (logFile3.is_open()) {
            logFile3 << "About to call LoadMaterial..." << std::endl;
            logFile3.close();
        }

        MaterialInstance* mat = MaterialManager::GetInstance().LoadMaterial(fileName);

        std::ofstream logFile4("Engine/Shader/Shader_Cache/log/ui_debug.txt", std::ios::app);
        if (logFile4.is_open()) {
            logFile4 << "LoadMaterial returned: " << (mat ? "SUCCESS" : "NULL") << std::endl;
            logFile4.close();
        }

        if (mat) {
            m_selectedMaterialName = mat->GetName();
            OutputDebugStringA(("OnLoadMaterial: Material loaded successfully: " + m_selectedMaterialName + "\n").c_str());
            OnApplyToMesh();
        } else {
            OutputDebugStringA("OnLoadMaterial: LoadMaterial returned NULL!\n");
        }
    } else {
        OutputDebugStringA("OnLoadMaterial: User cancelled or dialog failed\n");
    }
}

void MaterialEditorPanel::OnCreateNewMaterial() {
    if (strlen(m_newMaterialName) == 0) return;

    // 获取StandardPBR shader
    Shader* shader = MaterialManager::GetInstance().GetShader("StandardPBR");
    if (!shader) return;

    // 创建新材质
    MaterialInstance* mat = MaterialManager::GetInstance()
        .CreateMaterial(m_newMaterialName, shader);

    if (mat) {
        m_selectedMaterialName = m_newMaterialName;
        memset(m_newMaterialName, 0, sizeof(m_newMaterialName));
        // 自动应用到mesh
        OnApplyToMesh();
    }
}

void MaterialEditorPanel::SetSelectedMaterial(const std::string& materialName) {
    m_selectedMaterialName = materialName;
}

void MaterialEditorPanel::OnApplyToMesh() {
    if (!m_scene) return;

    MaterialInstance* material = MaterialManager::GetInstance()
        .GetMaterial(m_selectedMaterialName);

    if (!material) return;

    // 优先应用到目标Actor
    if (m_targetActor) {
        m_targetActor->SetMaterial(material);
        OutputDebugStringA("Material applied to target Actor\n");
    }
    // 否则应用到StaticMesh（向后兼容）
    else if (m_scene->GetStaticMesh()) {
        m_scene->GetStaticMesh()->SetMaterial(material);
        OutputDebugStringA("Material applied to StaticMesh\n");
    }
}
