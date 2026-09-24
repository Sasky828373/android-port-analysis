#pragma once

#include <atomic>
#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include "../wsi_platform.h"

namespace dxvk::wsi {

  class AndroidWsiDriver final : public WsiDriver {
  public:
    AndroidWsiDriver();
    ~AndroidWsiDriver();

    std::vector<const char *> getInstanceExtensions() override;

    HMONITOR getDefaultMonitor() override;
    HMONITOR enumMonitors(uint32_t index) override;
    HMONITOR enumMonitors(const LUID *adapterLUID[], uint32_t numLUIDs, uint32_t index) override;

    bool getDisplayName(HMONITOR hMonitor, WCHAR (&Name)[32]) override;
    bool getDesktopCoordinates(HMONITOR hMonitor, RECT* pRect) override;
    bool getDisplayMode(HMONITOR hMonitor, uint32_t modeNumber, WsiMode* pMode) override;
    bool getCurrentDisplayMode(HMONITOR hMonitor, WsiMode* pMode) override;
    bool getDesktopDisplayMode(HMONITOR hMonitor, WsiMode* pMode) override;
    WsiEdidData getMonitorEdid(HMONITOR hMonitor) override;

    void getWindowSize(HWND hWindow, uint32_t* pWidth, uint32_t* pHeight) override;
    void resizeWindow(HWND hWindow, DxvkWindowState* pState, uint32_t width, uint32_t height) override;
    void saveWindowState(HWND hWindow, DxvkWindowState* pState, bool saveStyle) override;
    void restoreWindowState(HWND hWindow, DxvkWindowState* pState, bool restoreCoordinates) override;
    bool setWindowMode(HMONITOR hMonitor, HWND hWindow, DxvkWindowState* pState, const WsiMode& mode) override;
    bool enterFullscreenMode(HMONITOR hMonitor, HWND hWindow, DxvkWindowState* pState, bool modeSwitch) override;
    bool leaveFullscreenMode(HWND hWindow, DxvkWindowState* pState) override;
    bool restoreDisplayMode() override;
    HMONITOR getWindowMonitor(HWND hWindow) override;
    bool isWindow(HWND hWindow) override;
    bool isMinimized(HWND hWindow) override;
    bool isOccluded(HWND hWindow) override;
    void updateFullscreenWindow(HMONITOR hMonitor, HWND hWindow, bool forceTopmost) override;

    VkResult createSurface(
      HWND hWindow,
      PFN_vkGetInstanceProcAddr pfnVkGetInstanceProcAddr,
      VkInstance instance,
      VkSurfaceKHR* pSurface) override;

  private:
    void updateExtent(ANativeWindow* window);
    WsiMode currentMode() const;

    std::atomic<uint32_t> m_width  { 1920u };
    std::atomic<uint32_t> m_height { 1080u };
  };

}
