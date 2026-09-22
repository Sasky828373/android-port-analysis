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
extern "C" __attribute__((visibility("default")))
bool gtav_native_renderer_alloc_command_buffer(VkCommandBuffer* out) {
 if(!out||!g.device||!g.commands) return false;
 VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
 ai.commandPool=g.commands; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
 return vkAllocateCommandBuffers(g.device,&ai,out)==VK_SUCCESS;
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_bind_pipeline(VkCommandBuffer cmd,VkPipelineBindPoint point,VkPipeline pipeline) {
 if(cmd&&pipeline) vkCmdBindPipeline(cmd,point,pipeline);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_bind_descriptors(VkCommandBuffer cmd,VkPipelineBindPoint point,VkPipelineLayout layout,
 uint32_t firstSet,uint32_t count,const VkDescriptorSet* sets) {
 if(cmd&&layout&&count&&sets) vkCmdBindDescriptorSets(cmd,point,layout,firstSet,count,sets,0,nullptr);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_update_uniform_buffer(VkDescriptorSet set,uint32_t binding,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize range) {
 if(!g.device||!set||!buffer) return;
 VkDescriptorBufferInfo bi{buffer,offset,range};
 VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
 w.dstSet=set; w.dstBinding=binding; w.descriptorCount=1; w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo=&bi;
 vkUpdateDescriptorSets(g.device,1,&w,0,nullptr);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_update_sampled_image(VkDescriptorSet set,uint32_t binding,VkImageView view,VkSampler sampler,VkImageLayout layout) {
 if(!g.device||!set||!view||!sampler) return;
 VkDescriptorImageInfo ii{sampler,view,layout};
 VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
 w.dstSet=set; w.dstBinding=binding; w.descriptorCount=1; w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&ii;
 vkUpdateDescriptorSets(g.device,1,&w,0,nullptr);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_dispatch(VkCommandBuffer cmd,uint32_t x,uint32_t y,uint32_t z) {
 if(cmd&&x&&y&&z) vkCmdDispatch(cmd,x,y,z);
}
}

extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_bind_vertex_buffer(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize offset) {
 if(!cmd||!buffer) return; vkCmdBindVertexBuffers(cmd,0,1,&buffer,&offset);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_bind_index_buffer(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize offset,VkIndexType type) {
 if(!cmd||!buffer) return; vkCmdBindIndexBuffer(cmd,buffer,offset,type);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_set_viewport(VkCommandBuffer cmd,float x,float y,float w,float h,float minDepth,float maxDepth) {
 if(!cmd) return; VkViewport v{x,y,w,h,minDepth,maxDepth}; vkCmdSetViewport(cmd,0,1,&v);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_set_scissor(VkCommandBuffer cmd,int32_t x,int32_t y,uint32_t w,uint32_t h) {
 if(!cmd) return; VkRect2D r{{x,y},{w,h}}; vkCmdSetScissor(cmd,0,1,&r);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_draw(VkCommandBuffer cmd,uint32_t vertices,uint32_t firstVertex) {
 if(cmd&&vertices) vkCmdDraw(cmd,vertices,1,firstVertex,0);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_draw_indexed(VkCommandBuffer cmd,uint32_t indices,uint32_t firstIndex,int32_t vertexOffset) {
 if(cmd&&indices) vkCmdDrawIndexed(cmd,indices,1,firstIndex,vertexOffset,0);
}
}

extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_begin_frame(){g.frame.fetch_add(1,std::memory_order_relaxed);}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_ready(){return g.device&&g.queue&&g.commands&&g.descriptors;}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_shutdown(){if(!g.device)return;vkDeviceWaitIdle(g.device);if(g.descriptors)vkDestroyDescriptorPool(g.device,g.descriptors,nullptr);if(g.commands)vkDestroyCommandPool(g.device,g.commands,nullptr);}
}
