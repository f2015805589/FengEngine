#pragma once
#include <functional>
#include <vector>
#include <string>

// 预定义的分辨率选项
struct ResolutionOption {
    int width;
    int height;
    const char* name;
};

// 分辨率变化回调函数类型
using ResolutionChangedCallback = std::function<void(int width, int height)>;

class Settings {
public:
    static Settings& GetInstance() {
        static Settings instance;
        return instance;
    }

    // 禁止拷贝
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    // 初始化
    void Initialize(int defaultWidth, int defaultHeight);

    // 分辨率相关
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    float GetAspectRatio() const { return static_cast<float>(m_width) / static_cast<float>(m_height); }

    // 设置分辨率（会触发回调）
    bool SetResolution(int width, int height);
    bool SetResolutionByIndex(int index);

    // 获取预定义分辨率列表
    const std::vector<ResolutionOption>& GetResolutionOptions() const { return m_resolutionOptions; }
    int GetCurrentResolutionIndex() const { return m_currentResolutionIndex; }

    // 注册分辨率变化回调
    void RegisterResolutionChangedCallback(ResolutionChangedCallback callback);

    // 相机设置
    float GetMouseSpeed() const { return m_mouseSpeed; }
    void SetMouseSpeed(float speed) { m_mouseSpeed = speed; }
    float GetMoveSpeed() const { return m_moveSpeed; }
    void SetMoveSpeed(float speed) { m_moveSpeed = speed; }

    // TAA设置
    float GetTaaJitterScale() const { return m_taaJitterScale; }
    void SetTaaJitterScale(float scale) { m_taaJitterScale = scale; }

    // 是否需要分辨率变更（用于延迟到安全时机执行）
    bool IsPendingResolutionChange() const { return m_pendingResolutionChange; }
    void GetPendingResolution(int& outWidth, int& outHeight) const {
        outWidth = m_pendingWidth;
        outHeight = m_pendingHeight;
    }
    bool ShouldResizeWindow() const { return m_shouldResizeWindow; }  // 是否需要调整窗口大小
    void ClearPendingResolutionChange() { m_pendingResolutionChange = false; }
    void RequestResolutionChange(int width, int height, bool fromUI = false);

private:
    Settings() = default;

    int m_width = 1280;
    int m_height = 720;
    int m_currentResolutionIndex = 0;

    // 延迟分辨率变更
    bool m_pendingResolutionChange = false;
    int m_pendingWidth = 0;
    int m_pendingHeight = 0;
    bool m_shouldResizeWindow = false;  // 是否需要调整窗口大小（UI选择时为true，窗口拖动时为false）

    float m_mouseSpeed = 5.0f;
    float m_moveSpeed = 50.0f;
    float m_taaJitterScale = 1.0f;  // TAA Jitter强度，默认1.0（标准-0.5到0.5像素）

    std::vector<ResolutionOption> m_resolutionOptions;
    std::vector<ResolutionChangedCallback> m_resolutionCallbacks;

    void NotifyResolutionChanged();
};
