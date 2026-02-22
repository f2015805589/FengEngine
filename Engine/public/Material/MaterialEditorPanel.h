#pragma once
#include <string>
#include <vector>

// 前向声明
class MaterialInstance;
struct ShaderParameter;
class Scene;
class Actor;

class MaterialEditorPanel {
public:
    MaterialEditorPanel();
    ~MaterialEditorPanel() = default;

    // 在ImGui循环中调用
    void RenderUI();

    // 设置当前选中的材质
    void SetSelectedMaterial(const std::string& materialName);

    // 设置Scene引用（用于应用材质到mesh）
    void SetScene(Scene* scene) { m_scene = scene; }

    // 设置目标Actor（用于应用材质）
    void SetTargetActor(Actor* actor) { m_targetActor = actor; }

    // 显示/隐藏窗口
    void Show() { m_showWindow = true; }
    void Hide() { m_showWindow = false; }
    bool IsVisible() const { return m_showWindow; }

private:
    std::string m_selectedMaterialName;
    bool m_showWindow;
    Scene* m_scene;  // 场景引用
    Actor* m_targetActor;  // 目标Actor（优先于StaticMesh）

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
    void OnApplyToMesh();  // 应用材质到mesh

    // 临时变量
    char m_newMaterialName[256];
    char m_savePath[512];
    char m_loadPath[512];
    bool m_showCreateDialog;
};
