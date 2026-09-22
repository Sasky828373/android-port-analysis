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
#include <android/log.h>

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
struct RageMirrorState {
 void* inputLayout{};
 void* vertexBuffers[16]{};
 uint32_t strides[16]{};
 uint32_t offsets[16]{};
 void* indexBuffer{};
 uint32_t indexFormat{};
 uint32_t indexOffset{};
 uint32_t topology{};
 void* vs{}; void* ps{}; void* cs{};
 uint32_t viewportCount{}; uint8_t viewports[256]{};
 uint32_t scissorCount{}; uint8_t scissors[256]{};
 void* vsCB[16]{}; void* psCB[16]{}; void* csCB[16]{};
 void* vsSRV[32]{}; void* psSRV[32]{}; void* csSRV[32]{};
 void* vsSampler[16]{}; void* psSampler[16]{}; void* csSampler[16]{};
 void* csUAV[16]{};
 void* rtv[8]{}; uint32_t rtvCount{}; void* dsv{};
};
static std::mutex mirrorMutex;
static std::unordered_map<void*,RageMirrorState> mirrorStates;
static std::atomic<uint32_t> captureBudget{4096};
static void capture(const char* kind,void* rage,uint64_t vk=0){
 if(!rage)return; uint32_t b=captureBudget.load(std::memory_order_relaxed);
 while(b&& !captureBudget.compare_exchange_weak(b,b-1,std::memory_order_relaxed)){}
 if(!b)return;
 __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-MAP","%s rage=%p vk=0x%llx",kind,rage,(unsigned long long)vk);
}

static RageMirrorState& mirror(void* ctx){return mirrorStates[ctx];}

struct HookTarget { uintptr_t va; void* replacement; uint32_t original[4]; void* trampoline; };
static uintptr_t gtavBase{};
static constexpr uintptr_t kVulkanRuntimeInitVa=0x618caf8;
static constexpr uint32_t kVulkanRuntimeInitExpected[4]={0x942906c6u,0xd0006d20u,0x90fe1322u,0x910bf042u};
static std::atomic<bool> vulkanInitGateMatched{false};
using OrigVulkanRuntimeInit=bool(*)(void*);
static OrigVulkanRuntimeInit origVulkanRuntimeInit{};
static std::atomic<bool> vulkanInitHookInstalled{false};


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
 if(!pBeginRendering)pBeginRendering=reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(d,"vkCmdBeginRenderingKHR"));
 pEndRendering=reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(d,"vkCmdEndRendering"));
 if(!pEndRendering)pEndRendering=reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(d,"vkCmdEndRenderingKHR"));
 pBarrier2=reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(d,"vkCmdPipelineBarrier2"));
 if(!pBarrier2)pBarrier2=reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(d,"vkCmdPipelineBarrier2KHR"));
 pSubmit2=reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(d,"vkQueueSubmit2"));
 if(!pSubmit2)pSubmit2=reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(d,"vkQueueSubmit2KHR"));
}
static uint64_t resourceKey(uint64_t rage,uint32_t kind){ return (rage<<3)^uint64_t(kind); }

extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_register_resource(uint64_t rage,uint64_t vk,uint32_t kind,uint32_t generation){if(!rage||!vk||!kind)return false;std::lock_guard<std::mutex> l(resourceMutex);resources[resourceKey(rage,kind)]={rage,vk,kind,generation};capture("REGISTER",(void*)(uintptr_t)rage,vk);return true;}
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
 if(!i||!p||!d||!q)return false;
 if(g.device==d&&g.queue==q&&g.commands&&g.descriptors)return true;
 if(g.device&&g.device!=d)return false;
 g.instance=i;g.physical=p;g.device=d;g.queue=q;g.family=family;load13(d);
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
using OrigIASetInputLayout=void(*)(void*,void*);
using OrigIASetVertexBuffers=void(*)(void*,uint32_t,uint32_t,void* const*,const uint32_t*,const uint32_t*);
using OrigIASetIndexBuffer=void(*)(void*,void*,uint32_t,uint32_t);
static OrigIASetInputLayout origIASetInputLayout{};
static OrigIASetVertexBuffers origIASetVertexBuffers{};
static OrigIASetIndexBuffer origIASetIndexBuffer{};
using OrigRSSetViewports=void(*)(void*,uint32_t,const void*);
using OrigRSSetScissorRects=void(*)(void*,uint32_t,const void*);
using OrigIASetPrimitiveTopology=void(*)(void*,uint32_t);
using OrigPSSetShader=void(*)(void*,void*,void* const*,uint32_t);
using OrigVSSetShader=void(*)(void*,void*,void* const*,uint32_t);
using OrigCSSetShader=void(*)(void*,void*,void* const*,uint32_t);
static OrigRSSetViewports origRSSetViewports{};
static OrigRSSetScissorRects origRSSetScissorRects{};
static OrigIASetPrimitiveTopology origIASetPrimitiveTopology{};
static OrigPSSetShader origPSSetShader{}; static OrigVSSetShader origVSSetShader{}; static OrigCSSetShader origCSSetShader{};

static void hookIASetInputLayout(void* ctx,void* layout){
 {std::lock_guard<std::mutex> l(mirrorMutex);mirror(ctx).inputLayout=layout;}
 if(origIASetInputLayout)origIASetInputLayout(ctx,layout);
}
static void hookIASetVertexBuffers(void* ctx,uint32_t first,uint32_t count,void* const* bufs,const uint32_t* strides,const uint32_t* offsets){
 {std::lock_guard<std::mutex> l(mirrorMutex);auto& s=mirror(ctx);for(uint32_t i=0;i<count&&first+i<16;i++){s.vertexBuffers[first+i]=bufs?bufs[i]:nullptr;s.strides[first+i]=strides?strides[i]:0;s.offsets[first+i]=offsets?offsets[i]:0;if(s.vertexBuffers[first+i])capture("VB",s.vertexBuffers[first+i]);}}
 if(origIASetVertexBuffers)origIASetVertexBuffers(ctx,first,count,bufs,strides,offsets);
}
static void hookIASetIndexBuffer(void* ctx,void* buf,uint32_t format,uint32_t offset){
 {std::lock_guard<std::mutex> l(mirrorMutex);auto& s=mirror(ctx);s.indexBuffer=buf;s.indexFormat=format;s.indexOffset=offset;capture("IB",buf);}
 if(origIASetIndexBuffer)origIASetIndexBuffer(ctx,buf,format,offset);
}
static void hookIASetPrimitiveTopology(void* ctx,uint32_t t){{std::lock_guard<std::mutex> l(mirrorMutex);mirror(ctx).topology=t;}if(origIASetPrimitiveTopology)origIASetPrimitiveTopology(ctx,t);}
static void hookVSSetShader(void* ctx,void* sh,void* const* ci,uint32_t n){{std::lock_guard<std::mutex> l(mirrorMutex);mirror(ctx).vs=sh;capture("VS",sh);}if(origVSSetShader)origVSSetShader(ctx,sh,ci,n);}
static void hookPSSetShader(void* ctx,void* sh,void* const* ci,uint32_t n){{std::lock_guard<std::mutex> l(mirrorMutex);mirror(ctx).ps=sh;capture("PS",sh);}if(origPSSetShader)origPSSetShader(ctx,sh,ci,n);}
static void hookCSSetShader(void* ctx,void* sh,void* const* ci,uint32_t n){{std::lock_guard<std::mutex> l(mirrorMutex);mirror(ctx).cs=sh;capture("CS",sh);}if(origCSSetShader)origCSSetShader(ctx,sh,ci,n);}
static void hookRSSetViewports(void* ctx,uint32_t n,const void* p){{std::lock_guard<std::mutex> l(mirrorMutex);auto& s=mirror(ctx);s.viewportCount=n>4?4:n;if(p)std::memcpy(s.viewports,p,s.viewportCount*24);}if(origRSSetViewports)origRSSetViewports(ctx,n,p);}
static void hookRSSetScissorRects(void* ctx,uint32_t n,const void* p){{std::lock_guard<std::mutex> l(mirrorMutex);auto& s=mirror(ctx);s.scissorCount=n>16?16:n;if(p)std::memcpy(s.scissors,p,s.scissorCount*16);}if(origRSSetScissorRects)origRSSetScissorRects(ctx,n,p);}
using OrigSetObjects=void(*)(void*,uint32_t,uint32_t,void* const*);
using OrigSetUAV=void(*)(void*,uint32_t,uint32_t,void* const*,const uint32_t*);
using OrigOMRT=void(*)(void*,uint32_t,void* const*,void*);
static OrigSetObjects oVSCB{},oPSCB{},oCSCB{},oVSSRV{},oPSSRV{},oCSSRV{},oVSSamp{},oPSSamp{},oCSSamp{};
static OrigSetUAV oCSUAV{}; static OrigOMRT oOMRT{};
static void mirrorObjs(void** dst,uint32_t cap,uint32_t first,uint32_t n,void* const* src){for(uint32_t i=0;i<n&&first+i<cap;i++)dst[first+i]=src?src[i]:nullptr;}
#define OBJHOOK(name,field,cap,orig) static void name(void* c,uint32_t f,uint32_t n,void* const* v){{std::lock_guard<std::mutex> l(mirrorMutex);mirrorObjs(mirror(c).field,cap,f,n,v);}if(orig)orig(c,f,n,v);}
OBJHOOK(hVSCB,vsCB,16,oVSCB) OBJHOOK(hPSCB,psCB,16,oPSCB) OBJHOOK(hCSCB,csCB,16,oCSCB)
OBJHOOK(hVSSRV,vsSRV,32,oVSSRV) OBJHOOK(hPSSRV,psSRV,32,oPSSRV) OBJHOOK(hCSSRV,csSRV,32,oCSSRV)
OBJHOOK(hVSSamp,vsSampler,16,oVSSamp) OBJHOOK(hPSSamp,psSampler,16,oPSSamp) OBJHOOK(hCSSamp,csSampler,16,oCSSamp)
#undef OBJHOOK
static void hCSUAV(void* c,uint32_t f,uint32_t n,void* const* v,const uint32_t* counts){{std::lock_guard<std::mutex> l(mirrorMutex);mirrorObjs(mirror(c).csUAV,16,f,n,v);}if(oCSUAV)oCSUAV(c,f,n,v,counts);}
static void hOMRT(void* c,uint32_t n,void* const* r,void* d){{std::lock_guard<std::mutex> l(mirrorMutex);auto& s=mirror(c);s.rtvCount=n>8?8:n;mirrorObjs(s.rtv,8,0,s.rtvCount,r);s.dsv=d;for(uint32_t i=0;i<s.rtvCount;i++)capture("RTV",s.rtv[i]);capture("DSV",d);}if(oOMRT)oOMRT(c,n,r,d);}


using OrigDraw=void(*)(void*,uint32_t,uint32_t); using OrigDrawIndexed=void(*)(void*,uint32_t,uint32_t,int32_t);
static OrigDraw origDraw{}; static OrigDrawIndexed origDrawIndexed{};
using OrigSubmissionBegin=VkCommandBuffer(*)(void*,bool);
static OrigSubmissionBegin origSubmissionBegin{};
static std::atomic<VkCommandBuffer> observedNativeCommandBuffer{VK_NULL_HANDLE};
static VkCommandBuffer hookSubmissionBegin(void* self,bool external){
 VkCommandBuffer cb=origSubmissionBegin?origSubmissionBegin(self,external):VK_NULL_HANDLE;
 observedNativeCommandBuffer.store(cb,std::memory_order_release);
 static std::atomic<bool> once{false};
 if(cb!=VK_NULL_HANDLE && !once.exchange(true,std::memory_order_acq_rel))
   __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-MAP","NATIVE-CB connected=%p",(void*)cb);
 return cb;
}
extern "C" __attribute__((visibility("default"))) VkCommandBuffer gtav_native_renderer_observed_command_buffer(){
 return observedNativeCommandBuffer.load(std::memory_order_acquire);
}

static void hookDraw(void* c,uint32_t n,uint32_t f){if(!gtav_native_renderer_rage_draw(c,n,f)&&origDraw)origDraw(c,n,f);}
static void hookDrawIndexed(void* c,uint32_t n,uint32_t f,int32_t v){if(!gtav_native_renderer_rage_draw_indexed(c,n,f,v)&&origDrawIndexed)origDrawIndexed(c,n,f,v);}
static bool hookVulkanRuntimeInit(void* self){
 bool ok=origVulkanRuntimeInit?origVulkanRuntimeInit(self):false;
 __android_log_print(ok?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"GTAV-NATIVE-MAP","VULKAN-INIT returned=%d",ok?1:0);
 if(ok){
   bool attached=attachFromGtavRuntime();
   __android_log_print(attached?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"GTAV-NATIVE-MAP","VULKAN-ATTACH ready=%d",attached?1:0);
 }
 return ok;
}
static bool installVulkanRuntimeInitHook(){
 if(vulkanInitHookInstalled.load(std::memory_order_acquire)) return true;
 if(!gtavBase) dl_iterate_phdr(findGtav,nullptr);
 if(!gtavBase) return false;
 uintptr_t target=gtavBase+kVulkanRuntimeInitVa;
 if(std::memcmp((void*)target,kVulkanRuntimeInitExpected,16)!=0) return false;
 uint32_t old[4]{}; void* tramp=nullptr;
 if(!patchJump(target,(void*)hookVulkanRuntimeInit,old,&tramp)) return false;
 origVulkanRuntimeInit=(OrigVulkanRuntimeInit)tramp;
 vulkanInitHookInstalled.store(true,std::memory_order_release);
 return true;
}

extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_install_draw_hooks(){
 if(!gtavBase)dl_iterate_phdr(findGtav,nullptr); if(!gtavBase){__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE-MAP","HOOKS libgtav-not-found");return false;}
 static constexpr uint32_t expectDraw[4]={0xf9400400u,0xf9400008u,0xf9403503u,0xd61f0060u};
 static constexpr uint32_t expectDrawIndexed[4]={0xf9400400u,0xf9400008u,0xf9403104u,0xd61f0080u};
 if(std::memcmp((void*)(gtavBase+0x61d254c),expectDraw,16)!=0) return false;
 if(std::memcmp((void*)(gtavBase+0x61d253c),expectDrawIndexed,16)!=0) return false;
 uint32_t a[4]{},b[4]{}; void *ta=nullptr,*tb=nullptr;
 bool x=patchJump(gtavBase+0x61d254c,(void*)hookDraw,a,&ta); if(x)origDraw=(OrigDraw)ta;
 bool y=patchJump(gtavBase+0x61d253c,(void*)hookDrawIndexed,b,&tb); if(y)origDrawIndexed=(OrigDrawIndexed)tb;
 static constexpr uint32_t expectSubmissionBegin[4]={0xd10283ffu,0xa9047bfdu,0xf9002bfbu,0xa90667fau};
 bool z=false; void* ts=nullptr; uint32_t s[4]{};
 if(std::memcmp((void*)(gtavBase+0x623526c),expectSubmissionBegin,16)==0){
   z=patchJump(gtavBase+0x623526c,(void*)hookSubmissionBegin,s,&ts);
   if(z)origSubmissionBegin=(OrigSubmissionBegin)ts;
 }
 uint32_t il[4]{},vb[4]{},ib[4]{}; void *til=nullptr,*tvb=nullptr,*tib=nullptr;
 bool sil=patchJump(gtavBase+0x61d2654,(void*)hookIASetInputLayout,il,&til); if(sil)origIASetInputLayout=(OrigIASetInputLayout)til;
 bool svb=patchJump(gtavBase+0x61d2698,(void*)hookIASetVertexBuffers,vb,&tvb); if(svb)origIASetVertexBuffers=(OrigIASetVertexBuffers)tvb;
 bool sib=patchJump(gtavBase+0x61d27fc,(void*)hookIASetIndexBuffer,ib,&tib); if(sib)origIASetIndexBuffer=(OrigIASetIndexBuffer)tib;
 auto install=[&](uintptr_t va,void* hook,void** orig){uint32_t o[4]{};void* t=nullptr;bool r=patchJump(gtavBase+va,hook,o,&t);if(r)*orig=t;return r;};
 bool stop=install(0x61d2968,(void*)hookIASetPrimitiveTopology,(void**)&origIASetPrimitiveTopology);
 bool svs=install(0x61d252c,(void*)hookVSSetShader,(void**)&origVSSetShader);
 bool sps=install(0x61d2494,(void*)hookPSSetShader,(void**)&origPSSetShader);
 bool scs=install(0x61d3134,(void*)hookCSSetShader,(void**)&origCSSetShader);
 bool svp=install(0x61d2e0c,(void*)hookRSSetViewports,(void**)&origRSSetViewports);
 bool ssr=install(0x61d2e1c,(void*)hookRSSetScissorRects,(void**)&origRSSetScissorRects);
 bool extra=true;
 extra&=install(0x61d23ac,(void*)hVSCB,(void**)&oVSCB); extra&=install(0x61d257c,(void*)hPSCB,(void**)&oPSCB); extra&=install(0x61d3154,(void*)hCSCB,(void**)&oCSCB);
 extra&=install(0x61d29ac,(void*)hVSSRV,(void**)&oVSSRV); extra&=install(0x61d2484,(void*)hPSSRV,(void**)&oPSSRV); extra&=install(0x61d3114,(void*)hCSSRV,(void**)&oCSSRV);
 extra&=install(0x61d2a60,(void*)hVSSamp,(void**)&oVSSamp); extra&=install(0x61d24a4,(void*)hPSSamp,(void**)&oPSSamp); extra&=install(0x61d3144,(void*)hCSSamp,(void**)&oCSSamp);
 extra&=install(0x61d3124,(void*)hCSUAV,(void**)&oCSUAV); extra&=install(0x61d2b48,(void*)hOMRT,(void**)&oOMRT);
 bool ok=x&&y&&z&&sil&&svb&&sib&&stop&&svs&&sps&&scs&&svp&&ssr&&extra; drawHooksInstalled.store(ok,std::memory_order_release); return ok;
}
__attribute__((constructor)) static void gtav_native_renderer_ctor(){
 __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-MAP","LOAD native_renderer pid=%d",(int)getpid());
 if(!gtavBase) dl_iterate_phdr(findGtav,nullptr);
 if(gtavBase){
   bool match=std::memcmp((void*)(gtavBase+kVulkanRuntimeInitVa),kVulkanRuntimeInitExpected,16)==0;
   vulkanInitGateMatched.store(match,std::memory_order_release);
   __android_log_print(match?ANDROID_LOG_INFO:ANDROID_LOG_ERROR,"GTAV-NATIVE-MAP",
     "VULKAN-INIT-GATE match=%d base=0x%llx",match?1:0,(unsigned long long)gtavBase);
 }
 if(vulkanInitGateMatched.load(std::memory_order_acquire)){
   bool vih=installVulkanRuntimeInitHook();
   __android_log_print(vih?ANDROID_LOG_INFO:ANDROID_LOG_ERROR,"GTAV-NATIVE-MAP","VULKAN-INIT-HOOK installed=%d",vih?1:0);
 }
 bool hooks=gtav_native_renderer_install_draw_hooks();
 __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-MAP","HOOKS installed=%d base=0x%llx",hooks?1:0,(unsigned long long)gtavBase);
}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_set_draw_state_provider(GtavNativeGetDrawState p){drawStateProvider=p;}
enum NativeResourceKind : uint32_t {
 NR_VERTEX_BUFFER=1, NR_INDEX_BUFFER=2, NR_VS=3, NR_PS=4, NR_CS=5,
 NR_RTV=6, NR_DSV=7, NR_CBUFFER=8, NR_SRV=9, NR_SAMPLER=10, NR_UAV=11,
 NR_GRAPHICS_PIPELINE=12, NR_PIPELINE_LAYOUT=13, NR_DESCRIPTOR_SET=14
};
static uint64_t resolveMapped(void* rage,uint32_t kind){
 return rage?gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)rage,kind):0;
}
static bool mirroredStateComplete(void* ctx){
 std::lock_guard<std::mutex> l(mirrorMutex);
 auto it=mirrorStates.find(ctx); if(it==mirrorStates.end()) return false;
 const auto& m=it->second;
 return m.inputLayout && m.vertexBuffers[0] && m.vs && m.ps && m.rtvCount>0 && m.rtv[0];
}
static bool buildMappedDrawState(void* ctx,GtavNativeDrawState* s){
 if(!s || !g.device || !g.queue) return false;
 RageMirrorState m{};
 { std::lock_guard<std::mutex> l(mirrorMutex);
   auto it=mirrorStates.find(ctx); if(it==mirrorStates.end()) return false; m=it->second; }
 if(!m.inputLayout || !m.vertexBuffers[0] || !m.vs || !m.ps || !m.rtvCount || !m.rtv[0]) return false;
 VkCommandBuffer cb=observedNativeCommandBuffer.load(std::memory_order_acquire);
 if(cb==VK_NULL_HANDLE) return false;
 uint64_t vb=resolveMapped(m.vertexBuffers[0],NR_VERTEX_BUFFER);
 uint64_t vs=resolveMapped(m.vs,NR_VS), ps=resolveMapped(m.ps,NR_PS);
 uint64_t rt=resolveMapped(m.rtv[0],NR_RTV);
 uint64_t pipe=gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)ctx,NR_GRAPHICS_PIPELINE);
 uint64_t layout=gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)ctx,NR_PIPELINE_LAYOUT);
 uint64_t desc=gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)ctx,NR_DESCRIPTOR_SET);
 if(!vb || !vs || !ps || !rt || !pipe || !layout || !desc) return false;
 std::memset(s,0,sizeof(*s)); s->command_buffer=cb;
 return true;
}
static bool getDrawState(void* ctx,GtavNativeDrawState* s){
 if(!s || !g.device || !g.queue) return false;
 if(drawStateProvider && drawStateProvider(ctx,s) && s->command_buffer!=VK_NULL_HANDLE) return true;
 return buildMappedDrawState(ctx,s);
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_draw(void* ctx,uint32_t vc,uint32_t first){GtavNativeDrawState s{};if(!getDrawState(ctx,&s))return false;vkCmdDraw(s.command_buffer,vc,1,first,0);return true;}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_draw_indexed(void* ctx,uint32_t ic,uint32_t first,int32_t vo){GtavNativeDrawState s{};if(!getDrawState(ctx,&s))return false;vkCmdDrawIndexed(s.command_buffer,ic,1,first,vo,0);return true;}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_dispatch(void* ctx,uint32_t x,uint32_t y,uint32_t z){GtavNativeDrawState s{};if(!getDrawState(ctx,&s))return false;vkCmdDispatch(s.command_buffer,x,y,z);return true;}

extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_begin_frame(){if(!g.device)attachFromGtavRuntime();g.frame.fetch_add(1,std::memory_order_relaxed);}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_ready(){if(!g.device)attachFromGtavRuntime();return g.device&&g.queue&&g.commands&&g.descriptors;}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_shutdown(){if(!g.device)return;vkDeviceWaitIdle(g.device);if(g.descriptors)vkDestroyDescriptorPool(g.device,g.descriptors,nullptr);if(g.commands)vkDestroyCommandPool(g.device,g.commands,nullptr);g.descriptors=VK_NULL_HANDLE;g.commands=VK_NULL_HANDLE;g.device=VK_NULL_HANDLE;g.queue=VK_NULL_HANDLE;}
extern "C" __attribute__((visibility("default"))) const GtavNativeDispatch* gtav_native_renderer_get_dispatch(){static const GtavNativeDispatch d{2,gtav_native_renderer_ready,gtav_native_renderer_begin_frame,gtav_native_renderer_register_resource,gtav_native_renderer_resolve_resource,gtav_native_renderer_unregister_resource,gtav_native_renderer_bind_vertex_buffer,gtav_native_renderer_bind_index_buffer,gtav_native_renderer_set_viewport,gtav_native_renderer_set_scissor,gtav_native_renderer_draw,gtav_native_renderer_draw_indexed,gtav_native_renderer_dispatch,gtav_native_renderer_set_draw_state_provider,gtav_native_renderer_rage_draw,gtav_native_renderer_rage_draw_indexed,gtav_native_renderer_rage_dispatch};return &d;}
}
