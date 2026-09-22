#include <vulkan/vulkan.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <link.h>
#include "native_dispatch.h"
#include <dlfcn.h>

namespace gtavnative {
struct Runtime { VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{}; uint32_t family{}; VkCommandPool commands{}; VkDescriptorPool descriptors{}; std::atomic<uint64_t> frame{0}; };
static Runtime g;
static std::mutex resourceMutex;
static std::unordered_map<uint64_t,GtavNativeResourceHandle> resources;
static PFN_vkCmdBeginRendering pBeginRendering{};
static PFN_vkCmdEndRendering pEndRendering{};
static PFN_vkCmdPipelineBarrier2 pBarrier2{};
static PFN_vkQueueSubmit2 pSubmit2{};
static std::atomic<bool> drawHooksInstalled{false};
static GtavNativeGetDrawState drawStateProvider{};
struct HookTarget { uintptr_t va; void* replacement; uint32_t original[4]; void* trampoline; };
static uintptr_t gtavBase{};
static int findGtav(struct dl_phdr_info* i,size_t,void*){ if(i&&i->dlpi_name&&std::strstr(i->dlpi_name,"libgtav.so")){gtavBase=i->dlpi_addr;return 1;} return 0; }
static void* makeTrampoline(uintptr_t target,const uint32_t original[4]){
 void* m=mmap(nullptr,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(m==MAP_FAILED)return nullptr;
 std::memcpy(m,original,16);
 uint32_t veneer[4]={0x58000050u,0xd61f0200u,0,0};
 uint64_t back=(uint64_t)(target+16); std::memcpy(&veneer[2],&back,sizeof(back));
 std::memcpy((uint8_t*)m+16,veneer,sizeof(veneer));
 __builtin___clear_cache((char*)m,(char*)m+32); return m;
}
static bool patchJump(uintptr_t target,void* replacement,uint32_t original[4],void** trampoline){
 std::memcpy(original,(void*)target,16); *trampoline=makeTrampoline(target,original); if(!*trampoline)return false;
 long ps=sysconf(_SC_PAGESIZE); uintptr_t page=target&~((uintptr_t)ps-1);
 if(mprotect((void*)page,ps,PROT_READ|PROT_WRITE|PROT_EXEC))return false;
 // AArch64 absolute 16-byte veneer: ldr x16,#8 ; br x16 ; .quad replacement.
 // Unlike B imm26 this works even when libgtav_native_renderer.so is > +/-128 MiB away.
 uint32_t veneer[4]={0x58000050u,0xd61f0200u,0,0};
 uint64_t dst=(uint64_t)(uintptr_t)replacement; std::memcpy(&veneer[2],&dst,sizeof(dst));
 std::memcpy((void*)target,veneer,sizeof(veneer));
 __builtin___clear_cache((char*)target,(char*)target+sizeof(veneer));
 mprotect((void*)page,ps,PROT_READ|PROT_EXEC); return true;
}
static void load13(VkDevice d){
 pBeginRendering=reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(d,"vkCmdBeginRendering"));
 pEndRendering=reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(d,"vkCmdEndRendering"));
 pBarrier2=reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(d,"vkCmdPipelineBarrier2"));
 pSubmit2=reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(d,"vkQueueSubmit2"));
}
static uint64_t resourceKey(uint64_t rage,uint32_t kind){ return (rage<<3)^uint64_t(kind); }

extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_register_resource(uint64_t rage,uint64_t vk,uint32_t kind,uint32_t generation){if(!rage||!vk||!kind)return false;std::lock_guard<std::mutex> l(resourceMutex);resources[resourceKey(rage,kind)]={rage,vk,kind,generation};return true;}
extern "C" __attribute__((visibility("default"))) uint64_t gtav_native_renderer_resolve_resource(uint64_t rage,uint32_t kind){std::lock_guard<std::mutex> l(resourceMutex);auto i=resources.find(resourceKey(rage,kind));return i==resources.end()?0:i->second.vk_handle;}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_unregister_resource(uint64_t rage,uint32_t kind){std::lock_guard<std::mutex> l(resourceMutex);resources.erase(resourceKey(rage,kind));}

extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_attach(VkInstance,VkPhysicalDevice,VkDevice,VkQueue,uint32_t);
using GetInstanceFn=VkInstance(*)();
using GetPhysicalDeviceFn=VkPhysicalDevice(*)();
using GetDeviceFn=VkDevice(*)();
using GetQueueFn=VkQueue(*)();
using GetQueueFamilyFn=uint32_t(*)();
static bool attachFromGtavRuntime(){
 if(!gtavBase) dl_iterate_phdr(findGtav,nullptr);
 if(!gtavBase) return false;
 auto gi=(GetInstanceFn)(gtavBase+0x6232890);
 auto gp=(GetPhysicalDeviceFn)(gtavBase+0x623289c);
 auto gd=(GetDeviceFn)(gtavBase+0x62328a8);
 auto gq=(GetQueueFn)(gtavBase+0x62328b4);
 auto gf=(GetQueueFamilyFn)(gtavBase+0x62328c0);
 VkInstance i=gi(); VkPhysicalDevice p=gp(); VkDevice d=gd(); VkQueue q=gq(); uint32_t family=gf();
 return i&&p&&d&&q&&gtav_native_renderer_attach(i,p,d,q,family);
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_attach(VkInstance i,VkPhysicalDevice p,VkDevice d,VkQueue q,uint32_t family){
 if(!i||!p||!d||!q)return false;g.instance=i;g.physical=p;g.device=d;g.queue=q;g.family=family;load13(d);
 VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT|VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;ci.queueFamilyIndex=family;
 if(vkCreateCommandPool(d,&ci,nullptr,&g.commands)!=VK_SUCCESS)return false;
 VkDescriptorPoolSize s[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4096},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8192},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2048},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2048}};
 VkDescriptorPoolCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};di.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;di.maxSets=8192;di.poolSizeCount=4;di.pPoolSizes=s;
 return vkCreateDescriptorPool(d,&di,nullptr,&g.descriptors)==VK_SUCCESS;
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_alloc_command_buffer(VkCommandBuffer* out){if(!out||!g.device||!g.commands)return false;VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=g.commands;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;return vkAllocateCommandBuffers(g.device,&a,out)==VK_SUCCESS;}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_bind_pipeline(VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline v){if(c&&v)vkCmdBindPipeline(c,p,v);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_bind_descriptors(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet* s){if(c&&l&&n&&s)vkCmdBindDescriptorSets(c,p,l,first,n,s,0,nullptr);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_update_uniform_buffer(VkDescriptorSet s,uint32_t b,VkBuffer v,VkDeviceSize o,VkDeviceSize r){if(!g.device||!s||!v)return;VkDescriptorBufferInfo bi{v,o,r};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=s;w.dstBinding=b;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w.pBufferInfo=&bi;vkUpdateDescriptorSets(g.device,1,&w,0,nullptr);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_update_sampled_image(VkDescriptorSet s,uint32_t b,VkImageView v,VkSampler sm,VkImageLayout l){if(!g.device||!s||!v||!sm)return;VkDescriptorImageInfo ii{sm,v,l};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=s;w.dstBinding=b;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&ii;vkUpdateDescriptorSets(g.device,1,&w,0,nullptr);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_bind_vertex_buffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o){if(c&&b)vkCmdBindVertexBuffers(c,0,1,&b,&o);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_bind_index_buffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t){if(c&&b)vkCmdBindIndexBuffer(c,b,o,t);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_set_viewport(VkCommandBuffer c,float x,float y,float w,float h,float a,float z){if(c){VkViewport v{x,y,w,h,a,z};vkCmdSetViewport(c,0,1,&v);}}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_set_scissor(VkCommandBuffer c,int32_t x,int32_t y,uint32_t w,uint32_t h){if(c){VkRect2D r{{x,y},{w,h}};vkCmdSetScissor(c,0,1,&r);}}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_draw(VkCommandBuffer c,uint32_t n,uint32_t f){if(c&&n)vkCmdDraw(c,n,1,f,0);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_draw_indexed(VkCommandBuffer c,uint32_t n,uint32_t f,int32_t v){if(c&&n)vkCmdDrawIndexed(c,n,1,f,v,0);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_dispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){if(c&&x&&y&&z)vkCmdDispatch(c,x,y,z);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_begin_rendering(VkCommandBuffer c,VkRect2D a,uint32_t n,const VkRenderingAttachmentInfo* col,const VkRenderingAttachmentInfo* dep){if(!c)return;VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};r.renderArea=a;r.layerCount=1;r.colorAttachmentCount=n;r.pColorAttachments=col;r.pDepthAttachment=dep;if(pBeginRendering)pBeginRendering(c,&r);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_end_rendering(VkCommandBuffer c){if(c&&pEndRendering)pEndRendering(c);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_copy_buffer(VkCommandBuffer c,VkBuffer s,VkBuffer d,VkDeviceSize n){if(c&&s&&d&&n){VkBufferCopy r{0,0,n};vkCmdCopyBuffer(c,s,d,1,&r);}}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_copy_image(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,const VkImageCopy* r,uint32_t n){if(c&&s&&d&&r&&n)vkCmdCopyImage(c,s,sl,d,dl,n,r);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_blit_image(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,const VkImageBlit* r,uint32_t n,VkFilter f){if(c&&s&&d&&r&&n)vkCmdBlitImage(c,s,sl,d,dl,n,r,f);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_clear_color(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearColorValue* v,const VkImageSubresourceRange* r){if(c&&i&&v&&r)vkCmdClearColorImage(c,i,l,v,1,r);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_clear_depth(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearDepthStencilValue* v,const VkImageSubresourceRange* r){if(c&&i&&v&&r)vkCmdClearDepthStencilImage(c,i,l,v,1,r);}
extern "C" __attribute__((visibility("default"))) VkResult gtav_native_renderer_create_shader(const uint32_t* s,size_t n,VkShaderModule* o){if(!g.device||!s||!n||!o)return VK_ERROR_INITIALIZATION_FAILED;VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=n;ci.pCode=s;return vkCreateShaderModule(g.device,&ci,nullptr,o);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_barrier2(VkCommandBuffer c,const VkDependencyInfo* i){if(c&&i&&pBarrier2)pBarrier2(c,i);}
extern "C" __attribute__((visibility("default"))) VkResult gtav_native_renderer_submit(VkCommandBuffer c,VkFence f){if(!g.queue||!c)return VK_ERROR_INITIALIZATION_FAILED;VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=c;VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.commandBufferInfoCount=1;si.pCommandBufferInfos=&cb;return pSubmit2?pSubmit2(g.queue,1,&si,f):VK_ERROR_EXTENSION_NOT_PRESENT;}
extern "C" bool gtav_native_renderer_rage_draw(void*,uint32_t,uint32_t);
extern "C" bool gtav_native_renderer_rage_draw_indexed(void*,uint32_t,uint32_t,int32_t);
extern "C" bool gtav_native_renderer_rage_dispatch(void*,uint32_t,uint32_t,uint32_t);
extern "C" bool gtav_native_renderer_install_draw_hooks();
using OrigDraw=void(*)(void*,uint32_t,uint32_t); using OrigDrawIndexed=void(*)(void*,uint32_t,uint32_t,int32_t);
static OrigDraw origDraw{}; static OrigDrawIndexed origDrawIndexed{};
static void hookDraw(void* c,uint32_t n,uint32_t f){if(!gtav_native_renderer_rage_draw(c,n,f)&&origDraw)origDraw(c,n,f);}
static void hookDrawIndexed(void* c,uint32_t n,uint32_t f,int32_t v){if(!gtav_native_renderer_rage_draw_indexed(c,n,f,v)&&origDrawIndexed)origDrawIndexed(c,n,f,v);}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_install_draw_hooks(){
 if(!gtavBase)dl_iterate_phdr(findGtav,nullptr); if(!gtavBase)return false;
 static constexpr uint32_t expectDraw[4]={0xf9400400u,0xf9400008u,0xf9403503u,0xd61f0060u};
 static constexpr uint32_t expectDrawIndexed[4]={0xf9400400u,0xf9400008u,0xf9403104u,0xd61f0080u};
 if(std::memcmp((void*)(gtavBase+0x61d254c),expectDraw,16)!=0) return false;
 if(std::memcmp((void*)(gtavBase+0x61d253c),expectDrawIndexed,16)!=0) return false;
 uint32_t a[4]{},b[4]{}; void *ta=nullptr,*tb=nullptr;
 bool x=patchJump(gtavBase+0x61d254c,(void*)hookDraw,a,&ta); if(x)origDraw=(OrigDraw)ta;
 bool y=patchJump(gtavBase+0x61d253c,(void*)hookDrawIndexed,b,&tb); if(y)origDrawIndexed=(OrigDrawIndexed)tb;
 bool ok=x&&y; drawHooksInstalled.store(ok,std::memory_order_release); return ok;
}
__attribute__((constructor)) static void gtav_native_renderer_ctor(){
 if(!gtavBase) dl_iterate_phdr(findGtav,nullptr);
 // Runtime may not be initialized at ELF constructor time; hook install is safe,
 // while attach is retried lazily by ready()/begin_frame().
 gtav_native_renderer_install_draw_hooks();
}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_set_draw_state_provider(GtavNativeGetDrawState p){drawStateProvider=p;}
static bool getDrawState(void* ctx,GtavNativeDrawState* s){return drawStateProvider&&s&&drawStateProvider(ctx,s)&&s->command_buffer!=VK_NULL_HANDLE;}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_draw(void* ctx,uint32_t vc,uint32_t first){GtavNativeDrawState s{};if(!getDrawState(ctx,&s))return false;vkCmdDraw(s.command_buffer,vc,1,first,0);return true;}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_draw_indexed(void* ctx,uint32_t ic,uint32_t first,int32_t vo){GtavNativeDrawState s{};if(!getDrawState(ctx,&s))return false;vkCmdDrawIndexed(s.command_buffer,ic,1,first,vo,0);return true;}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_dispatch(void* ctx,uint32_t x,uint32_t y,uint32_t z){GtavNativeDrawState s{};if(!getDrawState(ctx,&s))return false;vkCmdDispatch(s.command_buffer,x,y,z);return true;}

extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_begin_frame(){if(!g.device)attachFromGtavRuntime();g.frame.fetch_add(1,std::memory_order_relaxed);}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_ready(){if(!g.device)attachFromGtavRuntime();return g.device&&g.queue&&g.commands&&g.descriptors;}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_shutdown(){if(!g.device)return;vkDeviceWaitIdle(g.device);if(g.descriptors)vkDestroyDescriptorPool(g.device,g.descriptors,nullptr);if(g.commands)vkDestroyCommandPool(g.device,g.commands,nullptr);g.descriptors=VK_NULL_HANDLE;g.commands=VK_NULL_HANDLE;g.device=VK_NULL_HANDLE;g.queue=VK_NULL_HANDLE;}
extern "C" __attribute__((visibility("default"))) const GtavNativeDispatch* gtav_native_renderer_get_dispatch(){static const GtavNativeDispatch d{2,gtav_native_renderer_ready,gtav_native_renderer_begin_frame,gtav_native_renderer_register_resource,gtav_native_renderer_resolve_resource,gtav_native_renderer_unregister_resource,gtav_native_renderer_bind_vertex_buffer,gtav_native_renderer_bind_index_buffer,gtav_native_renderer_set_viewport,gtav_native_renderer_set_scissor,gtav_native_renderer_draw,gtav_native_renderer_draw_indexed,gtav_native_renderer_dispatch,gtav_native_renderer_set_draw_state_provider,gtav_native_renderer_rage_draw,gtav_native_renderer_rage_draw_indexed,gtav_native_renderer_rage_dispatch};return &d;}
}
