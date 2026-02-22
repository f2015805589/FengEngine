// PathUtils.h
// 项目路径工具 — 通过exe位置动态推算项目根目录，消除硬编码绝对路径

#pragma once
#include <string>
#include <windows.h>

// 获取项目根目录（FEngine/）
// exe 输出在 FEngine/x64/Debug/FEngine.exe，向上回溯3级
inline std::wstring GetProjectRoot() {
    static std::wstring root;
    if (root.empty()) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring path(exePath);
        for (int i = 0; i < 3; i++) {
            size_t pos = path.find_last_of(L"\\/");
            if (pos != std::wstring::npos) path = path.substr(0, pos);
        }
        root = path + L"\\";
    }
    return root;
}

// 获取 Engine 目录（FEngine/Engine/）
inline std::wstring GetEnginePath() {
    return GetProjectRoot() + L"Engine\\";
}

// 获取 Content 目录（FEngine/Content/）
inline std::wstring GetContentPath() {
    return GetProjectRoot() + L"Content\\";
}

// wstring 转 string 辅助（用于需要 narrow string 的场景）
inline std::string WToA(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}
