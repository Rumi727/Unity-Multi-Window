#include "MultiWindowShared.h"
#include "IUnityInterface.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <GL/glx.h>
#include <thread>
#include <mutex>
#include <string.h>
#include <unistd.h>

#define MY_EXPORT __attribute__((visibility("default")))

static GLXContext g_UnityCtx = nullptr; // 공유용 전역 컨텍스트

struct LinuxWindowContext {
    std::thread renderThread;
    std::mutex mutex;
    bool isRunning = false;
    WindowCommand cmd;

    // 콜백
    EventCallbackFunc eventCallback = nullptr;
    CloseCallbackFunc closeCallback = nullptr;
};

// ... GetARGBVisual 함수 (이전과 동일) ...
static XVisualInfo* GetARGBVisual(Display* dpy) {
    XVisualInfo templateVis; templateVis.depth = 32; templateVis.c_class = TrueColor; int n;
    return XGetVisualInfo(dpy, VisualDepthMask | VisualClassMask, &templateVis, &n);
}

void RenderThreadX11(LinuxWindowContext* ctx, void* texturePtr, int width, int height) {
    GLuint texID = (GLuint)(size_t)texturePtr;
    Display* dpy = XOpenDisplay(NULL);
    Window root = DefaultRootWindow(dpy);
    XVisualInfo* vi = GetARGBVisual(dpy);
    if (!vi) { GLint a[] = { GLX_RGBA,GLX_DOUBLEBUFFER,None }; vi = glXChooseVisual(dpy, 0, a); }

    GLXContext glCtx = glXCreateContext(dpy, vi, g_UnityCtx, GL_TRUE); // 공유

    XSetWindowAttributes swa; swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.border_pixel = 0; swa.background_pixel = 0;
    swa.event_mask = StructureNotifyMask | FocusChangeMask | KeyPressMask;

    Window win = XCreateWindow(dpy, root, 0, 0, width, height, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &swa);
    Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False); XSetWMProtocols(dpy, win, &wmDelete, 1);
    Atom wmHints = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);

    XMapWindow(dpy, win);
    glXMakeCurrent(dpy, win, glCtx);
    ctx->isRunning = true;

    while (ctx->isRunning) {
        while (XPending(dpy) > 0) {
            XEvent xev;
            XNextEvent(dpy, &xev);

            if (xev.type == ClientMessage && (Atom)xev.xclient.data.l[0] == wmDelete) {
                if (ctx->closeCallback && !ctx->closeCallback()) continue; // 취소
                ctx->isRunning = false;
                if (ctx->eventCallback) ctx->eventCallback(EVENT_CLOSED, 0, 0);
            }
            else if (xev.type == ConfigureNotify) {
                int w = xev.xconfigure.width; int h = xev.xconfigure.height;
                glViewport(0, 0, w, h);
                if (ctx->eventCallback) ctx->eventCallback(EVENT_RESIZED, w, h);
            }
            else if (xev.type == FocusIn) {
                if (ctx->eventCallback) ctx->eventCallback(EVENT_FOCUS_GAINED, 0, 0);
            }
            else if (xev.type == FocusOut) {
                if (ctx->eventCallback) ctx->eventCallback(EVENT_FOCUS_LOST, 0, 0);
            }
        }

        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            if (ctx->cmd.textureDirty) {
                texID = (GLuint)(size_t)ctx->cmd.newTexturePtr;
                ctx->cmd.textureDirty = false;
            }
            if (ctx->cmd.focusCmdDirty && ctx->cmd.setFocus) {
                XRaiseWindow(dpy, win); XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
                ctx->cmd.focusCmdDirty = false;
            }
            if (ctx->cmd.rectDirty) {
                XMoveResizeWindow(dpy, win, ctx->cmd.x, ctx->cmd.y, ctx->cmd.w, ctx->cmd.h);
                ctx->cmd.rectDirty = false;
            }
            if (ctx->cmd.titleDirty) { XStoreName(dpy, win, ctx->cmd.title); ctx->cmd.titleDirty = false; }
            if (ctx->cmd.styleDirty) {
                struct MwmHints { unsigned long flags, functions, decorations; long input_mode; unsigned long status; };
                MwmHints hints = { 0 }; hints.flags = 2; hints.decorations = ctx->cmd.borderless ? 0 : 1;
                XChangeProperty(dpy, win, wmHints, wmHints, 32, PropModeReplace, (unsigned char*)&hints, 5);
                ctx->cmd.styleDirty = false;
            }
        }

        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, texID);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, 1.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, 1.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, -1.0f);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
        glEnd();
        glXSwapBuffers(dpy, win);
        usleep(16000);
    }
    // 정리
}

extern "C" {
    UNITY_INTERFACE_EXPORT void UnityPluginLoad(IUnityInterfaces* i) { g_UnityCtx = glXGetCurrentContext(); }

    // [인스턴스 생성]
    UNITY_INTERFACE_EXPORT void* StartSubWindow(void* texturePtr, int w, int h) {
        LinuxWindowContext* ctx = new LinuxWindowContext();
        ctx->isRunning = true;
        ctx->renderThread = std::thread(RenderThreadX11, ctx, texturePtr, w, h);
        return (void*)ctx;
    }

    // [인스턴스 파괴]
    UNITY_INTERFACE_EXPORT void StopSubWindow(void* handle) {
        LinuxWindowContext* ctx = (LinuxWindowContext*)handle;
        if (!ctx) return;
        ctx->isRunning = false;
        if (ctx->renderThread.joinable()) ctx->renderThread.join();
        delete ctx;
    }

    // 모든 Setter 함수에 handle 추가 (D3D11과 동일하게 구현)
    UNITY_INTERFACE_EXPORT void SignalFrameReady(void* handle) {}
    UNITY_INTERFACE_EXPORT void SetEventCallback(void* handle, EventCallbackFunc cb) { ((LinuxWindowContext*)handle)->eventCallback = cb; }
    UNITY_INTERFACE_EXPORT void SetCloseCallback(void* handle, CloseCallbackFunc cb) { ((LinuxWindowContext*)handle)->closeCallback = cb; }
    UNITY_INTERFACE_EXPORT void UpdateTexture(void* handle, void* ptr) {
        LinuxWindowContext* c = (LinuxWindowContext*)handle; std::lock_guard<std::mutex> l(c->mutex); c->cmd.newTexturePtr = ptr; c->cmd.textureDirty = true;
    }
    UNITY_INTERFACE_EXPORT void FocusWindow(void* handle) { /* ... */ }
    UNITY_INTERFACE_EXPORT void SetConfig(void* handle, int x, int y, int w, int h, const char* title, bool b, bool t, bool r, bool min, bool max) {
        LinuxWindowContext* c = (LinuxWindowContext*)handle; std::lock_guard<std::mutex> l(c->mutex);
        c->cmd.x = x; c->cmd.y = y; c->cmd.w = w; c->cmd.h = h; strcpy(c->cmd.title, title);
        c->cmd.rectDirty = true; /* ... */
    }
}