#include "MultiWindowShared.h"
#include "IUnityInterface.h"
#include <windows.h>
#include <gl/GL.h>
#include <thread>
#include <mutex>
#include <dwmapi.h> // 투명화용

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "dwmapi.lib")

// --- 전역 변수 ---
static HGLRC g_UnityContext = NULL;
static std::thread g_RenderThread;
static HANDLE g_hRenderEvent = NULL;
static std::mutex g_Mutex;
static WindowCommand g_Cmd;
static SharedState g_State;
static EventCallbackFunc g_EventCallback = nullptr;
static CloseCallbackFunc g_CloseCallback = nullptr;

static LRESULT CALLBACK WndProcGL(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (!g_State.isRunning) return DefWindowProc(hWnd, message, wParam, lParam);

    switch (message) {
    case WM_CLOSE:
        if (g_CloseCallback && !g_CloseCallback()) return 0;
        g_State.isRunning = false;
        if (g_EventCallback) g_EventCallback(EVENT_CLOSED, 0, 0);
        return 0;
    case WM_SIZE:
        if (g_EventCallback) g_EventCallback(EVENT_RESIZED, LOWORD(lParam), HIWORD(lParam));
        break;
    case WM_MOVE:
        if (g_EventCallback) g_EventCallback(EVENT_MOVED, (short)LOWORD(lParam), (short)HIWORD(lParam));
        break;
    case WM_SETFOCUS:
        if (g_EventCallback) g_EventCallback(EVENT_FOCUS_GAINED, 0, 0);
        break;
    case WM_KILLFOCUS:
        if (g_EventCallback) g_EventCallback(EVENT_FOCUS_LOST, 0, 0);
        break;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

static void RenderThreadGL(void* texturePtr, int width, int height) {
    GLuint texID = (GLuint)(size_t)texturePtr;

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProcGL, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "GLSubWin", NULL };
    RegisterClassEx(&wc);
    HWND hWnd = CreateWindow("GLSubWin", "GL", WS_OVERLAPPEDWINDOW, 100, 100, width, height, NULL, NULL, wc.hInstance, NULL);

    HDC hDC = GetDC(hWnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(PIXELFORMATDESCRIPTOR), 1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA,
        32, 0,0,0,0,0,0, 8, 0, 0,0,0,0,0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0 };
    int format = ChoosePixelFormat(hDC, &pfd);
    SetPixelFormat(hDC, format, &pfd);

    HGLRC hRC = wglCreateContext(hDC);
    if (g_UnityContext) wglShareLists(g_UnityContext, hRC); // 컨텍스트 공유
    wglMakeCurrent(hDC, hRC);

    ShowWindow(hWnd, SW_SHOWDEFAULT);
    g_hRenderEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_State.isRunning = true;

    while (g_State.isRunning) {
        WaitForSingleObject(g_hRenderEvent, 1000);
        if (!g_State.isRunning) break;

        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (g_Cmd.textureDirty) {
                texID = (GLuint)(size_t)g_Cmd.newTexturePtr;
                g_Cmd.textureDirty = false;
            }
            if (g_Cmd.focusCmdDirty && g_Cmd.setFocus) {
                SetForegroundWindow(hWnd); SetFocus(hWnd);
                g_Cmd.focusCmdDirty = false;
            }
            if (g_Cmd.styleDirty) {
                LONG_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);
                if (g_Cmd.borderless) { style &= ~WS_OVERLAPPEDWINDOW; style |= WS_POPUP; }
                else { style |= WS_OVERLAPPEDWINDOW; style &= ~WS_POPUP; }
                SetWindowLongPtr(hWnd, GWL_STYLE, style);

                // 투명
                MARGINS m = { g_Cmd.transparent ? -1 : 0 };
                DwmExtendFrameIntoClientArea(hWnd, &m);
                SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE);
                g_Cmd.styleDirty = false;
            }
            if (g_Cmd.rectDirty) {
                SetWindowPos(hWnd, NULL, g_Cmd.x, g_Cmd.y, g_Cmd.w, g_Cmd.h, SWP_NOZORDER);
                glViewport(0, 0, g_Cmd.w, g_Cmd.h);
                g_Cmd.rectDirty = false;
            }
            if (g_Cmd.titleDirty) { SetWindowText(hWnd, g_Cmd.title); g_Cmd.titleDirty = false; }
        }

        // 렌더링 (투명 배경)
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texID);
        // 좌표계 상하 반전 처리
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, 1.0f); // Top-Left
        glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, 1.0f); // Top-Right
        glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, -1.0f); // Bottom-Right
        glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f); // Bottom-Left
        glEnd();
        SwapBuffers(hDC);
    }
    wglMakeCurrent(NULL, NULL); wglDeleteContext(hRC); ReleaseDC(hWnd, hDC); DestroyWindow(hWnd);
}

extern "C" {
    UNITY_INTERFACE_EXPORT void UnityPluginLoad(IUnityInterfaces* i) {
        g_UnityContext = wglGetCurrentContext();
    }
    UNITY_INTERFACE_EXPORT void UnityPluginUnload() {
        g_State.isRunning = false;
        if (g_RenderThread.joinable()) g_RenderThread.join();
    }

    UNITY_INTERFACE_EXPORT void StartSubWindow(void* texturePtr, int w, int h) {
        if (g_State.isRunning) return;
        g_State.isRunning = true; // 스레드 시작 전 true 설정
        g_RenderThread = std::thread(RenderThreadGL, texturePtr, w, h);
        g_RenderThread.detach();
    }
    UNITY_INTERFACE_EXPORT void StopSubWindow() {
        g_State.isRunning = false;
        if (g_hRenderEvent) SetEvent(g_hRenderEvent);
    }
    UNITY_INTERFACE_EXPORT void SignalFrameReady() {
        if (g_hRenderEvent) SetEvent(g_hRenderEvent);
    }
    UNITY_INTERFACE_EXPORT void SetEventCallback(EventCallbackFunc callback) { g_EventCallback = callback; }
    UNITY_INTERFACE_EXPORT void SetCloseCallback(CloseCallbackFunc callback) { g_CloseCallback = callback; }

    // API Setters
    UNITY_INTERFACE_EXPORT void UpdateTexture(void* newPtr) {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Cmd.newTexturePtr = newPtr;
        g_Cmd.textureDirty = true;
    }
    UNITY_INTERFACE_EXPORT void FocusWindow() {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Cmd.setFocus = true;
        g_Cmd.focusCmdDirty = true;
    }
    UNITY_INTERFACE_EXPORT void SetConfig(int x, int y, int w, int h, const char* title,
        bool borderless, bool transparent, bool resizable, bool minBtn, bool maxBtn) {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Cmd.x = x; g_Cmd.y = y; g_Cmd.w = w; g_Cmd.h = h;
        strncpy_s(g_Cmd.title, title, 1023);
        g_Cmd.borderless = borderless;
        g_Cmd.transparent = transparent;
        g_Cmd.resizable = resizable;
        g_Cmd.hasMinBtn = minBtn;
        g_Cmd.hasMaxBtn = maxBtn;

        g_Cmd.rectDirty = true;
        g_Cmd.titleDirty = true;
        g_Cmd.styleDirty = true;
    }
}