#include "public/Settings.h"

void Settings::Initialize(int defaultWidth, int defaultHeight) {
    m_width = defaultWidth;
    m_height = defaultHeight;

    // 定义预设分辨率选项
    m_resolutionOptions = {
        { 1280, 720, "1280x720 (720p)" },
        { 1920, 1080, "1920x1080 (1080p)" },
        { 2560, 1440, "2560x1440 (1440p)" },
        { 2560, 1600, "2560x1600 (2K)" }
    };

    // 找到当前分辨率对应的索引
    m_currentResolutionIndex = 0;
    for (int i = 0; i < static_cast<int>(m_resolutionOptions.size()); ++i) {
        if (m_resolutionOptions[i].width == m_width &&
            m_resolutionOptions[i].height == m_height) {
            m_currentResolutionIndex = i;
            break;
        }
    }
}

bool Settings::SetResolution(int width, int height) {
    if (width == m_width && height == m_height) {
        return false; // 没有变化
    }

    m_width = width;
    m_height = height;

    // 更新索引
    m_currentResolutionIndex = -1;
    for (int i = 0; i < static_cast<int>(m_resolutionOptions.size()); ++i) {
        if (m_resolutionOptions[i].width == m_width &&
            m_resolutionOptions[i].height == m_height) {
            m_currentResolutionIndex = i;
            break;
        }
    }

    NotifyResolutionChanged();
    return true;
}

bool Settings::SetResolutionByIndex(int index) {
    if (index < 0 || index >= static_cast<int>(m_resolutionOptions.size())) {
        return false;
    }

    const auto& option = m_resolutionOptions[index];
    return SetResolution(option.width, option.height);
}

void Settings::RegisterResolutionChangedCallback(ResolutionChangedCallback callback) {
    m_resolutionCallbacks.push_back(callback);
}

void Settings::NotifyResolutionChanged() {
    for (auto& callback : m_resolutionCallbacks) {
        if (callback) {
            callback(m_width, m_height);
        }
    }
}

void Settings::RequestResolutionChange(int width, int height, bool fromUI) {
    if (width != m_width || height != m_height) {
        m_pendingResolutionChange = true;
        m_pendingWidth = width;
        m_pendingHeight = height;
        m_shouldResizeWindow = fromUI;  // 只有从UI选择时才需要调整窗口大小
    }
}
