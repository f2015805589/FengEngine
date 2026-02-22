// Engine/public/Texture.h
#ifndef TEXTURE_H
#define TEXTURE_H

#include "public\Scene.h"
#include "public\BattleFireDirect.h"
#include <DirectXMath.h>
#include <windows.h>
#include <iostream>
#include <DirectXTex\DirectXTex.h>
#include <wincodec.h>
#include <comdef.h>
#include <shlwapi.h>
#include <wrl.h>
#include <stdexcept>
#include <string>
#include <DDSTextureLoader\DDSTextureLoader12.h>
#include <d3d12.h>
#include <d3dx12.h>

using Microsoft::WRL::ComPtr;

// 全局纹理结构体，用于存储纹理相关资源和信息
struct Texture
{
    std::string name;//纹理名
    wchar_t fileName[MAX_PATH];;//纹理所在路径的目录名
    ComPtr<ID3D12Resource> resource = nullptr;//返回的纹理资源
    ComPtr<ID3D12Resource> uploadHeap = nullptr;//返回的上传堆中的纹理资源
    std::unique_ptr<uint8_t[]> ddsData;  // 新增：存储DDS原始数据
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;  // 新增：存储子资源信息
};

#endif // TEXTURE_H