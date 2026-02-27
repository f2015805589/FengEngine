#include <windows.h>
#include <commdlg.h>
#include "public/BattleFireDirect.h"
#include "public/StaticMeshComponent.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "public/Scene.h"
#include <DirectXMath.h>
#include "public/ImguiPass.h"
#include "public/lightpass.h"
#include "public/ScreenPass.h"
#include "public/SkyPass.h"
#include "public/TaaPass.h"
#include "public/Material.h"
#include "public/Material/MaterialManager.h"
#include "public/Material/MaterialEditorPanel.h"
#include "public/Material/ShaderParser.h"
#include "public/ResourceManager.h"
#include "public/Settings.h"
#include "public/Texture/TextureManager.h"
#include "public/Texture/TexturePreviewPanel.h"
#include "public/Texture/TextureCompressor.h"
#include "public/PathUtils.h"
#include <fstream>

#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"winmm.lib")
#pragma comment(lib, "DirectXTex.lib")
#pragma comment(lib, "comdlg32.lib")

#include <d3d12.h>
#include <dxgi1_4.h>
#include <tchar.h>

#ifdef _DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif

#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

LPCTSTR gWindowClassName = L"BattleFire";
Scene* g_scene = nullptr;
MaterialEditorPanel* g_materialEditor = nullptr;        

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WindowProc(HWND inHWND, UINT inMSG, WPARAM inWParam, LPARAM inLParam) {
    if (ImGui_ImplWin32_WndProcHandler(inHWND, inMSG, inWParam, inLParam))
        return true;

    if (g_scene) {
        g_scene->HandleInput(inHWND, inMSG, inWParam, inLParam);
    }

    switch (inMSG) {
    case WM_CLOSE:
        PostQuitMessage(0);
        break;
    case WM_SIZE: {
        int newWidth = LOWORD(inLParam);
        int newHeight = HIWORD(inLParam);
        // 当窗口大小改变时，请求分辨率变更（排除最小化情况）
        if (newWidth > 0 && newHeight > 0) {
            Settings::GetInstance().RequestResolutionChange(newWidth, newHeight);
        }
    }
                break;
    default:
        return DefWindowProc(inHWND, inMSG, inWParam, inLParam);
    }
    return 0;
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd) {
    WNDCLASSEX wndClassEx;
    wndClassEx.cbSize = sizeof(WNDCLASSEX);
    wndClassEx.style = CS_HREDRAW | CS_VREDRAW;
    wndClassEx.cbClsExtra = 0;
    wndClassEx.cbWndExtra = 0;
    wndClassEx.hInstance = hInstance;
    wndClassEx.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wndClassEx.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    wndClassEx.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndClassEx.hbrBackground = NULL;
    wndClassEx.lpszMenuName = NULL;
    wndClassEx.lpszClassName = gWindowClassName;
    wndClassEx.lpfnWndProc = WindowProc;
    if (!RegisterClassEx(&wndClassEx)) {
        MessageBox(NULL, L"注册窗口类失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    int viewportWidth = 1280;
    int viewportHeight = 720;
    RECT rect = { 0, 0, viewportWidth, viewportHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowEx(NULL,
        gWindowClassName,
        L"FEngine",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        MessageBox(NULL, L"创建窗口失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    if (!InitD3D12(hwnd, viewportWidth, viewportHeight)) {
        MessageBox(NULL, L"初始化D3D12失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 初始化Settings
    Settings::GetInstance().Initialize(viewportWidth, viewportHeight);

    InitImGui(hwnd, gD3D12Device, gImGuiDescriptorHeap, gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));

    // 初始化材质管理器
    if (!MaterialManager::GetInstance().Initialize(gD3D12Device)) {
        MessageBox(NULL, L"MaterialManager初始化失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 初始化纹理管理器
    if (!TextureManager::GetInstance().Initialize(gD3D12Device)) {
        MessageBox(NULL, L"TextureManager初始化失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 初始化纹理压缩器
    TextureCompressor::GetInstance().Initialize(gD3D12Device);

    // 初始化纹理预览面板
    TexturePreviewPanel::GetInstance().Initialize(gD3D12Device);

    // 初始化材质编辑器面板
    g_materialEditor = new MaterialEditorPanel();

    g_scene = new Scene(viewportWidth, viewportHeight);

    // 设置材质编辑器的Scene引用
    g_materialEditor->SetScene(g_scene);

    ID3D12GraphicsCommandList* commandList = GetCommandList();
    ID3D12CommandAllocator* commandAllocator = GetCommandAllocator();

   
    if (!g_scene->AsyncLoadTextures()) {
        MessageBox(NULL, L"纹理初始化失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 等待异步纹理加载完成，避免CommandList冲突
    // 注意：这里应该等待纹理加载完成，但目前没有提供等待接口
    // 临时方案：延迟一下或者在Initialize之后再加载Actor
    Sleep(100);  // 给异步加载一些时间

    if (!g_scene->Initialize(commandList)) {//传入模型信息
        MessageBox(NULL, L"场景初始化失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 先等待GPU完成当前命令
    EndCommandList();
    WaitForCompletionOfCommandList();

    // 重置CommandList用于加载Level
    commandList->Reset(commandAllocator, nullptr);

    // 完成Level加载的命令
    EndCommandList();
    WaitForCompletionOfCommandList();

    // 重新开始用于后续初始化
    commandList->Reset(commandAllocator, nullptr);

    LightPass* lightPass = new LightPass(viewportWidth, viewportHeight, 4096);  // 包含4096x4096 Shadow Map
    lightPass->SetSceneConstantBuffer(g_scene->GetConstantBuffer());
    if (!lightPass->Initialize(commandList)) {
        MessageBox(NULL, L"LightPass初始化失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    ScreenPass*  screenPass = new ScreenPass();
    screenPass->SetSceneConstantBuffer(g_scene->GetConstantBuffer());
    if (!screenPass->Initialize(viewportWidth, viewportHeight)) {
        MessageBox(NULL, L"ScreenPass初始化失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 初始化SkyPass
    SkyPass* skyPass = new SkyPass();
    skyPass->SetSceneConstantBuffer(g_scene->GetConstantBuffer());
    if (!skyPass->Initialize(commandList, 500.0f)) {
        MessageBox(NULL, L"SkyPass初始化失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 初始化TaaPass
    TaaPass* taaPass = new TaaPass();
    taaPass->SetSceneConstantBuffer(g_scene->GetConstantBuffer());
    if (!taaPass->Initialize(viewportWidth, viewportHeight)) {
        MessageBox(NULL, L"TaaPass初始化失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    ID3D12RootSignature* rootSignature = InitRootSignature();

    // 设置MaterialManager的RootSignature（用于按需加载shader时自动创建PSO）
    MaterialManager::GetInstance().SetRootSignature(rootSignature);

    D3D12_SHADER_BYTECODE vs, ps;
    CreateShaderFromFile((GetEnginePath() + L"Shader/ndctriangle.hlsl").c_str(), "MainVS", "vs_5_0", &vs);
    CreateShaderFromFile((GetEnginePath() + L"Shader/ndctriangle.hlsl").c_str(), "MainPS", "ps_5_0", &ps);
    ID3D12PipelineState* BasePso = CreateScenePSO(rootSignature, vs, ps);
    if (BasePso) BasePso->SetName(L"BasePso");
    ID3D12PipelineState* gbufferPso = nullptr;  // GBuffer Pass PSO（StandardPBR Pass 0）
    ID3D12PipelineState* deferredLightingPso = nullptr;  // Deferred Lighting PSO（StandardPBR Pass 1）
    if (!BasePso) {
        MessageBox(NULL, L"创建PSO失败!", L"错误", MB_OK | MB_ICONERROR);
        ShutdownImGui();
        return -1;
    }

    D3D12_SHADER_BYTECODE lightVS, lightPS;
    CreateShaderFromFile((GetEnginePath() + L"Shader/lighting.hlsl").c_str(), "LightVS", "vs_5_0", &lightVS);
    CreateShaderFromFile((GetEnginePath() + L"Shader/lighting.hlsl").c_str(), "LightPS", "ps_5_0", &lightPS);
    ID3D12PipelineState* lightPso = lightPass->CreateLightPSO(rootSignature, lightVS, lightPS);
    if (!lightPso) {
        MessageBox(NULL, L"创建LightPass PSO失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 加载ShadowDepth着色器并创建PSO
    D3D12_SHADER_BYTECODE shadowVS, shadowPS;
    CreateShaderFromFile((GetEnginePath() + L"Shader/shadowdepth.hlsl").c_str(), "ShadowDepthVS", "vs_5_0", &shadowVS);
    CreateShaderFromFile((GetEnginePath() + L"Shader/shadowdepth.hlsl").c_str(), "ShadowDepthPS", "ps_5_0", &shadowPS);
    ID3D12PipelineState* shadowPso = lightPass->CreateShadowPSO(rootSignature, shadowVS, shadowPS);
    if (!shadowPso) {
        MessageBox(NULL, L"创建ShadowPass PSO失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 加载ScreenPass着色器
    D3D12_SHADER_BYTECODE screenVS, screenPS;
    CreateShaderFromFile((GetEnginePath() + L"Shader/screen.hlsl").c_str(), "VS", "vs_5_0", &screenVS);
    CreateShaderFromFile((GetEnginePath() + L"Shader/screen.hlsl").c_str(), "PS", "ps_5_0", &screenPS);
    ID3D12PipelineState*  screenPso = screenPass->CreatePSO(rootSignature, screenVS, screenPS);
    if (!screenPso) {
        MessageBox(NULL, L"创建ScreenPass PSO失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 加载TaaPass着色器
    D3D12_SHADER_BYTECODE taaVS, taaPS;
    CreateShaderFromFile((GetEnginePath() + L"Shader/TAA.hlsl").c_str(), "VSMain", "vs_5_0", &taaVS);
    CreateShaderFromFile((GetEnginePath() + L"Shader/TAA.hlsl").c_str(), "PSMain", "ps_5_0", &taaPS);
    ID3D12PipelineState* taaPso = taaPass->CreatePSO(rootSignature, taaVS, taaPS);
    if (!taaPso) {
        MessageBox(NULL, L"创建TaaPass PSO失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 加载TaaCopy着色器（用于将TAA结果复制到交换链）
    D3D12_SHADER_BYTECODE taaCopyVS, taaCopyPS;
    CreateShaderFromFile((GetEnginePath() + L"Shader/TaaCopy.hlsl").c_str(), "VSMain", "vs_5_0", &taaCopyVS);
    CreateShaderFromFile((GetEnginePath() + L"Shader/TaaCopy.hlsl").c_str(), "PSMain", "ps_5_0", &taaCopyPS);
    ID3D12PipelineState* taaCopyPso = taaPass->CreateCopyPSO(rootSignature, taaCopyVS, taaCopyPS);
    if (!taaCopyPso) {
        MessageBox(NULL, L"创建TaaCopy PSO失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    ID3D12PipelineState* UiPso = CreateUiPSO(rootSignature, vs, ps);
    if (UiPso) UiPso->SetName(L"UiPso");

    EndCommandList();
    WaitForCompletionOfCommandList();

    // ======= 材质系统初始化 =======
    // IMPORTANT: 材质初始化必须在WaitForCompletionOfCommandList()之后
    // 避免与AsyncLoadTextures()的后台线程发生CommandList冲突

    // RootSignature已在第216行设置，无需重复

    // ===== 步骤1: 使用ResourceManager扫描所有shader =====
    ResourceManager::GetInstance().Initialize(gD3D12Device, rootSignature);
    ResourceManager::GetInstance().ScanAndLoadAllResources();

    // 获取扫描到的所有shader资源
    const std::vector<ResourceInfo>& shaderResources = ResourceManager::GetInstance().GetAllShaderResources();
    std::cout << "\n========== Loading Non-Default Shaders ==========" << std::endl;
    
    // ===== 步骤2: 循环加载所有非默认shader（排除screen.shader和StandardPBR.shader） =====
    for (const auto& resInfo : shaderResources) {
        // 跳过StandardPBR（这是默认shader，最后加载）
        if (resInfo.name == "StandardPBR") {
            std::cout << "Skipping StandardPBR (will load as default shader later)" << std::endl;
            continue;
        }

        // 加载shader
        std::cout << "\nLoading shader: " << resInfo.name << std::endl;
        Shader* shader = MaterialManager::GetInstance().LoadShader(resInfo.filePath);
        if (!shader) {
            std::wstring errorMsg = L"加载 " + std::wstring(resInfo.name.begin(), resInfo.name.end()) +
                                   L" shader失败!\n请确认文件路径: " + resInfo.filePath;
            MessageBox(NULL, errorMsg.c_str(), L"错误", MB_OK | MB_ICONERROR);
            return -1;
        }

        // 编译shader
        if (!shader->CompileShaders(gD3D12Device)) {
            std::wstring errorMsg = L"编译 " + std::wstring(resInfo.name.begin(), resInfo.name.end()) + L" shader失败!";
            MessageBox(NULL, errorMsg.c_str(), L"错误", MB_OK | MB_ICONERROR);
            return -1;
        }

        // 为所有Pass创建PSO
        for (int i = 0; i < shader->GetPassCount(); i++) {
            ID3D12PipelineState* pso = shader->CreatePSO(gD3D12Device, rootSignature, i);
            if (!pso) {
                std::wstring errorMsg = L"创建 " + std::wstring(resInfo.name.begin(), resInfo.name.end()) +
                                       L" shader Pass " + std::to_wstring(i) + L" PSO失败!";
                MessageBox(NULL, errorMsg.c_str(), L"错误", MB_OK | MB_ICONERROR);
                return -1;
            }
        }

        std::cout << resInfo.name << " shader loaded with " << shader->GetPassCount() << " passes" << std::endl;
    }

    std::cout << "\n========== Loading Default Shader (StandardPBR) ==========" << std::endl;

    // ===== 步骤3: 加载默认shader (StandardPBR) =====
    // 1. 加载StandardPBR Shader（Unity风格）
    Shader* standardShader = MaterialManager::GetInstance().LoadShader((GetEnginePath() + L"Shader/StandardPBR.shader").c_str());
    if (!standardShader) {
        MessageBox(NULL, L"加载StandardPBR shader失败!\n请确认文件路径: Engine/Shader/StandardPBR.shader", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 2. 编译shader（会编译所有Pass）
    if (!standardShader->CompileShaders(gD3D12Device)) {
        MessageBox(NULL, L"编译StandardPBR shader失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 3. 为两个Pass分别创建PSO
    // Pass 0: GBuffer填充（用于BasePass）
    gbufferPso = standardShader->CreatePSO(gD3D12Device, rootSignature, 0);
    if (!gbufferPso) {
        MessageBox(NULL, L"创建StandardPBR GBuffer PSO失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // Pass 1: 延迟光照（用于ScreenPass）
    deferredLightingPso = standardShader->CreatePSO(gD3D12Device, rootSignature, 1);
    if (!deferredLightingPso) {
        MessageBox(NULL, L"创建StandardPBR Deferred Lighting PSO失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    std::cout << "StandardPBR shader loaded with " << standardShader->GetPassCount() << " passes" << std::endl;
    std::cout << "\n========== All Shaders Loaded Successfully ==========" << std::endl;

    // ===== 步骤4: 自动加载所有扫描到的材质文件 =====
    // 重新打开commandList以便加载材质纹理
    WaitForCompletionOfCommandList();
    commandAllocator->Reset();
    commandList->Reset(commandAllocator, nullptr);

    // 设置TextureManager的commandList，以便材质加载时可以加载纹理
    TextureManager::GetInstance().SetCommandList(commandList);

    const std::vector<ResourceInfo>& materialResources = ResourceManager::GetInstance().GetAllMaterialResources();
    std::cout << "\n========== Loading Materials ==========" << std::endl;
    std::cout << "Found " << materialResources.size() << " material files" << std::endl;

    for (const auto& matInfo : materialResources) {
        std::cout << "\nLoading material: " << matInfo.name << std::endl;
        MaterialInstance* material = MaterialManager::GetInstance().LoadMaterial(matInfo.filePath);
        if (material) {
            std::cout << "  Material '" << matInfo.name << "' loaded successfully" << std::endl;
        } else {
            std::wcout << L"  Warning: Failed to load material from: " << matInfo.filePath << std::endl;
        }
    }

    // 提交纹理加载命令
    EndCommandList();
    WaitForCompletionOfCommandList();

    std::cout << "\n========== Materials Loading Complete ==========" << std::endl;

    // 4. 加载默认材质（从文件加载，包含纹理配置）
    // 重新打开commandList以便加载默认材质的纹理
    commandAllocator->Reset();
    commandList->Reset(commandAllocator, nullptr);
    TextureManager::GetInstance().SetCommandList(commandList);

    MaterialInstance* defaultMaterial = MaterialManager::GetInstance().LoadMaterial((GetContentPath() + L"Materials\\DefaultPBR.ast").c_str());
    if (!defaultMaterial) {
        // 如果加载失败，创建一个空的默认材质
        defaultMaterial = MaterialManager::GetInstance().CreateMaterial("DefaultPBR", standardShader);
        if (!defaultMaterial) {
            MessageBox(NULL, L"创建默认材质失败!", L"错误", MB_OK | MB_ICONERROR);
            return -1;
        }
        // 设置材质参数
        defaultMaterial->SetVector("BaseColor", DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f));
        defaultMaterial->SetFloat("Roughness", 0.7f);
        defaultMaterial->SetFloat("Metallic", 0.0f);
    }

    // 提交默认材质纹理加载命令
    EndCommandList();
    WaitForCompletionOfCommandList();

    // 设置场景级Skylight强度（不再是材质参数）
    g_scene->SetSkylightIntensity(1.0f);

    // 6. 为场景中的mesh分配材质
    g_scene->GetStaticMesh()->SetMaterial(defaultMaterial);

    // 7. 设置材质编辑器的默认选中材质
    if (g_materialEditor) {
        g_materialEditor->SetSelectedMaterial("DefaultPBR");
    }
    // ======= 材质系统初始化完成 =======

    // ======= 加载Sky shader并创建SkyPass PSO =======
    std::cout << "\n========== Loading Sky Shader ==========" << std::endl;
    Shader* skyShader = MaterialManager::GetInstance().LoadShader((GetEnginePath() + L"Shader/SkyShader/Sky.shader").c_str());
    ID3D12PipelineState* skyPso = nullptr;
    if (skyShader) {
        if (skyShader->CompileShaders(gD3D12Device)) {
            // 使用SkyPass::CreatePSO创建PSO，确保input layout匹配天空球顶点格式（float3 POSITION）
            // skyShader->CreatePSO会走CreateScenePSO路径，input layout是场景mesh的float4x4格式，不匹配
            D3D12_SHADER_BYTECODE skyVS = skyShader->GetVertexShaderBytecode(0);
            D3D12_SHADER_BYTECODE skyPS = skyShader->GetPixelShaderBytecode(0);
            skyPso = skyPass->CreatePSO(rootSignature, skyVS, skyPS);
            if (skyPso) {
                std::cout << "Sky shader loaded and PSO created successfully" << std::endl;
            } else {
                std::cout << "Warning: Failed to create Sky PSO, using fallback" << std::endl;
            }
        } else {
            std::cout << "Warning: Failed to compile Sky shader" << std::endl;
        }
    } else {
        std::cout << "Warning: Failed to load Sky shader, sky rendering disabled" << std::endl;
    }
    std::cout << "========== Sky Shader Loading Complete ==========\n" << std::endl;

    // ======= 加载Level（必须在材质创建之后） =======
    // 重置CommandList用于加载Level
    commandList->Reset(commandAllocator, nullptr);

    // 加载默认关卡 (Default.level)
    if (!g_scene->LoadLevel((GetEnginePath() + L"Content\\Level\\Default.level").c_str(), commandList)) {
        MessageBox(NULL, L"加载Level失败!", L"错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    // 完成Level加载的命令
    EndCommandList();
    WaitForCompletionOfCommandList();
    // ======= Level加载完成 =======

    ShowWindow(hwnd, nShowCmd);
    UpdateWindow(hwnd);

    MSG msg = {};
    DWORD last_time = timeGetTime();
    bool showSettingWindow = false;
    bool showMainLightWindow = false;
    bool showActorWindow = false;     // Actor创建面板
    bool showSceneWindow = false;     // 场景窗口
    bool showResourceWindow = false;  // 资源管理器窗口
    bool showTexturePreview = false;  // 纹理预览面板
    static Actor* selectedActor = nullptr;  // 当前选中的Actor
    static bool showActorPanel = false;  // Actor面板（包含材质和Transform）
    static bool showMaterialEditorFromActor = false;  // 从Actor面板打开的材质编辑器
    static float mouseSpeed = 5.0f;
    static float moveSpeed = 50.0f;
    // 光照旋转角度（弧度），范围限制在-π到π
    static float lightRot[3] = { 0.0f, 0.0f, 0.0f };

    ImGuiIO& io = ImGui::GetIO();
    unsigned char* fontData;
    int fontWidth, fontHeight;
    io.Fonts->GetTexDataAsRGBA32(&fontData, &fontWidth, &fontHeight);

    while (true) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            WaitForCompletionOfCommandList();

            // ======= 处理分辨率变更请求 =======
            if (Settings::GetInstance().IsPendingResolutionChange()) {
                int newWidth, newHeight;
                Settings::GetInstance().GetPendingResolution(newWidth, newHeight);
                bool shouldResizeWindow = Settings::GetInstance().ShouldResizeWindow();
                Settings::GetInstance().ClearPendingResolutionChange();

                // 确保GPU完全空闲，所有命令都已完成
                WaitForCompletionOfCommandList();

                // 重置CommandAllocator（确保没有待执行的命令）
                // 注意：前一帧的EndCommandList已经close了commandList
                HRESULT hr = commandAllocator->Reset();
                if (FAILED(hr)) {
                    OutputDebugStringA("WARNING: commandAllocator->Reset() failed before resize\n");
                }

                // 创建一个dummy command list并关闭，确保command list状态正确
                hr = commandList->Reset(commandAllocator, nullptr);
                if (SUCCEEDED(hr)) {
                    commandList->Close();
                }

                // 使用FlushGPU确保GPU完全空闲
                FlushGPU();

                // 1. 调整交换链和深度缓冲（传递是否需要调整窗口大小）
                if (ResizeSwapChainAndDepthBuffer(newWidth, newHeight, shouldResizeWindow)) {
                    // 2. 调整Scene的离屏RT
                    g_scene->ResizeRenderTargets(newWidth, newHeight);

                    // 3. 调整LightPass
                    lightPass->Resize(newWidth, newHeight);

                    // 4. 调整ScreenPass
                    screenPass->Resize(newWidth, newHeight);

                    // 5. 调整TaaPass
                    taaPass->Resize(newWidth, newHeight);

                    // 5. 更新Settings
                    Settings::GetInstance().SetResolution(newWidth, newHeight);

                    // 调试输出
                    char debugMsg[256];
                    sprintf_s(debugMsg, "Resolution changed to %dx%d\n", newWidth, newHeight);
                    OutputDebugStringA(debugMsg);
                } else {
                    // Resize失败，输出错误信息
                    OutputDebugStringA("ERROR: ResizeSwapChainAndDepthBuffer failed!\n");
                }

                // 重置commandAllocator以便后续正常渲染
                commandAllocator->Reset();
            }

            commandAllocator->Reset();

            // 延迟纹理加载：在帧开始前、GPU空闲时处理待加载的纹理
            if (TexturePreviewPanel::GetInstance().HasPendingLoad()) {
                commandList->Reset(commandAllocator, nullptr);
                TextureManager::GetInstance().SetCommandList(commandList);
                TexturePreviewPanel::GetInstance().ProcessPendingLoad();
                EndCommandList();
                WaitForCompletionOfCommandList();
                commandAllocator->Reset();
            }

            DWORD current_time = timeGetTime();
            float deltaTime = (current_time - last_time) / 1000.0f;
            last_time = current_time;
            // TAA: 在帧开始时更新Jitter（必须在场景渲染之前）
            if (taaPass->IsEnabled()) {
                taaPass->UpdateJitter();
                XMFLOAT2 jitter = taaPass->GetJitterOffset();
                g_scene->SetJitterOffset(jitter.x, jitter.y);
            } else {
                g_scene->SetJitterOffset(0.0f, 0.0f);
            }

            g_scene->Update(deltaTime);  // 更新Scene（计算LiSPSM矩阵）

            //BasePass=======================================
            // 使用StandardPBR Pass 0（GBuffer填充）
            commandList->Reset(commandAllocator, gbufferPso);
            commandList->BeginEvent(0, L"BasePass", (UINT)(wcslen(L"BasePass")* sizeof(wchar_t)));
            BeginOffscreen(commandList);
            ID3D12DescriptorHeap* srvHeaps[] = { srvHeap};
            commandList->SetDescriptorHeaps(_countof(srvHeaps), srvHeaps);
            g_scene->Render(commandList, gbufferPso, rootSignature);
            commandList->EndEvent();
            EndCommandList();
            WaitForCompletionOfCommandList();  // Wait for BasePass to complete

            //LightPass=======================================
            // 执行LightPass（包含Shadow Map生成和光照计算）
            commandList->Reset(commandAllocator, shadowPso);
            commandList->BeginEvent(0, L"LightPass", (UINT)(wcslen(L"LightPass") * sizeof(wchar_t)));
            lightPass->RenderDirectLight(commandList, shadowPso, lightPso, rootSignature, g_scene, gDSRT);
            commandList->EndEvent();
            EndCommandList();
            WaitForCompletionOfCommandList();  // Wait for LightPass to complete

            //SkyPass=======================================
            // 执行SkyPass（渲染天空球，在ScreenPass之前）
            // 当TAA启用时，渲染到中间RT；否则渲染到交换链
            if (skyPso) {
                commandList->Reset(commandAllocator, skyPso);
                commandList->BeginEvent(0, L"SkyPass", (UINT)(wcslen(L"SkyPass") * sizeof(wchar_t)));

                if (taaPass->IsEnabled()) {
                    // TAA启用：渲染到中间RT
                    D3D12_CPU_DESCRIPTOR_HANDLE intermediateRTV = taaPass->GetIntermediateRTV();
                    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    commandList->ClearRenderTargetView(intermediateRTV, clearColor, 0, nullptr);
                    commandList->OMSetRenderTargets(1, &intermediateRTV, FALSE, nullptr);

                    // 设置视口和裁剪矩形（使用TaaPass的当前分辨率）
                    int currentWidth = taaPass->GetViewportWidth();
                    int currentHeight = taaPass->GetViewportHeight();
                    D3D12_VIEWPORT viewport = { 0, 0, (float)currentWidth, (float)currentHeight, 0.0f, 1.0f };
                    D3D12_RECT scissorRect = { 0, 0, currentWidth, currentHeight };
                    commandList->RSSetViewports(1, &viewport);
                    commandList->RSSetScissorRects(1, &scissorRect);
                } else {
                    // TAA禁用：渲染到交换链
                    BeginRenderToSwapChain(commandList, true, false);
                }

                ComPtr<ID3D12Resource> skyTextureForSky = g_scene->ReturnSkyCube();
                skyPass->Render(commandList, skyPso, rootSignature, skyTextureForSky);

                if (!taaPass->IsEnabled()) {
                    EndRenderToSwapChain(commandList);
                }

                commandList->EndEvent();
                EndCommandList();
                WaitForCompletionOfCommandList();  // Wait for SkyPass to complete
            }

            //ScreenPass======================================
            commandList->Reset(commandAllocator, deferredLightingPso);
            commandList->BeginEvent(0, L"ScreenPass", (UINT)(wcslen(L"ScreenPass") * sizeof(wchar_t)));

            if (taaPass->IsEnabled()) {
                // TAA启用：渲染到中间RT（不清空，保留SkyPass的结果）
                D3D12_CPU_DESCRIPTOR_HANDLE intermediateRTV = taaPass->GetIntermediateRTV();
                commandList->OMSetRenderTargets(1, &intermediateRTV, FALSE, nullptr);

                // 设置视口和裁剪矩形（使用TaaPass的当前分辨率）
                int currentWidth = taaPass->GetViewportWidth();
                int currentHeight = taaPass->GetViewportHeight();
                D3D12_VIEWPORT viewport = { 0, 0, (float)currentWidth, (float)currentHeight, 0.0f, 1.0f };
                D3D12_RECT scissorRect = { 0, 0, currentWidth, currentHeight };
                commandList->RSSetViewports(1, &viewport);
                commandList->RSSetScissorRects(1, &scissorRect);
            } else {
                // TAA禁用：渲染到交换链
                BeginRenderToSwapChain(commandList, false, false);
            }

            auto& sceneRTs = g_scene->m_offscreenRTs;
            ComPtr<ID3D12Resource> skyTexture = g_scene->ReturnSkyCube();
            // 渲染（使用深度缓冲代替Position RT，传入LightPass的阴影图）
            screenPass->Render(commandList, deferredLightingPso, rootSignature,
                sceneRTs[0], sceneRTs[1], sceneRTs[2],  // 3个GBuffer RT
                gDSRT,  // 深度缓冲用于位置重构
                skyTexture,
                lightPass->GetLightRT());  // LightPass输出的阴影图

            // 将深度缓冲转换回DEPTH_WRITE状态，供下一帧和BeginRenderToSwapChain使用
            D3D12_RESOURCE_BARRIER depthBarrier = {};
            depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            depthBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            depthBarrier.Transition.pResource = gDSRT;
            depthBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            depthBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            depthBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &depthBarrier);

            if (!taaPass->IsEnabled()) {
                EndRenderToSwapChain(commandList);
            }

            commandList->EndEvent();
            EndCommandList();
            WaitForCompletionOfCommandList();  // Wait for ScreenPass to complete

            // TAA Pass
            if (taaPass->IsEnabled()) {
                commandList->Reset(commandAllocator, taaPso);
                commandList->BeginEvent(0, L"TaaPass", (UINT)(wcslen(L"TaaPass") * sizeof(wchar_t)));

                // 获取Motion Vector RT
                ID3D12Resource* motionVectorRT = g_scene->GetMotionVectorRT();

                // 执行TAA（从中间RT读取，输出到历史缓冲）
                taaPass->RenderToSwapChain(commandList, taaPso, rootSignature,
                    motionVectorRT, gDSRT, GetCurrentSwapChainRTV());

                commandList->EndEvent();
                EndCommandList();
                WaitForCompletionOfCommandList();

                // 复制TAA结果到交换链
                commandList->Reset(commandAllocator, taaCopyPso);
                commandList->BeginEvent(0, L"TaaCopy", (UINT)(wcslen(L"TaaCopy") * sizeof(wchar_t)));

                BeginRenderToSwapChain(commandList, true, false);
                taaPass->CopyToSwapChain(commandList, taaCopyPso, rootSignature, GetCurrentSwapChainRTV());
                EndRenderToSwapChain(commandList);

                commandList->EndEvent();
                EndCommandList();
                WaitForCompletionOfCommandList();

                // 在帧结束时更新上一帧的 VP 矩阵
                g_scene->UpdatePreviousViewProjectionMatrix();

                // 交换历史缓冲
                taaPass->SwapHistoryBuffers();
            }

            //UiPass==========================================
            commandList->Reset(commandAllocator, UiPso);
            commandList->BeginEvent(0, L"UIPass", (UINT)(wcslen(L"UIPass") * sizeof(wchar_t)));
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // 主菜单栏（顶部）
            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("Window"))
                {
                    ImGui::MenuItem("Settings", NULL, &showSettingWindow);
                    ImGui::MenuItem("Main Light", NULL, &showMainLightWindow);
                    ImGui::MenuItem("Actor Creator", NULL, &showActorWindow);
                    ImGui::MenuItem("Scene", NULL, &showSceneWindow);
                    ImGui::MenuItem("Resource Manager", NULL, &showResourceWindow);
                    ImGui::MenuItem("Texture Preview", NULL, &showTexturePreview);
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Texture"))
                {
                    if (ImGui::MenuItem("Import Texture...")) {
                        // 打开文件对话框导入纹理
                        OPENFILENAMEW ofn = {};
                        wchar_t szFile[MAX_PATH] = {};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = hwnd;
                        ofn.lpstrFile = szFile;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.lpstrFilter = L"Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr;*.dds\0All Files\0*.*\0";
                        ofn.nFilterIndex = 1;
                        ofn.lpstrTitle = L"Import Texture";
                        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

                        if (GetOpenFileNameW(&ofn)) {
                            TextureAssetDesc desc;
                            desc.format = TextureCompressionFormat::BC3;
                            desc.generateMips = true;
                            desc.sRGB = true;
                            TextureManager::GetInstance().SetCommandList(commandList);
                            TextureAsset* tex = TextureManager::GetInstance().ImportTexture(szFile, desc);
                            if (tex) {
                                TexturePreviewPanel::GetInstance().SetTexture(tex);
                                std::cout << "Imported texture: " << tex->GetName() << std::endl;
                            } else {
                                std::cout << "Failed to import texture!" << std::endl;
                            }
                            // 无论是否成功都打开预览面板
                            showTexturePreview = true;
                        }
                    }
                    ImGui::Separator();
                    ImGui::MenuItem("Texture Preview Panel", NULL, &showTexturePreview);
                    ImGui::EndMenu();
                }

                // 显示FPS
                ImGui::Separator();
                ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

                ImGui::EndMainMenuBar();
            }

            if (showSettingWindow) {
                ImGui::Begin("Setting window", &showSettingWindow);
                ImGui::Text("It is Setting window!");
                ImGui::SliderFloat("Mouse Speed", &mouseSpeed, 0.1f, 100.0f);
                ImGui::SliderFloat("Move Speed", &moveSpeed, 0.1f, 100.0f);
                g_scene->SetCameraLookSpeed(mouseSpeed);
                g_scene->SetCameraMoveSpeed(moveSpeed);

                ImGui::Separator();
                ImGui::Text("TAA Settings");
                bool taaEnabled = taaPass->IsEnabled();
                if (ImGui::Checkbox("Enable TAA", &taaEnabled)) {
                    taaPass->SetEnabled(taaEnabled);
                }

                ImGui::Separator();
                ImGui::Text("Resolution Settings");

                // 获取分辨率选项
                const auto& resOptions = Settings::GetInstance().GetResolutionOptions();
                int currentResIndex = Settings::GetInstance().GetCurrentResolutionIndex();

                // 构建分辨率选项字符串
                std::vector<const char*> resNames;
                for (const auto& opt : resOptions) {
                    resNames.push_back(opt.name);
                }

                // 下拉选择框
                if (ImGui::Combo("Resolution", &currentResIndex, resNames.data(), (int)resNames.size())) {
                    const auto& selectedRes = resOptions[currentResIndex];
                    Settings::GetInstance().RequestResolutionChange(selectedRes.width, selectedRes.height, true);  // fromUI = true
                }

                // 显示当前分辨率
                ImGui::Text("Current: %dx%d", GetRenderWidth(), GetRenderHeight());

                if (ImGui::Button("close")) showSettingWindow = false;
                ImGui::End();
            }

            if (showMainLightWindow) {
                ImGui::Begin("MainLight window", &showMainLightWindow);
                ImGui::Text("Light Rotation Control (Radians)");
                // 光照旋转角度控制（绕XYZ轴）
                if (ImGui::Button("Reset Rotate"))
                {
                    lightRot[0] = 0.0f;
                    lightRot[1] = 0.0f;
                    lightRot[2] = 0.0f;
                }
                ImGui::SliderFloat("Rotate X", &lightRot[0], -10.0f, 10.0f);
                ImGui::SliderFloat("Rotate Y", &lightRot[1], -10.0f, 10.0f);
                ImGui::SliderFloat("Rotate Z", &lightRot[2], -10.0f, 10.0f);
                // 更新场景中的光照旋转角度
                g_scene->SetLightRotation(lightRot[0], lightRot[1], lightRot[2]);

                // Skylight控制（UE风格）
                ImGui::Separator();
                ImGui::Text("Skylight Settings (UE Style)");
                static float skylightIntensity = 1.0f;
                if (ImGui::SliderFloat("Intensity", &skylightIntensity, 0.0f, 5.0f)) {
                    g_scene->SetSkylightIntensity(skylightIntensity);
                }

                // Skylight颜色控制（UE风格的Tint功能）
                static float skylightColor[3] = { 1.0f, 1.0f, 1.0f };
                if (ImGui::ColorEdit3("Skylight Color", skylightColor)) {
                    g_scene->SetSkylightColor(skylightColor[0], skylightColor[1], skylightColor[2]);
                }

                // Shadowmap控制
                ImGui::Separator();
                ImGui::Text("Shadow Settings");
                static float shadowOrthoSize = 20.0f;
                if (ImGui::SliderFloat("Shadow Ortho Size", &shadowOrthoSize, 10.0f, 500.0f)) {
                    g_scene->SetShadowOrthoSize(shadowOrthoSize);
                }
                ImGui::Text("Smaller = Higher Resolution");

                static int shadowMode = 2;  // 默认PCSS
                const char* shadowModes[] = { "Hard Shadow", "PCF", "PCSS" };
                if (ImGui::Combo("Shadow Mode", &shadowMode, shadowModes, 3)) {
                    g_scene->SetShadowMode(shadowMode);
                }

                if (ImGui::Button("close")) showMainLightWindow = false;
                ImGui::End();
            }

            // Actor创建面板
            if (showActorWindow) {
                ImGui::Begin("Actor Creator", &showActorWindow);
                ImGui::Text("Create New Actor:");
                ImGui::Separator();

                // 通用Actor创建lambda
                auto CreateActorFromMesh = [&](const std::wstring& meshPath, const std::string& prefix, int& counter) {
                    WaitForCompletionOfCommandList();
                    commandAllocator->Reset();
                    commandList->Reset(commandAllocator, nullptr);

                    counter++;
                    std::string actorName = prefix + "_" + std::to_string(counter);
                    Actor* newActor = new Actor(actorName);
                    newActor->CreateConstantBuffer(gD3D12Device);
                    newActor->SetPosition(DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f));
                    newActor->SetRotation(DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f));
                    newActor->SetScale(DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f));

                    if (newActor->LoadFromMeshFile(meshPath)) {
                        const MeshAssetInfo& meshInfo = newActor->GetMeshAssetInfo();
                        std::wstring fbxPath = GetEnginePath();
                        fbxPath += std::wstring(meshInfo.fbxPath.begin(), meshInfo.fbxPath.end());
                        std::string fbxPathAnsi(fbxPath.begin(), fbxPath.end());

                        StaticMeshComponent* mesh = new StaticMeshComponent();
                        mesh->InitFromFile(commandList, fbxPathAnsi.c_str());
                        newActor->SetMesh(mesh);

                        MaterialInstance* defaultMaterial = MaterialManager::GetInstance().GetMaterial("DefaultPBR");
                        if (defaultMaterial) newActor->SetMaterial(defaultMaterial);

                        g_scene->GetActors().push_back(newActor);
                        EndCommandList();
                        WaitForCompletionOfCommandList();
                    } else {
                        char msg[128];
                        sprintf_s(msg, "Failed to load %s", prefix.c_str());
                        MessageBoxA(NULL, msg, "Error", MB_OK);
                        delete newActor;
                    }
                };

                static int sphereCounter = 0;
                static int boxCounter = 0;

                if (ImGui::Button("Add Sphere")) {
                    CreateActorFromMesh(GetEnginePath() + L"Content\\Actor\\Mesh\\sphere.mesh", "Sphere", sphereCounter);
                }

                ImGui::SameLine();
                if (ImGui::Button("Add Box")) {
                    CreateActorFromMesh(GetEnginePath() + L"Content\\Actor\\Mesh\\box.mesh", "Box", boxCounter);
                }

                ImGui::Separator();

                // 显示当前Actor数量
                ImGui::Text("Current Actors: %d", (int)g_scene->GetActors().size());

                if (ImGui::Button("Close")) showActorWindow = false;
                ImGui::End();
            }

            // Scene窗口 - 显示Actor列表
            if (showSceneWindow) {
                ImGui::Begin("Scene", &showSceneWindow);
                ImGui::Text("Scene Actors:");
                ImGui::Separator();

                auto& actors = g_scene->GetActors();
                for (int i = 0; i < actors.size(); i++) {
                    Actor* actor = actors[i];
                    const char* actorName = actor->GetName().c_str();

                    // Actor列表项
                    if (ImGui::Selectable(actorName, selectedActor == actor, ImGuiSelectableFlags_AllowDoubleClick)) {
                        selectedActor = actor;

                        // 双击时打开Actor面板
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            showActorPanel = true;
                        }
                    }
                }

                ImGui::End();
            }

            // Actor面板 - 显示材质和Transform
            if (showActorPanel && selectedActor) {
                ImGui::Begin("Actor Properties", &showActorPanel);

                ImGui::Text("Actor: %s", selectedActor->GetName().c_str());
                ImGui::Separator();

                // ===== 材质部分 =====
                if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                    const MeshAssetInfo& meshInfo = selectedActor->GetMeshAssetInfo();
                    ImGui::Text("Default Material: %s", meshInfo.defaultMaterial.c_str());

                    // 点击按钮打开材质编辑器
                    if (ImGui::Button("Edit Material")) {
                        // 设置材质编辑器选中当前材质
                        if (g_materialEditor) {
                            g_materialEditor->SetTargetActor(selectedActor);  // 设置目标Actor
                            g_materialEditor->SetSelectedMaterial(meshInfo.defaultMaterial);
                            g_materialEditor->Show();  // 显示窗口
                            showMaterialEditorFromActor = true;
                        }
                    }
                }

                ImGui::Separator();

                // ===== Transform部分 =====
                if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // 获取当前Transform
                    DirectX::XMFLOAT3 pos = selectedActor->GetPosition();
                    DirectX::XMFLOAT3 rot = selectedActor->GetRotation();
                    DirectX::XMFLOAT3 scale = selectedActor->GetScale();

                    // Position
                    ImGui::Text("Position");
                    if (ImGui::DragFloat("X##Pos", &pos.x, 0.1f)) {
                        selectedActor->SetPosition(pos);
                    }
                    if (ImGui::DragFloat("Y##Pos", &pos.y, 0.1f)) {
                        selectedActor->SetPosition(pos);
                    }
                    if (ImGui::DragFloat("Z##Pos", &pos.z, 0.1f)) {
                        selectedActor->SetPosition(pos);
                    }

                    ImGui::Spacing();

                    // Rotation (degrees)
                    ImGui::Text("Rotation (Degrees)");
                    if (ImGui::DragFloat("X##Rot", &rot.x, 1.0f, -180.0f, 180.0f)) {
                        selectedActor->SetRotation(rot);
                    }
                    if (ImGui::DragFloat("Y##Rot", &rot.y, 1.0f, -180.0f, 180.0f)) {
                        selectedActor->SetRotation(rot);
                    }
                    if (ImGui::DragFloat("Z##Rot", &rot.z, 1.0f, -180.0f, 180.0f)) {
                        selectedActor->SetRotation(rot);
                    }

                    ImGui::Spacing();

                    // Scale
                    ImGui::Text("Scale");
                    if (ImGui::DragFloat("X##Scale", &scale.x, 0.01f, 0.01f, 10.0f)) {
                        selectedActor->SetScale(scale);
                    }
                    if (ImGui::DragFloat("Y##Scale", &scale.y, 0.01f, 0.01f, 10.0f)) {
                        selectedActor->SetScale(scale);
                    }
                    if (ImGui::DragFloat("Z##Scale", &scale.z, 0.01f, 0.01f, 10.0f)) {
                        selectedActor->SetScale(scale);
                    }
                }

                ImGui::End();
            }

            // 材质编辑器（从Actor面板打开）
            if (g_materialEditor) {
                // 始终渲染（MaterialEditorPanel内部会根据m_showWindow判断）
                g_materialEditor->RenderUI();

                // 同步窗口状态：如果用户关闭了窗口，更新我们的标志
                if (showMaterialEditorFromActor && !g_materialEditor->IsVisible()) {
                    showMaterialEditorFromActor = false;
                }
            }

            // 资源管理器窗口
            if (showResourceWindow) {
                ResourceManager::GetInstance().ShowResourceWindow(&showResourceWindow);
            }

            // 纹理预览面板 - 检查两个条件：菜单开关或面板自身显示状态
            if (showTexturePreview || TexturePreviewPanel::GetInstance().IsVisible()) {
                showTexturePreview = true;
                TexturePreviewPanel::GetInstance().Show();
                TexturePreviewPanel::GetInstance().RenderUI();
                // 同步窗口关闭状态（用户点击X关闭）
                if (!TexturePreviewPanel::GetInstance().IsVisible()) {
                    showTexturePreview = false;
                }
            }

            ImGui::Render();
            BeginRenderToSwapChain(commandList, false);
            ID3D12DescriptorHeap* ppHeaps[] = { gImGuiDescriptorHeap };
            commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);

            EndRenderToSwapChain(commandList);
            commandList->EndEvent();
            EndCommandList();
            SwapD3D12Buffers();
        }
    }

    delete g_scene;
    delete g_materialEditor;

    // 清理纹理系统
    TexturePreviewPanel::GetInstance().Shutdown();
    TextureCompressor::GetInstance().Shutdown();
    TextureManager::GetInstance().Shutdown();

    MaterialManager::GetInstance().Shutdown();
    ShutdownImGui();
    BasePso->Release();
    lightPso->Release();
    screenPso->Release();  // 保留原有的screenPso清理
    UiPso->Release();
    // StandardPBR的PSO由Shader类管理，在MaterialManager::Shutdown()中会自动清理
    rootSignature->Release();
    return 0;
}