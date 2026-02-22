# 材质系统集成指南

## 已完成的工作

### 1. 核心类实现 ✓
- [x] `ShaderParameter.h` - 参数定义系统
- [x] `Shader.h/cpp` - Shader类（XML解析和编译）
- [x] `MaterialInstance.h/cpp` - 材质实例类（参数管理和GPU资源）
- [x] `MaterialManager.h/cpp` - 材质管理器单例

### 2. GPU资源集成 ✓
- [x] 扩展Root Signature添加槽位2 (b1) 用于材质常量缓冲区
- [x] 修改`StaticMeshComponent`支持材质绑定
- [x] 更新`Scene.cpp`调用Render时传递rootSignature

### 3. Shader和材质文件 ✓
- [x] `StandardPBR.shader.ast` - Shader定义XML
- [x] `standard_pbr.hlsl` - PBR shader代码
- [x] `DefaultPBR.ast` - 默认材质实例

---

## 如何在main.cpp中集成材质系统

### 步骤1: 添加Include
在`main.cpp`顶部添加：

```cpp
#include "public/Material/MaterialManager.h"
#include "public/Material/MaterialInstance.h"
```

### 步骤2: 初始化MaterialManager
在`WinMain`函数中，InitD3D12之后添加：

```cpp
// 在 InitD3D12(hwnd, viewportWidth, viewportHeight) 之后
// 在 InitImGui() 之前

// 初始化材质管理器
if (!MaterialManager::GetInstance().Initialize(gD3D12Device)) {
    MessageBox(NULL, L"MaterialManager初始化失败!", L"错误", MB_OK | MB_ICONERROR);
    return -1;
}

// 加载StandardPBR shader
Shader* standardShader = MaterialManager::GetInstance()
    .LoadShader(L"Content/Shader/StandardPBR.shader.ast");
if (!standardShader) {
    MessageBox(NULL, L"加载StandardPBR shader失败!", L"错误", MB_OK | MB_ICONERROR);
    return -1;
}

// 编译Shader并创建PSO（需要在root signature创建之后）
// 注意：这部分需要在InitRootSignature()之后执行
```

### 步骤3: 创建并分配材质
在Scene初始化完成后（`g_scene->Initialize(commandList)`之后）添加：

```cpp
// 在 g_scene->Initialize(commandList) 之后
// 在 LightPass 初始化之前

// 创建默认材质实例
Shader* standardShader = MaterialManager::GetInstance().GetShader("StandardPBR");
MaterialInstance* defaultMaterial = MaterialManager::GetInstance()
    .CreateMaterial("DefaultPBR", standardShader);

if (!defaultMaterial) {
    MessageBox(NULL, L"创建默认材质失败!", L"错误", MB_OK | MB_ICONERROR);
    return -1;
}

// 设置材质参数（可选，使用XML中定义的默认值）
defaultMaterial->SetVector("BaseColor", XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f));
defaultMaterial->SetFloat("Roughness", 0.7f);
defaultMaterial->SetFloat("Metallic", 0.0f);

// 为场景中的mesh分配材质
g_scene->GetStaticMesh()->SetMaterial(defaultMaterial);
```

### 步骤4: 在主循环渲染前更新材质
在主循环的Update部分添加（可选）：

```cpp
// 在 g_scene->Update(deltaTime) 之后
// 如果材质参数有修改，确保更新
// MaterialManager会自动管理脏标记
```

---

## MaterialEditorPanel完整实现

### Material EditorPanel.h

```cpp
#pragma once
#include <string>
#include <vector>
#include "Material/MaterialInstance.h"

class MaterialEditorPanel {
public:
    MaterialEditorPanel();
    ~MaterialEditorPanel() = default;

    // 在ImGui循环中调用
    void RenderUI();

    // 设置当前选中的材质
    void SetSelectedMaterial(const std::string& materialName);

private:
    std::string m_selectedMaterialName;
    bool m_showWindow;

    // UI渲染子函数
    void RenderMaterialSelector();
    void RenderParameterEditors();
    void RenderTextureSlots();
    void RenderControlButtons();

    // 参数类型特定的渲染
    void RenderFloatParameter(const ShaderParameter& param, MaterialInstance* material);
    void RenderVectorParameter(const ShaderParameter& param, MaterialInstance* material);
    void RenderIntParameter(const ShaderParameter& param, MaterialInstance* material);
    void RenderBoolParameter(const ShaderParameter& param, MaterialInstance* material);
    void RenderTextureParameter(const ShaderParameter& param, MaterialInstance* material);

    // 文件保存/加载
    void OnSaveMaterial();
    void OnLoadMaterial();
    void OnCreateNewMaterial();

    // 临时变量
    char m_newMaterialName[256];
    char m_savePath[512];
    char m_loadPath[512];
    bool m_showCreateDialog;
};
```

### MaterialEditorPanel.cpp

```cpp
#include "../public/Material/MaterialEditorPanel.h"
#include "../public/Material/MaterialManager.h"
#include "imgui.h"
#include <Windows.h>
#include <commdlg.h>

MaterialEditorPanel::MaterialEditorPanel()
    : m_showWindow(true)
    , m_showCreateDialog(false)
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

    ImGui::Text("%s:", param.name.c_str());
    ImGui::SameLine();
    ImGui::TextWrapped("%s", texPathStr.c_str());

    if (ImGui::Button(("Browse##" + param.name).c_str())) {
        // 打开文件对话框
        OPENFILENAMEW ofn;
        wchar_t fileName[MAX_PATH] = L"";
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = L"Texture Files\\0*.png;*.dds;*.jpg;*.tga\\0All Files\\0*.*\\0\\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        ofn.lpstrDefExt = L"png";

        if (GetOpenFileNameW(&ofn)) {
            material->SetTexture(param.name, fileName);
        }
    }
}

void MaterialEditorPanel::RenderTextureSlots() {
    MaterialInstance* material = MaterialManager::GetInstance()
        .GetMaterial(m_selectedMaterialName);
    if (!material || !material->GetShader()) return;

    ImGui::Text("Textures:");

    const auto& parameters = material->GetShader()->GetParameters();
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

    ImGui::SameLine();
    if (ImGui::Button("Create New")) {
        m_showCreateDialog = true;
    }
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
    ofn.lpstrFilter = L"Material Files\\0*.ast\\0All Files\\0*.*\\0\\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = L"ast";

    if (GetSaveFileNameW(&ofn)) {
        if (MaterialManager::GetInstance().SaveMaterial(material, fileName)) {
            // 保存成功
        }
    }
}

void MaterialEditorPanel::OnLoadMaterial() {
    // 打开文件对话框
    OPENFILENAMEW ofn;
    wchar_t fileName[MAX_PATH] = L"";
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"Material Files\\0*.ast\\0All Files\\0*.*\\0\\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"ast";

    if (GetOpenFileNameW(&ofn)) {
        MaterialInstance* mat = MaterialManager::GetInstance().LoadMaterial(fileName);
        if (mat) {
            m_selectedMaterialName = mat->GetName();
        }
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
    }
}

void MaterialEditorPanel::SetSelectedMaterial(const std::string& materialName) {
    m_selectedMaterialName = materialName;
}
```

---

## 在main.cpp中添加MaterialEditorPanel

### 1. Include头文件
```cpp
#include "public/Material/MaterialEditorPanel.h"
```

### 2. 声明全局变量
```cpp
MaterialEditorPanel* g_materialEditor = nullptr;
bool showMaterialEditor = false;
```

### 3. 初始化面板
在ImGui初始化后：
```cpp
g_materialEditor = new MaterialEditorPanel();
g_materialEditor->SetSelectedMaterial("DefaultPBR");
```

### 4. 在主窗口添加checkbox
在`showWindow`的ImGui::Begin块中添加：
```cpp
ImGui::Checkbox("Material Editor", &showMaterialEditor);
```

### 5. 渲染材质编辑器
在UI Pass的ImGui渲染部分添加：
```cpp
if (showMaterialEditor && g_materialEditor) {
    g_materialEditor->RenderUI();
}
```

### 6. 清理资源
在WinMain结尾添加：
```cpp
delete g_materialEditor;
MaterialManager::GetInstance().Shutdown();
```

---

## 下一步测试

1. **编译项目**
   - 确保所有新文件都添加到项目中
   - 检查include路径

2. **运行测试**
   - 加载StandardPBR shader
   - 创建DefaultPBR材质
   - 观察mesh是否使用了新材质
   - 打开材质编辑器，修改参数（颜色、粗糙度、金属度）
   - 观察实时效果

3. **常见问题排查**
   - 如果shader编译失败：检查.hlsl文件路径
   - 如果XML解析失败：确认文件编码为UTF-8
   - 如果材质不显示：检查Root Signature是否正确扩展
   - 如果参数不生效：确认材质CB是否正确绑定到槽位2

---

## 文件清单

### 新增文件 (12个)
1. Engine/public/Material/ShaderParameter.h
2. Engine/public/Material/Shader.h
3. Engine/private/Material/Shader.cpp
4. Engine/public/Material/MaterialInstance.h
5. Engine/private/Material/MaterialInstance.cpp
6. Engine/public/Material/MaterialManager.h
7. Engine/private/Material/MaterialManager.cpp
8. Engine/public/Material/MaterialEditorPanel.h
9. Engine/private/Material/MaterialEditorPanel.cpp
10. Content/Shader/StandardPBR.shader.ast
11. Content/Shader/standard_pbr.hlsl
12. Content/Materials/DefaultPBR.ast

### 修改文件 (5个)
1. Engine/private/BattleFireDirect.cpp (Root Signature扩展)
2. Engine/public/StaticMeshComponent.h (材质成员和方法)
3. Engine/private/StaticMeshComponent.cpp (Render方法)
4. Engine/public/Scene.h (GetStaticMesh方法)
5. Engine/private/Scene.cpp (Render调用)

---

## 材质系统架构总结

```
MaterialManager (单例)
    ├── 管理 Shader (StandardPBR, 等)
    │   ├── 解析 .shader.ast XML
    │   ├── 编译 VS/PS
    │   ├── 定义 Parameters (Float/Vector/Texture等)
    │   └── 创建 PSO
    │
    └── 管理 MaterialInstance (DefaultPBR, 等)
        ├── 引用 Shader
        ├── 存储参数值 (CPU)
        ├── 常量缓冲区 (GPU - b1)
        ├── 绑定到渲染管线
        └── 序列化到/从 .ast XML

StaticMeshComponent
    └── 拥有 MaterialInstance*
        └── Render时绑定材质CB到槽位2

Root Signature
    ├── Slot 0: Scene CB (b0)
    ├── Slot 1: SRV Table (t0-t99)
    └── Slot 2: Material CB (b1) ← 新增
```

---

恭喜！材质系统的核心功能已经实现完成！
