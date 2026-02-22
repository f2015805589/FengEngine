#ifndef CAMERA_H
#define CAMERA_H

#include <DirectXMath.h>
#include <Windows.h>

class Camera {
public:
    // 构造函数：初始化相机参数
    // fov: 视野角度（弧度）
    // aspectRatio: 宽高比
    // nearZ: 近裁剪面
    // farZ: 远裁剪面
    Camera(float fov, float aspectRatio, float nearZ, float farZ);

    // 更新相机视图矩阵及方向向量
    void Update(float deltaTime);

    // 处理输入事件
    void HandleInput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // 获取视图矩阵
    DirectX::XMMATRIX GetViewMatrix() const;

    // 获取投影矩阵
    DirectX::XMMATRIX GetProjectionMatrix() const;

    // 设置相机位置
    void SetPosition(float x, float y, float z);

    // 设置相机旋转角度
    // pitch: 俯仰角（弧度）
    // yaw: 偏航角（弧度）
    void SetRotation(float pitch, float yaw);

    // 设置旋转速度的方法
    void SetLookSpeed(float speed) { m_lookSpeed = speed * 0.001f; }

    // 添加设置移动速度的方法
    void SetMoveSpeed(float speed);

    // 设置宽高比（分辨率变更时调用）
    void SetAspectRatio(float aspectRatio);

    // 新增：获取相机位置（用于传递到着色器）
    DirectX::XMFLOAT3 GetPosition() const { return m_position; }

    // 新增：获取相机前向向量
    DirectX::XMFLOAT3 GetForward() const { return m_forward; }

    // 获取近远裁剪面（用于 TAA）
    float GetNearPlane() const { return m_nearZ; }
    float GetFarPlane() const { return m_farZ; }

private:
    DirectX::XMFLOAT3 m_position;       // 相机位置
    DirectX::XMFLOAT3 m_rotation;       // 相机旋转角度（俯仰、偏航、翻滚）
    DirectX::XMMATRIX m_viewMatrix;     // 视图矩阵
    DirectX::XMMATRIX m_projectionMatrix;// 投影矩阵
    DirectX::XMFLOAT3 m_forward;        // 相机前向向量（局部坐标系）
    DirectX::XMFLOAT3 m_right;          // 相机右向向量（局部坐标系）

    // 鼠标状态
    bool m_isMouseDown;                 // 左键是否按下
    bool m_isRightMouseDown;            // 右键是否按下
    POINT m_lastMousePos;               // 上一帧鼠标位置
    float m_moveSpeed;                  // 移动速度
    float m_lookSpeed;                  // 旋转速度（已乘以系数）
    float m_zoomSpeed;                  // 缩放速度

    // 投影矩阵参数（用于动态更新宽高比）
    float m_fov;
    float m_aspectRatio;
    float m_nearZ;
    float m_farZ;
};

#endif // CAMERA_H