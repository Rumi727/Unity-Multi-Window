#include "MultiWindowShared.h"
#include "IUnityInterface.h"

#include <d3d11.h> 
#include <d3d10_1.h>
#include "IUnityGraphicsD3D11.h"

#include <dwmapi.h>
#include <windows.h>
#include <thread>
#include <mutex>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

// --- 전역 변수 ---
static ID3D11Device* g_UnityDevice = nullptr;
static ID3D10Multithread* g_Multithread = nullptr;

struct D3D11WindowContext {
    std::thread renderThread;
    std::mutex mutex;
    HANDLE hRenderEvent = NULL;
    bool isRunning = false;

    HWND hWnd = NULL;
    WindowCommand cmd;
    ID3D11Texture2D* sharedTexture = nullptr;

    EventCallbackFunc eventCallback = nullptr;
    CloseCallbackFunc closeCallback = nullptr;
};

void SetupTransparency(HWND hWnd, bool enable) {
    MARGINS margins = { enable ? -1 : 0 };
    DwmExtendFrameIntoClientArea(hWnd, &margins);
}

LRESULT CALLBACK GlobalWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    D3D11WindowContext* ctx = nullptr;
    if (message == WM_NCCREATE) {
        CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
        ctx = (D3D11WindowContext*)pCreate->lpCreateParams;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)ctx);
        ctx->hWnd = hWnd;
    }
    else {
        ctx = (D3D11WindowContext*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    }

    if (!ctx) return DefWindowProc(hWnd, message, wParam, lParam);

    switch (message) {
    case WM_CLOSE:
        if (ctx->closeCallback && !ctx->closeCallback(ctx)) return 0;
        ctx->isRunning = false;
        if (ctx->eventCallback) ctx->eventCallback(ctx, EVENT_CLOSED, 0, 0);
        return 0;
    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        if (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED) {
            if (ctx->eventCallback) ctx->eventCallback(ctx, EVENT_RESIZED, w, h);
        }
        break;
    }
    case WM_MOVE:
        if (ctx->eventCallback) ctx->eventCallback(ctx, EVENT_MOVED, (short)LOWORD(lParam), (short)HIWORD(lParam));
        break;
    case WM_SETFOCUS:
        if (ctx->eventCallback) ctx->eventCallback(ctx, EVENT_FOCUS_GAINED, 0, 0);
        break;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void RenderThreadLoop(D3D11WindowContext* ctx, int width, int height) {
    // 1. 유니티 준비 대기
    for (int i = 0; i < 50; i++) { if (g_UnityDevice) break; Sleep(50); }
    if (!g_UnityDevice) { ctx->isRunning = false; return; }

    // [핵심 수정 1] 윈도우 클래스 이름을 인스턴스마다 다르게 설정 (충돌 방지)
    std::string className = "DX11SubWin_" + std::to_string((unsigned long long)ctx);

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, GlobalWndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, className.c_str(), NULL };
    RegisterClassEx(&wc);

    DWORD dwStyle = WS_OVERLAPPEDWINDOW;
    RECT winRect = { 0, 0, width, height };
    AdjustWindowRect(&winRect, dwStyle, FALSE);
    int adjW = winRect.right - winRect.left;
    int adjH = winRect.bottom - winRect.top;

    // 생성 시 고유 클래스 이름 사용
    HWND hWnd = CreateWindowEx(0, className.c_str(), "Init", dwStyle, 100, 100, adjW, adjH, NULL, NULL, wc.hInstance, ctx);

    IDXGIFactory1* factory = nullptr; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Width = width; sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    if (g_Multithread) g_Multithread->Enter();
    HRESULT hr = factory->CreateSwapChain(g_UnityDevice, &sd, &swapChain);
    if (g_Multithread) g_Multithread->Leave();

    if (FAILED(hr) || !swapChain) { if (factory) factory->Release(); DestroyWindow(hWnd); return; }

    ID3D11Texture2D* backBuffer = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);

    ID3D11DeviceContext* context = nullptr;
    g_UnityDevice->GetImmediateContext(&context);

    ShowWindow(hWnd, SW_SHOWDEFAULT);
    ctx->hRenderEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx->isRunning = true;

    while (ctx->isRunning) {
        WaitForSingleObject(ctx->hRenderEvent, 200);
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        if (!ctx->isRunning) break;

        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            if (ctx->cmd.textureDirty) {
                ctx->sharedTexture = (ID3D11Texture2D*)ctx->cmd.newTexturePtr;
                ctx->cmd.textureDirty = false;
            }
            if (ctx->cmd.focusCmdDirty && ctx->cmd.setFocus) {
                SetForegroundWindow(hWnd); SetFocus(hWnd); ctx->cmd.focusCmdDirty = false;
            }
            if (ctx->cmd.rectDirty) {
                RECT r = { 0, 0, ctx->cmd.w, ctx->cmd.h };
                AdjustWindowRect(&r, GetWindowLong(hWnd, GWL_STYLE), FALSE);
                SetWindowPos(hWnd, NULL, ctx->cmd.x, ctx->cmd.y, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
                ctx->cmd.rectDirty = false;
            }
            if (ctx->cmd.styleDirty) {
                LONG_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);
                if (ctx->cmd.borderless) { style &= ~WS_OVERLAPPEDWINDOW; style |= WS_POPUP; }
                else {
                    style |= WS_OVERLAPPEDWINDOW; style &= ~WS_POPUP;
                    if (ctx->cmd.hasMinBtn) style |= WS_MINIMIZEBOX; else style &= ~WS_MINIMIZEBOX;
                    if (ctx->cmd.hasMaxBtn) style |= WS_MAXIMIZEBOX; else style &= ~WS_MAXIMIZEBOX;
                    if (ctx->cmd.resizable) style |= WS_THICKFRAME; else style &= ~WS_THICKFRAME;
                }
                SetWindowLongPtr(hWnd, GWL_STYLE, style);
                SetupTransparency(hWnd, ctx->cmd.transparent);
                SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
                ctx->cmd.styleDirty = false;
            }
            if (ctx->cmd.titleDirty) { SetWindowText(hWnd, ctx->cmd.title); ctx->cmd.titleDirty = false; }
        }

        if (ctx->sharedTexture && backBuffer && context) {
            if (g_Multithread) g_Multithread->Enter();
            context->CopyResource(backBuffer, ctx->sharedTexture);
            if (g_Multithread) g_Multithread->Leave();
        }

        HRESULT res = swapChain->Present(1, 0);
        if (res == DXGI_ERROR_DEVICE_REMOVED || res == DXGI_ERROR_DEVICE_RESET) ctx->isRunning = false;
    }

    if (ctx->hRenderEvent) CloseHandle(ctx->hRenderEvent);
    if (backBuffer) backBuffer->Release();
    if (swapChain) swapChain->Release();
    if (factory) factory->Release();
    if (context) context->Release();
    DestroyWindow(hWnd);
    UnregisterClass(className.c_str(), wc.hInstance); // 클래스 해제
}

extern "C" {
    UNITY_INTERFACE_EXPORT void UnityPluginLoad(IUnityInterfaces* i) {
        auto* gfx = i->Get<IUnityGraphicsD3D11>();
        if (gfx) {
            g_UnityDevice = gfx->GetDevice();
            if (g_UnityDevice) {
                // ID3D10Multithread로 멀티스레드 보호 활성화
                g_UnityDevice->QueryInterface(__uuidof(ID3D10Multithread), (void**)&g_Multithread);
                if (g_Multithread) g_Multithread->SetMultithreadProtected(TRUE);
            }
        }
    }

    UNITY_INTERFACE_EXPORT void UnityPluginUnload() {
        if (g_Multithread) { g_Multithread->Release(); g_Multithread = nullptr; }
    }

    UNITY_INTERFACE_EXPORT void* StartSubWindow(void* texturePtr, int w, int h) {
        if (!g_UnityDevice) return nullptr; // 디바이스 없으면 시작 안 함 (안전장치)

        D3D11WindowContext* ctx = new D3D11WindowContext();
        ctx->sharedTexture = (ID3D11Texture2D*)texturePtr;
        ctx->isRunning = true;
        ctx->renderThread = std::thread(RenderThreadLoop, ctx, w, h);
        return (void*)ctx;
    }

    // ... (나머지 StopSubWindow 등 함수들은 그대로 유지) ...
    UNITY_INTERFACE_EXPORT void StopSubWindow(void* handle) {
        D3D11WindowContext* ctx = (D3D11WindowContext*)handle;
        if (!ctx) return;
        ctx->isRunning = false;
        if (ctx->hRenderEvent) SetEvent(ctx->hRenderEvent);
        if (ctx->renderThread.joinable()) ctx->renderThread.join();
        delete ctx;
    }

    UNITY_INTERFACE_EXPORT void SignalFrameReady(void* handle) {
        D3D11WindowContext* ctx = (D3D11WindowContext*)handle;
        if (ctx && ctx->hRenderEvent) SetEvent(ctx->hRenderEvent);
    }

    // ... (SetEventCallback, SetCloseCallback, UpdateTexture, FocusWindow, SetConfig 등은 기존과 동일) ...
    // 복사해서 넣으시면 됩니다.
    UNITY_INTERFACE_EXPORT void SetEventCallback(void* handle, EventCallbackFunc callback) {
        D3D11WindowContext* ctx = (D3D11WindowContext*)handle;
        if (ctx) ctx->eventCallback = callback;
    }
    UNITY_INTERFACE_EXPORT void SetCloseCallback(void* handle, CloseCallbackFunc callback) {
        D3D11WindowContext* ctx = (D3D11WindowContext*)handle;
        if (ctx) ctx->closeCallback = callback;
    }
    UNITY_INTERFACE_EXPORT void UpdateTexture(void* handle, void* newPtr) {
        D3D11WindowContext* ctx = (D3D11WindowContext*)handle;
        if (!ctx) return;
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->cmd.newTexturePtr = newPtr; ctx->cmd.textureDirty = true;
    }
    UNITY_INTERFACE_EXPORT void FocusWindow(void* handle) {
        D3D11WindowContext* ctx = (D3D11WindowContext*)handle;
        if (ctx) { std::lock_guard<std::mutex> lock(ctx->mutex); ctx->cmd.setFocus = true; ctx->cmd.focusCmdDirty = true; }
    }
    UNITY_INTERFACE_EXPORT void SetConfig(void* handle, int x, int y, int w, int h, const char* title,
        bool borderless, bool transparent, bool resizable, bool minBtn, bool maxBtn) {
        D3D11WindowContext* ctx = (D3D11WindowContext*)handle;
        if (!ctx) return;
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->cmd.x = x; ctx->cmd.y = y; ctx->cmd.w = w; ctx->cmd.h = h;
        strncpy_s(ctx->cmd.title, title, 1023);
        ctx->cmd.borderless = borderless; ctx->cmd.transparent = transparent;
        ctx->cmd.resizable = resizable; ctx->cmd.hasMinBtn = minBtn; ctx->cmd.hasMaxBtn = maxBtn;
        ctx->cmd.rectDirty = true; ctx->cmd.titleDirty = true; ctx->cmd.styleDirty = true;
    }
}