// PathUtils.h
// 项目路径工具 — 通过exe位置动态推算项目根目录，消除硬编码绝对路径

#pragma once
#include <string>
#include <windows.h>

// 获取项目根目录（FEngine/）
// 支持两种输出位置：
// 1. FEngine/x64/Debug/FEngine.exe → 回溯3级
// 2. FEngine/FEngine.exe → 回溯1级（exe直接在Solution目录）
inline std::wstring GetProjectRoot() {
    static std::wstring root;
    if (root.empty()) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring path(exePath);

        // 获取exe所在目录
        size_t pos = path.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring exeDir = path.substr(0, pos);

            // 检查Engine目录是否在exe同级目录下
            std::wstring testPath = exeDir + L"\\Engine\\";
            DWORD attr = GetFileAttributesW(testPath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                // exe在Solution根目录（FEngine/FEngine.exe）
                root = exeDir + L"\\";
            } else {
                // exe在子目录（FEngine/x64/Debug/FEngine.exe），回溯到找到Engine目录
                std::wstring searchPath = exeDir;
                for (int i = 0; i < 5; i++) {  // 最多回溯5级
                    pos = searchPath.find_last_of(L"\\/");
                    if (pos == std::wstring::npos) break;
                    searchPath = searchPath.substr(0, pos);

                    testPath = searchPath + L"\\Engine\\";
                    attr = GetFileAttributesW(testPath.c_str());
                    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                        root = searchPath + L"\\";
                        break;
                    }
                }

                // 如果还没找到，使用原来的3级回溯逻辑
                if (root.empty()) {
                    path = std::wstring(exePath);
                    for (int i = 0; i < 3; i++) {
                        pos = path.find_last_of(L"\\/");
                        if (pos != std::wstring::npos) path = path.substr(0, pos);
                    }
                    root = path + L"\\";
                }
            }
        }
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
