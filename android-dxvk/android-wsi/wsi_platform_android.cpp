#if defined(DXVK_WSI_ANDROID)

#include "wsi_platform_android.h"
#include "native/wsi/native_android.h"

#include "../../util/log/log.h"
#include "../../util/util_error.h"

#include <cstring>

namespace dxvk::wsi {

  AndroidWsiDriver::AndroidWsiDriver() {
    Logger::info("Android WSI: direct ANativeWindow backend enabled");
  }

  AndroidWsiDriver::~AndroidWsiDriver() {
  }

  std::vector<const char *> AndroidWsiDriver::getInstanceExtensions() {
    return {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
  }

  void AndroidWsiDriver::updateExtent(ANativeWindow* window) {
    if (!window)
      return;

    const int32_t width = ANativeWindow_getWidth(window);
    const int32_t height = ANativeWindow_getHeight(window);

    if (width > 0)
      m_width.store(uint32_t(width), std::memory_order_relaxed);
    if (height > 0)
      m_height.store(uint32_t(height), std::memory_order_relaxed);
  }

  WsiMode AndroidWsiDriver::currentMode() const {
    return WsiMode {
      m_width.load(std::memory_order_relaxed),
      m_height.load(std::memory_order_relaxed),
      WsiRational { 60, 1 },
      32,
      false,
    };
  }

  HMONITOR AndroidWsiDriver::getDefaultMonitor() {
    return androidDefaultMonitor();
  }

  HMONITOR AndroidWsiDriver::enumMonitors(uint32_t index) {
    return index == 0 ? androidDefaultMonitor() : nullptr;
  }

  HMONITOR AndroidWsiDriver::enumMonitors(const LUID *adapterLUID[], uint32_t numLUIDs, uint32_t index) {
    return enumMonitors(index);
  }

  bool AndroidWsiDriver::getDisplayName(HMONITOR hMonitor, WCHAR (&Name)[32]) {
    if (hMonitor != androidDefaultMonitor())
      return false;

    static const WCHAR name[] = L"\\\\.\\DISPLAY1";
    std::memset(Name, 0, sizeof(Name));
    std::memcpy(Name, name, sizeof(name));
    return true;
  }

  bool AndroidWsiDriver::getDesktopCoordinates(HMONITOR hMonitor, RECT* pRect) {
    if (hMonitor != androidDefaultMonitor() || !pRect)
      return false;

    pRect->left = 0;
    pRect->top = 0;
    pRect->right = LONG(m_width.load(std::memory_order_relaxed));
    pRect->bottom = LONG(m_height.load(std::memory_order_relaxed));
    return true;
  }

  bool AndroidWsiDriver::getDisplayMode(HMONITOR hMonitor, uint32_t modeNumber, WsiMode* pMode) {
    if (hMonitor != androidDefaultMonitor() || modeNumber != 0 || !pMode)
      return false;

    *pMode = currentMode();
    return true;
  }

  bool AndroidWsiDriver::getCurrentDisplayMode(HMONITOR hMonitor, WsiMode* pMode) {
    if (hMonitor != androidDefaultMonitor() || !pMode)
      return false;

    *pMode = currentMode();
    return true;
  }

  bool AndroidWsiDriver::getDesktopDisplayMode(HMONITOR hMonitor, WsiMode* pMode) {
    return getCurrentDisplayMode(hMonitor, pMode);
  }

  WsiEdidData AndroidWsiDriver::getMonitorEdid(HMONITOR hMonitor) {
    return { };
  }

  void AndroidWsiDriver::getWindowSize(HWND hWindow, uint32_t* pWidth, uint32_t* pHeight) {
    ANativeWindow* window = fromHwnd(hWindow);
    updateExtent(window);

    if (pWidth)
      *pWidth = m_width.load(std::memory_order_relaxed);
    if (pHeight)
      *pHeight = m_height.load(std::memory_order_relaxed);
  }

  void AndroidWsiDriver::resizeWindow(
      HWND hWindow,
      DxvkWindowState* pState,
      uint32_t width,
      uint32_t height) {
    ANativeWindow* window = fromHwnd(hWindow);

    // Vulkan owns the swapchain image geometry. Do not force a CPU-side
    // ANativeWindow buffer format here; just track DXGI's requested extent.
    if (width)
      m_width.store(width, std::memory_order_relaxed);
    if (height)
      m_height.store(height, std::memory_order_relaxed);

    updateExtent(window);
  }

  void AndroidWsiDriver::saveWindowState(HWND hWindow, DxvkWindowState* pState, bool saveStyle) {
  }

  void AndroidWsiDriver::restoreWindowState(HWND hWindow, DxvkWindowState* pState, bool restoreCoordinates) {
  }

  bool AndroidWsiDriver::setWindowMode(
      HMONITOR hMonitor,
      HWND hWindow,
      DxvkWindowState* pState,
      const WsiMode& mode) {
    if (hMonitor != androidDefaultMonitor())
      return false;

    if (mode.width)
      m_width.store(mode.width, std::memory_order_relaxed);
    if (mode.height)
      m_height.store(mode.height, std::memory_order_relaxed);
    return true;
  }

  bool AndroidWsiDriver::enterFullscreenMode(
      HMONITOR hMonitor,
      HWND hWindow,
      DxvkWindowState* pState,
      bool modeSwitch) {
    // Android game surfaces are already compositor-managed fullscreen surfaces.
    updateExtent(fromHwnd(hWindow));
    return hMonitor == androidDefaultMonitor() && fromHwnd(hWindow) != nullptr;
  }

  bool AndroidWsiDriver::leaveFullscreenMode(HWND hWindow, DxvkWindowState* pState) {
    return fromHwnd(hWindow) != nullptr;
  }

  bool AndroidWsiDriver::restoreDisplayMode() {
    return true;
  }

  HMONITOR AndroidWsiDriver::getWindowMonitor(HWND hWindow) {
    return fromHwnd(hWindow) ? androidDefaultMonitor() : nullptr;
  }

  bool AndroidWsiDriver::isWindow(HWND hWindow) {
    return fromHwnd(hWindow) != nullptr;
  }

  bool AndroidWsiDriver::isMinimized(HWND hWindow) {
    return false;
  }

  bool AndroidWsiDriver::isOccluded(HWND hWindow) {
    return false;
  }

  void AndroidWsiDriver::updateFullscreenWindow(HMONITOR hMonitor, HWND hWindow, bool forceTopmost) {
    updateExtent(fromHwnd(hWindow));
  }

  VkResult AndroidWsiDriver::createSurface(
      HWND hWindow,
      PFN_vkGetInstanceProcAddr pfnVkGetInstanceProcAddr,
      VkInstance instance,
      VkSurfaceKHR* pSurface) {
    ANativeWindow* window = fromHwnd(hWindow);

    if (!window || !pfnVkGetInstanceProcAddr || !instance || !pSurface)
      return VK_ERROR_INITIALIZATION_FAILED;

    updateExtent(window);

    auto vkCreateAndroidSurfaceKHR =
      reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        pfnVkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));

    if (!vkCreateAndroidSurfaceKHR)
      return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkAndroidSurfaceCreateInfoKHR info = { };
    info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    info.window = window;

    return vkCreateAndroidSurfaceKHR(instance, &info, nullptr, pSurface);
  }

  static bool createAndroidWsiDriver(WsiDriver **driver) {
    try {
      *driver = new AndroidWsiDriver();
      return true;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return false;
    }
  }

  WsiBootstrap AndroidWSI = {
    "Android",
    createAndroidWsiDriver,
  };

}

#endif
