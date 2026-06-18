#include "public\Camera.h"
#include <DirectXMath.h>
#include <imgui.h>

extern bool g_viewportHovered;

using namespace DirectX;

Camera::Camera(float fov, float aspectRatio, float nearZ, float farZ) {
    m_position = { 0.0f, 0.0f, -10.0f };
    m_rotation = { 0.0f, 0.0f, 0.0f };
    m_fov = fov;
    m_aspectRatio = aspectRatio;
    m_nearZ = nearZ;
    m_farZ = farZ;
    m_projectionMatrix = XMMatrixPerspectiveFovLH(m_fov, m_aspectRatio, m_nearZ, m_farZ);
    m_isMouseDown = false;
    m_isRightMouseDown = false;
    m_lastMousePos = { 0, 0 };
    m_moveSpeed = 50.0f;  // 默认移动速度
    m_lookSpeed = 0.005f; // 默认旋转速度 (5 * 0.001)
    m_zoomSpeed = 5.0f;   // 缩放速度
    m_forward = { 0.0f, 0.0f, 1.0f };  // 初始前向向量（沿Z轴正方向）
    m_right = { 1.0f, 0.0f, 0.0f };    // 初始右向向量（沿X轴正方向）
}

void Camera::Update(float deltaTime) {
    // 计算旋转矩阵（决定摄像机朝向）
    XMMATRIX camRot = XMMatrixRotationRollPitchYawFromVector(XMLoadFloat3(&m_rotation));

    // 计算前向向量（基于旋转，单位化）
    XMVECTOR forward = XMVector3TransformCoord(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), camRot);
    forward = XMVector3Normalize(forward);
    XMStoreFloat3(&m_forward, forward);

    // 计算右向向量（前向叉乘上方向，单位化）
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMVECTOR right = XMVector3Cross(forward, up);
    right = XMVector3Normalize(right);
    XMStoreFloat3(&m_right, right);

    // WASD 连续移动：每帧轮询按键状态，使用实际 deltaTime
    if (!ImGui::GetIO().WantCaptureKeyboard || g_viewportHovered) {
        float move = m_moveSpeed * deltaTime;
        if (GetAsyncKeyState('W') & 0x8000) {
            m_position.x += m_forward.x * move;
            m_position.y += m_forward.y * move;
            m_position.z += m_forward.z * move;
        }
        if (GetAsyncKeyState('S') & 0x8000) {
            m_position.x -= m_forward.x * move;
            m_position.y -= m_forward.y * move;
            m_position.z -= m_forward.z * move;
        }
        if (GetAsyncKeyState('A') & 0x8000) {
            m_position.x += m_right.x * move;
            m_position.y += m_right.y * move;
            m_position.z += m_right.z * move;
        }
        if (GetAsyncKeyState('D') & 0x8000) {
            m_position.x -= m_right.x * move;
            m_position.y -= m_right.y * move;
            m_position.z -= m_right.z * move;
        }
        if (GetAsyncKeyState('Q') & 0x8000) {
            m_position.y -= move;
        }
        if (GetAsyncKeyState('E') & 0x8000) {
            m_position.y += move;
        }
    }

    // 摄像机位置向量
    XMVECTOR pos = XMLoadFloat3(&m_position);

    // 目标点 = 摄像机位置 + 前向向量
    XMVECTOR target = XMVectorAdd(pos, forward);

    // 计算视图矩阵
    m_viewMatrix = XMMatrixLookAtLH(pos, target, up);
}

void Camera::HandleInput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // WM_LBUTTONUP / WM_RBUTTONUP 始终处理，确保拖拽状态被清除
    // （否则拖动 docking 分隔条后 m_isMouseDown 可能残留为 true）
    bool isButtonUp = (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP);
    if (!isButtonUp && ImGui::GetIO().WantCaptureMouse && !g_viewportHovered) {
        return;
    }

    switch (msg) {
    case WM_LBUTTONDOWN:
        m_isMouseDown = true;
        m_lastMousePos.x = LOWORD(lParam);
        m_lastMousePos.y = HIWORD(lParam);
        break;
    case WM_LBUTTONUP:
        m_isMouseDown = false;
        break;
    case WM_RBUTTONDOWN:
        m_isRightMouseDown = true;
        m_lastMousePos.x = LOWORD(lParam);
        m_lastMousePos.y = HIWORD(lParam);
        break;
    case WM_RBUTTONUP:
        m_isRightMouseDown = false;
        break;
    case WM_MOUSEMOVE:
        if (m_isMouseDown) {
            // 左键：仅绕竖直轴（Y轴）旋转（偏航）
            POINT currentPos;
            currentPos.x = LOWORD(lParam);
            currentPos.y = HIWORD(lParam);

            float deltaX = static_cast<float>(currentPos.x - m_lastMousePos.x);

            // 只更新偏航角（Y轴旋转）
            m_rotation.y -= deltaX * m_lookSpeed;

            m_lastMousePos = currentPos;
        }
        else if (m_isRightMouseDown) {
            // 右键：同时控制俯仰和偏航
            POINT currentPos;
            currentPos.x = LOWORD(lParam);
            currentPos.y = HIWORD(lParam);

            float deltaX = static_cast<float>(currentPos.x - m_lastMousePos.x);
            float deltaY = static_cast<float>(currentPos.y - m_lastMousePos.y);

            // 偏航角（Y轴旋转）
            m_rotation.y -= deltaX * m_lookSpeed;
            // 俯仰角（X轴旋转）
            m_rotation.x -= deltaY * m_lookSpeed;

            // 限制俯仰角范围（避免过度旋转）
            m_rotation.x = XMMin(XMMax(m_rotation.x, -XM_PIDIV2 + 0.1f), XM_PIDIV2 - 0.1f);

            m_lastMousePos = currentPos;
        }
        break;
    case WM_MOUSEWHEEL: {
        // 鼠标滚轮：沿摄像机朝向方向移动（前向/后向）
        int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        float moveAmount = static_cast<float>(zDelta) * 0.01f * m_zoomSpeed;

        // 沿摄像机前向向量移动（正值向前，负值向后）
        m_position.x += m_forward.x * moveAmount;
        m_position.y += m_forward.y * moveAmount;
        m_position.z += m_forward.z * moveAmount;
        break;
    }
    }
    // 注意：WASD 移动已移至 Camera::Update() 中每帧轮询，使用实际 deltaTime
}

XMMATRIX Camera::GetViewMatrix() const {
    return m_viewMatrix;
}

XMMATRIX Camera::GetProjectionMatrix() const {
    return m_projectionMatrix;
}

void Camera::SetPosition(float x, float y, float z) {
    m_position = { x, y, z };
}

void Camera::SetRotation(float pitch, float yaw) {
    m_rotation = { pitch, yaw, 0.0f };
}

// 添加设置移动速度的方法
void Camera::SetMoveSpeed(float speed) {
    m_moveSpeed = speed;
}

// 设置宽高比（分辨率变更时调用）
void Camera::SetAspectRatio(float aspectRatio) {
    m_aspectRatio = aspectRatio;
    m_projectionMatrix = XMMatrixPerspectiveFovLH(m_fov, m_aspectRatio, m_nearZ, m_farZ);
}