#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>
namespace gtavnative {
struct Runtime { VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{}; uint32_t family{}; VkCommandPool commands{}; VkDescriptorPool descriptors{}; std::atomic<uint64_t> frame{0}; };
static Runtime g;
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_attach(VkInstance i,VkPhysicalDevice p,VkDevice d,VkQueue q,uint32_t family) {
 if(!i||!p||!d||!q) return false; g.instance=i; g.physical=p; g.device=d; g.queue=q; g.family=family;
 VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; ci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT|VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; ci.queueFamilyIndex=family;
 if(vkCreateCommandPool(d,&ci,nullptr,&g.commands)!=VK_SUCCESS) return false;
 VkDescriptorPoolSize s[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4096},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8192},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2048},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2048}};
 VkDescriptorPoolCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; di.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; di.maxSets=8192; di.poolSizeCount=4; di.pPoolSizes=s;
 return vkCreateDescriptorPool(d,&di,nullptr,&g.descriptors)==VK_SUCCESS;
}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_begin_frame(){g.frame.fetch_add(1,std::memory_order_relaxed);}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_ready(){return g.device&&g.queue&&g.commands&&g.descriptors;}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_shutdown(){if(!g.device)return;vkDeviceWaitIdle(g.device);if(g.descriptors)vkDestroyDescriptorPool(g.device,g.descriptors,nullptr);if(g.commands)vkDestroyCommandPool(g.device,g.commands,nullptr);}
}
