// Release-performance build: GPU timestamp profiler compiled out.
// Profiling is valuable for diagnostics, but vkCmdWriteTimestamp/query readback plus
// file I/O in a 120-FPS target adds work that does not improve rendered output.
// Keep the same internal API so native_renderer.cpp stays ABI/source compatible.

static bool profileInit(){ return false; }
static void profileDestroy(){}
static void profileBeginFrame(uint32_t,VkCommandBuffer){}
static void profileEndFrame(uint32_t,VkCommandBuffer){}
static void profileReadAndLog(uint32_t){}
static void profilePassSwitch(void*,void*){}
static void profileCountDraw(bool,uint32_t){}
static uint32_t profileBeginExact(uint32_t,uintptr_t=0,uintptr_t=0){ return UINT32_MAX; }
static void profileEndExact(uint32_t){}
static void profileCancelExact(uint32_t){}
