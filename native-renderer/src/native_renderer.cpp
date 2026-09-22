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
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_begin_rendering(VkCommandBuffer cmd,VkRect2D area,uint32_t colorCount,const VkRenderingAttachmentInfo* colors,const VkRenderingAttachmentInfo* depth) {
 if(!cmd) return; VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea=area; ri.layerCount=1; ri.colorAttachmentCount=colorCount; ri.pColorAttachments=colors; ri.pDepthAttachment=depth; vkCmdBeginRendering(cmd,&ri);
}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_end_rendering(VkCommandBuffer cmd){if(cmd)vkCmdEndRendering(cmd);}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_copy_buffer(VkCommandBuffer cmd,VkBuffer src,VkBuffer dst,VkDeviceSize size) {
 if(!cmd||!src||!dst||!size)return; VkBufferCopy r{0,0,size}; vkCmdCopyBuffer(cmd,src,dst,1,&r);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_copy_image(VkCommandBuffer cmd,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,const VkImageCopy* regions,uint32_t count) {
 if(cmd&&src&&dst&&regions&&count)vkCmdCopyImage(cmd,src,sl,dst,dl,count,regions);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_blit_image(VkCommandBuffer cmd,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,const VkImageBlit* regions,uint32_t count,VkFilter filter) {
 if(cmd&&src&&dst&&regions&&count)vkCmdBlitImage(cmd,src,sl,dst,dl,count,regions,filter);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_clear_color(VkCommandBuffer cmd,VkImage image,VkImageLayout layout,const VkClearColorValue* value,const VkImageSubresourceRange* range) {
 if(cmd&&image&&value&&range)vkCmdClearColorImage(cmd,image,layout,value,1,range);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_clear_depth(VkCommandBuffer cmd,VkImage image,VkImageLayout layout,const VkClearDepthStencilValue* value,const VkImageSubresourceRange* range) {
 if(cmd&&image&&value&&range)vkCmdClearDepthStencilImage(cmd,image,layout,value,1,range);
}
extern "C" __attribute__((visibility("default")))
VkResult gtav_native_renderer_create_shader(const uint32_t* spirv,size_t bytes,VkShaderModule* out) {
 if(!g.device||!spirv||!bytes||!out)return VK_ERROR_INITIALIZATION_FAILED; VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=bytes;ci.pCode=spirv;return vkCreateShaderModule(g.device,&ci,nullptr,out);
}
extern "C" __attribute__((visibility("default")))
void gtav_native_renderer_barrier2(VkCommandBuffer cmd,const VkDependencyInfo* info){if(cmd&&info)vkCmdPipelineBarrier2(cmd,info);}
extern "C" __attribute__((visibility("default")))
VkResult gtav_native_renderer_submit(VkCommandBuffer cmd,VkFence fence){
 if(!g.queue||!cmd)return VK_ERROR_INITIALIZATION_FAILED; VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=cmd;VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.commandBufferInfoCount=1;si.pCommandBufferInfos=&cb;return vkQueueSubmit2(g.queue,1,&si,fence);
}
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
