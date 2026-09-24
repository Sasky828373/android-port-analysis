#pragma once

#include <windows.h>
#include <android/native_window.h>

namespace dxvk::wsi {

  inline ANativeWindow* fromHwnd(HWND hWindow) {
    return reinterpret_cast<ANativeWindow*>(hWindow);
  }

  inline HWND toHwnd(ANativeWindow* window) {
    return reinterpret_cast<HWND>(window);
  }

  inline HMONITOR androidDefaultMonitor() {
    return reinterpret_cast<HMONITOR>(uintptr_t(1));
  }

}
