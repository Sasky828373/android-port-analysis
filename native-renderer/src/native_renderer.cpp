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


// Legacy import symbols remain exported only so Android's loader can resolve libgtav.so.
// DXVK is intentionally not packaged. These guards are NOT a fake D3D implementation:
// returning fabricated COM objects would crash later and hide the real migration gap.
// Native Vulkan attaches to the engine runtime only after that runtime has valid handles.
static int32_t legacyD3DLeak(const char* name) {
  __android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE","unexpected legacy startup call: %s",name);
  return (int32_t)0x80004001u;
}
extern "C" __attribute__((visibility("default"))) int32_t CreateDXGIFactory(const void*, void** out) {
  if(out)*out=nullptr; return legacyD3DLeak("CreateDXGIFactory");
}
extern "C" __attribute__((visibility("default"))) int32_t D3D11CreateDevice(
    void*,uint32_t,void*,uint32_t,const uint32_t*,uint32_t,uint32_t,
    void** device,uint32_t* featureLevel,void** context) {
  if(device)*device=nullptr; if(featureLevel)*featureLevel=0; if(context)*context=nullptr;
  return legacyD3DLeak("D3D11CreateDevice");
}
extern "C" __attribute__((visibility("default"))) int32_t D3D11CreateDeviceAndSwapChain(
    void*,uint32_t,void*,uint32_t,const uint32_t*,uint32_t,uint32_t,const void*,
    void** swapchain,void** device,uint32_t* featureLevel,void** context) {
  if(swapchain)*swapchain=nullptr; if(device)*device=nullptr;
  if(featureLevel)*featureLevel=0; if(context)*context=nullptr;
  return legacyD3DLeak("D3D11CreateDeviceAndSwapChain");
}

namespace gtavnative {
struct Runtime { VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{}; uint32_t family{}; VkCommandPool commands{}; VkDescriptorPool descriptors{}; std::atomic<uint64_t> frame{0}; };
static Runtime g;
static std::mutex resourceMutex;
static std::unordered_map<uint64_t,GtavNativeResourceHandle> resources;
struct NativePipelineCacheEntry { VkPipeline pipeline{}; VkPipelineLayout layout{}; VkDescriptorSet descriptor{}; };
static std::mutex pipelineCacheMutex;
static std::unordered_map<uint64_t,NativePipelineCacheEntry> pipelineCache;
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
static std::atomic<uint32_t> captureBudget{256};
static std::atomic<uint32_t> attachAttempts{0};
static std::atomic<uint32_t> attachSuccesses{0};
static void capture(const char* kind,void* rage,uint64_t vk=0){
 if(!rage)return; uint32_t b=captureBudget.load(std::memory_order_relaxed);
 while(b&& !captureBudget.compare_exchange_weak(b,b-1,std::memory_order_relaxed)){}
 if(!b)return;
 __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-MAP","%s rage=%p vk=0x%llx",kind,rage,(unsigned long long)vk);
}

static RageMirrorState& mirror(void* ctx){return mirrorStates[ctx];}

struct HookTarget { uintptr_t va; void* replacement; uint32_t original[4]; void* trampoline; };
static uintptr_t gtavBase{};
 static int findGtav(struct dl_phdr_info* i,size_t,void*){ if(i&&i->dlpi_name&&std::strstr(i->dlpi_name,"libgtav.so")){gtavBase=i->dlpi_addr;return 1;} return 0; }
static void* makeTrampoline(uintptr_t target,const uint32_t original[4]){
 // Do not blindly relocate arbitrary AArch64 instructions. The copied prologue may
 // contain ADR/ADRP, literal LDR, B/BL or conditional branches whose PC-relative
 // target changes when moved into an mmap trampoline. Only accept the tiny leaf
 // veneers we have verified to be position-independent; reject everything else
 // instead of producing a latent SIGSEGV.
 auto pcRelative=[](uint32_t insn){
   if((insn&0x9f000000u)==0x10000000u) return true; // ADR/ADRP
   if((insn&0x7c000000u)==0x14000000u) return true; // B/BL
   if((insn&0xff000010u)==0x54000000u) return true; // B.cond
   if((insn&0x7e000000u)==0x34000000u) return true; // CBZ/CBNZ
   if((insn&0x7e000000u)==0x36000000u) return true; // TBZ/TBNZ
   if((insn&0x3b000000u)==0x18000000u) return true; // literal LDR/PRFM
   return false;
 };
 for(unsigned i=0;i<4;i++) if(pcRelative(original[i])){
   __android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE-MAP",
     "HOOK reject unsafe PC-relative prologue target=%p insn[%u]=0x%08x",
     (void*)target,i,original[i]);
   return nullptr;
 }
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
static uint64_t hashMix(uint64_t h,uint64_t v){h^=v+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);return h;}
static uint64_t graphicsStateKey(const RageMirrorState& s){
 uint64_t h=0xcbf29ce484222325ull;
 h=hashMix(h,(uintptr_t)s.inputLayout); h=hashMix(h,(uintptr_t)s.vs); h=hashMix(h,(uintptr_t)s.ps);
 h=hashMix(h,s.topology); h=hashMix(h,s.rtvCount); h=hashMix(h,(uintptr_t)s.dsv);
 for(unsigned i=0;i<s.rtvCount&&i<8;i++)h=hashMix(h,(uintptr_t)s.rtv[i]);
 for(unsigned i=0;i<16;i++){h=hashMix(h,(uintptr_t)s.vertexBuffers[i]);h=hashMix(h,s.strides[i]);}
 return h;
}

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
 const uint32_t attempt=attachAttempts.fetch_add(1,std::memory_order_relaxed)+1;
 if(!gtavBase) dl_iterate_phdr(findGtav,nullptr);
 if(!gtavBase){ if(attempt<=8) __android_log_print(ANDROID_LOG_WARN,"GTAV-NATIVE","ATTACH wait: libgtav base unavailable attempt=%u",attempt); return false; }
 auto gi=(GetInstanceFn)(gtavBase+0x6232890);
 auto gp=(GetPhysicalDeviceFn)(gtavBase+0x623289c);
 auto gd=(GetDeviceFn)(gtavBase+0x62328a8);
 auto gq=(GetQueueFn)(gtavBase+0x62328b4);
 auto gf=(GetQueueFamilyFn)(gtavBase+0x62328c0);
 VkInstance i=gi(); VkPhysicalDevice p=gp(); VkDevice d=gd(); VkQueue q=gq(); uint32_t family=gf();
 if(!i||!p||!d||!q){ if(attempt<=32 || (attempt%120)==0) __android_log_print(ANDROID_LOG_WARN,"GTAV-NATIVE","ATTACH wait attempt=%u i=%p p=%p d=%p q=%p family=%u",attempt,(void*)i,(void*)p,(void*)d,(void*)q,family); return false; }
 const bool ok=gtav_native_renderer_attach(i,p,d,q,family);
 if(ok && attachSuccesses.fetch_add(1,std::memory_order_relaxed)==0) __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE","ATTACH READY i=%p p=%p d=%p q=%p family=%u",(void*)i,(void*)p,(void*)d,(void*)q,family);
 return ok;
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_attach(VkInstance i,VkPhysicalDevice p,VkDevice d,VkQueue q,uint32_t family){
 if(!i||!p||!d||!q)return false;
 if(g.device==d&&g.queue==q&&g.commands&&g.descriptors)return true;
 if(g.device&&g.device!=d)return false;
 g.instance=i;g.physical=p;g.device=d;g.queue=q;g.family=family;load13(d);
 VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT|VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;ci.queueFamilyIndex=family;
 VkResult poolResult=vkCreateCommandPool(d,&ci,nullptr,&g.commands); if(poolResult!=VK_SUCCESS){__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE","vkCreateCommandPool failed=%d family=%u",(int)poolResult,family);g.commands=VK_NULL_HANDLE;return false;}
 VkDescriptorPoolSize s[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4096},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8192},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2048},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2048}};
 VkDescriptorPoolCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};di.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;di.maxSets=8192;di.poolSizeCount=4;di.pPoolSizes=s;
 VkResult descResult=vkCreateDescriptorPool(d,&di,nullptr,&g.descriptors); if(descResult!=VK_SUCCESS){__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE","vkCreateDescriptorPool failed=%d",(int)descResult);vkDestroyCommandPool(d,g.commands,nullptr);g.commands=VK_NULL_HANDLE;g.descriptors=VK_NULL_HANDLE;return false;} return true;
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
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_draw_instanced(VkCommandBuffer c,uint32_t vertexCount,uint32_t instanceCount,uint32_t firstVertex,uint32_t firstInstance){if(c&&vertexCount&&instanceCount)vkCmdDraw(c,vertexCount,instanceCount,firstVertex,firstInstance);}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_draw_indexed_instanced(VkCommandBuffer c,uint32_t indexCount,uint32_t instanceCount,uint32_t firstIndex,int32_t vertexOffset,uint32_t firstInstance){if(c&&indexCount&&instanceCount)vkCmdDrawIndexed(c,indexCount,instanceCount,firstIndex,vertexOffset,firstInstance);}
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
using OrigDispatch=void(*)(void*,uint32_t,uint32_t,uint32_t);
static OrigDraw origDraw{}; static OrigDrawIndexed origDrawIndexed{}; static OrigDispatch origDispatch{};
using OrigSubmissionBegin=VkCommandBuffer(*)(void*,bool);
static OrigSubmissionBegin origSubmissionBegin{};
static std::atomic<VkCommandBuffer> observedNativeCommandBuffer{VK_NULL_HANDLE};
struct NativeWrappedImage {
 VkImage image;
 uint32_t format;
 uint32_t pad0;
 uint64_t opaque10,opaque18,opaque20,opaque28,opaque30,opaque38,opaque40,opaque48,opaque50,opaque58,opaque60;
 uint32_t aspect;
 uint32_t pad6c;
 void* resource;
};
static_assert(offsetof(NativeWrappedImage,image)==0);
static_assert(offsetof(NativeWrappedImage,aspect)==0x68);
static_assert(offsetof(NativeWrappedImage,resource)==0x70);
using WrapTextureFn=void(*)(void*,NativeWrappedImage*);
using WrapRenderTargetFn=void(*)(void*,NativeWrappedImage*);
using AllocateDescriptorSetFn=VkDescriptorSet(*)(uint32_t,VkDescriptorSetLayout);
using GetOrCreateShaderModuleFn=VkShaderModule(*)(const uint32_t*,size_t,const char*);
static WrapTextureFn rageWrapTexture{};
static WrapRenderTargetFn rageWrapRenderTarget{};
static AllocateDescriptorSetFn rageAllocateDescriptorSet{};
static GetOrCreateShaderModuleFn rageGetOrCreateShaderModule{};
static void resolveNativeMappingFns(){
 if(!gtavBase)return;
 rageWrapRenderTarget=(WrapRenderTargetFn)(gtavBase+0x6233c00);
 rageWrapTexture=(WrapTextureFn)(gtavBase+0x6233ddc);
 rageAllocateDescriptorSet=(AllocateDescriptorSetFn)(gtavBase+0x6233f1c);
 rageGetOrCreateShaderModule=(GetOrCreateShaderModuleFn)(gtavBase+0x62334b0);
}
static void registerImageMeta(void* rage,const NativeWrappedImage& w);
static bool mapWrappedImage(void* rage,uint32_t kind,bool renderTarget){
 if(!rage)return false;
 uint64_t existing=gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)rage,kind);
 if(existing)return true;
 resolveNativeMappingFns();
 NativeWrappedImage w{};
 if(renderTarget){if(!rageWrapRenderTarget)return false;rageWrapRenderTarget(rage,&w);}
 else {if(!rageWrapTexture)return false;rageWrapTexture(rage,&w);}
 if(!w.image)return false;
 gtav_native_renderer_register_resource((uint64_t)(uintptr_t)rage,(uint64_t)(uintptr_t)w.image,kind,1);
 registerImageMeta(rage,w);
 // WrapTexture/WrapRenderTarget retain the underlying interface in +0x70.
 // This wrapper is temporary, so balance that retained COM-style reference.
 if(w.resource){
   void** vt=*reinterpret_cast<void***>(w.resource);
   if(vt){
     using ReleaseFn=uint32_t(*)(void*);
     auto release=reinterpret_cast<ReleaseFn>(vt[2]);
     if(release)release(w.resource);
   }
   w.resource=nullptr;
 }
 capture(renderTarget?"WRAP-RT":"WRAP-TEX",rage,(uint64_t)(uintptr_t)w.image);
 return true;
}

static VkCommandBuffer hookSubmissionBegin(void* self,bool external){
 // This is the first verified point where GTA is actively using its native Vulkan submission path.
 // Attach here instead of relying on a constructor-time probe or an external bridge call.
 if(!g.device) attachFromGtavRuntime();
 g.frame.fetch_add(1,std::memory_order_relaxed);
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

static std::atomic<uint64_t> nativeDraws{0},nativeIndexedDraws{0},nativeDispatches{0},fallbackDraws{0};
static void hookDraw(void* c,uint32_t n,uint32_t f){
 if(!g.device) attachFromGtavRuntime();
 if(gtav_native_renderer_rage_draw(c,n,f)){nativeDraws.fetch_add(1,std::memory_order_relaxed);return;}
 fallbackDraws.fetch_add(1,std::memory_order_relaxed);if(origDraw)origDraw(c,n,f);
}
static void hookDrawIndexed(void* c,uint32_t n,uint32_t f,int32_t v){
 if(!g.device) attachFromGtavRuntime();
 if(gtav_native_renderer_rage_draw_indexed(c,n,f,v)){nativeIndexedDraws.fetch_add(1,std::memory_order_relaxed);return;}
 fallbackDraws.fetch_add(1,std::memory_order_relaxed);if(origDrawIndexed)origDrawIndexed(c,n,f,v);
}
static void hookDispatch(void* c,uint32_t x,uint32_t y,uint32_t z){
 if(!g.device) attachFromGtavRuntime();
 if(gtav_native_renderer_rage_dispatch(c,x,y,z)){nativeDispatches.fetch_add(1,std::memory_order_relaxed);return;}
 fallbackDraws.fetch_add(1,std::memory_order_relaxed);if(origDispatch)origDispatch(c,x,y,z);
}
extern "C" __attribute__((visibility("default"))) uint64_t gtav_native_renderer_native_commands(){
 return nativeDraws.load(std::memory_order_relaxed)+nativeIndexedDraws.load(std::memory_order_relaxed)+nativeDispatches.load(std::memory_order_relaxed);
}
extern "C" __attribute__((visibility("default"))) uint64_t gtav_native_renderer_fallback_commands(){return fallbackDraws.load(std::memory_order_relaxed);}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_cutover_active(){
 return drawHooksInstalled.load(std::memory_order_acquire)&&g.device&&observedNativeCommandBuffer.load(std::memory_order_acquire)!=VK_NULL_HANDLE;
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
 uint32_t dc[4]{};void* tdc=nullptr;bool sd=patchJump(gtavBase+0x61d2da8,(void*)hookDispatch,dc,&tdc);if(sd)origDispatch=(OrigDispatch)tdc;
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
 // Do not call grVulkanRuntime accessors from the ELF constructor: GTA may not have
 // initialized the runtime singleton yet. The verified Submission::Begin hook performs
 // the first attach when the engine is actually entering native Vulkan work.
 bool attached=false;
 // Do not patch libgtav text from an ELF constructor. At this point the loader may
 // have mapped libgtav but its C++/graphics runtime is not initialized yet, and the
 // migration is not complete enough to replace the D3D11 context safely. Constructor-
 // time hooks were executing before a valid graphics bootstrap existed and are the
 // remaining deterministic startup-crash vector.
 bool hooks=false;
 __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-MAP","CTOR initial-attach=%d",attached?1:0);
 __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-MAP","HOOKS deferred=%d base=0x%llx",hooks?1:0,(unsigned long long)gtavBase);
}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_set_draw_state_provider(GtavNativeGetDrawState p){drawStateProvider=p;}
enum NativeResourceKind : uint32_t {
 NR_VERTEX_BUFFER=1, NR_INDEX_BUFFER=2, NR_VS=3, NR_PS=4, NR_CS=5,
 NR_RTV=6, NR_DSV=7, NR_CBUFFER=8, NR_SRV=9, NR_SAMPLER=10, NR_UAV=11,
 NR_GRAPHICS_PIPELINE=12, NR_PIPELINE_LAYOUT=13, NR_DESCRIPTOR_SET=14
};

extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_register_buffer(uint64_t rageBuffer,VkBuffer buffer,uint32_t kind){
 if(!rageBuffer||!buffer)return false;
 if(kind!=NR_VERTEX_BUFFER&&kind!=NR_INDEX_BUFFER&&kind!=NR_CBUFFER)return false;
 return gtav_native_renderer_register_resource(rageBuffer,(uint64_t)(uintptr_t)buffer,kind,0);
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_register_sampler(uint64_t rageSampler,VkSampler sampler){
 return rageSampler&&sampler&&gtav_native_renderer_register_resource(rageSampler,(uint64_t)(uintptr_t)sampler,NR_SAMPLER,0);
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_register_image_view(uint64_t rageResource,VkImageView view,uint32_t kind){
 if(!rageResource||!view)return false;
 if(kind!=NR_SRV&&kind!=NR_RTV&&kind!=NR_DSV&&kind!=NR_UAV)return false;
 return gtav_native_renderer_register_resource(rageResource,(uint64_t)(uintptr_t)view,kind,0);
}
extern "C" __attribute__((visibility("default"))) VkShaderModule gtav_native_renderer_create_shader_module(const uint32_t* code,size_t bytes){
 if(!g.device||!code||bytes<20||(bytes&3)||code[0]!=0x07230203u)return VK_NULL_HANDLE;
 VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=bytes;ci.pCode=code;
 VkShaderModule m=VK_NULL_HANDLE;return vkCreateShaderModule(g.device,&ci,nullptr,&m)==VK_SUCCESS?m:VK_NULL_HANDLE;
}
extern "C" __attribute__((visibility("default"))) VkDescriptorSetLayout gtav_native_renderer_create_descriptor_set_layout(const VkDescriptorSetLayoutBinding* bindings,uint32_t count){
 if(!g.device)return VK_NULL_HANDLE;VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ci.bindingCount=count;ci.pBindings=bindings;
 VkDescriptorSetLayout l=VK_NULL_HANDLE;return vkCreateDescriptorSetLayout(g.device,&ci,nullptr,&l)==VK_SUCCESS?l:VK_NULL_HANDLE;
}
extern "C" __attribute__((visibility("default"))) VkPipelineLayout gtav_native_renderer_create_pipeline_layout(const VkDescriptorSetLayout* sets,uint32_t count){
 if(!g.device)return VK_NULL_HANDLE;VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};ci.setLayoutCount=count;ci.pSetLayouts=sets;
 VkPipelineLayout l=VK_NULL_HANDLE;return vkCreatePipelineLayout(g.device,&ci,nullptr,&l)==VK_SUCCESS?l:VK_NULL_HANDLE;
}
extern "C" __attribute__((visibility("default"))) VkDescriptorSet gtav_native_renderer_alloc_descriptor_set(VkDescriptorSetLayout layout){
 if(!g.device||!g.descriptors||!layout)return VK_NULL_HANDLE;VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=g.descriptors;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;
 VkDescriptorSet s=VK_NULL_HANDLE;return vkAllocateDescriptorSets(g.device,&ai,&s)==VK_SUCCESS?s:VK_NULL_HANDLE;
}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_update_descriptors(const VkWriteDescriptorSet* writes,uint32_t count){
 if(g.device&&writes&&count)vkUpdateDescriptorSets(g.device,count,writes,0,nullptr);
}
extern "C" __attribute__((visibility("default"))) VkPipeline gtav_native_renderer_create_graphics_pipeline(const VkGraphicsPipelineCreateInfo* ci){
 if(!g.device||!ci)return VK_NULL_HANDLE;VkPipeline p=VK_NULL_HANDLE;return vkCreateGraphicsPipelines(g.device,VK_NULL_HANDLE,1,ci,nullptr,&p)==VK_SUCCESS?p:VK_NULL_HANDLE;
}
extern "C" __attribute__((visibility("default"))) VkPipeline gtav_native_renderer_create_compute_pipeline(const VkComputePipelineCreateInfo* ci){
 if(!g.device||!ci)return VK_NULL_HANDLE;VkPipeline p=VK_NULL_HANDLE;return vkCreateComputePipelines(g.device,VK_NULL_HANDLE,1,ci,nullptr,&p)==VK_SUCCESS?p:VK_NULL_HANDLE;
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_register_graphics_state(uint64_t key,VkPipeline p,VkPipelineLayout l,VkDescriptorSet d){
 if(!key||!p||!l||!d)return false;{std::lock_guard<std::mutex> g2(pipelineCacheMutex);pipelineCache[key]={p,l,d};}
 gtav_native_renderer_register_resource(key,(uint64_t)(uintptr_t)p,NR_GRAPHICS_PIPELINE,0);
 gtav_native_renderer_register_resource(key,(uint64_t)(uintptr_t)l,NR_PIPELINE_LAYOUT,0);
 gtav_native_renderer_register_resource(key,(uint64_t)(uintptr_t)d,NR_DESCRIPTOR_SET,0);return true;
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_register_compute_state(uint64_t rageShader,VkPipeline p,VkPipelineLayout l,VkDescriptorSet d){
 if(!rageShader||!p||!l||!d)return false;
 const uint64_t key=hashMix(rageShader,0x43534e4154495645ull);
 gtav_native_renderer_register_resource(key,(uint64_t)(uintptr_t)p,NR_GRAPHICS_PIPELINE,0);
 gtav_native_renderer_register_resource(key,(uint64_t)(uintptr_t)l,NR_PIPELINE_LAYOUT,0);
 gtav_native_renderer_register_resource(key,(uint64_t)(uintptr_t)d,NR_DESCRIPTOR_SET,0);
 {std::lock_guard<std::mutex> g2(pipelineCacheMutex);pipelineCache[key]={p,l,d};}
 return true;
}
struct NativeImageMeta { VkImage image{}; VkFormat format{VK_FORMAT_UNDEFINED}; VkImageAspectFlags aspect{}; };
static std::mutex imageMetaMutex;
static std::unordered_map<uint64_t,NativeImageMeta> imageMeta;
static std::unordered_map<uint64_t,VkImageView> imageViews;
static void invalidateImageResource(uint64_t rage){
 std::lock_guard<std::mutex> l(imageMetaMutex);
 auto v=imageViews.find(rage);
 if(v!=imageViews.end()){if(v->second&&g.device)vkDestroyImageView(g.device,v->second,nullptr);imageViews.erase(v);}
 imageMeta.erase(rage);
}
static void registerImageMeta(void* rage,const NativeWrappedImage& w){
 if(!rage||!w.image)return;
 const uint64_t key=(uint64_t)(uintptr_t)rage;
 NativeImageMeta m{};m.image=w.image;m.format=(VkFormat)w.format;m.aspect=(VkImageAspectFlags)w.aspect;
 std::lock_guard<std::mutex> l(imageMetaMutex);
 auto it=imageMeta.find(key);
 // If GTA reuses a RAGE object for a different native image/format, an old
 // cached view is no longer valid. Destroy it before publishing new metadata.
 if(it!=imageMeta.end()&&(it->second.image!=m.image||it->second.format!=m.format||it->second.aspect!=m.aspect)){
   auto v=imageViews.find(key);
   if(v!=imageViews.end()){if(v->second&&g.device)vkDestroyImageView(g.device,v->second,nullptr);imageViews.erase(v);}
 }
 imageMeta[key]=m;
}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_unregister_image_resource(uint64_t rage,uint32_t kind){
 if(!rage)return;
 {std::lock_guard<std::mutex> l(resourceMutex);resources.erase(resourceKey(rage,kind));}
 std::lock_guard<std::mutex> l(imageMetaMutex);
 auto v=imageViews.find(rage);if(v!=imageViews.end()){if(v->second&&g.device)vkDestroyImageView(g.device,v->second,nullptr);imageViews.erase(v);}
 imageMeta.erase(rage);
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_get_image_meta(uint64_t rageResource,VkImage* image,VkFormat* format,VkImageAspectFlags* aspect){
 std::lock_guard<std::mutex> l(imageMetaMutex);auto it=imageMeta.find(rageResource);if(it==imageMeta.end())return false;
 if(image)*image=it->second.image;if(format)*format=it->second.format;if(aspect)*aspect=it->second.aspect;return true;
}
extern "C" __attribute__((visibility("default"))) VkImageView gtav_native_renderer_create_image_view(uint64_t rageResource){
 if(!g.device||!rageResource)return VK_NULL_HANDLE;
 NativeImageMeta m{};
 {std::lock_guard<std::mutex> l(imageMetaMutex);auto v=imageViews.find(rageResource);if(v!=imageViews.end())return v->second;auto it=imageMeta.find(rageResource);if(it==imageMeta.end())return VK_NULL_HANDLE;m=it->second;}
 if(!m.image||m.format==VK_FORMAT_UNDEFINED||!m.aspect)return VK_NULL_HANDLE;
 VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=m.image;ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.format=m.format;
 ci.subresourceRange.aspectMask=m.aspect;ci.subresourceRange.baseMipLevel=0;ci.subresourceRange.levelCount=1;ci.subresourceRange.baseArrayLayer=0;ci.subresourceRange.layerCount=1;
 VkImageView view=VK_NULL_HANDLE;if(vkCreateImageView(g.device,&ci,nullptr,&view)!=VK_SUCCESS)return VK_NULL_HANDLE;
 {std::lock_guard<std::mutex> l(imageMetaMutex);auto [it,inserted]=imageViews.emplace(rageResource,view);if(!inserted){vkDestroyImageView(g.device,view,nullptr);view=it->second;}}
 // Publish the renderer-owned view into every image-backed role already known
 // for this RAGE object. This lets the native descriptor/rendering path resolve
 // an actual VkImageView instead of only the wrapped VkImage handle.
 for(uint32_t kind: {NR_RTV,NR_DSV,NR_SRV,NR_UAV}){
   if(gtav_native_renderer_resolve_resource(rageResource,kind))
     gtav_native_renderer_register_image_view(rageResource,view,kind);
 }
 return view;
}

static uint64_t resolveMapped(void* rage,uint32_t kind){
 return rage?gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)rage,kind):0;
}
static void captureMappedState(void* ctx,const RageMirrorState& m){
 auto reg=[&](const char* tag,void* rage,uint32_t kind){
   if(!rage)return;
   uint64_t vk=resolveMapped(rage,kind);
   if(vk) capture(tag,rage,vk);
 };
 for(unsigned i=0;i<16;i++) reg("MAP-VB",m.vertexBuffers[i],NR_VERTEX_BUFFER);
 reg("MAP-IB",m.indexBuffer,NR_INDEX_BUFFER);
 reg("MAP-VS",m.vs,NR_VS); reg("MAP-PS",m.ps,NR_PS); reg("MAP-CS",m.cs,NR_CS);
 for(unsigned i=0;i<m.rtvCount&&i<8;i++) reg("MAP-RTV",m.rtv[i],NR_RTV);
 reg("MAP-DSV",m.dsv,NR_DSV);
 for(unsigned i=0;i<16;i++){reg("MAP-VSCB",m.vsCB[i],NR_CBUFFER);reg("MAP-PSCB",m.psCB[i],NR_CBUFFER);reg("MAP-CSCB",m.csCB[i],NR_CBUFFER);}
 for(unsigned i=0;i<32;i++){reg("MAP-VSSRV",m.vsSRV[i],NR_SRV);reg("MAP-PSSRV",m.psSRV[i],NR_SRV);reg("MAP-CSSRV",m.csSRV[i],NR_SRV);}
 for(unsigned i=0;i<16;i++){reg("MAP-VSSAMP",m.vsSampler[i],NR_SAMPLER);reg("MAP-PSSAMP",m.psSampler[i],NR_SAMPLER);reg("MAP-CSSAMP",m.csSampler[i],NR_SAMPLER);reg("MAP-CSUAV",m.csUAV[i],NR_UAV);}
 uint64_t stateKey=graphicsStateKey(m);
 uint64_t pipe=0,layout=0,desc=0;
 {std::lock_guard<std::mutex> l(pipelineCacheMutex);auto it=pipelineCache.find(stateKey);if(it!=pipelineCache.end()){pipe=(uint64_t)(uintptr_t)it->second.pipeline;layout=(uint64_t)(uintptr_t)it->second.layout;desc=(uint64_t)(uintptr_t)it->second.descriptor;}}
 if(!pipe)pipe=gtav_native_renderer_resolve_resource(stateKey,NR_GRAPHICS_PIPELINE);
 if(!layout)layout=gtav_native_renderer_resolve_resource(stateKey,NR_PIPELINE_LAYOUT);
 if(!desc)desc=gtav_native_renderer_resolve_resource(stateKey,NR_DESCRIPTOR_SET);
 // Compatibility fallback for already-registered context keyed state.
 if(!pipe)pipe=gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)ctx,NR_GRAPHICS_PIPELINE);
 if(!layout)layout=gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)ctx,NR_PIPELINE_LAYOUT);
 if(!desc)desc=gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)ctx,NR_DESCRIPTOR_SET);
 if(pipe)capture("MAP-PIPE",ctx,pipe); if(layout)capture("MAP-LAYOUT",ctx,layout); if(desc)capture("MAP-DESC",ctx,desc);
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
 // Use GTA's own native wrappers to obtain real Vulkan images and materialize
 // reusable views for every active render target / depth target / sampled image.
 for(unsigned i=0;i<m.rtvCount&&i<8;i++){
   if(!m.rtv[i])continue;
   if(!mapWrappedImage(m.rtv[i],NR_RTV,true))return false;
   if(!gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)m.rtv[i]))return false;
 }
 if(m.dsv){
   if(!mapWrappedImage(m.dsv,NR_DSV,true))return false;
   if(!gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)m.dsv))return false;
 }
 for(unsigned i=0;i<32;i++){
   if(m.vsSRV[i]){if(!mapWrappedImage(m.vsSRV[i],NR_SRV,false))return false;if(!gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)m.vsSRV[i]))return false;}
   if(m.psSRV[i]){if(!mapWrappedImage(m.psSRV[i],NR_SRV,false))return false;if(!gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)m.psSRV[i]))return false;}
   if(m.csSRV[i]){if(!mapWrappedImage(m.csSRV[i],NR_SRV,false))return false;if(!gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)m.csSRV[i]))return false;}
 }
 captureMappedState(ctx,m);
 VkCommandBuffer cb=observedNativeCommandBuffer.load(std::memory_order_acquire);
 if(cb==VK_NULL_HANDLE) return false;
 uint64_t vb=resolveMapped(m.vertexBuffers[0],NR_VERTEX_BUFFER);
 uint64_t vs=resolveMapped(m.vs,NR_VS), ps=resolveMapped(m.ps,NR_PS);
 uint64_t rt=resolveMapped(m.rtv[0],NR_RTV);
 uint64_t stateKey=graphicsStateKey(m);
 uint64_t pipe=gtav_native_renderer_resolve_resource(stateKey,NR_GRAPHICS_PIPELINE);
 uint64_t layout=gtav_native_renderer_resolve_resource(stateKey,NR_PIPELINE_LAYOUT);
 uint64_t desc=gtav_native_renderer_resolve_resource(stateKey,NR_DESCRIPTOR_SET);
 // Do not enter native draw until every GPU object required by that draw has a real Vulkan mapping.
 // This deliberately prevents raw RAGE/D3D pointers from ever reaching vkCmd*.
 if(!vb || !vs || !ps || !rt || !pipe || !layout || !desc) return false;
 if(m.indexBuffer && !resolveMapped(m.indexBuffer,NR_INDEX_BUFFER)) return false;
 for(unsigned i=0;i<16;i++) if(m.vertexBuffers[i] && !resolveMapped(m.vertexBuffers[i],NR_VERTEX_BUFFER)) return false;
 for(unsigned i=0;i<16;i++) {
   if(m.vsCB[i]&&!resolveMapped(m.vsCB[i],NR_CBUFFER))return false;
   if(m.psCB[i]&&!resolveMapped(m.psCB[i],NR_CBUFFER))return false;
   if(m.vsSampler[i]&&!resolveMapped(m.vsSampler[i],NR_SAMPLER))return false;
   if(m.psSampler[i]&&!resolveMapped(m.psSampler[i],NR_SAMPLER))return false;
 }
 for(unsigned i=0;i<32;i++) {
   if(m.vsSRV[i]&&!resolveMapped(m.vsSRV[i],NR_SRV))return false;
   if(m.psSRV[i]&&!resolveMapped(m.psSRV[i],NR_SRV))return false;
 }
 std::memset(s,0,sizeof(*s));
 s->command_buffer=cb;
 s->pipeline=(VkPipeline)(uintptr_t)pipe;
 s->pipeline_layout=(VkPipelineLayout)(uintptr_t)layout;
 s->descriptor_set=(VkDescriptorSet)(uintptr_t)desc;
 s->vertex_buffer=(VkBuffer)(uintptr_t)vb;
 s->vertex_offset=m.offsets[0];
 uint64_t ib=resolveMapped(m.indexBuffer,NR_INDEX_BUFFER);
 if(ib){
   if(m.indexFormat==57) s->index_type=VK_INDEX_TYPE_UINT16;
   else if(m.indexFormat==42) s->index_type=VK_INDEX_TYPE_UINT32;
   else return false;
   s->index_buffer=(VkBuffer)(uintptr_t)ib;s->index_offset=m.indexOffset;
 }
 return true;
}
static bool getDrawState(void* ctx,GtavNativeDrawState* s){
 if(!s || !g.device || !g.queue) return false;
 if(drawStateProvider && drawStateProvider(ctx,s) && s->command_buffer!=VK_NULL_HANDLE) return true;
 return buildMappedDrawState(ctx,s);
}
static void applyMirroredDynamicState(void* ctx,VkCommandBuffer cb){
 RageMirrorState m{};
 {std::lock_guard<std::mutex> l(mirrorMutex);auto it=mirrorStates.find(ctx);if(it==mirrorStates.end())return;m=it->second;}
 if(m.viewportCount){
   uint32_t n=m.viewportCount>4?4:m.viewportCount;
   vkCmdSetViewport(cb,0,n,reinterpret_cast<const VkViewport*>(m.viewports));
 }
 if(m.scissorCount){
   uint32_t n=m.scissorCount>16?16:m.scissorCount;
   vkCmdSetScissor(cb,0,n,reinterpret_cast<const VkRect2D*>(m.scissors));
 }
}
static bool bindMappedGraphicsState(void* ctx,const GtavNativeDrawState& s,bool indexed){
 if(!s.command_buffer||!s.pipeline||!s.pipeline_layout||!s.descriptor_set||!s.vertex_buffer)return false;
 if(indexed&&!s.index_buffer)return false;
 vkCmdBindPipeline(s.command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,s.pipeline);
 // Bind every active mirrored vertex stream, not only slot 0. This keeps native
 // multi-stream vertex input identical to the RAGE/D3D state before a draw.
 RageMirrorState m{};
 {std::lock_guard<std::mutex> l(mirrorMutex);auto it=mirrorStates.find(ctx);if(it==mirrorStates.end())return false;m=it->second;}
 VkBuffer vbs[16]{}; VkDeviceSize offsets[16]{};
 uint32_t last=0;
 for(uint32_t i=0;i<16;i++){
   if(!m.vertexBuffers[i])continue;
   uint64_t mapped=resolveMapped(m.vertexBuffers[i],NR_VERTEX_BUFFER);
   if(!mapped)return false;
   vbs[i]=(VkBuffer)(uintptr_t)mapped; offsets[i]=m.offsets[i]; last=i+1;
 }
 if(!last)return false;
 // Vulkan requires valid handles for every element in a single bind call, so
 // preserve holes by binding contiguous active runs.
 for(uint32_t first=0;first<last;){
   while(first<last&&!vbs[first])first++;
   if(first>=last)break;
   uint32_t end=first+1;while(end<last&&vbs[end])end++;
   vkCmdBindVertexBuffers(s.command_buffer,first,end-first,&vbs[first],&offsets[first]);
   first=end;
 }
 if(indexed)vkCmdBindIndexBuffer(s.command_buffer,s.index_buffer,s.index_offset,s.index_type);
 vkCmdBindDescriptorSets(s.command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,s.pipeline_layout,0,1,&s.descriptor_set,0,nullptr);
 applyMirroredDynamicState(ctx,s.command_buffer);
 return true;
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_draw(void* ctx,uint32_t vc,uint32_t first){GtavNativeDrawState s{};if(!getDrawState(ctx,&s)||!bindMappedGraphicsState(ctx,s,false))return false;applyMirroredDynamicState(ctx,s.command_buffer);vkCmdDraw(s.command_buffer,vc,1,first,0);return true;}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_draw_indexed(void* ctx,uint32_t ic,uint32_t first,int32_t vo){GtavNativeDrawState s{};if(!getDrawState(ctx,&s)||!bindMappedGraphicsState(ctx,s,true))return false;applyMirroredDynamicState(ctx,s.command_buffer);vkCmdDrawIndexed(s.command_buffer,ic,1,first,vo,0);return true;}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_dispatch(void* ctx,uint32_t x,uint32_t y,uint32_t z){
 if(!ctx||!x||!y||!z||!g.device)return false;
 RageMirrorState m{};{std::lock_guard<std::mutex> l(mirrorMutex);auto it=mirrorStates.find(ctx);if(it==mirrorStates.end())return false;m=it->second;}
 if(!m.cs)return false;
 VkCommandBuffer cb=observedNativeCommandBuffer.load(std::memory_order_acquire);if(!cb)return false;
 uint64_t cs=resolveMapped(m.cs,NR_CS);if(!cs)return false;
 uint64_t key=hashMix((uint64_t)(uintptr_t)m.cs,0x43534e4154495645ull);
 uint64_t pipe=0,layout=0,desc=0;
 {std::lock_guard<std::mutex> l(pipelineCacheMutex);auto it=pipelineCache.find(key);if(it!=pipelineCache.end()){pipe=(uint64_t)(uintptr_t)it->second.pipeline;layout=(uint64_t)(uintptr_t)it->second.layout;desc=(uint64_t)(uintptr_t)it->second.descriptor;}}
 if(!pipe)pipe=gtav_native_renderer_resolve_resource(key,NR_GRAPHICS_PIPELINE);
 if(!layout)layout=gtav_native_renderer_resolve_resource(key,NR_PIPELINE_LAYOUT);
 if(!desc)desc=gtav_native_renderer_resolve_resource(key,NR_DESCRIPTOR_SET);
 if(!pipe||!layout||!desc)return false;
 for(unsigned i=0;i<16;i++){
  if(m.csCB[i]&&!resolveMapped(m.csCB[i],NR_CBUFFER))return false;
  if(m.csSampler[i]&&!resolveMapped(m.csSampler[i],NR_SAMPLER))return false;
  if(m.csUAV[i]&&!resolveMapped(m.csUAV[i],NR_UAV))return false;
 }
 for(unsigned i=0;i<32;i++)if(m.csSRV[i]){
  if(!mapWrappedImage(m.csSRV[i],NR_SRV,false))return false;
  if(!gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)m.csSRV[i]))return false;
 }
 vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,(VkPipeline)(uintptr_t)pipe);
 vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,(VkPipelineLayout)(uintptr_t)layout,0,1,(VkDescriptorSet*)&desc,0,nullptr);
 vkCmdDispatch(cb,x,y,z);return true;
}

extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_begin_frame(){if(!g.device)attachFromGtavRuntime();g.frame.fetch_add(1,std::memory_order_relaxed);}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_ready(){
 if(!g.device)attachFromGtavRuntime();
 return g.device&&g.queue&&g.commands&&g.descriptors&&drawHooksInstalled.load(std::memory_order_acquire);
}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_shutdown(){
 if(!g.device)return;
 vkDeviceWaitIdle(g.device);
 // Image views are owned by this renderer even though the VkImages are GTA-owned.
 // Destroy views before dropping the VkDevice and clear all cached metadata.
 {std::lock_guard<std::mutex> l(imageMetaMutex);
  for(auto& it:imageViews)if(it.second)vkDestroyImageView(g.device,it.second,nullptr);
  imageViews.clear();imageMeta.clear();}
 {std::lock_guard<std::mutex> l(pipelineCacheMutex);pipelineCache.clear();}
 if(g.descriptors)vkDestroyDescriptorPool(g.device,g.descriptors,nullptr);
 if(g.commands)vkDestroyCommandPool(g.device,g.commands,nullptr);
 g.descriptors=VK_NULL_HANDLE;g.commands=VK_NULL_HANDLE;g.device=VK_NULL_HANDLE;g.queue=VK_NULL_HANDLE;
}
extern "C" __attribute__((visibility("default"))) const GtavNativeDispatch* gtav_native_renderer_get_dispatch(){static const GtavNativeDispatch d{2,gtav_native_renderer_ready,gtav_native_renderer_begin_frame,gtav_native_renderer_register_resource,gtav_native_renderer_resolve_resource,gtav_native_renderer_unregister_resource,gtav_native_renderer_bind_vertex_buffer,gtav_native_renderer_bind_index_buffer,gtav_native_renderer_set_viewport,gtav_native_renderer_set_scissor,gtav_native_renderer_draw,gtav_native_renderer_draw_indexed,gtav_native_renderer_dispatch,gtav_native_renderer_set_draw_state_provider,gtav_native_renderer_rage_draw,gtav_native_renderer_rage_draw_indexed,gtav_native_renderer_rage_dispatch};return &d;}
}
