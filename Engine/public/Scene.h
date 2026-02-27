#ifndef SCENE_H
#define SCENE_H

#include "Camera.h"
#include "StaticMeshComponent.h"
#include "public/Material.h"
#include "public/Actor.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <future>  // 必须包含此头文件
#include <d3dx12.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

extern ID3D12DescriptorHeap* srvHeap;



class Scene {
public:
    Scene(int viewportWidth, int viewportHeight);
    ~Scene();
    std::vector<ID3D12Resource*> CreateOffscreenRTs(int width, int height, ID3D12DescriptorHeap* rtvHeap, UINT rtvDescriptorSize);
    bool Initialize(ID3D12GraphicsCommandList* commandList);
    void Update(float deltaTime);
    void Render(ID3D12GraphicsCommandList* commandList, ID3D12PipelineState* pso, ID3D12RootSignature* rootSignature);
    void HandleInput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void SetCameraLookSpeed(float speed) { m_camera.SetLookSpeed(speed); }
    void SetCameraMoveSpeed(float speed) { m_camera.SetMoveSpeed(speed); }
    Camera& GetCamera() { return m_camera; }  // 获取相机引用
    void SetLightRotation(float x, float y, float z) { m_lightRotation = { x, y, z }; }
    void SetSkylightIntensity(float intensity) { m_skylightIntensity = intensity; }
    float GetSkylightIntensity() const { return m_skylightIntensity; }

    // Shadowmap正交投影范围控制
    void SetShadowOrthoSize(float size) { m_shadowOrthoSize = size; }
    float GetShadowOrthoSize() const { return m_shadowOrthoSize; }

    // 阴影模式：0=Hard, 1=PCF, 2=PCSS
    void SetShadowMode(int mode) { m_shadowMode = mode; }
    int GetShadowMode() const { return m_shadowMode; }

    // Skylight颜色调节（UE风格的Tint功能）
    void SetSkylightColor(float r, float g, float b) {
        m_skylightColor = DirectX::XMFLOAT3(r, g, b);
    }
    DirectX::XMFLOAT3 GetSkylightColor() const { return m_skylightColor; }

    ID3D12Resource* GetConstantBuffer() const { return m_constantBuffer; }

    // 获取mesh（用于材质分配）
    StaticMeshComponent* GetStaticMesh() { return &m_staticMesh; }

    // Actor管理
    Actor* CreateActor(const std::string& name);
    bool LoadActorFromMeshFile(const std::wstring& meshFilePath, ID3D12GraphicsCommandList* commandList);
    void RemoveActor(Actor* actor);
    std::vector<Actor*>& GetActors() { return m_actors; }
    Actor* GetActorByName(const std::string& name);

    // Level管理
    bool LoadLevel(const std::wstring& levelFilePath, ID3D12GraphicsCommandList* commandList);
    bool SaveLevel(const std::wstring& levelFilePath);

    // LiSPSM矩阵计算（Light Space Perspective Shadow Maps）
    DirectX::XMMATRIX CalculateLiSPSMMatrix(const DirectX::XMVECTOR& lightDir,
                                            const DirectX::XMMATRIX& cameraView,
                                            const DirectX::XMMATRIX& cameraProj);
    // 标准正交阴影矩阵（LiSPSM退化情况使用）
    DirectX::XMMATRIX CalculateStandardShadowMatrix(const DirectX::XMVECTOR& lightDir);

    // 异步加载纹理（对外接口）
    bool AsyncLoadTextures();
    
    //SRV
    //ComPtr<ID3D12DescriptorHeap> srvHeap; // SRV描述符堆
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle; // SRV的GPU可见句柄
    UINT srvDescriptorSize; // 描述符大小

    // 新增：设置SRV描述符堆
    void CreateSRVHeap();
    // 新增：创建纹理的SRV
    bool CreateTextureSRV(ID3D12GraphicsCommandList* commandList);

    // 动态更新纹理SRV（用于材质系统）
    // slotIndex: SRV槽位索引 (1=BaseColor, 2=Normal, 3=ORM)
    void UpdateTextureSRV(UINT slotIndex, ID3D12Resource* textureResource);

    // Bindless纹理系统：分配SRV槽位（从t10开始）
    // 返回分配的槽位索引，失败返回UINT_MAX
    static UINT AllocateBindlessSRVSlot();
    // 释放SRV槽位
    static void FreeBindlessSRVSlot(UINT slotIndex);
    // 在指定槽位创建纹理SRV
    static void CreateBindlessTextureSRV(UINT slotIndex, ID3D12Resource* textureResource);
    // 获取全局SRV堆
    static ID3D12DescriptorHeap* GetGlobalSRVHeap() { return srvHeap; }

    std::vector<ID3D12Resource*> m_offscreenRTs; // 存储4个离屏RT (Albedo, Normal, ORM, MotionVector)

    // 获取 Motion Vector RT（用于 TAA）
    ID3D12Resource* GetMotionVectorRT() const {
        return m_offscreenRTs.size() > 3 ? m_offscreenRTs[3] : nullptr;
    }

    // TAA 相关方法
    void SetJitterOffset(float x, float y) {
        m_previousJitterOffset = m_jitterOffset;
        m_jitterOffset = DirectX::XMFLOAT2(x, y);
    }
    DirectX::XMFLOAT2 GetJitterOffset() const { return m_jitterOffset; }
    void UpdatePreviousViewProjectionMatrix();  // 在帧结束时调用，保存当前帧的 VP 矩阵

    ComPtr<ID3D12Resource> ReturnSkyCube() {return m_skyTexture;};

    // 分辨率变更时重新创建RTs
    bool ResizeRenderTargets(int newWidth, int newHeight);
    int GetViewportWidth() const { return m_viewportWidth; }
    int GetViewportHeight() const { return m_viewportHeight; }
private:
    // 通用纹理加载（替代原来4个独立函数）
    bool LoadAndUploadTexture(const wchar_t* pngPath, const char* textureName, bool isCubemap);
    bool LoadTextures();
    // 异步加载相关成员
    std::future<bool> m_textureLoadFuture;  // 异步任务句柄
    std::atomic<bool> m_textureLoaded;      // 加载是否完成（原子变量，线程安全）
    std::atomic<bool> m_textureLoadSuccess; // 加载是否成功（原子变量）

    ComPtr<ID3D12Resource> m_skyTexture; // 存储天空盒纹理

    StaticMeshComponent m_staticMesh;
    Camera m_camera;
    ID3D12Resource* m_constantBuffer;//常量缓冲区b0
    // 修改：常量缓冲区大小调整为72（适配HLSL 16字节对齐）
    void* m_mappedConstantBuffer = nullptr;      // 映射的常量缓冲区内存指针
    ID3D12Resource* m_texBuffer;////常量缓冲区b1
    SceneCBData m_cbData;  // 共享CB结构体，替代 float m_matrices[176]

    // TAA 相关：上一帧的 ViewProjection 矩阵
    DirectX::XMMATRIX m_previousViewProjectionMatrix = DirectX::XMMatrixIdentity();
    DirectX::XMFLOAT2 m_jitterOffset = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 m_previousJitterOffset = { 0.0f, 0.0f };
    int m_viewportWidth;
    int m_viewportHeight;
    DirectX::XMFLOAT3 m_lightRotation;
    DirectX::XMFLOAT3 m_lightDirection;
    float m_skylightIntensity = 1.0f;  // 场景级Skylight强度
    DirectX::XMFLOAT3 m_skylightColor = DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);  // Skylight颜色（默认白色）
    float m_shadowOrthoSize = 20.0f;  // Shadowmap正交投影范围（默认20）
    int m_shadowMode = 2;             // 阴影模式：0=Hard, 1=PCF, 2=PCSS（默认PCSS）


    ID3D12DescriptorHeap* m_rtvHeap = nullptr;   // 离屏RT的RTV描述符堆
    UINT m_rtvDescriptorSize = 0;                // RTV描述符大小

    // Actor列表
    std::vector<Actor*> m_actors;
};

#endif // SCENE_H