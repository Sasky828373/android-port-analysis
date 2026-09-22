#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
extern "C" {
struct GtavNativeDispatch {
 uint32_t abi_version;
 bool (*ready)();
 void (*begin_frame)();
 void (*bind_vertex)(VkCommandBuffer,VkBuffer,VkDeviceSize);
 void (*bind_index)(VkCommandBuffer,VkBuffer,VkDeviceSize,VkIndexType);
 void (*viewport)(VkCommandBuffer,float,float,float,float,float,float);
 void (*scissor)(VkCommandBuffer,int32_t,int32_t,uint32_t,uint32_t);
 void (*draw)(VkCommandBuffer,uint32_t,uint32_t);
 void (*draw_indexed)(VkCommandBuffer,uint32_t,uint32_t,int32_t);
 void (*dispatch)(VkCommandBuffer,uint32_t,uint32_t,uint32_t);
};
__attribute__((visibility("default"))) const GtavNativeDispatch* gtav_native_renderer_get_dispatch();
}
