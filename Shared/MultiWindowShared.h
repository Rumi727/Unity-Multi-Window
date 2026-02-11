#pragma once

enum NativeEventType {
    EVENT_CLOSED = 0, EVENT_MOVED = 1, EVENT_RESIZED = 2,
    EVENT_FOCUS_GAINED = 3, EVENT_FOCUS_LOST = 4,
    EVENT_MINIMIZED = 5, EVENT_MAXIMIZED = 6, EVENT_RESTORED = 7
};

typedef void (*EventCallbackFunc)(void* handle, int type, int data1, int data2);
typedef bool (*CloseCallbackFunc)(void* handle);

struct WindowCommand {
    bool rectDirty = false; int x, y, w, h;
    bool titleDirty = false; char title[1024];
    bool styleDirty = false; bool borderless = false; bool transparent = false;
    bool resizable = true; bool hasMinBtn = true; bool hasMaxBtn = true;
    bool focusCmdDirty = false; bool setFocus = false;
    bool textureDirty = false; void* newTexturePtr = nullptr;
};