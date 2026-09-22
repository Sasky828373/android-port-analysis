#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
extern "C" {
enum GtavNativeResourceKind : uint32_t { GTAV_NATIVE_BUFFER=1, GTAV_NATIVE_IMAGE_VIEW=2, GTAV_NATIVE_SAMPLER=3 };
struct GtavNativeResourceHandle { uint64_t rage_handle; uint64_t vk_handle; uint32_t kind; uint32_t generation; };
struct GtavNativeDrawState { VkCommandBuffer command_buffer; uint32_t flags; };
using GtavNativeGetDrawState = bool(*)(void*,GtavNativeDrawState*);
struct GtavNativeDispatch {
 uint32_t abi_version;
 bool (*ready)();
 void (*begin_frame)();
 bool (*register_resource)(uint64_t,uint64_t,uint32_t,uint32_t);
 uint64_t (*resolve_resource)(uint64_t,uint32_t);
 void (*unregister_resource)(uint64_t,uint32_t);
 void (*bind_vertex)(VkCommandBuffer,VkBuffer,VkDeviceSize);
 void (*bind_index)(VkCommandBuffer,VkBuffer,VkDeviceSize,VkIndexType);
 void (*viewport)(VkCommandBuffer,float,float,float,float,float,float);
 void (*scissor)(VkCommandBuffer,int32_t,int32_t,uint32_t,uint32_t);
 void (*draw)(VkCommandBuffer,uint32_t,uint32_t);
 void (*draw_indexed)(VkCommandBuffer,uint32_t,uint32_t,int32_t);
 void (*dispatch)(VkCommandBuffer,uint32_t,uint32_t,uint32_t);
 void (*set_draw_state_provider)(GtavNativeGetDrawState);
 bool (*rage_draw)(void*,uint32_t,uint32_t);
 bool (*rage_draw_indexed)(void*,uint32_t,uint32_t,int32_t);
 bool (*rage_dispatch)(void*,uint32_t,uint32_t,uint32_t);
};
__attribute__((visibility("default"))) const GtavNativeDispatch* gtav_native_renderer_get_dispatch();
}
