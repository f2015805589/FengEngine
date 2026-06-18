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

// ȫ������ṹ�壬���ڴ洢���������Դ����Ϣ
struct Texture
{
    std::string name;//������
    wchar_t fileName[MAX_PATH];;//��������·����Ŀ¼��
    ComPtr<ID3D12Resource> resource = nullptr;//���ص�������Դ
    ComPtr<ID3D12Resource> uploadHeap = nullptr;//���ص��ϴ����е�������Դ
    std::unique_ptr<uint8_t[]> ddsData;  // �������洢DDSԭʼ����
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;  // �������洢����Դ��Ϣ
};

#endif // TEXTURE_H