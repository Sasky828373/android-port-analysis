// Build trigger: present-bridge diagnostics
#include <vulkan/vulkan.h>
#if __has_include("dxbc/dxbc_api.h")
#define GTAV_HAVE_DXBC_SPIRV 1
#include "dxbc/dxbc_api.h"
#include "spirv/spirv_builder.h"
#include "spirv/spirv_mapping.h"
#else
#define GTAV_HAVE_DXBC_SPIRV 0
#endif
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <link.h>
#include "native_dispatch.h"
#include <dlfcn.h>
#include <android/log.h>
#include <android/native_window.h>

// SDL is already shipped by the APK and libgtav uses its Vulkan window path.
// Resolve these dynamically so the renderer does not acquire a hard SDL link dependency.
struct SDL_Window;
using SDLVulkanCreateSurfaceFn=int(*)(SDL_Window*,VkInstance,VkSurfaceKHR*);
using SDLVulkanGetDrawableSizeFn=void(*)(SDL_Window*,int*,int*);
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <ucontext.h>
#include <cstdio>
#include <cstdlib>



extern "C" bool gtav_native_renderer_install_early_vulkan_hook();
extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance,const char*);
namespace gtavnative { static bool installGtavDlsymCallHook(); }
namespace gtavdiag {
static const char* kPath="/storage/emulated/0/Games/GTAV/Config/gtav-native-crash.txt";
static std::atomic<uint32_t> seq{0};
static std::atomic<const char*> last{"native-renderer-loaded"};
static thread_local const char* tlsLast="native-renderer-loaded";
static void ensureDir(){ mkdir("/storage/emulated/0/Games",0775); mkdir("/storage/emulated/0/Games/GTAV",0775); mkdir("/storage/emulated/0/Games/GTAV/Config",0775); }
static void append(const char* s){ ensureDir(); int fd=open(kPath,O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC,0664); if(fd>=0){write(fd,s,strlen(s));close(fd);} }
static void checkpoint(const char* name,const char* detail=nullptr){
 last.store(name,std::memory_order_relaxed); tlsLast=name; char b[768];
 int n=snprintf(b,sizeof(b),"SEQ=%u CHECKPOINT=%s tid=%ld%s%s\n",seq.fetch_add(1)+1,name,(long)syscall(SYS_gettid),detail?" ":"",detail?detail:"");
 if(n>0) append(b); __android_log_print(ANDROID_LOG_INFO,"GTAV-DIAG","%s%s%s",name,detail?" ":"",detail?detail:"");
}
static char* puthex(char* p,uint64_t v){static const char h[]="0123456789abcdef";*p++='0';*p++='x';bool s=false;for(int i=15;i>=0;--i){unsigned d=(v>>(i*4))&15;if(d||s||i==0){*p++=h[d];s=true;}}return p;}
static char* putdec(char* p,unsigned v){char t[16];int n=0;do{t[n++]=char('0'+v%10);v/=10;}while(v);while(n)*p++=t[--n];return p;}
static void crashHandler(int sig,siginfo_t* si,void* ctx){
 char b[512],*p=b; const char* a="\n=== GTAV NATIVE CRASH ===\nsignal=";memcpy(p,a,strlen(a));p+=strlen(a);p=putdec(p,(unsigned)sig);
 const char* q=" fault=";memcpy(p,q,strlen(q));p+=strlen(q);p=puthex(p,(uint64_t)(uintptr_t)(si?si->si_addr:nullptr));
#if defined(__aarch64__)
 ucontext_t* uc=(ucontext_t*)ctx;const char* r=" pc=";memcpy(p,r,strlen(r));p+=strlen(r);p=puthex(p,(uint64_t)uc->uc_mcontext.pc);
 const char* s=" sp=";memcpy(p,s,strlen(s));p+=strlen(s);p=puthex(p,(uint64_t)uc->uc_mcontext.sp);
 const char* l=" lr=";memcpy(p,l,strlen(l));p+=strlen(l);p=puthex(p,(uint64_t)uc->uc_mcontext.regs[30]);
#endif
 const char* ti=" tid=";memcpy(p,ti,strlen(ti));p+=strlen(ti);p=putdec(p,(unsigned)syscall(SYS_gettid));
 char tname[17]{};syscall(SYS_prctl,PR_GET_NAME,tname,0,0,0);
 const char* tn=" thread_name=";memcpy(p,tn,strlen(tn));p+=strlen(tn);size_t tnn=strnlen(tname,16);memcpy(p,tname,tnn);p+=tnn;
 const char* x=" thread_last=";memcpy(p,x,strlen(x));p+=strlen(x);const char* z=tlsLast;size_t zn=strlen(z);memcpy(p,z,zn);p+=zn; const char* gx=" global_last=";memcpy(p,gx,strlen(gx));p+=strlen(gx);z=last.load(std::memory_order_relaxed);zn=strlen(z);memcpy(p,z,zn);p+=zn;*p++='\n';
 int fd=open(kPath,O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC,0664);if(fd>=0){
   write(fd,b,p-b);
#if defined(__aarch64__)
   char d[4096]; int n=0; Dl_info pi{},li{};
   if(dladdr((void*)uc->uc_mcontext.pc,&pi)&&pi.dli_fbase)
     n+=snprintf(d+n,sizeof(d)-n,"pc_module=%s pc_base=%p pc_offset=0x%llx\n",pi.dli_fname?pi.dli_fname:"?",pi.dli_fbase,(unsigned long long)(uc->uc_mcontext.pc-(uintptr_t)pi.dli_fbase));
   if(dladdr((void*)uc->uc_mcontext.regs[30],&li)&&li.dli_fbase)
     n+=snprintf(d+n,sizeof(d)-n,"lr_module=%s lr_base=%p lr_offset=0x%llx\n",li.dli_fname?li.dli_fname:"?",li.dli_fbase,(unsigned long long)(uc->uc_mcontext.regs[30]-(uintptr_t)li.dli_fbase));
   for(int i=0;i<31&&n<(int)sizeof(d)-80;i++) n+=snprintf(d+n,sizeof(d)-n,"x%d=%p%s",i,(void*)uc->uc_mcontext.regs[i],(i%4)==3?"\n":" ");
   n+=snprintf(d+n,sizeof(d)-n,"\n");
   if(n>0) write(fd,d,(size_t)n);
#endif
   close(fd);
 }
 signal(sig,SIG_DFL);syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),sig);
}
__attribute__((constructor)) static void install(){
 ensureDir();
 setenv("GTAV_VULKAN_BACKEND","native",1);
 int fd=open(kPath,O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,0664);if(fd>=0){const char* h="GTAV native Vulkan self-diagnostic v2\n";write(fd,h,strlen(h));close(fd);}
 struct sigaction sa{};sa.sa_sigaction=crashHandler;sigemptyset(&sa.sa_mask);sa.sa_flags=SA_SIGINFO|SA_RESETHAND;
 int sigs[]={SIGSEGV,SIGABRT,SIGBUS,SIGILL,SIGFPE,SIGTRAP};for(int s:sigs)sigaction(s,&sa,nullptr);checkpoint("diagnostic-installed");
}
}

// Legacy import symbols remain exported only so Android's loader can resolve libgtav.so.
// DXVK is intentionally not packaged. These guards are NOT a fake D3D implementation:
// returning fabricated COM objects would crash later and hide the real migration gap.
// Native Vulkan attaches to the engine runtime only after that runtime has valid handles.
static int32_t legacyD3DLeak(const char* name) {
  gtavdiag::checkpoint("legacy-d3d-entry",name);
  // A DXGI/D3D11 entry here means the engine is still taking its legacy bootstrap.
  // Do not return E_NOTIMPL: the caller treats device creation failure as fatal before
  // grVulkanRuntime can become available. Abort this path deterministically and leave
  // a unique marker instead of returning null COM objects that crash later.
  __android_log_print(ANDROID_LOG_FATAL,"GTAV-NATIVE","FATAL legacy graphics bootstrap reached: %s",name);
  __builtin_trap();
}
// CreateDXGIFactory is reached before D3D11CreateDeviceAndSwapChain in the real
// engine bootstrap. Do not trap here: InitClass uses the factory only as an
// optional adapter-enumeration path and can fall back to the default adapter.
// Returning a normal failure with a null output lets that fallback execute and
// moves diagnostics to the actual device-creation boundary.
namespace {
struct CompatDXGIFactory { void** vtbl; };
struct CompatDXGIAdapter { void** vtbl; };
struct CompatDXGIOutput { void** vtbl; uint8_t pad[8]; uint32_t modeCount; };
static CompatDXGIFactory gCompatFactory{};
static CompatDXGIAdapter gCompatAdapter{};
static CompatDXGIOutput gCompatOutput{};
static void* gFactoryVtable[12]{};
static void* gAdapterVtable[10]{};

static int32_t compatQueryInterface(void* self, const void*, void** out) {
  gtavdiag::checkpoint("compat-dxgi-query-interface");
  if (!out) return (int32_t)0x80004003u;
  *out=self;
  return 0;
}
static int32_t compatDXGISetPrivateData(void*,const void*,uint32_t,const void*){return 0;}
static int32_t compatDXGISetPrivateDataInterface(void*,const void*,void*){return 0;}
static int32_t compatDXGIGetPrivateData(void*,const void*,uint32_t* n,void*){if(n)*n=0;return (int32_t)0x80004005u;}
static int32_t compatDXGIObjectGetParent(void* self,const void*,void** out){
  gtavdiag::checkpoint("compat-dxgi-object-get-parent");
  if(!out)return (int32_t)0x80004003u;
  if(self==&gCompatAdapter){*out=&gCompatFactory;return 0;}
  if(self==&gCompatOutput){*out=&gCompatAdapter;return 0;}
  *out=&gCompatFactory;return 0;
}
static uint32_t compatAddRef(void*) { return 2; }
static uint32_t compatRelease(void*) { return 1; }
static int32_t compatEnumAdapters(void*, uint32_t index, void** out) {
  gtavdiag::checkpoint("compat-dxgi-enum-adapters");
  if (!out) return (int32_t)0x80004003u;
  if (index != 0) { *out=nullptr; return (int32_t)0x887A0002u; } // DXGI_ERROR_NOT_FOUND
  *out=&gCompatAdapter; return 0;
}
static int32_t compatFactoryMakeWindowAssociation(void*,void*,uint32_t){
  // Android owns the window/surface association. GTA calls this through the
  // IDXGIFactory parent returned by the adapter; treating it as a successful
  // no-op is the correct compatibility behavior.
  gtavdiag::checkpoint("compat-dxgi-factory-make-window-association");
  return 0;
}
static int32_t compatFactoryGetWindowAssociation(void*,void** out){
  gtavdiag::checkpoint("compat-dxgi-factory-get-window-association");
  if(out)*out=nullptr;
  return 0;
}
static int32_t compatFactoryUnsupported(void*,...){
  gtavdiag::checkpoint("compat-dxgi-factory-unsupported-benign");
  return 0;
}
static void* gOutputVtable[32]{};
static int32_t compatOutputQueryInterface(void* self,const void*,void** out){ if(!out)return (int32_t)0x80004003u; *out=self; return 0; }
static uint32_t compatOutputAddRef(void*){return 2;}
static uint32_t compatOutputRelease(void*){return 1;}
static int32_t compatOutputGetDesc(void*,void* desc){
  gtavdiag::checkpoint("compat-dxgi-output-get-desc");
  if(!desc)return (int32_t)0x80004003u;
  memset(desc,0,0x80);
  return 0;
}

static int32_t compatOutputGetDisplayModeList(void*,uint32_t,uint32_t,uint32_t* count,void* modes){
  gtavdiag::checkpoint("compat-dxgi-output-get-display-mode-list");
  if(!count)return (int32_t)0x80004003u;
  // Android surface owns the actual presentation modes. Expose one conservative
  // mode so grcAdapterD3D11Output creates a valid engine-side output object.
  if(!modes){*count=1;return 0;}
  if(*count<1)return (int32_t)0x887A0003u;
  memset(modes,0,28);
  auto* p=(uint8_t*)modes;
  *(uint32_t*)(p+0)=1920; *(uint32_t*)(p+4)=1080;
  *(uint32_t*)(p+8)=60; *(uint32_t*)(p+12)=1;
  *(uint32_t*)(p+16)=28; // DXGI_FORMAT_R8G8B8A8_UNORM
  *(uint32_t*)(p+20)=0; *(uint32_t*)(p+24)=0;
  *count=1; return 0;
}
static int32_t compatOutputUnsupported(void*,...){
  // Some engine paths probe optional IDXGIOutput methods and then treat the
  // HRESULT as data. E_NOTIMPL (0x80004001) therefore becomes a bogus pointer.
  // Keep unsupported output probes benign while the Android surface owns output state.
  gtavdiag::checkpoint("compat-dxgi-output-unsupported-benign");
  return 0;
}
static int32_t compatEnumOutputs(void*, uint32_t index, void** out) {
  gtavdiag::checkpoint("compat-dxgi-enum-outputs");
  if (!out) return (int32_t)0x80004003u;
  if(index!=0){*out=nullptr;return (int32_t)0x887A0002u;}
  static bool once=false;
  if(!once){ once=true; for(void*& p:gOutputVtable)p=(void*)compatOutputUnsupported;
    gOutputVtable[0]=(void*)compatOutputQueryInterface; gOutputVtable[1]=(void*)compatOutputAddRef; gOutputVtable[2]=(void*)compatOutputRelease;
    gOutputVtable[3]=(void*)compatDXGISetPrivateData; gOutputVtable[4]=(void*)compatDXGISetPrivateDataInterface; gOutputVtable[5]=(void*)compatDXGIGetPrivateData; gOutputVtable[6]=(void*)compatDXGIObjectGetParent;
    gOutputVtable[7]=(void*)compatOutputGetDesc; gOutputVtable[8]=(void*)compatOutputGetDisplayModeList; gCompatOutput.vtbl=gOutputVtable; gCompatOutput.modeCount=1;
  }
  *out=&gCompatOutput; return 0;
}
static int32_t compatAdapterCheckInterfaceSupport(void*, const void*, int64_t*);
static int32_t compatGetDesc(void*, void* desc) {
  gtavdiag::checkpoint("compat-dxgi-get-desc");
  if (!desc) return (int32_t)0x80004003u;
  // Android ABI observed in grcAdapterD3D11: the descriptor uses 4-byte
  // wchar_t and is read through +0x220, not the 312-byte Windows layout.
  memset(desc,0,0x230);
  auto* p=(uint8_t*)desc;
  *(uint32_t*)(p+0x200)=0;
  *(uint64_t*)(p+0x210)=0x20000000ull;
  *(uint64_t*)(p+0x218)=0;
  *(uint64_t*)(p+0x220)=0x20000000ull;
  return 0;
}
static void initCompatDXGI() {
  static bool once=false; if(once)return; once=true;
  // Factory slots observed by disassembly: Release @ +0x10, EnumAdapters @ +0x38.
  gFactoryVtable[0]=(void*)compatQueryInterface;
  gFactoryVtable[1]=(void*)compatAddRef;
  gFactoryVtable[2]=(void*)compatRelease;
  gFactoryVtable[3]=(void*)compatDXGISetPrivateData;
  gFactoryVtable[4]=(void*)compatDXGISetPrivateDataInterface;
  gFactoryVtable[5]=(void*)compatDXGIGetPrivateData;
  gFactoryVtable[6]=(void*)compatDXGIObjectGetParent;
  gFactoryVtable[7]=(void*)compatEnumAdapters;
  gFactoryVtable[8]=(void*)compatFactoryMakeWindowAssociation;
  gFactoryVtable[9]=(void*)compatFactoryGetWindowAssociation;
  gFactoryVtable[10]=(void*)compatFactoryUnsupported;
  gFactoryVtable[11]=(void*)compatFactoryUnsupported;
  gCompatFactory.vtbl=gFactoryVtable;
  // Adapter slots observed by grcAdapterD3D11:
  // Release @ +0x10, EnumOutputs @ +0x38, GetDesc @ +0x40.
  gAdapterVtable[0]=(void*)compatQueryInterface;
  gAdapterVtable[1]=(void*)compatAddRef;
  gAdapterVtable[2]=(void*)compatRelease;
  gAdapterVtable[3]=(void*)compatDXGISetPrivateData;
  gAdapterVtable[4]=(void*)compatDXGISetPrivateDataInterface;
  gAdapterVtable[5]=(void*)compatDXGIGetPrivateData;
  gAdapterVtable[6]=(void*)compatDXGIObjectGetParent;
  gAdapterVtable[7]=(void*)compatEnumOutputs;
  gAdapterVtable[8]=(void*)compatGetDesc;
  gAdapterVtable[9]=(void*)compatAdapterCheckInterfaceSupport;
  // IDXGIAdapter::CheckInterfaceSupport is slot 9/+0x48. Keep one extra slot
  // available because post-RetrieveVideoMemory bootstrap can query driver support.
  // (gAdapterVtable is enlarged below before assignment.)
  gCompatAdapter.vtbl=gAdapterVtable;
}
}
extern "C" __attribute__((visibility("default"))) int32_t CreateDXGIFactory(const void*, void** out) {
  gtavdiag::checkpoint("compat-dxgi-factory");
  if(!out) return (int32_t)0x80004003u;
  initCompatDXGI();
  *out=&gCompatFactory;
  return 0;
}
namespace {
struct CompatD3D11Object { void** vtbl; };
static CompatD3D11Object gCompatD3DDevice{};
static CompatD3D11Object gCompatD3DContext{};
static CompatD3D11Object gCompatDXGIDevice{};
static void* gD3DDeviceVtable[64]{};
static void* gD3DContextVtable[128]{};
static void* gDXGIDeviceVtable[16]{};

static bool compatGuidEq(const void* a,uint32_t d1,uint16_t d2,uint16_t d3,const uint8_t d4[8]){
  if(!a)return false;const uint8_t* p=(const uint8_t*)a;uint32_t x1;uint16_t x2,x3;
  std::memcpy(&x1,p,4);std::memcpy(&x2,p+4,2);std::memcpy(&x3,p+6,2);
  return x1==d1&&x2==d2&&x3==d3&&std::memcmp(p+8,d4,8)==0;
}
static int32_t compatD3DQueryInterface(void* self,const void* iid,void** out) {
  gtavdiag::checkpoint("compat-d3d11-query-interface");
  if(!out) return (int32_t)0x80004003u;
  if(self==&gCompatD3DDevice){
    static const uint8_t iidDXGIDeviceD4[8]={0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c};
    static const uint8_t iidD3D11DeviceD4[8]={0x82,0x53,0x81,0x9d,0xf9,0xbb,0xf1,0x40};
    static const uint8_t iidIUnknownD4[8]={0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46};
    if(compatGuidEq(iid,0x54ec77fau,0x1377,0x44e6,iidDXGIDeviceD4)){*out=&gCompatDXGIDevice;return 0;}
    if(compatGuidEq(iid,0xdb6f6ddbu,0xac77,0x4e88,iidD3D11DeviceD4)||
       compatGuidEq(iid,0x00000000u,0x0000,0x0000,iidIUnknownD4)){*out=self;return 0;}
    *out=nullptr;
    gtavdiag::checkpoint("compat-d3d11-query-interface-unsupported");
    return (int32_t)0x80004002u;
  }
  *out=self;
  return 0;
}
static uint32_t compatD3DAddRef(void*) { return 2; }
static uint32_t compatD3DRelease(void*) { return 1; }
static uint32_t compatD3DGetFeatureLevel(void*) {
  gtavdiag::checkpoint("compat-d3d11-get-feature-level");
  return 0xb000u;
}
static int32_t compatChildGetPrivateData(void*,const void*,uint32_t* size,void*){gtavdiag::checkpoint("compat-child-get-private-data");if(size)*size=0;return (int32_t)0x80004005u;}
static void compatChildGetDevice(void*,void** out){gtavdiag::checkpoint("compat-child-get-device");if(out)*out=&gCompatD3DDevice;}
static int32_t compatChildSetPrivateDataInterface(void*,const void*,void*){gtavdiag::checkpoint("compat-child-set-private-data-interface");return 0;}
extern "C" void gtavnative_compat_mirror_input_layout(void*,void*);
extern "C" void gtavnative_compat_mirror_vertex_buffers(void*,uint32_t,uint32_t,void* const*,const uint32_t*,const uint32_t*);
extern "C" void gtavnative_compat_mirror_index_buffer(void*,void*,uint32_t,uint32_t);
extern "C" void gtavnative_compat_mirror_topology(void*,uint32_t);
extern "C" void gtavnative_compat_mirror_shader(void*,uint32_t,void*);
extern "C" void gtavnative_compat_mirror_viewports(void*,uint32_t,const void*);
extern "C" void gtavnative_compat_mirror_scissors(void*,uint32_t,const void*);
extern "C" void gtavnative_compat_mirror_objs(void*,uint32_t,uint32_t,uint32_t,void* const*);
extern "C" void gtavnative_compat_mirror_render_targets(void*,uint32_t,void* const*,void*);
extern "C" void gtavnative_compat_mirror_blend_state(void*,void*,const float*,uint32_t);
extern "C" void gtavnative_compat_mirror_depth_state(void*,void*,uint32_t);
extern "C" bool gtavnative_compat_draw(void*,uint32_t,uint32_t);
extern "C" bool gtavnative_compat_draw_indexed(void*,uint32_t,uint32_t,int32_t);
extern "C" bool gtavnative_compat_dispatch(void*,uint32_t,uint32_t,uint32_t);
static std::atomic<uint64_t> gMegaDrawSerial{0};
static thread_local uint64_t tlsMegaDrawId=0;
static std::atomic<uint32_t> gMegaDrawOk{0},gMegaDrawFail{0},gMegaDescriptorFail{0},gMegaImageFail{0};
static std::atomic<uint32_t> gMegaNoopCalls{0},gMegaCopyOps{0},gMegaTextureUpdates{0};
static void compatContextNoop(void*,...){gtavdiag::checkpoint("compat-context-noop");}
// Render-path split diagnostics. Keep ABI-compatible signatures for the hot
// ID3D11DeviceContext slots so the crash log identifies the command that GTA
// actually issued instead of collapsing everything into compat-context-noop.
static void compatCtxVSSetConstantBuffers(void* c,uint32_t f,uint32_t n,void* const* v){char d[192];snprintf(d,sizeof(d),"ctx=%p first=%u count=%u p0=%p p1=%p",c,f,n,(v&&n>0)?v[0]:nullptr,(v&&n>1)?v[1]:nullptr);gtavdiag::checkpoint("black-probe-vs-cb-set",d);gtavnative_compat_mirror_objs(c,0,f,n,v);}
static void compatCtxPSSetShaderResources(void* c,uint32_t f,uint32_t n,void* const* v){gtavdiag::checkpoint("compat-context-ps-set-shader-resources");gtavnative_compat_mirror_objs(c,4,f,n,v);}
static void compatCtxPSSetShader(void* c,void* sh,void* const*,uint32_t){gtavdiag::checkpoint("compat-context-ps-set-shader");gtavnative_compat_mirror_shader(c,1,sh);}
static void compatCtxPSSetSamplers(void* c,uint32_t f,uint32_t n,void* const* v){gtavdiag::checkpoint("compat-context-ps-set-samplers");gtavnative_compat_mirror_objs(c,7,f,n,v);}
static void compatCtxVSSetShader(void* c,void* sh,void* const*,uint32_t){gtavdiag::checkpoint("compat-context-vs-set-shader");gtavnative_compat_mirror_shader(c,0,sh);}
static void compatCtxDrawIndexed(void* c,uint32_t n,uint32_t f,int32_t v){gtavdiag::checkpoint("compat-context-draw-indexed");gtavnative_compat_draw_indexed(c,n,f,v);}
static void compatCtxDraw(void* c,uint32_t n,uint32_t f){gtavdiag::checkpoint("compat-context-draw");gtavnative_compat_draw(c,n,f);}
static void compatCtxPSSetConstantBuffers(void* c,uint32_t f,uint32_t n,void* const* v){char d[192];snprintf(d,sizeof(d),"ctx=%p first=%u count=%u p0=%p p1=%p",c,f,n,(v&&n>0)?v[0]:nullptr,(v&&n>1)?v[1]:nullptr);gtavdiag::checkpoint("black-probe-ps-cb-set",d);gtavnative_compat_mirror_objs(c,1,f,n,v);}
static void compatCtxIASetInputLayout(void* c,void* v){gtavdiag::checkpoint("compat-context-ia-set-input-layout");gtavnative_compat_mirror_input_layout(c,v);}
static void compatCtxIASetVertexBuffers(void* c,uint32_t f,uint32_t n,void* const* v,const uint32_t* s,const uint32_t* o){gtavdiag::checkpoint("compat-context-ia-set-vertex-buffers");gtavnative_compat_mirror_vertex_buffers(c,f,n,v,s,o);}
static void compatCtxIASetIndexBuffer(void* c,void* b,uint32_t f,uint32_t o){gtavdiag::checkpoint("compat-context-ia-set-index-buffer");gtavnative_compat_mirror_index_buffer(c,b,f,o);}
static void compatCtxDrawIndexedInstanced(void* c,uint32_t ic,uint32_t inst,uint32_t first,int32_t vo,uint32_t fi){gMegaNoopCalls.fetch_add(1);char d[256];snprintf(d,sizeof(d),"ctx=%p ic=%u inst=%u first=%u vo=%d firstInst=%u UNIMPLEMENTED",c,ic,inst,first,vo,fi);gtavdiag::checkpoint("MEGA-NOOP-DRAW-INDEXED-INSTANCED",d);}
static void compatCtxDrawInstanced(void* c,uint32_t vc,uint32_t inst,uint32_t first,uint32_t fi){gMegaNoopCalls.fetch_add(1);char d[224];snprintf(d,sizeof(d),"ctx=%p vc=%u inst=%u first=%u firstInst=%u UNIMPLEMENTED",c,vc,inst,first,fi);gtavdiag::checkpoint("MEGA-NOOP-DRAW-INSTANCED",d);}
static void compatCtxIASetPrimitiveTopology(void* c,uint32_t t){gtavdiag::checkpoint("compat-context-ia-set-primitive-topology");gtavnative_compat_mirror_topology(c,t);}
static void compatCtxOMSetRenderTargets(void* c,uint32_t n,void* const* r,void* d){gtavdiag::checkpoint("compat-context-om-set-render-targets");gtavnative_compat_mirror_render_targets(c,n,r,d);}
static void compatCtxOMSetBlendState(void* c,void* state,const float* factor,uint32_t mask){gtavdiag::checkpoint("compat-context-om-set-blend-state");gtavnative_compat_mirror_blend_state(c,state,factor,mask);}
static void compatCtxOMSetDepthStencilState(void* c,void* state,uint32_t ref){gtavdiag::checkpoint("compat-context-om-set-depth-stencil-state");gtavnative_compat_mirror_depth_state(c,state,ref);}
static void compatCtxDispatch(void* c,uint32_t x,uint32_t y,uint32_t z){gtavdiag::checkpoint("compat-context-dispatch");gtavnative_compat_dispatch(c,x,y,z);}
static void compatCtxRSSetState(void* c,void* state){gtavdiag::checkpoint("compat-context-rs-set-state");gtavnative_compat_mirror_objs(c,12,0,1,&state);}
static void compatCtxRSSetViewports(void* c,uint32_t n,const void* p){gtavdiag::checkpoint("compat-context-rs-set-viewports");gtavnative_compat_mirror_viewports(c,n,p);}
static void compatCtxRSSetScissorRects(void* c,uint32_t n,const void* p){gtavdiag::checkpoint("compat-context-rs-set-scissor-rects");gtavnative_compat_mirror_scissors(c,n,p);}
struct CompatD3D11Box { uint32_t left,top,front,right,bottom,back; };
static void compatUpdateBacking(void*,uint32_t,const CompatD3D11Box*,const void*,uint32_t,uint32_t);
static void compatCopySubresourceBacking(void*,uint32_t,uint32_t,uint32_t,uint32_t,void*,uint32_t,const CompatD3D11Box*);
static void compatCtxUpdateSubresource(void*,void* dst,uint32_t sub,const void* box,const void* src,uint32_t srcRow,uint32_t srcDepth){
 gMegaTextureUpdates.fetch_add(1,std::memory_order_relaxed);{const CompatD3D11Box* b=reinterpret_cast<const CompatD3D11Box*>(box);char d[320];snprintf(d,sizeof(d),"dst=%p sub=%u src=%p row=%u depth=%u box=%s%u,%u,%u-%u,%u,%u",dst,sub,src,srcRow,srcDepth,b?"":"FULL:",b?b->left:0,b?b->top:0,b?b->front:0,b?b->right:0,b?b->bottom:0,b?b->back:0);gtavdiag::checkpoint("MEGA-UPDATE-SUBRESOURCE",d);}
 compatUpdateBacking(dst,sub,reinterpret_cast<const CompatD3D11Box*>(box),src,srcRow,srcDepth);
}
static void compatClearRTVBacking(void*,const float*); static void compatCtxClearRenderTargetView(void*,void* v,const float* c){gtavdiag::checkpoint("compat-context-clear-rtv");compatClearRTVBacking(v,c);}
static void compatClearDSVBacking(void*,uint32_t,float,uint8_t); static void compatCtxClearDepthStencilView(void*,void* v,uint32_t f,float d,uint8_t s){gtavdiag::checkpoint("compat-context-clear-dsv");compatClearDSVBacking(v,f,d,s);}
// Split the remaining high-frequency D3D11 context ABI instead of routing it
// through a variadic no-op. These are state/copy/query operations used during
// the bootstrap render loop; dedicated signatures avoid ABI ambiguity and make
// the next trace actionable without changing ownership of engine objects.
static void compatCtxGSSetConstantBuffers(void*,uint32_t,uint32_t,void* const*){gtavdiag::checkpoint("compat-context-gs-set-constant-buffers");}
static void compatCtxGSSetShader(void*,void*,void* const*,uint32_t){gtavdiag::checkpoint("compat-context-gs-set-shader");}
static void compatCtxVSSetShaderResources(void* c,uint32_t f,uint32_t n,void* const* v){gtavdiag::checkpoint("compat-context-vs-set-shader-resources");gtavnative_compat_mirror_objs(c,3,f,n,v);}
static void compatCtxVSSetSamplers(void* c,uint32_t f,uint32_t n,void* const* v){gtavdiag::checkpoint("compat-context-vs-set-samplers");gtavnative_compat_mirror_objs(c,6,f,n,v);}
static void compatCtxSetPredication(void*,void*,int){gtavdiag::checkpoint("compat-context-set-predication");}
static void compatCtxGSSetShaderResources(void*,uint32_t,uint32_t,void* const*){gtavdiag::checkpoint("compat-context-gs-set-shader-resources");}
static void compatCtxGSSetSamplers(void*,uint32_t,uint32_t,void* const*){gtavdiag::checkpoint("compat-context-gs-set-samplers");}
static void compatCtxOMSetRTUAV(void*,uint32_t,void* const*,void*,uint32_t,uint32_t,void* const*,const uint32_t*){gtavdiag::checkpoint("compat-context-om-set-rt-uav");}
static void compatCtxSOSetTargets(void*,uint32_t,void* const*,const uint32_t*){gtavdiag::checkpoint("compat-context-so-set-targets");}
static void compatCtxDrawAuto(void* c){gMegaNoopCalls.fetch_add(1);char d[96];snprintf(d,sizeof(d),"ctx=%p UNIMPLEMENTED",c);gtavdiag::checkpoint("MEGA-NOOP-DRAW-AUTO",d);}
static void compatCtxDrawIndexedInstancedIndirect(void* c,void* a,uint32_t o){gMegaNoopCalls.fetch_add(1);char d[160];snprintf(d,sizeof(d),"ctx=%p args=%p off=%u UNIMPLEMENTED",c,a,o);gtavdiag::checkpoint("MEGA-NOOP-DRAW-INDEXED-INDIRECT",d);}
static void compatCtxDrawInstancedIndirect(void* c,void* a,uint32_t o){gMegaNoopCalls.fetch_add(1);char d[160];snprintf(d,sizeof(d),"ctx=%p args=%p off=%u UNIMPLEMENTED",c,a,o);gtavdiag::checkpoint("MEGA-NOOP-DRAW-INDIRECT",d);}
static void compatCtxDispatchIndirect(void* c,void* a,uint32_t o){gMegaNoopCalls.fetch_add(1);char d[160];snprintf(d,sizeof(d),"ctx=%p args=%p off=%u UNIMPLEMENTED",c,a,o);gtavdiag::checkpoint("MEGA-NOOP-DISPATCH-INDIRECT",d);}
static void compatCtxCopySubresourceRegion(void*,void* dst,uint32_t dstSub,uint32_t dstX,uint32_t dstY,uint32_t dstZ,void* src,uint32_t srcSub,const void* srcBox){
 gtavdiag::checkpoint("compat-context-copy-subresource-region");
 compatCopySubresourceBacking(dst,dstSub,dstX,dstY,dstZ,src,srcSub,reinterpret_cast<const CompatD3D11Box*>(srcBox));
}
static void compatCopyBacking(void*,void*); static void compatCtxCopyResource(void*,void* dst,void* src){gMegaCopyOps.fetch_add(1);char d[160];snprintf(d,sizeof(d),"dst=%p src=%p",dst,src);gtavdiag::checkpoint("MEGA-COPY-RESOURCE",d);compatCopyBacking(dst,src);}
static void compatCtxCopyStructureCount(void*,void*,uint32_t,void*){gtavdiag::checkpoint("compat-context-copy-structure-count");}
static void compatCtxClearUAVUint(void*,void*,const uint32_t*){gtavdiag::checkpoint("compat-context-clear-uav-uint");}
static void compatCtxClearUAVFloat(void*,void*,const float*){gtavdiag::checkpoint("compat-context-clear-uav-float");}
static void compatCtxGenerateMips(void* c,void* srv){gMegaNoopCalls.fetch_add(1);char d[160];snprintf(d,sizeof(d),"ctx=%p srv=%p UNIMPLEMENTED",c,srv);gtavdiag::checkpoint("MEGA-NOOP-GENERATE-MIPS",d);}
static void compatCtxSetResourceMinLOD(void*,void*,float){gtavdiag::checkpoint("compat-context-set-resource-min-lod");}
static float compatCtxGetResourceMinLOD(void*,void*){gtavdiag::checkpoint("compat-context-get-resource-min-lod");return 0.0f;}
static void compatResolveBacking(void*,uint32_t,void*,uint32_t); static void compatCtxResolveSubresource(void*,void* dst,uint32_t ds,void* src,uint32_t ss,uint32_t){gtavdiag::checkpoint("compat-context-resolve-subresource");compatResolveBacking(dst,ds,src,ss);}
static void compatCtxExecuteCommandList(void*,void*,int){gtavdiag::checkpoint("compat-context-execute-command-list");}
static void compatCtxHSSetShaderResources(void*,uint32_t,uint32_t,void* const*){gtavdiag::checkpoint("compat-context-hs-set-shader-resources");}
static void compatCtxHSSetShader(void*,void*,void* const*,uint32_t){gtavdiag::checkpoint("compat-context-hs-set-shader");}
static void compatCtxHSSetSamplers(void*,uint32_t,uint32_t,void* const*){gtavdiag::checkpoint("compat-context-hs-set-samplers");}
static void compatCtxHSSetConstantBuffers(void*,uint32_t,uint32_t,void* const*){gtavdiag::checkpoint("compat-context-hs-set-constant-buffers");}
static void compatCtxDSSetShaderResources(void*,uint32_t,uint32_t,void* const*){gtavdiag::checkpoint("compat-context-ds-set-shader-resources");}
static void compatCtxDSSetShader(void*,void*,void* const*,uint32_t){gtavdiag::checkpoint("compat-context-ds-set-shader");}
static void compatCtxDSSetSamplers(void*,uint32_t,uint32_t,void* const*){gtavdiag::checkpoint("compat-context-ds-set-samplers");}
static void compatCtxDSSetConstantBuffers(void*,uint32_t,uint32_t,void* const*){gtavdiag::checkpoint("compat-context-ds-set-constant-buffers");}
static void compatCtxCSSetShaderResources(void* c,uint32_t f,uint32_t n,void* const* v){gtavdiag::checkpoint("compat-context-cs-set-shader-resources");gtavnative_compat_mirror_objs(c,5,f,n,v);}
static void compatCtxCSSetUnorderedAccessViews(void* c,uint32_t f,uint32_t n,void* const* v,const uint32_t*){gtavdiag::checkpoint("compat-context-cs-set-uavs");gtavnative_compat_mirror_objs(c,9,f,n,v);}
static void compatCtxCSSetShader(void* c,void* sh,void* const*,uint32_t){gtavdiag::checkpoint("compat-context-cs-set-shader");gtavnative_compat_mirror_shader(c,2,sh);}
static void compatCtxCSSetSamplers(void* c,uint32_t f,uint32_t n,void* const* v){gtavdiag::checkpoint("compat-context-cs-set-samplers");gtavnative_compat_mirror_objs(c,8,f,n,v);}
static void compatCtxCSSetConstantBuffers(void* c,uint32_t f,uint32_t n,void* const* v){gtavdiag::checkpoint("compat-context-cs-set-constant-buffers");gtavnative_compat_mirror_objs(c,2,f,n,v);}
static int32_t compatDeviceGetPrivateData(void* s,const void* g,uint32_t* n,void* d){return compatChildGetPrivateData(s,g,n,d);}
static int32_t compatDeviceSetPrivateDataInterface(void*,const void*,void*){return 0;}
static uint32_t compatDeviceGetCreationFlags(void*){return 0;}
static int32_t compatDeviceRemovedReason(void*){return 0;}
static void compatDeviceGetImmediateContext(void*,void** out){if(out)*out=&gCompatD3DContext;}
static int32_t compatDeviceSetExceptionMode(void*,uint32_t){return 0;}
static uint32_t compatDeviceGetExceptionMode(void*){return 0;}
static int32_t compatD3DUnsupported(void*) {
  gtavdiag::checkpoint("compat-d3d11-unsupported-method");
  return (int32_t)0x80004001u;
}
#define GTAV_SLOT_STUB(kind,n) static int32_t compat##kind##Slot##n(void*, ...) { gtavdiag::checkpoint("compat-" #kind "-slot-" #n); return (int32_t)0x80004001u; }
GTAV_SLOT_STUB(Resource,0)
GTAV_SLOT_STUB(Resource,1)
GTAV_SLOT_STUB(Resource,2)
GTAV_SLOT_STUB(Resource,3)
GTAV_SLOT_STUB(Resource,4)
GTAV_SLOT_STUB(Resource,5)
GTAV_SLOT_STUB(Resource,6)
GTAV_SLOT_STUB(Resource,7)
GTAV_SLOT_STUB(Resource,8)
GTAV_SLOT_STUB(Resource,9)
GTAV_SLOT_STUB(Resource,10)
GTAV_SLOT_STUB(Resource,11)
GTAV_SLOT_STUB(Resource,12)
GTAV_SLOT_STUB(Resource,13)
GTAV_SLOT_STUB(Resource,14)
GTAV_SLOT_STUB(Resource,15)
GTAV_SLOT_STUB(View,0)
GTAV_SLOT_STUB(View,1)
GTAV_SLOT_STUB(View,2)
GTAV_SLOT_STUB(View,3)
GTAV_SLOT_STUB(View,4)
GTAV_SLOT_STUB(View,5)
GTAV_SLOT_STUB(View,6)
GTAV_SLOT_STUB(View,7)
GTAV_SLOT_STUB(View,8)
GTAV_SLOT_STUB(View,9)
GTAV_SLOT_STUB(View,10)
GTAV_SLOT_STUB(View,11)
GTAV_SLOT_STUB(View,12)
GTAV_SLOT_STUB(View,13)
GTAV_SLOT_STUB(View,14)
GTAV_SLOT_STUB(View,15)
GTAV_SLOT_STUB(State,0)
GTAV_SLOT_STUB(State,1)
GTAV_SLOT_STUB(State,2)
GTAV_SLOT_STUB(State,3)
GTAV_SLOT_STUB(State,4)
GTAV_SLOT_STUB(State,5)
GTAV_SLOT_STUB(State,6)
GTAV_SLOT_STUB(State,7)
GTAV_SLOT_STUB(State,8)
GTAV_SLOT_STUB(State,9)
GTAV_SLOT_STUB(State,10)
GTAV_SLOT_STUB(State,11)
GTAV_SLOT_STUB(State,12)
GTAV_SLOT_STUB(State,13)
GTAV_SLOT_STUB(State,14)
GTAV_SLOT_STUB(State,15)
GTAV_SLOT_STUB(Query,0)
GTAV_SLOT_STUB(Query,1)
GTAV_SLOT_STUB(Query,2)
GTAV_SLOT_STUB(Query,3)
GTAV_SLOT_STUB(Query,4)
GTAV_SLOT_STUB(Query,5)
GTAV_SLOT_STUB(Query,6)
GTAV_SLOT_STUB(Query,7)
GTAV_SLOT_STUB(Query,8)
GTAV_SLOT_STUB(Query,9)
GTAV_SLOT_STUB(Query,10)
GTAV_SLOT_STUB(Query,11)
GTAV_SLOT_STUB(Query,12)
GTAV_SLOT_STUB(Query,13)
GTAV_SLOT_STUB(Query,14)
GTAV_SLOT_STUB(Query,15)
GTAV_SLOT_STUB(BackBuffer,0)
GTAV_SLOT_STUB(BackBuffer,1)
GTAV_SLOT_STUB(BackBuffer,2)
GTAV_SLOT_STUB(BackBuffer,3)
GTAV_SLOT_STUB(BackBuffer,4)
GTAV_SLOT_STUB(BackBuffer,5)
GTAV_SLOT_STUB(BackBuffer,6)
GTAV_SLOT_STUB(BackBuffer,7)
GTAV_SLOT_STUB(BackBuffer,8)
GTAV_SLOT_STUB(BackBuffer,9)
GTAV_SLOT_STUB(BackBuffer,10)
GTAV_SLOT_STUB(BackBuffer,11)
GTAV_SLOT_STUB(BackBuffer,12)
GTAV_SLOT_STUB(BackBuffer,13)
GTAV_SLOT_STUB(BackBuffer,14)
GTAV_SLOT_STUB(BackBuffer,15)
GTAV_SLOT_STUB(DXGIDevice,0)
GTAV_SLOT_STUB(DXGIDevice,1)
GTAV_SLOT_STUB(DXGIDevice,2)
GTAV_SLOT_STUB(DXGIDevice,3)
GTAV_SLOT_STUB(DXGIDevice,4)
GTAV_SLOT_STUB(DXGIDevice,5)
GTAV_SLOT_STUB(DXGIDevice,6)
GTAV_SLOT_STUB(DXGIDevice,7)
GTAV_SLOT_STUB(DXGIDevice,8)
GTAV_SLOT_STUB(DXGIDevice,9)
GTAV_SLOT_STUB(DXGIDevice,10)
GTAV_SLOT_STUB(DXGIDevice,11)
GTAV_SLOT_STUB(DXGIDevice,12)
GTAV_SLOT_STUB(DXGIDevice,13)
GTAV_SLOT_STUB(DXGIDevice,14)
GTAV_SLOT_STUB(DXGIDevice,15)

GTAV_SLOT_STUB(Device,3)
GTAV_SLOT_STUB(Device,4)
GTAV_SLOT_STUB(Device,5)
GTAV_SLOT_STUB(Device,6)
GTAV_SLOT_STUB(Device,7)
GTAV_SLOT_STUB(Device,8)
GTAV_SLOT_STUB(Device,9)
GTAV_SLOT_STUB(Device,10)
GTAV_SLOT_STUB(Device,11)
GTAV_SLOT_STUB(Device,12)
GTAV_SLOT_STUB(Device,13)
GTAV_SLOT_STUB(Device,14)
GTAV_SLOT_STUB(Device,15)
GTAV_SLOT_STUB(Device,16)
GTAV_SLOT_STUB(Device,17)
GTAV_SLOT_STUB(Device,18)
GTAV_SLOT_STUB(Device,19)
GTAV_SLOT_STUB(Device,20)
GTAV_SLOT_STUB(Device,21)
GTAV_SLOT_STUB(Device,22)
GTAV_SLOT_STUB(Device,23)
GTAV_SLOT_STUB(Device,24)
GTAV_SLOT_STUB(Device,25)
GTAV_SLOT_STUB(Device,26)
GTAV_SLOT_STUB(Device,27)
GTAV_SLOT_STUB(Device,28)
GTAV_SLOT_STUB(Device,29)
GTAV_SLOT_STUB(Device,30)
GTAV_SLOT_STUB(Device,31)
GTAV_SLOT_STUB(Device,32)
GTAV_SLOT_STUB(Device,33)
GTAV_SLOT_STUB(Device,34)
GTAV_SLOT_STUB(Device,35)
GTAV_SLOT_STUB(Device,36)
GTAV_SLOT_STUB(Device,37)
GTAV_SLOT_STUB(Device,38)
GTAV_SLOT_STUB(Device,39)
GTAV_SLOT_STUB(Device,40)
GTAV_SLOT_STUB(Device,41)
GTAV_SLOT_STUB(Device,42)
GTAV_SLOT_STUB(Device,43)
GTAV_SLOT_STUB(Device,44)
GTAV_SLOT_STUB(Device,45)
GTAV_SLOT_STUB(Device,46)
GTAV_SLOT_STUB(Device,47)
GTAV_SLOT_STUB(Device,48)
GTAV_SLOT_STUB(Device,49)
GTAV_SLOT_STUB(Device,50)
GTAV_SLOT_STUB(Device,51)
GTAV_SLOT_STUB(Device,52)
GTAV_SLOT_STUB(Device,53)
GTAV_SLOT_STUB(Device,54)
GTAV_SLOT_STUB(Device,55)
GTAV_SLOT_STUB(Device,56)
GTAV_SLOT_STUB(Device,57)
GTAV_SLOT_STUB(Device,58)
GTAV_SLOT_STUB(Device,59)
GTAV_SLOT_STUB(Device,60)
GTAV_SLOT_STUB(Device,61)
GTAV_SLOT_STUB(Device,62)
GTAV_SLOT_STUB(Device,63)
GTAV_SLOT_STUB(Context,3)
GTAV_SLOT_STUB(Context,4)
GTAV_SLOT_STUB(Context,5)
GTAV_SLOT_STUB(Context,6)
GTAV_SLOT_STUB(Context,7)
GTAV_SLOT_STUB(Context,8)
GTAV_SLOT_STUB(Context,9)
GTAV_SLOT_STUB(Context,10)
GTAV_SLOT_STUB(Context,11)
GTAV_SLOT_STUB(Context,12)
GTAV_SLOT_STUB(Context,13)
GTAV_SLOT_STUB(Context,14)
GTAV_SLOT_STUB(Context,15)
GTAV_SLOT_STUB(Context,16)
GTAV_SLOT_STUB(Context,17)
GTAV_SLOT_STUB(Context,18)
GTAV_SLOT_STUB(Context,19)
GTAV_SLOT_STUB(Context,20)
GTAV_SLOT_STUB(Context,21)
GTAV_SLOT_STUB(Context,22)
GTAV_SLOT_STUB(Context,23)
GTAV_SLOT_STUB(Context,24)
GTAV_SLOT_STUB(Context,25)
GTAV_SLOT_STUB(Context,26)
GTAV_SLOT_STUB(Context,27)
GTAV_SLOT_STUB(Context,28)
GTAV_SLOT_STUB(Context,29)
GTAV_SLOT_STUB(Context,30)
GTAV_SLOT_STUB(Context,31)
GTAV_SLOT_STUB(Context,32)
GTAV_SLOT_STUB(Context,33)
GTAV_SLOT_STUB(Context,34)
GTAV_SLOT_STUB(Context,35)
GTAV_SLOT_STUB(Context,36)
GTAV_SLOT_STUB(Context,37)
GTAV_SLOT_STUB(Context,38)
GTAV_SLOT_STUB(Context,39)
GTAV_SLOT_STUB(Context,40)
GTAV_SLOT_STUB(Context,41)
GTAV_SLOT_STUB(Context,42)
GTAV_SLOT_STUB(Context,43)
GTAV_SLOT_STUB(Context,44)
GTAV_SLOT_STUB(Context,45)
GTAV_SLOT_STUB(Context,46)
GTAV_SLOT_STUB(Context,47)
GTAV_SLOT_STUB(Context,48)
GTAV_SLOT_STUB(Context,49)
GTAV_SLOT_STUB(Context,50)
GTAV_SLOT_STUB(Context,51)
GTAV_SLOT_STUB(Context,52)
GTAV_SLOT_STUB(Context,53)
GTAV_SLOT_STUB(Context,54)
GTAV_SLOT_STUB(Context,55)
GTAV_SLOT_STUB(Context,56)
GTAV_SLOT_STUB(Context,57)
GTAV_SLOT_STUB(Context,58)
GTAV_SLOT_STUB(Context,59)
GTAV_SLOT_STUB(Context,60)
GTAV_SLOT_STUB(Context,61)
GTAV_SLOT_STUB(Context,62)
GTAV_SLOT_STUB(Context,63)
#undef GTAV_SLOT_STUB


// ID3D11DeviceContext::Map is slot 14/+0x70. ResetClipPlanes maps a
// dynamic buffers with D3D11_MAP_WRITE_DISCARD. Scaleform BeginVertices can
// consume roughly 20 MiB from a mapped streaming vertex buffer before discard;
// keep a 32 MiB guarded-compatible arena so returned pointers cannot run past
// the old 4 MiB scratch boundary.
struct CompatMappedSubresource { void* pData; uint32_t rowPitch; uint32_t depthPitch; };
alignas(4096) static uint8_t gCompatMapScratch[32 * 1024 * 1024]{};
static int32_t compatD3DMap(void*, void* resource, uint32_t subresource, uint32_t, uint32_t, CompatMappedSubresource* mapped) {
  gtavdiag::checkpoint("compat-d3d11-map");
  if(!mapped) return (int32_t)0x80004003u;
  // Until resource types are declared below, keep the proven guarded arena.
  // Per-resource backing is selected by a helper defined after CompatResourceObject.
  mapped->pData=gCompatMapScratch;
  mapped->rowPitch=(uint32_t)sizeof(gCompatMapScratch);
  mapped->depthPitch=(uint32_t)sizeof(gCompatMapScratch);
  extern bool compatMapResourceBacking(void*, CompatMappedSubresource*);
  extern bool compatMapResourceBackingSubresource(void*, uint32_t, CompatMappedSubresource*);
  (void)compatMapResourceBackingSubresource(resource,subresource,mapped);
  return 0;
}
static void compatMarkDirty(void*);
static void compatD3DUnmap(void*, void* resource, uint32_t subresource) {
  // Unmap may be reached with engine/native resources as well as our compat
  // resource shells. Never dereference or mutate a foreign object here.
  // The CPU backing store itself was already written through the pointer
  // returned by Map; only a registered compat resource needs its upload
  // generation advanced.
  if(!resource){
    gtavdiag::checkpoint("compat-d3d11-unmap-null");
    return;
  }
  extern bool compatIsRegisteredResource(void*);
  if(!compatIsRegisteredResource(resource)){
    char d[96];snprintf(d,sizeof(d),"resource=%p sub=%u",resource,subresource);
    gtavdiag::checkpoint("compat-d3d11-unmap-foreign-skip",d);
    return;
  }
  compatMarkDirty(resource);
  gtavdiag::checkpoint("compat-d3d11-unmap-dirty");
}
// Shader creation is consumed as an object pointer by grcProgram::CreateShader.
// Returning E_NOTIMPL through the generic stub leaves the out-object undefined
// and later crashes on Release. Return a tiny COM object instead; actual shader
// execution is intercepted by the native Vulkan renderer hooks.
struct CompatShaderObject { void** vtbl; std::vector<uint8_t> bytecode; };
struct CompatInputElement { std::string semantic; uint32_t semanticIndex{},format{},slot{},offset{},inputClass{},stepRate{}; };
struct CompatD3D11InputElementDesc64 {
 const char* semanticName;
 uint32_t semanticIndex;
 uint32_t format;
 uint32_t inputSlot;
 uint32_t alignedByteOffset;
 uint32_t inputSlotClass;
 uint32_t instanceDataStepRate;
};
static_assert(sizeof(CompatD3D11InputElementDesc64)==32,"D3D11_INPUT_ELEMENT_DESC ARM64 ABI mismatch");
struct CompatInputLayoutObject { void** vtbl; std::vector<uint8_t> signature; std::vector<CompatInputElement> elements; };
static void* gCompatShaderVtable[8]{};
static void* gCompatInputLayoutVtable[8]{};
static std::mutex gCompatShaderMutex;
static std::vector<CompatShaderObject*> gCompatShaders;
static std::vector<CompatInputLayoutObject*> gCompatInputLayouts;
static int32_t compatShaderQI(void* self,const void*,void** out){if(!out)return (int32_t)0x80004003u;*out=self;return 0;}
static uint32_t compatShaderAddRef(void*){return 2;}
static uint32_t compatShaderRelease(void*){return 1;}
static int32_t compatSetPrivateData(void*, const void*, uint32_t, const void*);
static void initCompatShaderVtables(){
 static bool once=false;if(once)return;once=true;
 for(void** t:{gCompatShaderVtable,gCompatInputLayoutVtable}){t[0]=(void*)compatShaderQI;t[1]=(void*)compatShaderAddRef;t[2]=(void*)compatShaderRelease;t[3]=(void*)compatChildGetDevice;t[4]=(void*)compatChildGetPrivateData;t[5]=(void*)compatSetPrivateData;t[6]=(void*)compatChildSetPrivateDataInterface;}
}
static int32_t compatCreateInputLayout(void*,const void* raw,uint32_t count,const void* shader,size_t shaderBytes,void** out){
 gtavdiag::checkpoint("compat-d3d11-create-input-layout");if(!out)return (int32_t)0x80004003u;initCompatShaderVtables();
 auto* o=new CompatInputLayoutObject{};o->vtbl=gCompatInputLayoutVtable;if(shader&&shaderBytes)o->signature.assign((const uint8_t*)shader,(const uint8_t*)shader+shaderBytes);
 if(raw&&count&&count<=32){
  const auto* p=reinterpret_cast<const CompatD3D11InputElementDesc64*>(raw);
  o->elements.reserve(count);
  for(uint32_t i=0;i<count;i++){
   CompatInputElement x{};
   x.semanticIndex=p[i].semanticIndex;x.format=p[i].format;x.slot=p[i].inputSlot;
   x.offset=p[i].alignedByteOffset;x.inputClass=p[i].inputSlotClass;x.stepRate=p[i].instanceDataStepRate;
   if(p[i].semanticName){size_t n=strnlen(p[i].semanticName,64);x.semantic.assign(p[i].semanticName,n);}
   char d[192];snprintf(d,sizeof(d),"i=%u sem=%s%u fmt=%u slot=%u off=%u class=%u step=%u",i,x.semantic.c_str(),x.semanticIndex,x.format,x.slot,x.offset,x.inputClass,x.stepRate);
   gtavdiag::checkpoint("native-input-layout-element",d);
   o->elements.push_back(std::move(x));
  }
 }
 {std::lock_guard<std::mutex> l(gCompatShaderMutex);gCompatInputLayouts.push_back(o);}
 {char d[160];snprintf(d,sizeof(d),"raw=%p count=%u elems=%zu shaderBytes=%zu out=%p",raw,count,o->elements.size(),shaderBytes,o);gtavdiag::checkpoint("native-input-layout-created",d);}
 *out=o;return 0;
}
static int32_t compatCreateShader(void*,const void* code,size_t bytes,void*,void** out){
 gtavdiag::checkpoint("compat-d3d11-create-shader");if(!out)return (int32_t)0x80004003u;initCompatShaderVtables();
 auto* o=new CompatShaderObject{};o->vtbl=gCompatShaderVtable;if(code&&bytes)o->bytecode.assign((const uint8_t*)code,(const uint8_t*)code+bytes);
 {std::lock_guard<std::mutex> l(gCompatShaderMutex);gCompatShaders.push_back(o);}*out=o;return 0;
}
static bool compatShaderObject(void* p){
 if(!p)return false;std::lock_guard<std::mutex> l(gCompatShaderMutex);
 return std::find(gCompatShaders.begin(),gCompatShaders.end(),(CompatShaderObject*)p)!=gCompatShaders.end();
}
static CompatInputLayoutObject* compatInputLayoutObject(void* p){
 if(!p)return nullptr;std::lock_guard<std::mutex> l(gCompatShaderMutex);
 auto* x=(CompatInputLayoutObject*)p;return std::find(gCompatInputLayouts.begin(),gCompatInputLayouts.end(),x)!=gCompatInputLayouts.end()?x:nullptr;
}

static CompatInputLayoutObject* compatResolveInputLayout(void* layout,void* vs){
 if(auto* direct=compatInputLayoutObject(layout))return direct;
 if(!layout||!vs||!compatShaderObject(vs))return nullptr;
 auto* shader=(CompatShaderObject*)vs;
 std::lock_guard<std::mutex> l(gCompatShaderMutex);
 for(auto* il:gCompatInputLayouts){
   if(!il||il->signature.size()!=shader->bytecode.size())continue;
   if(!il->signature.empty()&&std::memcmp(il->signature.data(),shader->bytecode.data(),il->signature.size())==0){
     char d[128];snprintf(d,sizeof(d),"layout=%p recovered=%p elems=%zu",layout,il,il->elements.size());
     gtavdiag::checkpoint("native-input-layout-recovered-by-vs",d);
     return il;
   }
 }
 char d[128];snprintf(d,sizeof(d),"layout=%p vs=%p",layout,vs);
 gtavdiag::checkpoint("native-input-layout-unresolved",d);
 return nullptr;
}
static bool compatShaderIsSpirv(const CompatShaderObject* s){
 if(!s||s->bytecode.size()<20||(s->bytecode.size()&3))return false;
 uint32_t magic=0;std::memcpy(&magic,s->bytecode.data(),sizeof(magic));return magic==0x07230203u;
}

// Minimal D3D11 resource/view shells used only for the engine bootstrap ABI.
// They keep valid COM objects and descriptors alive while the actual draw path is
// migrated to Vulkan. Returning E_NOTIMPL with a null out pointer here is unsafe:
// GTA consumes the created RTV/DSV/SRV objects immediately.
struct CompatResourceObject {
 void** vtbl;size_t descSize;uint8_t desc[64];std::vector<uint8_t> backing;uint64_t version{1};
 uint32_t pendingClearFlags{};float pendingClearColor[4]{};float pendingClearDepth{1.0f};uint8_t pendingClearStencil{};
};
struct CompatViewObject { void** vtbl; CompatResourceObject* resource; size_t descSize; uint8_t desc[32]; };
static void* gCompatBufferVtable[16]{};
static void* gCompatTexture1DVtable[16]{};
static void* gCompatTexture2DVtable[16]{};
static void* gCompatTexture3DVtable[16]{};
static void* gCompatViewVtable[16]{};
static std::mutex gCompatObjectMutex;
static std::vector<CompatResourceObject*> gCompatResources;
static std::vector<CompatViewObject*> gCompatViews;
static CompatViewObject* compatViewObject(void* p){
 if(!p)return nullptr;
 std::lock_guard<std::mutex> l(gCompatObjectMutex);
 auto* v=(CompatViewObject*)p;
 return std::find(gCompatViews.begin(),gCompatViews.end(),v)!=gCompatViews.end()?v:nullptr;
}
static CompatResourceObject* compatResourceObject(void* p){
 if(!p)return nullptr;
 std::lock_guard<std::mutex> l(gCompatObjectMutex);
 auto* r=(CompatResourceObject*)p;
 return std::find(gCompatResources.begin(),gCompatResources.end(),r)!=gCompatResources.end()?r:nullptr;
}
bool compatIsRegisteredResource(void* p){
 if(!p)return false;
 std::lock_guard<std::mutex> l(gCompatObjectMutex);
 auto* r=(CompatResourceObject*)p;
 return std::find(gCompatResources.begin(),gCompatResources.end(),r)!=gCompatResources.end();
}
static int32_t compatChildQI(void* self,const void*,void** out){if(!out)return (int32_t)0x80004003u;*out=self;return 0;}
static uint32_t compatChildAddRef(void*){return 2;}
static uint32_t compatChildRelease(void*){return 1;}
static void compatResourceGetType(void* self,uint32_t* out){
  if(!out)return;
  if(!self){*out=0;return;}
  auto* o=(CompatResourceObject*)self;
  if(o->vtbl==gCompatBufferVtable)*out=1;
  else if(o->vtbl==gCompatTexture1DVtable)*out=2;
  else if(o->vtbl==gCompatTexture2DVtable)*out=3;
  else if(o->vtbl==gCompatTexture3DVtable)*out=4;
  else *out=0;
}
static void compatResourceSetEvictionPriority(void*,uint32_t){}
static uint32_t compatResourceGetEvictionPriority(void*){return 0;}
static void compatResourceGetDesc(void* self,void* out){
  gtavdiag::checkpoint("compat-resource-get-desc");
  if(self&&out){auto* o=(CompatResourceObject*)self;std::memcpy(out,o->desc,o->descSize);}
}
static void compatViewGetResource(void* self,void** out){
  gtavdiag::checkpoint("compat-view-get-resource");
  if(out)*out=self?((CompatViewObject*)self)->resource:nullptr;
}
static void compatViewGetDesc(void* self,void* out){
  gtavdiag::checkpoint("compat-view-get-desc");
  if(self&&out){auto* o=(CompatViewObject*)self;std::memcpy(out,o->desc,o->descSize);}
}
static void initCompatResourceVtables(){
  static bool once=false;if(once)return;once=true;
  void** tables[]={gCompatBufferVtable,gCompatTexture1DVtable,gCompatTexture2DVtable,gCompatTexture3DVtable};
  for(void** t:tables){for(int i=0;i<16;i++){static void* slots[16]={(void*)compatResourceSlot0,(void*)compatResourceSlot1,(void*)compatResourceSlot2,(void*)compatResourceSlot3,(void*)compatResourceSlot4,(void*)compatResourceSlot5,(void*)compatResourceSlot6,(void*)compatResourceSlot7,(void*)compatResourceSlot8,(void*)compatResourceSlot9,(void*)compatResourceSlot10,(void*)compatResourceSlot11,(void*)compatResourceSlot12,(void*)compatResourceSlot13,(void*)compatResourceSlot14,(void*)compatResourceSlot15};t[i]=slots[i];}t[0]=(void*)compatChildQI;t[1]=(void*)compatChildAddRef;t[2]=(void*)compatChildRelease;t[3]=(void*)compatChildGetDevice;t[4]=(void*)compatChildGetPrivateData;t[5]=(void*)compatSetPrivateData;t[6]=(void*)compatChildSetPrivateDataInterface;t[7]=(void*)compatResourceGetType;t[8]=(void*)compatResourceSetEvictionPriority;t[9]=(void*)compatResourceGetEvictionPriority;}
  // ID3D11Buffer::GetDesc slot 10; Texture1D/2D/3D GetDesc slots 10/10/10.
  gCompatBufferVtable[10]=(void*)compatResourceGetDesc;
  gCompatTexture1DVtable[10]=(void*)compatResourceGetDesc;
  gCompatTexture2DVtable[10]=(void*)compatResourceGetDesc;
  gCompatTexture3DVtable[10]=(void*)compatResourceGetDesc;
  {void* slots[16]={(void*)compatViewSlot0,(void*)compatViewSlot1,(void*)compatViewSlot2,(void*)compatViewSlot3,(void*)compatViewSlot4,(void*)compatViewSlot5,(void*)compatViewSlot6,(void*)compatViewSlot7,(void*)compatViewSlot8,(void*)compatViewSlot9,(void*)compatViewSlot10,(void*)compatViewSlot11,(void*)compatViewSlot12,(void*)compatViewSlot13,(void*)compatViewSlot14,(void*)compatViewSlot15};for(int i=0;i<16;i++)gCompatViewVtable[i]=slots[i];}
  gCompatViewVtable[0]=(void*)compatChildQI; gCompatViewVtable[1]=(void*)compatChildAddRef; gCompatViewVtable[2]=(void*)compatChildRelease;
  gCompatViewVtable[3]=(void*)compatChildGetDevice; gCompatViewVtable[4]=(void*)compatChildGetPrivateData; gCompatViewVtable[5]=(void*)compatSetPrivateData; gCompatViewVtable[6]=(void*)compatChildSetPrivateDataInterface; gCompatViewVtable[7]=(void*)compatViewGetResource; gCompatViewVtable[8]=(void*)compatViewGetDesc;
}
static void compatFormatLayout(uint32_t,uint32_t&,uint32_t&,uint32_t&);
static size_t compatTexture2DLayout(const uint32_t*,uint32_t,uint32_t*,uint32_t*,size_t*);
static CompatResourceObject* makeCompatResource(const void* desc,size_t bytes,const char* checkpoint,void** vtbl){
  gtavdiag::checkpoint(checkpoint);initCompatResourceVtables();
  auto* o=new CompatResourceObject{};o->vtbl=vtbl;o->descSize=std::min(bytes,sizeof(o->desc));
  if(desc)std::memcpy(o->desc,desc,std::min(bytes,sizeof(o->desc)));
  size_t storage=0; if(desc){const uint32_t* d=(const uint32_t*)desc; if(vtbl==gCompatBufferVtable)storage=d[0]; else if(vtbl==gCompatTexture1DVtable)storage=(size_t)d[0]*4u; else if(vtbl==gCompatTexture2DVtable)storage=compatTexture2DLayout(d,0,nullptr,nullptr,nullptr); else if(vtbl==gCompatTexture3DVtable)storage=(size_t)d[0]*std::max(1u,d[1])*std::max(1u,d[2])*4u;} if(storage)o->backing.resize(std::min<size_t>(storage,256u*1024u*1024u));
  std::lock_guard<std::mutex> l(gCompatObjectMutex);gCompatResources.push_back(o);return o;
}
extern "C" bool gtavnative_compat_register_view_resource(void* view,void* resource,uint32_t kind,bool renderTarget);
static CompatViewObject* makeCompatView(void* resource,const void* desc,size_t bytes,const char* checkpoint){
  gtavdiag::checkpoint(checkpoint);initCompatResourceVtables();
  auto* o=new CompatViewObject{};o->vtbl=gCompatViewVtable;o->resource=(CompatResourceObject*)resource;o->descSize=std::min(bytes,sizeof(o->desc));
  if(desc)std::memcpy(o->desc,desc,std::min(bytes,sizeof(o->desc)));
  std::lock_guard<std::mutex> l(gCompatObjectMutex);gCompatViews.push_back(o);return o;
}
static void compatFormatLayout(uint32_t fmt,uint32_t& bw,uint32_t& bh,uint32_t& bytes){
  bw=bh=1; bytes=4;
  switch(fmt){
    case 1:case 2:case 3:case 4: bytes=16; break;
    case 9:case 10:case 11:case 12:case 13:case 14:case 15:case 16: bytes=8; break;
    case 17:case 18:case 19:case 20:case 21:case 22:case 23: bytes=4; break;
    case 24:case 25:case 26: bytes=4; break;
    case 27:case 28:case 29:case 30:case 31:case 32:case 33:case 34:case 35:case 36:case 37:case 38:case 39:case 40:case 41:case 42:case 43:case 44:case 45:case 46:case 47: bytes=4; break;
    case 48:case 49:case 50:case 51:case 52:case 53:case 54:case 55:case 56:case 57:case 58:case 59: bytes=2; break;
    case 60:case 61:case 62:case 63:case 64:case 65: bytes=1; break;
    case 70:case 71:case 72:case 79:case 80:case 81: bw=bh=4; bytes=8; break;
    case 73:case 74:case 75:case 76:case 77:case 78:case 82:case 83:case 84:case 94:case 95:case 96:case 97:case 98:case 99: bw=bh=4; bytes=16; break;
    default: break;
  }
}
static size_t compatTexture2DLayout(const uint32_t* d,uint32_t targetSub,uint32_t* rowOut,uint32_t* depthOut,size_t* offOut){
  uint32_t w=std::max(1u,d[0]),h=std::max(1u,d[1]),mips=std::max(1u,d[2]),arrays=std::max(1u,d[3]),fmt=d[4];
  uint32_t bw,bh,bpb; compatFormatLayout(fmt,bw,bh,bpb);
  size_t total=0,targetOff=0; uint32_t tr=0,td=0;
  uint32_t count=std::min<uint32_t>(mips*arrays,4096u);
  for(uint32_t s=0;s<count;s++){
    uint32_t mip=s%mips, mw=std::max(1u,w>>std::min(mip,31u)), mh=std::max(1u,h>>std::min(mip,31u));
    uint32_t row=((mw+bw-1)/bw)*bpb, rows=(mh+bh-1)/bh, depth=row*rows;
    if(s==targetSub){targetOff=total;tr=row;td=depth;}
    total+=depth;
  }
  if(targetSub>=count){targetOff=0;tr=((w+bw-1)/bw)*bpb;td=tr*((h+bh-1)/bh);}
  if(rowOut)*rowOut=tr;if(depthOut)*depthOut=td;if(offOut)*offOut=targetOff;return total;
}
bool compatMapResourceBackingSubresource(void* resource,uint32_t subresource,CompatMappedSubresource* mapped){
  if(!resource||!mapped)return false;
  auto* o=compatResourceObject(resource);
  if(!o)return false;
  if(o->vtbl==gCompatTexture2DVtable&&o->descSize>=44){
    uint32_t row=0,depth=0;size_t off=0,total=compatTexture2DLayout((const uint32_t*)o->desc,subresource,&row,&depth,&off);
    total=std::min<size_t>(std::max<size_t>(total,depth),256u*1024u*1024u);
    if(o->backing.size()<total)o->backing.resize(total);
    if(off>=o->backing.size())off=0;
    mapped->pData=o->backing.data()+off;mapped->rowPitch=row;mapped->depthPitch=depth;return true;
  }
  if(o->backing.empty())o->backing.resize(sizeof(gCompatMapScratch));
  mapped->pData=o->backing.data();mapped->rowPitch=(uint32_t)std::min<size_t>(o->backing.size(),0xffffffffu);mapped->depthPitch=mapped->rowPitch;return true;
}
bool compatMapResourceBacking(void* resource,CompatMappedSubresource* mapped){return compatMapResourceBackingSubresource(resource,0,mapped);}
static void compatMarkDirty(void* resource){if(auto* r=compatResourceObject(resource))r->version++;}
static void compatUpdateBacking(void* dst,uint32_t sub,const CompatD3D11Box* box,const void* src,uint32_t srcRow,uint32_t srcDepth){
 if(!dst||!src)return;
 auto* r=compatResourceObject(dst);if(!r)return;
 CompatMappedSubresource m{};if(!compatMapResourceBackingSubresource(dst,sub,&m)||!m.pData)return;
 size_t base=(size_t)((uint8_t*)m.pData-r->backing.data());if(base>=r->backing.size())return;size_t cap=r->backing.size()-base;

 if(r->vtbl==gCompatTexture2DVtable&&r->descSize>=44&&m.rowPitch){
   const uint32_t* d=(const uint32_t*)r->desc;
   uint32_t mips=std::max(1u,d[2]),mip=sub%mips;
   uint32_t mw=std::max(1u,d[0]>>std::min(mip,31u)),mh=std::max(1u,d[1]>>std::min(mip,31u));
   uint32_t bw=1,bh=1,bpb=4;compatFormatLayout(d[4],bw,bh,bpb);
   uint32_t left=0,top=0,right=mw,bottom=mh;
   if(box){
     left=std::min(box->left,mw);top=std::min(box->top,mh);
     right=std::min(std::max(box->right,left),mw);bottom=std::min(std::max(box->bottom,top),mh);
   }
   uint32_t lb=left/bw,tb=top/bh,rb=(right+bw-1)/bw,bb=(bottom+bh-1)/bh;
   uint32_t copyBytes=(rb>lb)?(rb-lb)*bpb:0,copyRows=(bb>tb)?(bb-tb):0;
   uint32_t sr=srcRow?srcRow:copyBytes;
   uint8_t* dp=(uint8_t*)m.pData+(size_t)tb*m.rowPitch+(size_t)lb*bpb;
   const uint8_t* sp=(const uint8_t*)src;
   for(uint32_t y=0;y<copyRows;y++){
     size_t dstOff=(size_t)(dp-(uint8_t*)m.pData)+(size_t)y*m.rowPitch;
     if(dstOff>=cap)break;
     size_t n=std::min<size_t>(copyBytes,std::min<size_t>(sr,cap-dstOff));
     if(n)std::memcpy(dp+(size_t)y*m.rowPitch,sp+(size_t)y*sr,n);
   }
   static std::atomic<uint32_t> boxBudget{256};uint32_t b=boxBudget.fetch_sub(1,std::memory_order_relaxed);
   if(b>0){char q[224];snprintf(q,sizeof(q),"tex=%p sub=%u fmt=%u box=%u,%u-%u,%u blocks=%ux%u srcRow=%u dstRow=%u",dst,sub,d[4],left,top,right,bottom,rb-lb,bb-tb,sr,m.rowPitch);gtavdiag::checkpoint("native-texture-update-region",q);}
 }else{
   size_t begin=0,end=cap;
   if(box){begin=std::min<size_t>(box->left,cap);end=std::min<size_t>(std::max(box->right,box->left),cap);}
   size_t n=end>begin?end-begin:0;if(n)std::memcpy((uint8_t*)m.pData+begin,src,std::min<size_t>(n,srcDepth?srcDepth:(srcRow?srcRow:n)));
 }
 r->version++;
}

static void compatCopySubresourceBacking(void* dst,uint32_t dstSub,uint32_t dstX,uint32_t dstY,uint32_t dstZ,void* src,uint32_t srcSub,const CompatD3D11Box* srcBox){
 (void)dstZ;
 auto* d=compatResourceObject(dst);auto* s=compatResourceObject(src);if(!d||!s)return;
 if(d->vtbl==gCompatTexture2DVtable&&s->vtbl==gCompatTexture2DVtable&&d->descSize>=44&&s->descSize>=44){
   CompatMappedSubresource dm{},sm{};if(!compatMapResourceBackingSubresource(dst,dstSub,&dm)||!compatMapResourceBackingSubresource(src,srcSub,&sm))return;
   const uint32_t* dd=(const uint32_t*)d->desc;const uint32_t* sd=(const uint32_t*)s->desc;
   if(dd[4]!=sd[4]){gtavdiag::checkpoint("native-copy-subresource-format-mismatch");return;}
   uint32_t smips=std::max(1u,sd[2]),smip=srcSub%smips;
   uint32_t sw=std::max(1u,sd[0]>>std::min(smip,31u)),sh=std::max(1u,sd[1]>>std::min(smip,31u));
   uint32_t bw=1,bh=1,bpb=4;compatFormatLayout(sd[4],bw,bh,bpb);
   uint32_t l=srcBox?std::min(srcBox->left,sw):0,t=srcBox?std::min(srcBox->top,sh):0;
   uint32_t r=srcBox?std::min(std::max(srcBox->right,l),sw):sw,b=srcBox?std::min(std::max(srcBox->bottom,t),sh):sh;
   uint32_t lb=l/bw,tb=t/bh,rb=(r+bw-1)/bw,bb=(b+bh-1)/bh;
   uint32_t dlb=dstX/bw,dtb=dstY/bh,bytes=(rb-lb)*bpb,rows=bb-tb;
   uint8_t* dp=(uint8_t*)dm.pData+(size_t)dtb*dm.rowPitch+(size_t)dlb*bpb;
   const uint8_t* sp=(const uint8_t*)sm.pData+(size_t)tb*sm.rowPitch+(size_t)lb*bpb;
   size_t dbase=(size_t)((uint8_t*)dm.pData-d->backing.data()),sbase=(size_t)((uint8_t*)sm.pData-s->backing.data());
   for(uint32_t y=0;y<rows;y++){
     size_t doff=dbase+(size_t)(dp-(uint8_t*)dm.pData)+(size_t)y*dm.rowPitch;
     size_t soff=sbase+(size_t)(sp-(const uint8_t*)sm.pData)+(size_t)y*sm.rowPitch;
     if(doff>=d->backing.size()||soff>=s->backing.size())break;
     size_t n=std::min<size_t>(bytes,std::min(d->backing.size()-doff,s->backing.size()-soff));
     if(n)std::memmove(dp+(size_t)y*dm.rowPitch,sp+(size_t)y*sm.rowPitch,n);
   }
   d->version++;
   char q[224];snprintf(q,sizeof(q),"src=%p[%u] box=%u,%u-%u,%u dst=%p[%u] at=%u,%u fmt=%u",src,srcSub,l,t,r,b,dst,dstSub,dstX,dstY,sd[4]);gtavdiag::checkpoint("native-copy-subresource-region-applied",q);
   return;
 }
 if(d->vtbl==gCompatBufferVtable&&s->vtbl==gCompatBufferVtable){
   size_t sl=srcBox?srcBox->left:0,sr=srcBox?srcBox->right:s->backing.size();sl=std::min(sl,s->backing.size());sr=std::min(std::max(sr,sl),s->backing.size());
   size_t n=std::min(sr-sl,d->backing.size()>dstX?d->backing.size()-dstX:0);if(n){std::memmove(d->backing.data()+dstX,s->backing.data()+sl,n);d->version++;}
 }
}
static void compatCopyBacking(void* dst,void* src){auto* d=compatResourceObject(dst);auto* s=compatResourceObject(src);if(!d||!s)return;if(d->backing.size()<s->backing.size())d->backing.resize(s->backing.size());if(!s->backing.empty())std::memcpy(d->backing.data(),s->backing.data(),s->backing.size());d->version++;}
static void compatResolveBacking(void* dst,uint32_t ds,void* src,uint32_t ss){auto* dr=compatResourceObject(dst);auto* sr=compatResourceObject(src);if(!dr||!sr)return;CompatMappedSubresource d{},s{};if(!compatMapResourceBackingSubresource(dst,ds,&d)||!compatMapResourceBackingSubresource(src,ss,&s))return;size_t n=std::min<size_t>(d.depthPitch?d.depthPitch:d.rowPitch,s.depthPitch?s.depthPitch:s.rowPitch);if(n){std::memcpy(d.pData,s.pData,n);dr->version++;}}
struct CompatPendingNativeColorClear { float color[4]{}; };
static std::mutex gCompatPendingClearMutex;
static std::unordered_map<void*,CompatPendingNativeColorClear> gCompatPendingNativeColorClears;
static void compatClearRTVBacking(void* view,const float* color){
 if(!view||!color)return;
 auto* v=compatViewObject(view);if(!v)return;
 void* resource=v->resource;
 if(auto* r=compatResourceObject(resource)){
   for(int i=0;i<4;i++)r->pendingClearColor[i]=color[i];
   r->pendingClearFlags|=0x100u;
   if(!r->backing.empty()){
     uint32_t fmt=r->descSize>=20?((uint32_t*)r->desc)[4]:28;
     if(fmt==28||fmt==29||fmt==87||fmt==88){
       uint8_t q[4];for(int i=0;i<4;i++){float x=std::max(0.0f,std::min(1.0f,color[i]));q[i]=(uint8_t)(x*255.0f+0.5f);}
       if(fmt==87||fmt==88)std::swap(q[0],q[2]);
       for(size_t i=0;i+4<=r->backing.size();i+=4)std::memcpy(r->backing.data()+i,q,4);
     }else if(color[0]==0&&color[1]==0&&color[2]==0&&color[3]==0)std::memset(r->backing.data(),0,r->backing.size());
   }
   return;
 }
 // Swapchain/backbuffer and other foreign D3D resources are not CompatResourceObject.
 // Preserve D3D11 ClearRenderTargetView semantics instead of silently dropping the clear.
 {
   std::lock_guard<std::mutex> l(gCompatPendingClearMutex);
   auto& pc=gCompatPendingNativeColorClears[resource];
   for(int i=0;i<4;i++)pc.color[i]=color[i];
 }
 char d[128];snprintf(d,sizeof(d),"view=%p resource=%p rgba=%.3f,%.3f,%.3f,%.3f",view,resource,color[0],color[1],color[2],color[3]);
 gtavdiag::checkpoint("native-pending-foreign-rtv-clear",d);
}
static void compatClearDSVBacking(void* view,uint32_t flags,float depth,uint8_t stencil){
 if(!view)return;auto* v=compatViewObject(view);if(!v)return;auto* r=compatResourceObject(v->resource);if(!r)return;r->pendingClearFlags|=(flags&3u);r->pendingClearDepth=depth;r->pendingClearStencil=stencil;
 if(r->backing.empty())return;uint32_t fmt=r->descSize>=20?((uint32_t*)r->desc)[4]:0;
 if((flags&1)&&fmt==40){for(size_t i=0;i+4<=r->backing.size();i+=4)std::memcpy(r->backing.data()+i,&depth,4);}
 else if((flags&1)&&fmt==45){uint32_t d=(uint32_t)(std::max(0.0f,std::min(1.0f,depth))*16777215.0f);uint32_t p=(d&0xffffffu)|((uint32_t)stencil<<24);for(size_t i=0;i+4<=r->backing.size();i+=4)std::memcpy(r->backing.data()+i,&p,4);}
}
static int32_t compatCreateBuffer(void*,const void* desc,const void* init,void** out){
  if(!out)return (int32_t)0x80004003u;auto* o=makeCompatResource(desc,24,"compat-d3d11-create-buffer",gCompatBufferVtable);*out=o;
  if(init&&o){const void* p=nullptr;uint32_t pitch=0,slice=0;std::memcpy(&p,init,8);std::memcpy(&pitch,(const uint8_t*)init+8,4);std::memcpy(&slice,(const uint8_t*)init+12,4);if(p&&!o->backing.empty())std::memcpy(o->backing.data(),p,std::min<size_t>(o->backing.size(),slice?slice:(pitch?pitch:o->backing.size())));}
  return 0;
}
static int32_t compatCreateTexture1D(void*,const void* desc,const void*,void** out){
  if(!out)return (int32_t)0x80004003u;*out=makeCompatResource(desc,32,"compat-d3d11-create-texture1d",gCompatTexture1DVtable);return 0;
}
static int32_t compatCreateTexture2D(void*,const void* desc,const void* init,void** out){
  if(!out)return (int32_t)0x80004003u;auto* o=makeCompatResource(desc,44,"compat-d3d11-create-texture2d",gCompatTexture2DVtable);*out=o;
  if(init&&desc&&o){const uint32_t* d=(const uint32_t*)desc;uint32_t count=std::min<uint32_t>(std::max(1u,d[2])*std::max(1u,d[3]),4096u);for(uint32_t s=0;s<count;s++){const uint8_t* sd=(const uint8_t*)init+s*16;const void* p=nullptr;uint32_t row=0,slice=0;std::memcpy(&p,sd,8);std::memcpy(&row,sd+8,4);std::memcpy(&slice,sd+12,4);if(p)compatUpdateBacking(o,s,nullptr,p,row,slice);}}
  return 0;
}
static int32_t compatCreateTexture3D(void*,const void* desc,const void*,void** out){
  if(!out)return (int32_t)0x80004003u;*out=makeCompatResource(desc,36,"compat-d3d11-create-texture3d",gCompatTexture3DVtable);return 0;
}
static int32_t compatCreateSRV(void*,void* resource,const void* desc,void** out){
  if(!out)return (int32_t)0x80004003u;*out=makeCompatView(resource,desc,24,"compat-d3d11-create-srv");return 0;
}
static int32_t compatCreateUAV(void*,void* resource,const void* desc,void** out){
  if(!out)return (int32_t)0x80004003u;*out=makeCompatView(resource,desc,20,"compat-d3d11-create-uav");return 0;
}
static int32_t compatCreateRTV(void*,void* resource,const void* desc,void** out){
  if(!out)return (int32_t)0x80004003u;*out=makeCompatView(resource,desc,20,"compat-d3d11-create-rtv");return 0;
}
static int32_t compatCreateDSV(void*,void* resource,const void* desc,void** out){
  if(!out)return (int32_t)0x80004003u;*out=makeCompatView(resource,desc,24,"compat-d3d11-create-dsv");return 0;
}

struct CompatStateObject { void** vtbl; size_t descSize; uint8_t desc[320]; };
struct CompatQueryObject { void** vtbl; uint32_t query; uint32_t miscFlags; std::atomic<uint32_t> ended{0}; };
static void* gCompatStateVtable[16]{};
static void* gCompatQueryVtable[16]{};
static std::vector<CompatStateObject*> gCompatStates;
static std::vector<CompatQueryObject*> gCompatQueries;
static void compatStateGetDesc(void* self,void* out){ if(self&&out){auto* o=(CompatStateObject*)self;std::memcpy(out,o->desc,o->descSize);} }
static uint32_t compatQueryDataSize(uint32_t q){
  switch(q){case 0:return 4;case 1:case 2:return 8;case 3:return 16;case 4:return 88;case 5:return 4;case 6:return 16;case 7:return 4;default:return 8;}
}
static uint32_t compatQueryGetDataSize(void* self){return self?compatQueryDataSize(((CompatQueryObject*)self)->query):0;}
static void compatQueryGetDesc(void* self,void* out){ if(self&&out){auto* q=(CompatQueryObject*)self;((uint32_t*)out)[0]=q->query;((uint32_t*)out)[1]=q->miscFlags;} }
static void initCompatStateVtables(){
  static bool once=false;if(once)return;once=true;
  {void* slots[16]={(void*)compatStateSlot0,(void*)compatStateSlot1,(void*)compatStateSlot2,(void*)compatStateSlot3,(void*)compatStateSlot4,(void*)compatStateSlot5,(void*)compatStateSlot6,(void*)compatStateSlot7,(void*)compatStateSlot8,(void*)compatStateSlot9,(void*)compatStateSlot10,(void*)compatStateSlot11,(void*)compatStateSlot12,(void*)compatStateSlot13,(void*)compatStateSlot14,(void*)compatStateSlot15};for(int i=0;i<16;i++)gCompatStateVtable[i]=slots[i];}
  {void* slots[16]={(void*)compatQuerySlot0,(void*)compatQuerySlot1,(void*)compatQuerySlot2,(void*)compatQuerySlot3,(void*)compatQuerySlot4,(void*)compatQuerySlot5,(void*)compatQuerySlot6,(void*)compatQuerySlot7,(void*)compatQuerySlot8,(void*)compatQuerySlot9,(void*)compatQuerySlot10,(void*)compatQuerySlot11,(void*)compatQuerySlot12,(void*)compatQuerySlot13,(void*)compatQuerySlot14,(void*)compatQuerySlot15};for(int i=0;i<16;i++)gCompatQueryVtable[i]=slots[i];}
  for(void** t:{gCompatStateVtable,gCompatQueryVtable}){t[0]=(void*)compatChildQI;t[1]=(void*)compatChildAddRef;t[2]=(void*)compatChildRelease;t[3]=(void*)compatChildGetDevice;t[4]=(void*)compatChildGetPrivateData;t[5]=(void*)compatSetPrivateData;t[6]=(void*)compatChildSetPrivateDataInterface;}
  // All ID3D11DeviceChild-derived state/query objects require slot 6
  // SetPrivateDataInterface. Returning E_NOTIMPL here was consumed as a pointer
  // by libgtav after Draw and caused the 0x80004001 crash.
  gCompatStateVtable[7]=(void*)compatStateGetDesc;
  gCompatQueryVtable[7]=(void*)compatQueryGetDataSize;
  gCompatQueryVtable[8]=(void*)compatQueryGetDesc;
}
static int32_t makeCompatState(const void* desc,size_t bytes,void** out,const char* cp){
  gtavdiag::checkpoint(cp); if(!out)return (int32_t)0x80004003u; initCompatStateVtables();
  auto* o=new CompatStateObject{};o->vtbl=gCompatStateVtable;o->descSize=std::min(bytes,sizeof(o->desc));if(desc)std::memcpy(o->desc,desc,o->descSize);
  {std::lock_guard<std::mutex> l(gCompatObjectMutex);gCompatStates.push_back(o);}*out=o;return 0;
}
static CompatStateObject* compatStateObject(void* p){
 if(!p)return nullptr;std::lock_guard<std::mutex> l(gCompatObjectMutex);auto* s=(CompatStateObject*)p;
 return std::find(gCompatStates.begin(),gCompatStates.end(),s)!=gCompatStates.end()?s:nullptr;
}
static int32_t compatCreateBlendState(void*,const void* d,void** o){return makeCompatState(d,264,o,"compat-d3d11-create-blend-state");}
static int32_t compatCreateDepthStencilState(void*,const void* d,void** o){return makeCompatState(d,52,o,"compat-d3d11-create-depth-stencil-state");}
static int32_t compatCreateRasterizerState(void*,const void* d,void** o){return makeCompatState(d,40,o,"compat-d3d11-create-rasterizer-state");}
static int32_t compatCreateSamplerState(void*,const void* d,void** o){return makeCompatState(d,52,o,"compat-d3d11-create-sampler-state");}
static int32_t compatCreateQuery(void*,const void* d,void** out){
  gtavdiag::checkpoint("compat-d3d11-create-query");if(!out)return (int32_t)0x80004003u;initCompatStateVtables();
  auto* q=new CompatQueryObject{};q->vtbl=gCompatQueryVtable;if(d){q->query=((const uint32_t*)d)[0];q->miscFlags=((const uint32_t*)d)[1];}
  {std::lock_guard<std::mutex> l(gCompatObjectMutex);gCompatQueries.push_back(q);}*out=q;return 0;
}
static int32_t compatCreatePredicate(void* d,const void* q,void** out){gtavdiag::checkpoint("compat-d3d11-create-predicate");return compatCreateQuery(d,q,out);}
static void compatContextBegin(void*,void* q){gtavdiag::checkpoint("compat-d3d11-query-begin");if(q)((CompatQueryObject*)q)->ended.store(0);}
static void compatContextEnd(void*,void* q){gtavdiag::checkpoint("compat-d3d11-query-end");if(q)((CompatQueryObject*)q)->ended.store(1);}
static int32_t compatContextGetData(void*,void* q,void* data,uint32_t bytes,uint32_t){
  gtavdiag::checkpoint("compat-d3d11-query-get-data");
  if(!q)return 0;
  auto* cq=(CompatQueryObject*)q;
  if(!cq->ended.load())return 1;
  if(data&&bytes){
    std::memset(data,0,bytes);
    uint32_t need=compatQueryDataSize(cq->query);
    if(bytes>=4 && (cq->query==0 || cq->query==5 || cq->query==7)) *(uint32_t*)data=1;
    else if(bytes>=8 && (cq->query==1 || cq->query==2)) *(uint64_t*)data=1;
    else if(cq->query==3 && bytes>=16){((uint64_t*)data)[0]=1000000000ull;((uint32_t*)data)[2]=0;}
    (void)need;
  }
  return 0;
}
static VkFormat compatDxgiFormat(uint32_t f){
 switch(f){
  // R32G32B32A32
  case 1:case 2:return VK_FORMAT_R32G32B32A32_SFLOAT;
  case 3:return VK_FORMAT_R32G32B32A32_UINT;
  case 4:return VK_FORMAT_R32G32B32A32_SINT;
  // R32G32B32
  case 5:case 6:return VK_FORMAT_R32G32B32_SFLOAT;
  case 7:return VK_FORMAT_R32G32B32_UINT;
  case 8:return VK_FORMAT_R32G32B32_SINT;
  // R16G16B16A16
  case 9:case 10:return VK_FORMAT_R16G16B16A16_SFLOAT;
  case 11:return VK_FORMAT_R16G16B16A16_UNORM;
  case 12:return VK_FORMAT_R16G16B16A16_UINT;
  case 13:return VK_FORMAT_R16G16B16A16_SNORM;
  case 14:return VK_FORMAT_R16G16B16A16_SINT;
  // R32G32
  case 15:case 16:return VK_FORMAT_R32G32_SFLOAT;
  case 17:return VK_FORMAT_R32G32_UINT;
  case 18:return VK_FORMAT_R32G32_SINT;
  // R32G8X24 families. Vulkan has no exact typeless equivalent; choose the
  // depth/stencil representation used by D3D views of these resources.
  case 19:case 20:return VK_FORMAT_D32_SFLOAT_S8_UINT;
  case 21:return VK_FORMAT_D32_SFLOAT;
  case 22:return VK_FORMAT_S8_UINT;
  // packed HDR/color
  case 23:case 24:return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  case 25:return VK_FORMAT_A2B10G10R10_UINT_PACK32;
  case 26:return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
  // R8G8B8A8
  case 27:case 28:return VK_FORMAT_R8G8B8A8_UNORM;
  case 29:return VK_FORMAT_R8G8B8A8_SRGB;
  case 30:return VK_FORMAT_R8G8B8A8_UINT;
  case 31:return VK_FORMAT_R8G8B8A8_SNORM;
  case 32:return VK_FORMAT_R8G8B8A8_SINT;
  // R16G16
  case 33:case 34:return VK_FORMAT_R16G16_SFLOAT;
  case 35:return VK_FORMAT_R16G16_UNORM;
  case 36:return VK_FORMAT_R16G16_UINT;
  case 37:return VK_FORMAT_R16G16_SNORM;
  case 38:return VK_FORMAT_R16G16_SINT;
  // R32 / depth aliases
  case 39:case 40:return VK_FORMAT_D32_SFLOAT;
  case 41:return VK_FORMAT_R32_SFLOAT;
  case 42:return VK_FORMAT_R32_UINT;
  case 43:return VK_FORMAT_R32_SINT;
  // D24S8 / view aliases
  case 44:case 45:return VK_FORMAT_D24_UNORM_S8_UINT;
  case 46:return VK_FORMAT_D24_UNORM_S8_UINT;
  case 47:return VK_FORMAT_S8_UINT;
  // R8G8
  case 48:case 49:return VK_FORMAT_R8G8_UNORM;
  case 50:return VK_FORMAT_R8G8_UINT;
  case 51:return VK_FORMAT_R8G8_SNORM;
  case 52:return VK_FORMAT_R8G8_SINT;
  // R16 / D16
  case 53:case 54:return VK_FORMAT_R16_SFLOAT;
  case 55:return VK_FORMAT_D16_UNORM;
  case 56:return VK_FORMAT_R16_UNORM;
  case 57:return VK_FORMAT_R16_UINT;
  case 58:return VK_FORMAT_R16_SNORM;
  case 59:return VK_FORMAT_R16_SINT;
  // R8 / A8
  case 60:case 61:return VK_FORMAT_R8_UNORM;
  case 62:return VK_FORMAT_R8_UINT;
  case 63:return VK_FORMAT_R8_SNORM;
  case 64:return VK_FORMAT_R8_SINT;
  case 65:return VK_FORMAT_R8_UNORM;
  case 67:return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
  // BCn
  case 70:case 71:return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
  case 72:return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
  case 73:case 74:return VK_FORMAT_BC2_UNORM_BLOCK;
  case 75:return VK_FORMAT_BC2_SRGB_BLOCK;
  case 76:case 77:return VK_FORMAT_BC3_UNORM_BLOCK;
  case 78:return VK_FORMAT_BC3_SRGB_BLOCK;
  case 79:case 80:return VK_FORMAT_BC4_UNORM_BLOCK;
  case 81:return VK_FORMAT_BC4_SNORM_BLOCK;
  case 82:case 83:return VK_FORMAT_BC5_UNORM_BLOCK;
  case 84:return VK_FORMAT_BC5_SNORM_BLOCK;
  // legacy BGRA/BGRX
  case 85:return VK_FORMAT_R5G6B5_UNORM_PACK16;
  case 86:return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
  case 87:case 88:case 90:case 92:return VK_FORMAT_B8G8R8A8_UNORM;
  case 89:return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  case 91:case 93:return VK_FORMAT_B8G8R8A8_SRGB;
  // BC6/7
  case 94:case 95:return VK_FORMAT_BC6H_UFLOAT_BLOCK;
  case 96:return VK_FORMAT_BC6H_SFLOAT_BLOCK;
  case 97:case 98:return VK_FORMAT_BC7_UNORM_BLOCK;
  case 99:return VK_FORMAT_BC7_SRGB_BLOCK;
  default:return VK_FORMAT_UNDEFINED;
 }
}
static int32_t compatCheckFormatSupport(void*,uint32_t fmt,uint32_t* out){
  gtavdiag::checkpoint("compat-d3d11-check-format-support");if(!out)return (int32_t)0x80004003u;
  if(compatDxgiFormat(fmt)==VK_FORMAT_UNDEFINED){*out=0;return (int32_t)0x80070057u;}
  *out=0x1u|0x2u|0x20u|0x100u|0x200u|0x4000u|0x10000u;return 0;
}
static int32_t compatCheckMSAA(void*,uint32_t fmt,uint32_t samples,uint32_t* out){
  gtavdiag::checkpoint("compat-d3d11-check-msaa");if(!out)return (int32_t)0x80004003u;*out=0;
  if(compatDxgiFormat(fmt)==VK_FORMAT_UNDEFINED||samples!=1)return (int32_t)0x80070057u;*out=1;return 0;
}
static int32_t compatCheckFeatureSupport(void*,uint32_t feature,void* data,uint32_t bytes){
  gtavdiag::checkpoint("compat-d3d11-check-feature-support");if(data&&bytes)std::memset(data,0,bytes);
  return 0;
}

static int32_t compatSetPrivateData(void*, const void*, uint32_t, const void*) {
  // grcEffect::SetPIXLabel calls ID3D11DeviceChild::SetPrivateData (+0x28)
  // on shader/resource child objects, not on the immediate context.
  gtavdiag::checkpoint("compat-d3d11-set-private-data");
  return 0;
}
static int32_t compatAdapterCheckInterfaceSupport(void*, const void*, int64_t* version) {
  gtavdiag::checkpoint("compat-dxgi-adapter-check-interface-support");
  if(version) *version=0;
  // DXGI_ERROR_UNSUPPORTED: callers must take their fallback instead of consuming
  // an uninitialised output object/version.
  return (int32_t)0x887A0004u;
}
static int32_t compatDXGIDeviceGetParent(void* self, const void*, void** out) {
  gtavdiag::checkpoint("compat-dxgi-device-get-parent");
  if(self!=&gCompatDXGIDevice){
    // This slot is also reached by a non-COM ABI path in libgtav that consumes
    // x0 as a pointer. Returning an HRESULT there becomes the bogus address
    // 0x80004002. Return null instead so the foreign path can take its fallback.
    gtavdiag::checkpoint("compat-dxgi-device-get-parent-foreign-call");
    return 0;
  }
  if(!out) return (int32_t)0x80004003u;
  initCompatDXGI();
  *out=&gCompatAdapter;
  return 0;
}
static int32_t compatDXGIDeviceGetAdapter(void* self, void** out) {
  gtavdiag::checkpoint("compat-dxgi-device-get-adapter");
  if(self!=&gCompatDXGIDevice){
    gtavdiag::checkpoint("compat-dxgi-device-get-adapter-foreign-call");
    return 0;
  }
  if(!out) return (int32_t)0x80004003u;
  initCompatDXGI();
  *out=&gCompatAdapter;
  return 0;
}
static int32_t compatDXGIGetParentUnsupported(void*, const void*, void** out) {
  gtavdiag::checkpoint("compat-dxgi-get-parent-unsupported");
  if(out)*out=nullptr;
  // SuppressAltEnter probes additional parent interfaces. Returning E_NOINTERFACE
  // is the correct safe fallback and prevents it consuming a bogus object.
  return (int32_t)0x80004002u;
}

static void initCompatD3D11() {
  static bool once=false; if(once)return; once=true;
  for(void*& p:gD3DDeviceVtable) p=(void*)compatD3DUnsupported;
  for(void*& p:gD3DContextVtable) p=(void*)compatD3DUnsupported;
  gD3DDeviceVtable[3]=(void*)compatCreateBuffer;
  gD3DDeviceVtable[4]=(void*)compatCreateTexture1D;
  gD3DDeviceVtable[5]=(void*)compatCreateTexture2D;
  gD3DDeviceVtable[6]=(void*)compatCreateTexture3D;
  gD3DDeviceVtable[7]=(void*)compatCreateSRV;
  gD3DDeviceVtable[8]=(void*)compatCreateUAV;
  gD3DDeviceVtable[9]=(void*)compatCreateRTV;
  gD3DDeviceVtable[10]=(void*)compatCreateDSV;
  gD3DDeviceVtable[11]=(void*)compatCreateInputLayout;
  gD3DDeviceVtable[12]=(void*)compatDeviceSlot12;
  gD3DDeviceVtable[13]=(void*)compatDeviceSlot13;
  gD3DDeviceVtable[14]=(void*)compatDeviceSlot14;
  gD3DDeviceVtable[15]=(void*)compatDeviceSlot15;
  gD3DDeviceVtable[16]=(void*)compatDeviceSlot16;
  gD3DDeviceVtable[17]=(void*)compatDeviceSlot17;
  gD3DDeviceVtable[18]=(void*)compatDeviceSlot18;
  gD3DDeviceVtable[19]=(void*)compatDeviceSlot19;
  gD3DDeviceVtable[20]=(void*)compatCreateBlendState;
  gD3DDeviceVtable[21]=(void*)compatCreateDepthStencilState;
  gD3DDeviceVtable[22]=(void*)compatCreateRasterizerState;
  gD3DDeviceVtable[23]=(void*)compatCreateSamplerState;
  gD3DDeviceVtable[24]=(void*)compatCreateQuery;
  gD3DDeviceVtable[25]=(void*)compatCreatePredicate;
  gD3DDeviceVtable[26]=(void*)compatDeviceSlot26;
  gD3DDeviceVtable[27]=(void*)compatDeviceSlot27;
  gD3DDeviceVtable[28]=(void*)compatDeviceSlot28;
  gD3DDeviceVtable[29]=(void*)compatCheckFormatSupport;
  gD3DDeviceVtable[30]=(void*)compatCheckMSAA;
  gD3DDeviceVtable[31]=(void*)compatDeviceSlot31;
  gD3DDeviceVtable[32]=(void*)compatDeviceSlot32;
  gD3DDeviceVtable[33]=(void*)compatCheckFeatureSupport;
  gD3DDeviceVtable[34]=(void*)compatDeviceGetPrivateData;
  gD3DDeviceVtable[35]=(void*)compatSetPrivateData;
  gD3DDeviceVtable[36]=(void*)compatDeviceSetPrivateDataInterface;
  gD3DDeviceVtable[37]=(void*)compatDeviceSlot37;
  gD3DDeviceVtable[38]=(void*)compatDeviceGetCreationFlags;
  gD3DDeviceVtable[39]=(void*)compatDeviceRemovedReason;
  gD3DDeviceVtable[40]=(void*)compatDeviceGetImmediateContext;
  gD3DDeviceVtable[41]=(void*)compatDeviceSetExceptionMode;
  gD3DDeviceVtable[42]=(void*)compatDeviceGetExceptionMode;
  gD3DDeviceVtable[43]=(void*)compatDeviceSlot43;
  gD3DDeviceVtable[44]=(void*)compatDeviceSlot44;
  gD3DDeviceVtable[45]=(void*)compatDeviceSlot45;
  gD3DDeviceVtable[46]=(void*)compatDeviceSlot46;
  gD3DDeviceVtable[47]=(void*)compatDeviceSlot47;
  gD3DDeviceVtable[48]=(void*)compatDeviceSlot48;
  gD3DDeviceVtable[49]=(void*)compatDeviceSlot49;
  gD3DDeviceVtable[50]=(void*)compatDeviceSlot50;
  gD3DDeviceVtable[51]=(void*)compatDeviceSlot51;
  gD3DDeviceVtable[52]=(void*)compatDeviceSlot52;
  gD3DDeviceVtable[53]=(void*)compatDeviceSlot53;
  gD3DDeviceVtable[54]=(void*)compatDeviceSlot54;
  gD3DDeviceVtable[55]=(void*)compatDeviceSlot55;
  gD3DDeviceVtable[56]=(void*)compatDeviceSlot56;
  gD3DDeviceVtable[57]=(void*)compatDeviceSlot57;
  gD3DDeviceVtable[58]=(void*)compatDeviceSlot58;
  gD3DDeviceVtable[59]=(void*)compatDeviceSlot59;
  gD3DDeviceVtable[60]=(void*)compatDeviceSlot60;
  gD3DDeviceVtable[61]=(void*)compatDeviceSlot61;
  gD3DDeviceVtable[62]=(void*)compatDeviceSlot62;
  gD3DDeviceVtable[63]=(void*)compatDeviceSlot63;
  gD3DContextVtable[3]=(void*)compatContextSlot3;
  gD3DContextVtable[4]=(void*)compatContextSlot4;
  gD3DContextVtable[5]=(void*)compatContextSlot5;
  gD3DContextVtable[6]=(void*)compatContextSlot6;
  gD3DContextVtable[7]=(void*)compatCtxVSSetConstantBuffers;
  gD3DContextVtable[8]=(void*)compatCtxPSSetShaderResources;
  gD3DContextVtable[9]=(void*)compatCtxPSSetShader;
  gD3DContextVtable[10]=(void*)compatCtxPSSetSamplers;
  gD3DContextVtable[11]=(void*)compatCtxVSSetShader;
  gD3DContextVtable[12]=(void*)compatCtxDrawIndexed;
  gD3DContextVtable[13]=(void*)compatCtxDraw;
  gD3DContextVtable[14]=(void*)compatContextSlot14;
  gD3DContextVtable[15]=(void*)compatContextSlot15;
  gD3DContextVtable[16]=(void*)compatCtxPSSetConstantBuffers;
  gD3DContextVtable[17]=(void*)compatCtxIASetInputLayout;
  gD3DContextVtable[18]=(void*)compatCtxIASetVertexBuffers;
  gD3DContextVtable[19]=(void*)compatCtxIASetIndexBuffer;
  gD3DContextVtable[20]=(void*)compatCtxDrawIndexedInstanced;
  gD3DContextVtable[21]=(void*)compatCtxDrawInstanced;
  gD3DContextVtable[22]=(void*)compatCtxGSSetConstantBuffers;
  gD3DContextVtable[23]=(void*)compatCtxGSSetShader;
  gD3DContextVtable[24]=(void*)compatCtxIASetPrimitiveTopology;
  gD3DContextVtable[25]=(void*)compatCtxVSSetShaderResources;
  gD3DContextVtable[26]=(void*)compatCtxVSSetSamplers;
  gD3DContextVtable[27]=(void*)compatContextBegin;
  gD3DContextVtable[28]=(void*)compatContextEnd;
  gD3DContextVtable[29]=(void*)compatContextGetData;
  gD3DContextVtable[30]=(void*)compatCtxSetPredication;
  gD3DContextVtable[31]=(void*)compatCtxGSSetShaderResources;
  gD3DContextVtable[32]=(void*)compatCtxGSSetSamplers;
  gD3DContextVtable[33]=(void*)compatCtxOMSetRenderTargets;
  gD3DContextVtable[34]=(void*)compatCtxOMSetRTUAV;
  gD3DContextVtable[35]=(void*)compatCtxOMSetBlendState;
  gD3DContextVtable[36]=(void*)compatCtxOMSetDepthStencilState;
  gD3DContextVtable[37]=(void*)compatCtxSOSetTargets;
  gD3DContextVtable[38]=(void*)compatCtxDrawAuto;
  gD3DContextVtable[39]=(void*)compatCtxDrawIndexedInstancedIndirect;
  gD3DContextVtable[40]=(void*)compatCtxDrawInstancedIndirect;
  gD3DContextVtable[41]=(void*)compatCtxDispatch;
  gD3DContextVtable[42]=(void*)compatCtxDispatchIndirect;
  gD3DContextVtable[43]=(void*)compatCtxRSSetState;
  gD3DContextVtable[44]=(void*)compatCtxRSSetViewports;
  gD3DContextVtable[45]=(void*)compatCtxRSSetScissorRects;
  gD3DContextVtable[46]=(void*)compatCtxCopySubresourceRegion;
  gD3DContextVtable[47]=(void*)compatCtxCopyResource;
  gD3DContextVtable[48]=(void*)compatCtxUpdateSubresource;
  gD3DContextVtable[49]=(void*)compatCtxCopyStructureCount;
  gD3DContextVtable[50]=(void*)compatCtxClearRenderTargetView;
  gD3DContextVtable[51]=(void*)compatCtxClearUAVUint;
  gD3DContextVtable[52]=(void*)compatCtxClearUAVFloat;
  gD3DContextVtable[53]=(void*)compatCtxClearDepthStencilView;
  gD3DContextVtable[54]=(void*)compatCtxGenerateMips;
  gD3DContextVtable[55]=(void*)compatCtxSetResourceMinLOD;
  gD3DContextVtable[56]=(void*)compatCtxGetResourceMinLOD;
  gD3DContextVtable[57]=(void*)compatCtxResolveSubresource;
  gD3DContextVtable[58]=(void*)compatCtxExecuteCommandList;
  gD3DContextVtable[59]=(void*)compatCtxHSSetShaderResources;
  gD3DContextVtable[60]=(void*)compatCtxHSSetShader;
  gD3DContextVtable[61]=(void*)compatCtxHSSetSamplers;
  gD3DContextVtable[62]=(void*)compatCtxHSSetConstantBuffers;
  gD3DContextVtable[63]=(void*)compatCtxDSSetShaderResources;
  // Diagnose every remaining ID3D11DeviceContext getter/state slot individually.\n  gD3DContextVtable[64]=(void*)compatContextSlot64;\n  gD3DContextVtable[65]=(void*)compatContextSlot65;\n  gD3DContextVtable[66]=(void*)compatContextSlot66;\n  gD3DContextVtable[67]=(void*)compatContextSlot67;\n  gD3DContextVtable[68]=(void*)compatContextSlot68;\n  gD3DContextVtable[69]=(void*)compatContextSlot69;\n  gD3DContextVtable[70]=(void*)compatContextSlot70;\n  gD3DContextVtable[71]=(void*)compatContextSlot71;\n  gD3DContextVtable[72]=(void*)compatContextSlot72;\n  gD3DContextVtable[73]=(void*)compatContextSlot73;\n  gD3DContextVtable[74]=(void*)compatContextSlot74;\n  gD3DContextVtable[75]=(void*)compatContextSlot75;\n  gD3DContextVtable[76]=(void*)compatContextSlot76;\n  gD3DContextVtable[77]=(void*)compatContextSlot77;\n  gD3DContextVtable[78]=(void*)compatContextSlot78;\n  gD3DContextVtable[79]=(void*)compatContextSlot79;\n  gD3DContextVtable[80]=(void*)compatContextSlot80;\n  gD3DContextVtable[81]=(void*)compatContextSlot81;\n  gD3DContextVtable[82]=(void*)compatContextSlot82;\n  gD3DContextVtable[83]=(void*)compatContextSlot83;\n  gD3DContextVtable[84]=(void*)compatContextSlot84;\n  gD3DContextVtable[85]=(void*)compatContextSlot85;\n  gD3DContextVtable[86]=(void*)compatContextSlot86;\n  gD3DContextVtable[87]=(void*)compatContextSlot87;\n  gD3DContextVtable[88]=(void*)compatContextSlot88;\n  gD3DContextVtable[89]=(void*)compatContextSlot89;\n  gD3DContextVtable[90]=(void*)compatContextSlot90;\n  gD3DContextVtable[91]=(void*)compatContextSlot91;\n  gD3DContextVtable[92]=(void*)compatContextSlot92;\n  gD3DContextVtable[93]=(void*)compatContextSlot93;\n  gD3DContextVtable[94]=(void*)compatContextSlot94;\n  gD3DContextVtable[95]=(void*)compatContextSlot95;\n  gD3DContextVtable[96]=(void*)compatContextSlot96;\n  gD3DContextVtable[97]=(void*)compatContextSlot97;\n  gD3DContextVtable[98]=(void*)compatContextSlot98;\n  gD3DContextVtable[99]=(void*)compatContextSlot99;\n  gD3DContextVtable[100]=(void*)compatContextSlot100;\n  gD3DContextVtable[101]=(void*)compatContextSlot101;\n  gD3DContextVtable[102]=(void*)compatContextSlot102;\n  gD3DContextVtable[103]=(void*)compatContextSlot103;\n  gD3DContextVtable[104]=(void*)compatContextSlot104;\n  gD3DContextVtable[105]=(void*)compatContextSlot105;\n  gD3DContextVtable[106]=(void*)compatContextSlot106;\n  gD3DContextVtable[107]=(void*)compatContextSlot107;\n  gD3DContextVtable[108]=(void*)compatContextSlot108;\n  gD3DContextVtable[109]=(void*)compatContextSlot109;\n  gD3DContextVtable[110]=(void*)compatContextSlot110;\n  gD3DContextVtable[111]=(void*)compatContextSlot111;\n  gD3DContextVtable[112]=(void*)compatContextSlot112;\n  gD3DContextVtable[113]=(void*)compatContextSlot113;\n  gD3DContextVtable[114]=(void*)compatContextSlot114;\n  gD3DContextVtable[115]=(void*)compatContextSlot115;\n  gD3DContextVtable[116]=(void*)compatContextSlot116;\n  gD3DContextVtable[117]=(void*)compatContextSlot117;\n  gD3DContextVtable[118]=(void*)compatContextSlot118;\n  gD3DContextVtable[119]=(void*)compatContextSlot119;\n  gD3DContextVtable[120]=(void*)compatContextSlot120;\n  gD3DContextVtable[121]=(void*)compatContextSlot121;\n  gD3DContextVtable[122]=(void*)compatContextSlot122;\n  gD3DContextVtable[123]=(void*)compatContextSlot123;\n  gD3DContextVtable[124]=(void*)compatContextSlot124;\n  gD3DContextVtable[125]=(void*)compatContextSlot125;\n  gD3DContextVtable[126]=(void*)compatContextSlot126;\n  gD3DContextVtable[127]=(void*)compatContextSlot127;\n
  gD3DContextVtable[64]=(void*)compatCtxDSSetShader;
  gD3DContextVtable[65]=(void*)compatCtxDSSetSamplers;
  gD3DContextVtable[66]=(void*)compatCtxDSSetConstantBuffers;
  gD3DContextVtable[67]=(void*)compatCtxCSSetShaderResources;
  gD3DContextVtable[68]=(void*)compatCtxCSSetUnorderedAccessViews;
  gD3DContextVtable[69]=(void*)compatCtxCSSetShader;
  gD3DContextVtable[70]=(void*)compatCtxCSSetSamplers;
  gD3DContextVtable[71]=(void*)compatCtxCSSetConstantBuffers;

  // ID3D11DeviceContext: Map=14, Unmap=15.
  gD3DContextVtable[14]=(void*)compatD3DMap;
  gD3DContextVtable[15]=(void*)compatD3DUnmap;
  gD3DDeviceVtable[0]=(void*)compatD3DQueryInterface;
  gD3DDeviceVtable[1]=(void*)compatD3DAddRef;
  gD3DDeviceVtable[2]=(void*)compatD3DRelease;
  // RetrieveVideoMemory exact trace:
  // device QI -> returned interface slot 6/+0x30 GetParent -> adapter slot 8/+0x40 GetDesc.
  {void* slots[16]={(void*)compatDXGIDeviceSlot0,(void*)compatDXGIDeviceSlot1,(void*)compatDXGIDeviceSlot2,(void*)compatDXGIDeviceSlot3,(void*)compatDXGIDeviceSlot4,(void*)compatDXGIDeviceSlot5,(void*)compatDXGIDeviceSlot6,(void*)compatDXGIDeviceSlot7,(void*)compatDXGIDeviceSlot8,(void*)compatDXGIDeviceSlot9,(void*)compatDXGIDeviceSlot10,(void*)compatDXGIDeviceSlot11,(void*)compatDXGIDeviceSlot12,(void*)compatDXGIDeviceSlot13,(void*)compatDXGIDeviceSlot14,(void*)compatDXGIDeviceSlot15};for(int i=0;i<16;i++)gDXGIDeviceVtable[i]=slots[i];}
  gDXGIDeviceVtable[0]=(void*)compatD3DQueryInterface;
  gDXGIDeviceVtable[1]=(void*)compatD3DAddRef;
  gDXGIDeviceVtable[2]=(void*)compatD3DRelease;
  gDXGIDeviceVtable[3]=(void*)compatDXGISetPrivateData;
  gDXGIDeviceVtable[4]=(void*)compatDXGISetPrivateDataInterface;
  gDXGIDeviceVtable[5]=(void*)compatDXGIGetPrivateData;
  gDXGIDeviceVtable[6]=(void*)compatDXGIDeviceGetParent;
  gDXGIDeviceVtable[7]=(void*)compatDXGIDeviceGetAdapter;
  gCompatDXGIDevice.vtbl=gDXGIDeviceVtable;
  // ID3D11Device::GetFeatureLevel is slot 37 / byte offset 0x128.
  // libgtav's grcDevice::GetDXFeatureLevelSupported consumes this exact slot.
  // ID3D11Device shader creation slots used by grcProgram::CreateShader:
  // VS=12/+0x60, GS=13/+0x68, PS=15/+0x78, HS=16/+0x80,
  // DS=17/+0x88, CS=18/+0x90.
  gD3DDeviceVtable[12]=(void*)compatCreateShader;
  gD3DDeviceVtable[13]=(void*)compatCreateShader;
  gD3DDeviceVtable[15]=(void*)compatCreateShader;
  gD3DDeviceVtable[16]=(void*)compatCreateShader;
  gD3DDeviceVtable[17]=(void*)compatCreateShader;
  gD3DDeviceVtable[18]=(void*)compatCreateShader;
  gD3DDeviceVtable[37]=(void*)compatD3DGetFeatureLevel;
  gD3DContextVtable[0]=(void*)compatD3DQueryInterface;
  gD3DContextVtable[1]=(void*)compatD3DAddRef;
  gD3DContextVtable[2]=(void*)compatD3DRelease;
   gCompatD3DDevice.vtbl=gD3DDeviceVtable;
  gCompatD3DContext.vtbl=gD3DContextVtable;
}
}
extern "C" __attribute__((visibility("default"))) int32_t D3D11CreateDevice(
    void*,uint32_t,void*,uint32_t,const uint32_t*,uint32_t,uint32_t,
    void** device,uint32_t* featureLevel,void** context) {
  gtavdiag::checkpoint("compat-d3d11-create-device");
  initCompatD3D11();
  if(device)*device=&gCompatD3DDevice;
  if(featureLevel)*featureLevel=0xb000u;
  if(context)*context=&gCompatD3DContext;
  return 0;
}
namespace {
extern "C" void gtav_native_renderer_begin_frame();
struct CompatSwapChainObject { void** vtbl; };
static CompatSwapChainObject gCompatSwapChain{};
static void* gSwapChainVtable[32]{};
static int32_t compatSwapUnsupported(void*,...) {
  gtavdiag::checkpoint("compat-swapchain-unsupported");
  return (int32_t)0x80004001u;
}
static int32_t compatSwapQueryInterface(void* self,const void*,void** out) {
  gtavdiag::checkpoint("compat-swapchain-query-interface");
  if(!out)return (int32_t)0x80004003u; *out=self; return 0;
}
static uint32_t compatSwapAddRef(void*){return 2;}
static uint32_t compatSwapRelease(void*){return 1;}
static int32_t compatSwapPresent(void*,uint32_t syncInterval,uint32_t flags) {
  static std::atomic<uint32_t> presents{0};
  uint32_t n=presents.fetch_add(1,std::memory_order_relaxed)+1;
  if(n<=8 || (n%120)==0) {
    gtavdiag::checkpoint("compat-swapchain-present");
    __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-PRESENT","present=%u sync=%u flags=0x%x",n,syncInterval,flags);
  }
  // GTA's native Vulkan runtime owns the real Android surface/swapchain. The
  // compatibility swapchain is only the D3D11-shaped engine frontend; never
  // create or present a second Android/HWUI surface here.
  gtav_native_renderer_begin_frame();
  return 0;
}
static int32_t compatSwapGetDevice(void*,const void*,void** out){
  if(!out)return (int32_t)0x80004003u; *out=&gCompatD3DDevice; return 0;
}
static int32_t compatSwapSetFullscreenState(void*,int,void*){return 0;}
static int32_t compatSwapGetFullscreenState(void*,int* fullscreen,void** output){
  if(fullscreen)*fullscreen=0; if(output)*output=nullptr; return 0;
}
static std::atomic<uint32_t> gCompatSwapWidth{1920},gCompatSwapHeight{1080},gCompatSwapFormat{28};
static int32_t compatSwapResizeBuffers(void*,uint32_t count,uint32_t width,uint32_t height,uint32_t format,uint32_t flags){
  if(width)gCompatSwapWidth.store(width,std::memory_order_relaxed);
  if(height)gCompatSwapHeight.store(height,std::memory_order_relaxed);
  if(format)gCompatSwapFormat.store(format,std::memory_order_relaxed);
  gtavdiag::checkpoint("compat-swapchain-resize-buffers");
  __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-PRESENT","resize count=%u extent=%ux%u fmt=%u flags=0x%x",count,width,height,format,flags);
  return 0;
}
static int32_t compatSwapResizeTarget(void*,const void*){return 0;}
static int32_t compatSwapGetFrameStatistics(void*,void*){return (int32_t)0x80004001u;}
static int32_t compatSwapGetLastPresentCount(void*,uint32_t* out){
  static std::atomic<uint32_t> count{0}; if(out)*out=count.fetch_add(1)+1; return 0;
}
static int32_t compatSwapGetDesc(void*,void* desc) {
  gtavdiag::checkpoint("compat-swapchain-get-desc");
  if(!desc)return (int32_t)0x80004003u;
  memset(desc,0,72);
  // Keep a sane bootstrap size. Android/Vulkan owns the real surface extent.
  auto* p=(uint8_t*)desc;
  *(uint32_t*)(p+0)=gCompatSwapWidth.load(std::memory_order_relaxed);
  *(uint32_t*)(p+4)=gCompatSwapHeight.load(std::memory_order_relaxed);
  *(uint32_t*)(p+8)=60; // nominal refresh numerator
  *(uint32_t*)(p+12)=1; // nominal refresh denominator
  *(uint32_t*)(p+16)=gCompatSwapFormat.load(std::memory_order_relaxed);
  *(uint32_t*)(p+28)=1; // sample count
  *(uint32_t*)(p+32)=0; // sample quality
  *(uint32_t*)(p+52)=2; // buffer count
  *(uint32_t*)(p+56)=gCompatSwapWidth.load(std::memory_order_relaxed);
  *(uint32_t*)(p+60)=gCompatSwapHeight.load(std::memory_order_relaxed);
  return 0;
}

struct CompatBackBuffer { void** vtbl; };
static CompatBackBuffer gCompatBackBuffer{};
static void* gBackBufferVtable[16]{};
static int32_t compatBackBufferQI(void* self,const void*,void** out) {
  if(!out)return (int32_t)0x80004003u; *out=self; return 0;
}
static uint32_t compatBackBufferAddRef(void*){return 2;}
static uint32_t compatBackBufferRelease(void*){return 1;}
static int32_t compatBackBufferSetPrivateData(void*,const void*,uint32_t,const void*){return 0;}
static void compatBackBufferGetDevice(void*,void** out){if(out)*out=&gCompatD3DDevice;}
static int32_t compatBackBufferGetPrivateData(void*,const void*,uint32_t* n,void*){if(n)*n=0;return (int32_t)0x80004005u;}
static int32_t compatBackBufferSetPrivateDataInterface(void*,const void*,void*){return 0;}
static void compatBackBufferGetType(void*,uint32_t* out){if(out)*out=3;}
static void compatBackBufferSetEvictionPriority(void*,uint32_t){}
static uint32_t compatBackBufferGetEvictionPriority(void*){return 0;}
static void compatBackBufferGetDesc(void*,void* desc) {
  gtavdiag::checkpoint("compat-backbuffer-get-desc");
  if(!desc)return;
  // D3D11_TEXTURE2D_DESC: Width, Height, MipLevels, ArraySize, Format,
  // SampleDesc{Count,Quality}, Usage, BindFlags, CPUAccessFlags, MiscFlags.
  auto* p=(uint8_t*)desc; memset(p,0,44);
  *(uint32_t*)(p+0)=gCompatSwapWidth.load(std::memory_order_relaxed); *(uint32_t*)(p+4)=gCompatSwapHeight.load(std::memory_order_relaxed);
  *(uint32_t*)(p+8)=1; *(uint32_t*)(p+12)=1;
  *(uint32_t*)(p+16)=gCompatSwapFormat.load(std::memory_order_relaxed); // DXGI_FORMAT_R8G8B8A8_UNORM
  *(uint32_t*)(p+20)=1; // sample count
  *(uint32_t*)(p+28)=0; // D3D11_USAGE_DEFAULT
  *(uint32_t*)(p+32)=0x28; // RENDER_TARGET | SHADER_RESOURCE
}
static void initCompatBackBuffer(){
  static bool once=false;if(once)return;once=true;
  {void* slots[16]={(void*)compatBackBufferSlot0,(void*)compatBackBufferSlot1,(void*)compatBackBufferSlot2,(void*)compatBackBufferSlot3,(void*)compatBackBufferSlot4,(void*)compatBackBufferSlot5,(void*)compatBackBufferSlot6,(void*)compatBackBufferSlot7,(void*)compatBackBufferSlot8,(void*)compatBackBufferSlot9,(void*)compatBackBufferSlot10,(void*)compatBackBufferSlot11,(void*)compatBackBufferSlot12,(void*)compatBackBufferSlot13,(void*)compatBackBufferSlot14,(void*)compatBackBufferSlot15};for(int i=0;i<16;i++)gBackBufferVtable[i]=slots[i];}
  gBackBufferVtable[0]=(void*)compatBackBufferQI;
  gBackBufferVtable[1]=(void*)compatBackBufferAddRef;
  gBackBufferVtable[2]=(void*)compatBackBufferRelease;
  gBackBufferVtable[3]=(void*)compatBackBufferGetDevice;
  gBackBufferVtable[4]=(void*)compatBackBufferGetPrivateData;
  gBackBufferVtable[5]=(void*)compatBackBufferSetPrivateData;
  gBackBufferVtable[6]=(void*)compatBackBufferSetPrivateDataInterface;
  gBackBufferVtable[7]=(void*)compatBackBufferGetType;
  gBackBufferVtable[8]=(void*)compatBackBufferSetEvictionPriority;
  gBackBufferVtable[9]=(void*)compatBackBufferGetEvictionPriority;
  // ID3D11Texture2D::GetDesc = slot 10 / +0x50.
  gBackBufferVtable[10]=(void*)compatBackBufferGetDesc;
  gCompatBackBuffer.vtbl=gBackBufferVtable;
}
static int32_t compatSwapGetBuffer(void*,uint32_t index,const void*,void** out) {
  gtavdiag::checkpoint("compat-swapchain-get-buffer");
  if(!out)return (int32_t)0x80004003u;
  if(index!=0){*out=nullptr;return (int32_t)0x887A0002u;}
  initCompatBackBuffer(); *out=&gCompatBackBuffer; return 0;
}
static void initCompatSwapChain() {
  static bool once=false;if(once)return;once=true;
  for(void*& p:gSwapChainVtable)p=(void*)compatSwapUnsupported;
  gSwapChainVtable[0]=(void*)compatSwapQueryInterface;
  gSwapChainVtable[1]=(void*)compatSwapAddRef;
  gSwapChainVtable[2]=(void*)compatSwapRelease;
  gSwapChainVtable[7]=(void*)compatSwapGetDevice;
  gSwapChainVtable[8]=(void*)compatSwapPresent;
  // grcTextureFactoryDX11::Reset consumes IDXGISwapChain::GetBuffer at
  // slot 9/+0x48, then calls ID3D11Texture2D::GetDesc at +0x50.
  gSwapChainVtable[9]=(void*)compatSwapGetBuffer;
  gSwapChainVtable[10]=(void*)compatSwapSetFullscreenState;
  gSwapChainVtable[11]=(void*)compatSwapGetFullscreenState;
  // Exact InitClass trace consumes swapchain vtable +0x60 immediately.
  gSwapChainVtable[12]=(void*)compatSwapGetDesc;
  gSwapChainVtable[13]=(void*)compatSwapResizeBuffers;
  gSwapChainVtable[14]=(void*)compatSwapResizeTarget;
  gSwapChainVtable[16]=(void*)compatSwapGetFrameStatistics;
  gSwapChainVtable[17]=(void*)compatSwapGetLastPresentCount;
  gCompatSwapChain.vtbl=gSwapChainVtable;
}
}
extern "C" __attribute__((visibility("default"))) int32_t D3D11CreateDeviceAndSwapChain(
    void*,uint32_t,void*,uint32_t,const uint32_t*,uint32_t,uint32_t,const void*,
    void** swapchain,void** device,uint32_t* featureLevel,void** context) {
  gtavdiag::checkpoint("compat-d3d11-create-device-and-swapchain");
  initCompatD3D11(); initCompatSwapChain();
  if(swapchain)*swapchain=&gCompatSwapChain;
  if(device)*device=&gCompatD3DDevice;
  if(featureLevel)*featureLevel=0xb000u;
  if(context)*context=&gCompatD3DContext;
  return 0;
}

namespace gtavnative {
struct Runtime { VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{}; uint32_t family{}; VkCommandPool commands{}; VkDescriptorPool descriptors{}; std::atomic<uint64_t> frame{0}; };
static Runtime g;

// Capture the SDL window that libgtav itself creates. This gives the native renderer
// the exact Android Surface used by the game instead of inventing a second window.
static std::atomic<SDL_Window*> gGameSDLWindow{nullptr};
static void* gameSDLHandle(){
 static void* h=nullptr;
 if(!h){
#ifdef RTLD_NOLOAD
   h=dlopen("libSDL2.so",RTLD_NOW|RTLD_NOLOAD);
#endif
   if(!h)h=dlopen("libSDL2.so",RTLD_NOW|RTLD_LOCAL);
 }
 return h;
}
using SDLCreateWindowFn=SDL_Window*(*)(const char*,int,int,int,int,uint32_t);
extern "C" __attribute__((visibility("default"))) SDL_Window* SDL_CreateWindow(const char* title,int x,int y,int w,int h,uint32_t flags){
 static SDLCreateWindowFn real=nullptr;
 if(!real){
   void* h=gameSDLHandle();
   if(h)real=(SDLCreateWindowFn)dlsym(h,"SDL_CreateWindow");
   if(!real)real=(SDLCreateWindowFn)dlsym(RTLD_NEXT,"SDL_CreateWindow");
 }
 if(!real){gtavdiag::checkpoint("native-sdl-create-window-unresolved");return nullptr;}
 SDL_Window* win=real(title,x,y,w,h,flags);
 if(win){gGameSDLWindow.store(win,std::memory_order_release);gtavdiag::checkpoint("native-sdl-window-captured");}
 return win;
}
struct PresentProbeRuntime {
 VkInstance instance{}; VkSurfaceKHR surface{}; VkPhysicalDevice physical{}; uint32_t family{UINT32_MAX};
 VkDevice device{}; VkQueue queue{}; VkSwapchainKHR swapchain{}; VkFormat format{VK_FORMAT_UNDEFINED}; VkExtent2D extent{};
 VkSurfaceTransformFlagBitsKHR surfaceTransform{VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR};
 std::vector<VkImage> images;
};
static PresentProbeRuntime gPresentProbe;
static void* systemVulkanHandle(){static void* h=nullptr;if(!h){h=dlopen("libvulkan.so",RTLD_NOW|RTLD_NOLOAD);if(!h)h=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);}return h;}
static PFN_vkGetInstanceProcAddr realVkGIPA(){using F=PFN_vkGetInstanceProcAddr;static F f=nullptr;if(!f)f=(F)dlsym(systemVulkanHandle(),"vkGetInstanceProcAddr");return f;}
static PFN_vkGetDeviceProcAddr realVkGDPA(){using F=PFN_vkGetDeviceProcAddr;static F f=nullptr;if(!f)f=(F)dlsym(systemVulkanHandle(),"vkGetDeviceProcAddr");return f;}
extern "C" __attribute__((visibility("default"))) VkResult vkCreateInstance(const VkInstanceCreateInfo* in,const VkAllocationCallbacks* a,VkInstance* out){
 using Fn=VkResult(*)(const VkInstanceCreateInfo*,const VkAllocationCallbacks*,VkInstance*);static Fn real=nullptr;if(!real)real=(Fn)dlsym(systemVulkanHandle(),"vkCreateInstance");if(!real)return VK_ERROR_INITIALIZATION_FAILED;
 if(!in)return real(in,a,out);std::vector<const char*> e;for(uint32_t i=0;i<in->enabledExtensionCount;i++)e.push_back(in->ppEnabledExtensionNames[i]);
 auto add=[&](const char* n){for(auto x:e)if(x&&strcmp(x,n)==0)return;e.push_back(n);};add(VK_KHR_SURFACE_EXTENSION_NAME);add("VK_KHR_android_surface");
 VkInstanceCreateInfo ci=*in;ci.enabledExtensionCount=(uint32_t)e.size();ci.ppEnabledExtensionNames=e.data();VkResult r=real(&ci,a,out);if(r==VK_SUCCESS)gtavdiag::checkpoint("native-vk-instance-surface-ext-injected");return r;
}
extern "C" __attribute__((visibility("default"))) VkResult vkCreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo* in,const VkAllocationCallbacks* a,VkDevice* out){
 using Fn=VkResult(*)(VkPhysicalDevice,const VkDeviceCreateInfo*,const VkAllocationCallbacks*,VkDevice*);static Fn real=nullptr;if(!real)real=(Fn)dlsym(systemVulkanHandle(),"vkCreateDevice");if(!real)return VK_ERROR_INITIALIZATION_FAILED;
 if(!in)return real(p,in,a,out);std::vector<const char*> e;for(uint32_t i=0;i<in->enabledExtensionCount;i++)e.push_back(in->ppEnabledExtensionNames[i]);
 bool has=false;for(auto x:e)if(x&&strcmp(x,VK_KHR_SWAPCHAIN_EXTENSION_NAME)==0)has=true;if(!has)e.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
 VkDeviceCreateInfo ci=*in;ci.enabledExtensionCount=(uint32_t)e.size();ci.ppEnabledExtensionNames=e.data();VkResult r=real(p,&ci,a,out);if(r==VK_SUCCESS)gtavdiag::checkpoint("native-vk-device-swapchain-ext-injected");return r;
}static PFN_vkVoidFunction gtavInterceptGIPA(VkInstance instance,const char* name){
 auto real=realVkGIPA();if(!real||!name)return nullptr;
 static std::atomic<uint32_t> lookups{0};
 uint32_t n=lookups.fetch_add(1,std::memory_order_relaxed);
 if(n<64){
   char detail[256];snprintf(detail,sizeof(detail),"n=%u instance=%p name=%s",n,(void*)instance,name);
   gtavdiag::checkpoint("native-vkgipa-proc-lookup",detail);
 }
 if(strcmp(name,"vkCreateInstance")==0){gtavdiag::checkpoint("native-vkgipa-create-instance-intercept");return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateInstance);}
 if(strcmp(name,"vkCreateDevice")==0){gtavdiag::checkpoint("native-vkgipa-create-device-intercept");return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDevice);}
 if(strcmp(name,"vkGetDeviceProcAddr")==0){gtavdiag::checkpoint("native-vkgipa-get-device-proc-intercept");return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);}
 return real(instance,name);
}
extern "C" __attribute__((visibility("default"))) PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance,const char* name){
 return gtavInterceptGIPA(instance,name);
}
extern "C" __attribute__((visibility("default"))) PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device,const char* name){
 auto real=realVkGDPA();if(!real||!name)return nullptr;return real(device,name);
}



static bool probeGameAndroidSurface(){
 static std::atomic<bool> ready{false},attempted{false};if(ready.load())return true;SDL_Window* win=gGameSDLWindow.load();if(!win||!g.instance||!g.device||!g.physical||!g.queue)return false;if(attempted.exchange(true))return false;void* sdl=gameSDLHandle();auto create=(SDLVulkanCreateSurfaceFn)(sdl?dlsym(sdl,"SDL_Vulkan_CreateSurface"):nullptr);auto size=(SDLVulkanGetDrawableSizeFn)(sdl?dlsym(sdl,"SDL_Vulkan_GetDrawableSize"):nullptr);if(!create)return false;VkSurfaceKHR surface{};if(!create(win,g.instance,&surface)||!surface){gtavdiag::checkpoint("native-engine-surface-create-failed");return false;}int w=0,h=0;if(size)size(win,&w,&h);gtavdiag::checkpoint("native-android-surface-ready");auto support=(PFN_vkGetPhysicalDeviceSurfaceSupportKHR)vkGetInstanceProcAddr(g.instance,"vkGetPhysicalDeviceSurfaceSupportKHR");auto caps=(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)vkGetInstanceProcAddr(g.instance,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR");auto formats=(PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)vkGetInstanceProcAddr(g.instance,"vkGetPhysicalDeviceSurfaceFormatsKHR");auto modes=(PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)vkGetInstanceProcAddr(g.instance,"vkGetPhysicalDeviceSurfacePresentModesKHR");VkBool32 yes=0;VkSurfaceCapabilitiesKHR cp{};uint32_t fc=0,mc=0;if(!support||!caps||!formats||!modes||support(g.physical,g.family,surface,&yes)!=VK_SUCCESS||!yes||caps(g.physical,surface,&cp)!=VK_SUCCESS){gtavdiag::checkpoint("native-engine-present-queue-incompatible");return false;}formats(g.physical,surface,&fc,nullptr);modes(g.physical,surface,&mc,nullptr);std::vector<VkSurfaceFormatKHR> fs(fc);if(fc)formats(g.physical,surface,&fc,fs.data());std::vector<VkPresentModeKHR> ms(mc);if(mc)modes(g.physical,surface,&mc,ms.data());VkSurfaceFormatKHR sf=fc?fs[0]:VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};for(auto& f:fs)if(f.format==VK_FORMAT_R8G8B8A8_UNORM){sf=f;break;}VkPresentModeKHR pm=VK_PRESENT_MODE_FIFO_KHR;for(auto m:ms)if(m==VK_PRESENT_MODE_MAILBOX_KHR){pm=m;break;}VkExtent2D ex=cp.currentExtent;if(ex.width==UINT32_MAX)ex={(uint32_t)std::max(1,w),(uint32_t)std::max(1,h)};if(!(cp.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)){gtavdiag::checkpoint("native-engine-no-transfer-dst");return false;}uint32_t ic=cp.minImageCount+1;if(cp.maxImageCount&&ic>cp.maxImageCount)ic=cp.maxImageCount;VkSurfaceTransformFlagBitsKHR chosenTransform=(cp.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)?VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:cp.currentTransform;
{char td[224];snprintf(td,sizeof(td),"drawable=%dx%d extent=%ux%u current=0x%x supported=0x%x chosen=0x%x",w,h,ex.width,ex.height,(unsigned)cp.currentTransform,(unsigned)cp.supportedTransforms,(unsigned)chosenTransform);gtavdiag::checkpoint("native-surface-transform",td);}
VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};si.surface=surface;si.minImageCount=ic;si.imageFormat=sf.format;si.imageColorSpace=sf.colorSpace;si.imageExtent=ex;si.imageArrayLayers=1;si.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;si.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;si.preTransform=chosenTransform;si.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;si.presentMode=pm;si.clipped=VK_TRUE;auto cs=(PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(g.device,"vkCreateSwapchainKHR");auto gi=(PFN_vkGetSwapchainImagesKHR)vkGetDeviceProcAddr(g.device,"vkGetSwapchainImagesKHR");if(!cs||!gi){gtavdiag::checkpoint("native-engine-swapchain-procs-unresolved");return false;}VkSwapchainKHR sc{};if(cs(g.device,&si,nullptr,&sc)!=VK_SUCCESS){gtavdiag::checkpoint("native-engine-swapchain-create-failed");return false;}uint32_t ni=0;gi(g.device,sc,&ni,nullptr);std::vector<VkImage> imgs(ni);gi(g.device,sc,&ni,imgs.data());gPresentProbe={};gPresentProbe.instance=g.instance;gPresentProbe.surface=surface;gPresentProbe.physical=g.physical;gPresentProbe.family=g.family;gPresentProbe.device=g.device;gPresentProbe.queue=g.queue;gPresentProbe.swapchain=sc;gPresentProbe.format=sf.format;gPresentProbe.extent=ex;gPresentProbe.surfaceTransform=chosenTransform;gPresentProbe.images=std::move(imgs);gtavdiag::checkpoint("native-engine-swapchain-ready");ready.store(true);return true;
}
static std::mutex descriptorPoolMutex;
static std::vector<VkDescriptorPool> descriptorPools;
static uint32_t descriptorPoolGeneration=0;

static VkDescriptorPool createCompatDescriptorPool(uint32_t scale){
 if(!g.device)return VK_NULL_HANDLE;
 const uint32_t sets=16384u*std::max(1u,scale);
 VkDescriptorPoolSize s[]={
   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,sets*8u},
   {VK_DESCRIPTOR_TYPE_SAMPLER,sets*8u},
   {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,sets*16u},
   {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,sets*4u},
   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,sets*4u},
   {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,sets*2u},
   {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,sets*2u},
   {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,sets*2u}
 };
 VkDescriptorPoolCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
 di.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
 di.maxSets=sets;di.poolSizeCount=(uint32_t)(sizeof(s)/sizeof(s[0]));di.pPoolSizes=s;
 VkDescriptorPool pool=VK_NULL_HANDLE;
 if(vkCreateDescriptorPool(g.device,&di,nullptr,&pool)!=VK_SUCCESS)return VK_NULL_HANDLE;
 return pool;
}

static std::mutex resourceMutex;
static std::unordered_map<uint64_t,GtavNativeResourceHandle> resources;
struct NativePipelineCacheEntry { VkPipeline pipeline{}; VkPipelineLayout layout{}; VkDescriptorSet descriptor{}; };
static std::mutex pipelineCacheMutex;
static std::unordered_map<uint64_t,NativePipelineCacheEntry> pipelineCache;
static std::mutex graphicsPipelineCreateMutex;
static PFN_vkCmdBeginRendering pBeginRendering{};
static PFN_vkCmdEndRendering pEndRendering{};
static PFN_vkCmdPipelineBarrier2 pBarrier2{};
static PFN_vkQueueSubmit2 pSubmit2{};
static constexpr uint32_t PROFILE_PASS=1,PROFILE_DISPATCH=2,PROFILE_COPY_BUFFER=3,PROFILE_COPY_IMAGE=4,PROFILE_BLIT=5,PROFILE_RESOLVE=6;
static void profilePassSwitch(void* rtv0,void* dsv);
static void profileCountDraw(bool indexed,uint32_t elements);
static uint32_t profileBeginExact(uint32_t kind,uintptr_t a,uintptr_t b);
static void profileEndExact(uint32_t token);
static void profileCancelExact(uint32_t token);
static void profileBeginFrame(uint32_t slot,VkCommandBuffer cb);
static void profileEndFrame(uint32_t slot,VkCommandBuffer cb);
static void profileReadAndLog(uint32_t slot);
static bool profileInit();
static void profileDestroy();
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
 void* blendState{}; void* depthState{}; void* rasterState{};
 float blendFactor[4]{1.0f,1.0f,1.0f,1.0f};
 uint32_t sampleMask{0xffffffffu};
 uint32_t stencilRef{};
};
static std::mutex mirrorMutex;
static std::unordered_map<void*,RageMirrorState> mirrorStates;
static std::atomic<void*> gCompatLastInputLayout{nullptr};
static std::atomic<uint64_t> gBlackProbeFrame{0};
static std::atomic<uint32_t> gBlackProbeDraws{0};
static std::atomic<uint32_t> gBlackProbeExpectedCbv{0},gBlackProbeBoundCbv{0},gBlackProbeMissingCbv{0};
static std::atomic<uint32_t> gBlackProbeExpectedSrv{0},gBlackProbeBoundSrv{0},gBlackProbeMissingSrv{0};
static std::atomic<uint32_t> gBlackProbeExpectedSampler{0},gBlackProbeBoundSampler{0},gBlackProbeMissingSampler{0};
static std::atomic<uint32_t> gBlackProbeDescriptorWrites{0};


static RageMirrorState mergeCompatAliasState(const RageMirrorState& base){
 RageMirrorState out=base;
 void* wanted=gCompatLastInputLayout.load(std::memory_order_acquire);
 const RageMirrorState* best=nullptr;
 for(const auto& kv:mirrorStates){
   const auto& s=kv.second;
   if(wanted&&s.inputLayout==wanted){best=&s;break;}
   if(!best&&s.vs&&s.ps&&s.rtvCount&&s.rtv[0])best=&s;
 }
 if(!best)return out;
 auto fillPtr=[](void*& d,void* s){if(!d&&s)d=s;};
 fillPtr(out.inputLayout,best->inputLayout);
 for(uint32_t i=0;i<16;i++){
   fillPtr(out.vertexBuffers[i],best->vertexBuffers[i]);
   if(!out.strides[i]&&best->strides[i])out.strides[i]=best->strides[i];
   if(!out.offsets[i]&&best->offsets[i])out.offsets[i]=best->offsets[i];
   fillPtr(out.vsCB[i],best->vsCB[i]);fillPtr(out.psCB[i],best->psCB[i]);fillPtr(out.csCB[i],best->csCB[i]);
   fillPtr(out.vsSampler[i],best->vsSampler[i]);fillPtr(out.psSampler[i],best->psSampler[i]);fillPtr(out.csSampler[i],best->csSampler[i]);fillPtr(out.csUAV[i],best->csUAV[i]);
 }
 for(uint32_t i=0;i<32;i++){fillPtr(out.vsSRV[i],best->vsSRV[i]);fillPtr(out.psSRV[i],best->psSRV[i]);fillPtr(out.csSRV[i],best->csSRV[i]);}
 fillPtr(out.indexBuffer,best->indexBuffer);if(!out.indexFormat)out.indexFormat=best->indexFormat;if(!out.indexOffset)out.indexOffset=best->indexOffset;if(!out.topology)out.topology=best->topology;
 fillPtr(out.vs,best->vs);fillPtr(out.ps,best->ps);fillPtr(out.cs,best->cs);
 if(!out.viewportCount&&best->viewportCount){out.viewportCount=best->viewportCount;std::memcpy(out.viewports,best->viewports,sizeof(out.viewports));}
 if(!out.scissorCount&&best->scissorCount){out.scissorCount=best->scissorCount;std::memcpy(out.scissors,best->scissors,sizeof(out.scissors));}
 if(!out.rtvCount&&best->rtvCount){out.rtvCount=best->rtvCount;for(uint32_t i=0;i<8;i++)out.rtv[i]=best->rtv[i];}
 fillPtr(out.dsv,best->dsv);
 if(!out.blendState&&best->blendState){out.blendState=best->blendState;std::memcpy(out.blendFactor,best->blendFactor,sizeof(out.blendFactor));out.sampleMask=best->sampleMask;}
 if(!out.depthState&&best->depthState){out.depthState=best->depthState;out.stencilRef=best->stencilRef;}
 fillPtr(out.rasterState,best->rasterState);
 return out;
}
static std::atomic<void*> lastCompatPrimaryRTV{nullptr};
static std::atomic<void*> lastCompatDrawnRTV{nullptr};
static std::atomic<void*> lastCompatFinalTransferDst{nullptr};
static std::atomic<void*> lastCompatFullSizeRTV{nullptr};
static std::atomic<uint64_t> compatPresentWriteSerial{0};
static std::atomic<uint64_t> lastCompatDrawnSerial{0};
static std::atomic<uint64_t> lastCompatTransferSerial{0};
static std::atomic<uint64_t> lastCompatFullSizeSerial{0};
static void resetCompatPresentSourcesForNewFrame(){
 lastCompatPrimaryRTV.store(nullptr,std::memory_order_release);
 lastCompatDrawnRTV.store(nullptr,std::memory_order_release);
 lastCompatFinalTransferDst.store(nullptr,std::memory_order_release);
 lastCompatFullSizeRTV.store(nullptr,std::memory_order_release);
 lastCompatDrawnSerial.store(0,std::memory_order_release);
 lastCompatTransferSerial.store(0,std::memory_order_release);
 lastCompatFullSizeSerial.store(0,std::memory_order_release);
 gBlackProbeFrame.fetch_add(1,std::memory_order_relaxed);
 gBlackProbeDraws.store(0,std::memory_order_relaxed);
 gBlackProbeExpectedCbv.store(0,std::memory_order_relaxed);gBlackProbeBoundCbv.store(0,std::memory_order_relaxed);gBlackProbeMissingCbv.store(0,std::memory_order_relaxed);
 gBlackProbeExpectedSrv.store(0,std::memory_order_relaxed);gBlackProbeBoundSrv.store(0,std::memory_order_relaxed);gBlackProbeMissingSrv.store(0,std::memory_order_relaxed);
 gBlackProbeExpectedSampler.store(0,std::memory_order_relaxed);gBlackProbeBoundSampler.store(0,std::memory_order_relaxed);gBlackProbeMissingSampler.store(0,std::memory_order_relaxed);
 gBlackProbeDescriptorWrites.store(0,std::memory_order_relaxed);
 gMegaDrawOk.store(0,std::memory_order_relaxed);gMegaDrawFail.store(0,std::memory_order_relaxed);
 gMegaDescriptorFail.store(0,std::memory_order_relaxed);gMegaImageFail.store(0,std::memory_order_relaxed);
 gMegaNoopCalls.store(0,std::memory_order_relaxed);gMegaCopyOps.store(0,std::memory_order_relaxed);gMegaTextureUpdates.store(0,std::memory_order_relaxed);
 gtavdiag::checkpoint("native-present-sources-reset");
}
static bool hasUsableEnginePresentSource(){return lastCompatFullSizeRTV.load(std::memory_order_acquire)!=nullptr||lastCompatFinalTransferDst.load(std::memory_order_acquire)!=nullptr||lastCompatDrawnRTV.load(std::memory_order_acquire)!=nullptr;}
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
extern "C" void gtavnative_compat_mirror_blend_state(void* c,void* state,const float* factor,uint32_t mask){
 std::lock_guard<std::mutex> l(mirrorMutex);auto& m=mirror(c);m.blendState=state;m.sampleMask=mask;
 if(factor)std::memcpy(m.blendFactor,factor,sizeof(m.blendFactor));else for(float& v:m.blendFactor)v=1.0f;
 char d[192];snprintf(d,sizeof(d),"ctx=%p state=%p factor=%.3f,%.3f,%.3f,%.3f mask=0x%x",c,state,m.blendFactor[0],m.blendFactor[1],m.blendFactor[2],m.blendFactor[3],mask);gtavdiag::checkpoint("native-blend-state-mirrored",d);
}
extern "C" void gtavnative_compat_mirror_depth_state(void* c,void* state,uint32_t ref){
 std::lock_guard<std::mutex> l(mirrorMutex);auto& m=mirror(c);m.depthState=state;m.stencilRef=ref;
 char d[128];snprintf(d,sizeof(d),"ctx=%p state=%p stencilRef=%u",c,state,ref);gtavdiag::checkpoint("native-depth-state-mirrored",d);
}
extern "C" void gtavnative_compat_mirror_input_layout(void* c,void* v){bool known=compatInputLayoutObject(v)!=nullptr;if(v)gCompatLastInputLayout.store(v,std::memory_order_release);{std::lock_guard<std::mutex> l(mirrorMutex);mirror(c).inputLayout=v;}static std::atomic<uint32_t> budget{64};uint32_t n=budget.fetch_sub(1,std::memory_order_relaxed);if(n>0){char d[128];snprintf(d,sizeof(d),"ctx=%p layout=%p known=%d",c,v,known?1:0);gtavdiag::checkpoint("native-input-layout-mirrored",d);}}
extern "C" void gtavnative_compat_mirror_vertex_buffers(void* c,uint32_t f,uint32_t n,void* const* v,const uint32_t* s,const uint32_t* o){std::lock_guard<std::mutex> l(mirrorMutex);auto& m=mirror(c);for(uint32_t i=0;i<n&&f+i<16;i++){m.vertexBuffers[f+i]=v?v[i]:nullptr;m.strides[f+i]=s?s[i]:0;m.offsets[f+i]=o?o[i]:0;}}
extern "C" void gtavnative_compat_mirror_index_buffer(void* c,void* b,uint32_t f,uint32_t o){std::lock_guard<std::mutex> l(mirrorMutex);auto& m=mirror(c);m.indexBuffer=b;m.indexFormat=f;m.indexOffset=o;}
extern "C" void gtavnative_compat_mirror_topology(void* c,uint32_t t){std::lock_guard<std::mutex> l(mirrorMutex);mirror(c).topology=t;}
extern "C" void gtavnative_compat_mirror_shader(void* c,uint32_t stage,void* sh){std::lock_guard<std::mutex> l(mirrorMutex);auto& m=mirror(c);if(stage==0)m.vs=sh;else if(stage==1)m.ps=sh;else if(stage==2)m.cs=sh;}
extern "C" void gtavnative_compat_mirror_viewports(void* c,uint32_t n,const void* p){std::lock_guard<std::mutex> l(mirrorMutex);auto& m=mirror(c);m.viewportCount=n>4?4:n;if(p)std::memcpy(m.viewports,p,m.viewportCount*24);}
extern "C" void gtavnative_compat_mirror_scissors(void* c,uint32_t n,const void* p){std::lock_guard<std::mutex> l(mirrorMutex);auto& m=mirror(c);m.scissorCount=n>16?16:n;if(p)std::memcpy(m.scissors,p,m.scissorCount*16);}
extern "C" void gtavnative_compat_mirror_objs(void* c,uint32_t k,uint32_t f,uint32_t n,void* const* v){std::lock_guard<std::mutex> l(mirrorMutex);auto& m=mirror(c);void** d=nullptr;uint32_t cap=16;switch(k){case 0:d=m.vsCB;break;case 1:d=m.psCB;break;case 2:d=m.csCB;break;case 3:d=m.vsSRV;cap=32;break;case 4:d=m.psSRV;cap=32;break;case 5:d=m.csSRV;cap=32;break;case 6:d=m.vsSampler;break;case 7:d=m.psSampler;break;case 8:d=m.csSampler;break;case 9:d=m.csUAV;break;case 10:d=&m.blendState;cap=1;break;case 11:d=&m.depthState;cap=1;break;case 12:d=&m.rasterState;cap=1;break;default:return;}for(uint32_t i=0;i<n&&f+i<cap;i++)d[f+i]=v?v[i]:nullptr;}
extern "C" void gtavnative_compat_mirror_render_targets(void* c,uint32_t n,void* const* r,void* d){if(n&&r&&r[0])lastCompatPrimaryRTV.store(r[0],std::memory_order_release);std::lock_guard<std::mutex> l(mirrorMutex);auto& m=mirror(c);m.rtvCount=n>8?8:n;for(uint32_t i=0;i<8;i++)m.rtv[i]=(i<m.rtvCount&&r)?r[i]:nullptr;m.dsv=d;}


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
 auto resolve=[&](const char* core,const char* khr)->PFN_vkVoidFunction{
   PFN_vkVoidFunction p=nullptr;
   if(d)p=vkGetDeviceProcAddr(d,core);
   if(!p&&d&&khr)p=vkGetDeviceProcAddr(d,khr);
   if(!p&&g.instance)p=vkGetInstanceProcAddr(g.instance,core);
   if(!p&&g.instance&&khr)p=vkGetInstanceProcAddr(g.instance,khr);
   if(!p)p=reinterpret_cast<PFN_vkVoidFunction>(dlsym(RTLD_DEFAULT,core));
   if(!p&&khr)p=reinterpret_cast<PFN_vkVoidFunction>(dlsym(RTLD_DEFAULT,khr));
   return p;
 };
 pBeginRendering=reinterpret_cast<PFN_vkCmdBeginRendering>(resolve("vkCmdBeginRenderingKHR","vkCmdBeginRendering"));
 pEndRendering=reinterpret_cast<PFN_vkCmdEndRendering>(resolve("vkCmdEndRenderingKHR","vkCmdEndRendering"));
 pBarrier2=reinterpret_cast<PFN_vkCmdPipelineBarrier2>(resolve("vkCmdPipelineBarrier2","vkCmdPipelineBarrier2KHR"));
 pSubmit2=reinterpret_cast<PFN_vkQueueSubmit2>(resolve("vkQueueSubmit2","vkQueueSubmit2KHR"));
 if(pBeginRendering&&pEndRendering)gtavdiag::checkpoint("native-dynamic-rendering-ready");
 else gtavdiag::checkpoint("native-dynamic-rendering-missing");
}
static PFN_vkCmdBeginRendering resolveBeginRenderingNow(){
 PFN_vkCmdBeginRendering p=nullptr;
 if(g.device)p=reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(g.device,"vkCmdBeginRenderingKHR"));
 if(!p&&g.device)p=reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(g.device,"vkCmdBeginRendering"));
 if(!p&&g.instance)p=reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetInstanceProcAddr(g.instance,"vkCmdBeginRenderingKHR"));
 if(!p&&g.instance)p=reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetInstanceProcAddr(g.instance,"vkCmdBeginRendering"));
 if(!p)p=reinterpret_cast<PFN_vkCmdBeginRendering>(dlsym(RTLD_DEFAULT,"vkCmdBeginRenderingKHR"));
 if(!p)p=reinterpret_cast<PFN_vkCmdBeginRendering>(dlsym(RTLD_DEFAULT,"vkCmdBeginRendering"));
 return p;
}
static PFN_vkCmdEndRendering resolveEndRenderingNow(){
 PFN_vkCmdEndRendering p=nullptr;
 if(g.device)p=reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(g.device,"vkCmdEndRenderingKHR"));
 if(!p&&g.device)p=reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(g.device,"vkCmdEndRendering"));
 if(!p&&g.instance)p=reinterpret_cast<PFN_vkCmdEndRendering>(vkGetInstanceProcAddr(g.instance,"vkCmdEndRenderingKHR"));
 if(!p&&g.instance)p=reinterpret_cast<PFN_vkCmdEndRendering>(vkGetInstanceProcAddr(g.instance,"vkCmdEndRendering"));
 if(!p)p=reinterpret_cast<PFN_vkCmdEndRendering>(dlsym(RTLD_DEFAULT,"vkCmdEndRenderingKHR"));
 if(!p)p=reinterpret_cast<PFN_vkCmdEndRendering>(dlsym(RTLD_DEFAULT,"vkCmdEndRendering"));
 return p;
}
static void endCompatRenderingNow(VkCommandBuffer cb){
 if(!cb)return;
 auto end=resolveEndRenderingNow();
 if(end)end(cb); else gtavdiag::checkpoint("native-render-scope-end-proc-missing");
}

static uint64_t resourceKey(uint64_t rage,uint32_t kind){ return (rage<<3)^uint64_t(kind); }
static uint64_t hashMix(uint64_t h,uint64_t v){h^=v+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);return h;}
static uint64_t graphicsStateKey(const RageMirrorState& s){
 uint64_t h=0xcbf29ce484222325ull;
 h=hashMix(h,(uintptr_t)s.inputLayout); h=hashMix(h,(uintptr_t)s.vs); h=hashMix(h,(uintptr_t)s.ps);
 h=hashMix(h,s.topology); h=hashMix(h,s.rtvCount); h=hashMix(h,(uintptr_t)s.dsv);
 h=hashMix(h,(uintptr_t)s.blendState);h=hashMix(h,(uintptr_t)s.depthState);h=hashMix(h,(uintptr_t)s.rasterState);
 for(unsigned i=0;i<s.rtvCount&&i<8;i++)h=hashMix(h,(uintptr_t)s.rtv[i]);
 for(unsigned i=0;i<16;i++){h=hashMix(h,(uintptr_t)s.vertexBuffers[i]);h=hashMix(h,s.strides[i]);h=hashMix(h,s.offsets[i]);}
 // Descriptor layout depends on which fallback slots exist, but not on the
 // identity of the resource in each slot. Hash presence masks so a pipeline
 // layout is rebuilt only when the descriptor shape changes.
 uint64_t m0=0,m1=0,m2=0;
 for(unsigned i=0;i<16;i++){if(s.vsCB[i])m0|=1ull<<i;if(s.psCB[i])m0|=1ull<<(16+i);if(s.vsSampler[i])m1|=1ull<<i;if(s.psSampler[i])m1|=1ull<<(16+i);}
 for(unsigned i=0;i<32;i++){if(s.vsSRV[i])m2|=1ull<<i;if(s.psSRV[i])m2|=1ull<<(32+i);}
 h=hashMix(h,m0);h=hashMix(h,m1);h=hashMix(h,m2);
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
 gtavdiag::checkpoint("attach-attempt");
 const uint32_t attempt=attachAttempts.fetch_add(1,std::memory_order_relaxed)+1;
 if(!gtavBase) dl_iterate_phdr(findGtav,nullptr);
 if(!gtavBase){ if(attempt<=8) __android_log_print(ANDROID_LOG_WARN,"GTAV-NATIVE","ATTACH wait: libgtav base unavailable attempt=%u",attempt); return false; }
 // libgtav is now mapped: patch its adapter Initialize before our first forced late init.
 installGtavDlsymCallHook();
 gtav_native_renderer_install_early_vulkan_hook();
 auto gi=(GetInstanceFn)(gtavBase+0x6232890);
 auto gp=(GetPhysicalDeviceFn)(gtavBase+0x623289c);
 auto gd=(GetDeviceFn)(gtavBase+0x62328a8);
 auto gq=(GetQueueFn)(gtavBase+0x62328b4);
 auto gf=(GetQueueFamilyFn)(gtavBase+0x62328c0);
 VkInstance i=gi(); VkPhysicalDevice p=gp(); VkDevice d=gd(); VkQueue q=gq(); uint32_t family=gf();
 if(!i||!p||!d||!q){
   // Some Android launch paths reach the D3D-shaped bootstrap without invoking
   // grVulkanNativeDeviceAdapter::Initialize. Retry the native adapter once after
   // libgtav is fully loaded, then re-read the runtime handles.
   static std::atomic<bool> initTried{false};
   if(!initTried.exchange(true,std::memory_order_acq_rel)){
     gtavdiag::checkpoint("vulkan-native-late-init");
     using InitNativeFn=bool(*)();
     auto initNative=(InitNativeFn)(gtavBase+0x622f0b8);
     (void)initNative();
     i=gi(); p=gp(); d=gd(); q=gq(); family=gf();
   }
 }
 if(!i||!p||!d||!q){ gtavdiag::checkpoint("vulkan-runtime-handles-not-ready"); if(attempt<=32 || (attempt%120)==0) __android_log_print(ANDROID_LOG_WARN,"GTAV-NATIVE","ATTACH wait attempt=%u i=%p p=%p d=%p q=%p family=%u",attempt,(void*)i,(void*)p,(void*)d,(void*)q,family); return false; }
 const bool ok=gtav_native_renderer_attach(i,p,d,q,family);
 if(ok) gtavdiag::checkpoint("vulkan-native-attached"); else gtavdiag::checkpoint("vulkan-native-attach-failed");
 if(ok && attachSuccesses.fetch_add(1,std::memory_order_relaxed)==0) __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE","ATTACH READY i=%p p=%p d=%p q=%p family=%u",(void*)i,(void*)p,(void*)d,(void*)q,family);
 return ok;
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_attach(VkInstance i,VkPhysicalDevice p,VkDevice d,VkQueue q,uint32_t family){
 if(!i||!p||!d||!q)return false;
 if(g.device==d&&g.queue==q&&g.commands&&g.descriptors){
   if(!pBeginRendering||!pEndRendering)load13(d);
   return true;
 }
 if(g.device&&g.device!=d)return false;
 g.instance=i;g.physical=p;g.device=d;g.queue=q;g.family=family;load13(d);
 VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT|VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;ci.queueFamilyIndex=family;
 VkResult poolResult=vkCreateCommandPool(d,&ci,nullptr,&g.commands); if(poolResult!=VK_SUCCESS){__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE","vkCreateCommandPool failed=%d family=%u",(int)poolResult,family);g.commands=VK_NULL_HANDLE;return false;}
 {
   std::lock_guard<std::mutex> l(descriptorPoolMutex);
   g.descriptors=createCompatDescriptorPool(1);
   if(!g.descriptors){__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE","vkCreateDescriptorPool failed");vkDestroyCommandPool(d,g.commands,nullptr);g.commands=VK_NULL_HANDLE;return false;}
   descriptorPools.clear();descriptorPools.push_back(g.descriptors);descriptorPoolGeneration=1;
 }
 return true;
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
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_copy_buffer(VkCommandBuffer c,VkBuffer s,VkBuffer d,VkDeviceSize n){if(c&&s&&d&&n){uint32_t t=profileBeginExact(3u,(uintptr_t)s,(uintptr_t)d);VkBufferCopy r{0,0,n};vkCmdCopyBuffer(c,s,d,1,&r);profileEndExact(t);}}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_copy_image(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,const VkImageCopy* r,uint32_t n){if(c&&s&&d&&r&&n){uint32_t t=profileBeginExact(4u,(uintptr_t)s,(uintptr_t)d);vkCmdCopyImage(c,s,sl,d,dl,n,r);profileEndExact(t);}}
extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_blit_image(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,const VkImageBlit* r,uint32_t n,VkFilter f){if(c&&s&&d&&r&&n){uint32_t t=profileBeginExact(5u,(uintptr_t)s,(uintptr_t)d);vkCmdBlitImage(c,s,sl,d,dl,n,r,f);profileEndExact(t);}}
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
 if(layout)gCompatLastInputLayout.store(layout,std::memory_order_release);
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
static void hOMRT(void* c,uint32_t n,void* const* r,void* d){void* r0=(r&&n)?r[0]:nullptr;profilePassSwitch(r0,d);{std::lock_guard<std::mutex> l(mirrorMutex);auto& s=mirror(c);s.rtvCount=n>8?8:n;mirrorObjs(s.rtv,8,0,s.rtvCount,r);s.dsv=d;for(uint32_t i=0;i<s.rtvCount;i++)capture("RTV",s.rtv[i]);capture("DSV",d);}if(oOMRT)oOMRT(c,n,r,d);}


using OrigDraw=void(*)(void*,uint32_t,uint32_t); using OrigDrawIndexed=void(*)(void*,uint32_t,uint32_t,int32_t);
using OrigDispatch=void(*)(void*,uint32_t,uint32_t,uint32_t);
static OrigDraw origDraw{}; static OrigDrawIndexed origDrawIndexed{}; static OrigDispatch origDispatch{};
using OrigSubmissionBegin=VkCommandBuffer(*)(void*,bool);
using OrigTransitionOwnedResources=void(*)(void*,VkCommandBuffer);
using OrigPassEndAndSubmit=bool(*)(void*);
static OrigSubmissionBegin origSubmissionBegin{};
static OrigTransitionOwnedResources origTransitionOwnedResources{};
static OrigPassEndAndSubmit origPassEndAndSubmit{};
static std::atomic<VkCommandBuffer> observedNativeCommandBuffer{VK_NULL_HANDLE};
static thread_local VkCommandBuffer tlsNativeCommandBuffer=VK_NULL_HANDLE;
static std::atomic<uint64_t> observedCommandBufferEpoch{0};
static inline void publishNativeCommandBuffer(VkCommandBuffer cb,const char* source);

// Fallback recording path for the D3D-shaped compatibility frontend.
// The historical RAGE submission hook addresses are not executed by this build,
// so keep our own small ring of primary command buffers on GTA's real graphics queue.
struct CompatFrameCmd {
 VkCommandBuffer cb{VK_NULL_HANDLE};
 VkFence fence{VK_NULL_HANDLE};
 bool inFlight{false};
};
static CompatFrameCmd gCompatFrameCmd[3]{};
static uint32_t gCompatFrameIndex=0;
static VkCommandBuffer gCompatRecordingCB=VK_NULL_HANDLE;
static bool gCompatFrameCmdReady=false;
struct BlackProbeReadback { VkBuffer buffer{VK_NULL_HANDLE}; VkDeviceMemory memory{VK_NULL_HANDLE}; void* mapped{nullptr}; VkDeviceSize size{4096}; bool recorded{false}; VkFormat format{VK_FORMAT_UNDEFINED}; uint32_t width{0},height{0}; };
static BlackProbeReadback gBlackProbeReadback[3]{};
static bool ensureBlackProbeReadback();
static void recordBlackProbeReadback(VkCommandBuffer,VkImage,VkFormat,uint32_t,uint32_t,uint32_t);
static void logBlackProbeReadback(uint32_t);


#include "gpu_profiler.inl"

static VkSemaphore gPresentAcquire[3]{},gPresentDone[3]{};static bool gPresentSyncReady=false;static bool ensureEnginePresentSync();static bool recordEnginePresentCopy(VkCommandBuffer,uint32_t);static bool createCompatOwnedImage(void*,uint32_t,void*);static bool hasUsableEnginePresentSource();
static bool ensureCompatFrameCommandRing(){
 if(gCompatFrameCmdReady)return true;
 if(!g.device||!g.commands)return false;
 VkCommandBuffer bufs[3]{};
 VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
 ai.commandPool=g.commands; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=3;
 if(vkAllocateCommandBuffers(g.device,&ai,bufs)!=VK_SUCCESS)return false;
 for(uint32_t i=0;i<3;i++){
   gCompatFrameCmd[i].cb=bufs[i];
   VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if(vkCreateFence(g.device,&fi,nullptr,&gCompatFrameCmd[i].fence)!=VK_SUCCESS)return false;
 }
 if(!profileInit())gtavdiag::checkpoint("gpu-profiler-unavailable");
 if(!ensureBlackProbeReadback())gtavdiag::checkpoint("black-probe-readback-init-failed");
 gCompatFrameCmdReady=true;
 gtavdiag::checkpoint("compat-command-ring-ready");
 return true;
}

static void submitAndBeginCompatFrameCommand(){
 if(!ensureCompatFrameCommandRing())return;
 if(gCompatRecordingCB){
   profileEndFrame(gCompatFrameIndex,gCompatRecordingCB);
   CompatFrameCmd& prev=gCompatFrameCmd[gCompatFrameIndex];
   uint32_t pix=UINT32_MAX; bool presentReady=false;
   VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_TRANSFER_BIT;
   VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
   auto ac=(PFN_vkAcquireNextImageKHR)(gPresentProbe.swapchain?vkGetDeviceProcAddr(g.device,"vkAcquireNextImageKHR"):nullptr);
   auto qp=(PFN_vkQueuePresentKHR)(gPresentProbe.swapchain?vkGetDeviceProcAddr(g.device,"vkQueuePresentKHR"):nullptr);
   if(!gPresentProbe.swapchain)gtavdiag::checkpoint("native-engine-present-no-swapchain");
   else if(!ac||!qp)gtavdiag::checkpoint("native-engine-present-procs-missing");
   else if(!ensureEnginePresentSync())gtavdiag::checkpoint("native-engine-present-sync-failed");
   else if(!hasUsableEnginePresentSource())gtavdiag::checkpoint("native-engine-present-deferred-no-rendered-source");
   else if(!createCompatOwnedImage(&gCompatBackBuffer,2u,&gCompatBackBuffer))gtavdiag::checkpoint("native-engine-present-backbuffer-create-failed");
   else{
     VkResult ar=ac(g.device,gPresentProbe.swapchain,1000000000ull,gPresentAcquire[gCompatFrameIndex],VK_NULL_HANDLE,&pix);
     if(ar==VK_SUCCESS||ar==VK_SUBOPTIMAL_KHR){
       gtavdiag::checkpoint("native-engine-acquire-ok");
       if(recordEnginePresentCopy(gCompatRecordingCB,pix)){
         gtavdiag::checkpoint("native-engine-copy-recorded");
         si.waitSemaphoreCount=1;si.pWaitSemaphores=&gPresentAcquire[gCompatFrameIndex];si.pWaitDstStageMask=&waitStage;
         si.signalSemaphoreCount=1;si.pSignalSemaphores=&gPresentDone[gCompatFrameIndex];presentReady=true;
       }else gtavdiag::checkpoint("native-engine-copy-record-failed");
     }else{
       char d[64];snprintf(d,sizeof(d),"result=%d",(int)ar);gtavdiag::checkpoint("native-engine-acquire-failed",d);
     }
   }
   VkResult er=vkEndCommandBuffer(gCompatRecordingCB);
   if(er==VK_SUCCESS){
     si.commandBufferCount=1;si.pCommandBuffers=&gCompatRecordingCB;
     VkResult sr=vkQueueSubmit(g.queue,1,&si,prev.fence);
     if(sr==VK_SUCCESS){
       prev.inFlight=true;gtavdiag::checkpoint("compat-command-buffer-submitted");
       if(presentReady){
         VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&gPresentDone[gCompatFrameIndex];pi.swapchainCount=1;pi.pSwapchains=&gPresentProbe.swapchain;pi.pImageIndices=&pix;
         VkResult pr=qp(g.queue,&pi);
         if(pr==VK_SUCCESS||pr==VK_SUBOPTIMAL_KHR){gtavdiag::checkpoint("native-engine-frame-presented");char bd[320];uint32_t ec=gBlackProbeExpectedCbv.load(),bc=gBlackProbeBoundCbv.load(),mc=gBlackProbeMissingCbv.load(),es=gBlackProbeExpectedSrv.load(),bs=gBlackProbeBoundSrv.load(),ms=gBlackProbeMissingSrv.load(),ep=gBlackProbeExpectedSampler.load(),bp=gBlackProbeBoundSampler.load(),mp=gBlackProbeMissingSampler.load(),dw=gBlackProbeDescriptorWrites.load(),dr=gBlackProbeDraws.load();const char* verdict=(mc?"MISSING_CBV":ms?"MISSING_SRV":mp?"MISSING_SAMPLER":dr==0?"NO_DRAWS":"DRAW_AND_PRESENT_OK");snprintf(bd,sizeof(bd),"frame=%llu verdict=%s draws=%u cbv=%u/%u miss=%u srv=%u/%u miss=%u samp=%u/%u miss=%u writes=%u",(unsigned long long)gBlackProbeFrame.load(),verdict,dr,bc,ec,mc,bs,es,ms,bp,ep,mp,dw);gtavdiag::checkpoint("BLACKSCREEN-PROBE",bd);char md[256];snprintf(md,sizeof(md),"frame=%llu drawOk=%u drawFail=%u descFail=%u imageFail=%u noop=%u copy=%u texUpd=%u",(unsigned long long)gBlackProbeFrame.load(),gMegaDrawOk.load(),gMegaDrawFail.load(),gMegaDescriptorFail.load(),gMegaImageFail.load(),gMegaNoopCalls.load(),gMegaCopyOps.load(),gMegaTextureUpdates.load());gtavdiag::checkpoint("MEGA-FRAME-SUMMARY",md);}
         else{char d[64];snprintf(d,sizeof(d),"result=%d",(int)pr);gtavdiag::checkpoint("native-engine-queue-present-failed",d);}
       }
     }else{char d[64];snprintf(d,sizeof(d),"result=%d",(int)sr);gtavdiag::checkpoint("compat-command-buffer-submit-failed",d);}
   }else{char d[64];snprintf(d,sizeof(d),"result=%d",(int)er);gtavdiag::checkpoint("compat-command-buffer-end-failed",d);}
   if(tlsNativeCommandBuffer==gCompatRecordingCB)tlsNativeCommandBuffer=VK_NULL_HANDLE;
   VkCommandBuffer expected=gCompatRecordingCB;observedNativeCommandBuffer.compare_exchange_strong(expected,VK_NULL_HANDLE,std::memory_order_acq_rel);
   gCompatRecordingCB=VK_NULL_HANDLE;gCompatFrameIndex=(gCompatFrameIndex+1)%3;
 }
 CompatFrameCmd& next=gCompatFrameCmd[gCompatFrameIndex];
 if(next.inFlight){VkResult wr=vkWaitForFences(g.device,1,&next.fence,VK_TRUE,1000000000ull);if(wr!=VK_SUCCESS){gtavdiag::checkpoint("compat-command-buffer-fence-wait-failed");return;}profileReadAndLog(gCompatFrameIndex);logBlackProbeReadback(gCompatFrameIndex);vkResetFences(g.device,1,&next.fence);next.inFlight=false;}
 resetCompatPresentSourcesForNewFrame();
 if(vkResetCommandBuffer(next.cb,0)!=VK_SUCCESS){gtavdiag::checkpoint("compat-command-buffer-reset-failed");return;}
 VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
 if(vkBeginCommandBuffer(next.cb,&bi)!=VK_SUCCESS){gtavdiag::checkpoint("compat-command-buffer-begin-failed");return;}
 gCompatRecordingCB=next.cb;profileBeginFrame(gCompatFrameIndex,next.cb);publishNativeCommandBuffer(next.cb,"compat-command-buffer-recording");
}
static inline void publishNativeCommandBuffer(VkCommandBuffer cb,const char* source){
 if(!cb)return;
 tlsNativeCommandBuffer=cb;
 observedNativeCommandBuffer.store(cb,std::memory_order_release);
 uint64_t e=observedCommandBufferEpoch.fetch_add(1,std::memory_order_relaxed)+1;
 if(e<=8 || (e%2048)==0)gtavdiag::checkpoint(source);
}
static inline VkCommandBuffer currentNativeCommandBuffer(){
 if(tlsNativeCommandBuffer)return tlsNativeCommandBuffer;
 return observedNativeCommandBuffer.load(std::memory_order_acquire);
}
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
static bool createCompatOwnedImage(void* resource,uint32_t kind,void* publish);
extern "C" bool gtavnative_compat_register_view_resource(void* view,void* resource,uint32_t kind,bool renderTarget){
 if(!view||!resource)return false;
 // Preserve the COM view identity while also registering its underlying texture/resource.
 // Native mapping still happens lazily once GTA's Vulkan wrapper is available.
 uint64_t mapped=gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)resource,kind);
 if(mapped)gtav_native_renderer_register_resource((uint64_t)(uintptr_t)view,mapped,kind,1);
 return true;
}
static void* compatUnderlyingResource(void* p){
 auto* v=compatViewObject(p);
 return v?v->resource:nullptr;
}
static bool mapWrappedImage(void* rage,uint32_t kind,bool renderTarget){
 if(!rage)return false;
 uint64_t existing=gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)rage,kind);
 void* original=rage;
 // A resource mapping alone is not enough for presentation: older draw-time
 // registrations can contain only VkImage identity and no extent/layout meta.
 // Do not early-return for native RAGE objects; re-wrap once so registerImageMeta()
 // publishes the metadata required by full-size RTV selection.
 if(void* resource=compatUnderlyingResource(rage))rage=resource;
 existing=gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)rage,kind);
 if(existing && original!=rage)gtav_native_renderer_register_resource((uint64_t)(uintptr_t)original,existing,kind,1);
 // Compat D3D objects are our own shells, not RAGE native wrapper objects.
 // Materialize a real Vulkan image for them instead of passing their address
 // into grcTexture/grcRenderTarget wrapper code.
 bool compatObject=(rage==&gCompatBackBuffer);
 if(!compatObject){auto* rr=compatResourceObject(rage);compatObject=(rr&&rr->vtbl==gCompatTexture2DVtable);}
 if(compatObject && createCompatOwnedImage(rage,kind,original))return true;
 resolveNativeMappingFns();
 NativeWrappedImage w{};
 if(renderTarget){if(!rageWrapRenderTarget)return false;rageWrapRenderTarget(rage,&w);}
 else {if(!rageWrapTexture)return false;rageWrapTexture(rage,&w);}
 if(!w.image)return false;
 gtav_native_renderer_register_resource((uint64_t)(uintptr_t)rage,(uint64_t)(uintptr_t)w.image,kind,1);
 if(original!=rage)gtav_native_renderer_register_resource((uint64_t)(uintptr_t)original,(uint64_t)(uintptr_t)w.image,kind,1);
 registerImageMeta(original,w);
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
 if(!g.device)attachFromGtavRuntime();g.frame.fetch_add(1,std::memory_order_relaxed);
 VkCommandBuffer ret=origSubmissionBegin?origSubmissionBegin(self,external):VK_NULL_HANDLE;
 if(ret)publishNativeCommandBuffer(ret,"native-command-buffer-submission");else gtavdiag::checkpoint("native-submission-begin-no-cb");
 return ret;
}
static void hookTransitionOwnedResources(void* self,VkCommandBuffer cb){
 if(cb)publishNativeCommandBuffer(cb,"native-command-buffer-pass-transition");
 if(origTransitionOwnedResources)origTransitionOwnedResources(self,cb);
}
static bool hookPassEndAndSubmit(void* self){
 VkCommandBuffer mine=tlsNativeCommandBuffer;
 bool ok=origPassEndAndSubmit?origPassEndAndSubmit(self):false;
 tlsNativeCommandBuffer=VK_NULL_HANDLE;
 if(mine){VkCommandBuffer expected=mine;observedNativeCommandBuffer.compare_exchange_strong(expected,VK_NULL_HANDLE,std::memory_order_acq_rel);}
 gtavdiag::checkpoint("native-command-buffer-pass-ended");
 return ok;
}
extern "C" __attribute__((visibility("default"))) VkCommandBuffer gtav_native_renderer_observed_command_buffer(){
 return observedNativeCommandBuffer.load(std::memory_order_acquire);
}

static std::atomic<uint64_t> nativeDraws{0},nativeIndexedDraws{0},nativeDispatches{0},fallbackDraws{0};
static void hookDraw(void* c,uint32_t n,uint32_t f){
 if(!g.device) attachFromGtavRuntime();
 if(gtav_native_renderer_rage_draw(c,n,f)){nativeDraws.fetch_add(1,std::memory_order_relaxed);profileCountDraw(false,n);return;}
 fallbackDraws.fetch_add(1,std::memory_order_relaxed);if(origDraw)origDraw(c,n,f);
}
static void hookDrawIndexed(void* c,uint32_t n,uint32_t f,int32_t v){
 if(!g.device) attachFromGtavRuntime();
 if(gtav_native_renderer_rage_draw_indexed(c,n,f,v)){nativeIndexedDraws.fetch_add(1,std::memory_order_relaxed);profileCountDraw(true,n);return;}
 fallbackDraws.fetch_add(1,std::memory_order_relaxed);if(origDrawIndexed)origDrawIndexed(c,n,f,v);
}
static void hookDispatch(void* c,uint32_t x,uint32_t y,uint32_t z){
 if(!g.device) attachFromGtavRuntime();
 uint32_t t=profileBeginExact(PROFILE_DISPATCH,x,(uintptr_t(y)<<32)|z);
 if(gtav_native_renderer_rage_dispatch(c,x,y,z)){nativeDispatches.fetch_add(1,std::memory_order_relaxed);profileEndExact(t);return;}
 profileCancelExact(t);fallbackDraws.fetch_add(1,std::memory_order_relaxed);if(origDispatch)origDispatch(c,x,y,z);
}
extern "C" __attribute__((visibility("default"))) uint64_t gtav_native_renderer_native_commands(){
 return nativeDraws.load(std::memory_order_relaxed)+nativeIndexedDraws.load(std::memory_order_relaxed)+nativeDispatches.load(std::memory_order_relaxed);
}
extern "C" __attribute__((visibility("default"))) uint64_t gtav_native_renderer_fallback_commands(){return fallbackDraws.load(std::memory_order_relaxed);}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_cutover_active(){
 return drawHooksInstalled.load(std::memory_order_acquire)&&g.device&&observedNativeCommandBuffer.load(std::memory_order_acquire)!=VK_NULL_HANDLE;
}
using GtavDlsymFn=void*(*)(void*,const char*);
static GtavDlsymFn origGtavDlsym=nullptr;
static void* hookGtavDlsym(void* handle,const char* name){
 void* p=origGtavDlsym?origGtavDlsym(handle,name):nullptr;
 if(name){
   static std::atomic<uint32_t> dlsymLookups{0};
   uint32_t n=dlsymLookups.fetch_add(1,std::memory_order_relaxed);
   if(n<32){char detail[256];snprintf(detail,sizeof(detail),"n=%u name=%s result=%p",n,name,p);gtavdiag::checkpoint("native-vulkan-loader-dlsym-lookup",detail);}
   if(strcmp(name,"vkGetInstanceProcAddr")==0){
     gtavdiag::checkpoint("native-vulkan-loader-dlsym-gipa-intercept");
     return reinterpret_cast<void*>(&gtavInterceptGIPA);
   }
   // Some GTA loader paths resolve the global entry points directly with dlsym
   // instead of asking GIPA for them. Intercept those before the first instance/device exists.
   if(strcmp(name,"vkCreateInstance")==0){gtavdiag::checkpoint("native-vulkan-loader-dlsym-create-instance-intercept");return reinterpret_cast<void*>(&vkCreateInstance);}
   if(strcmp(name,"vkCreateDevice")==0){gtavdiag::checkpoint("native-vulkan-loader-dlsym-create-device-intercept");return reinterpret_cast<void*>(&vkCreateDevice);}
   if(strcmp(name,"vkGetDeviceProcAddr")==0){gtavdiag::checkpoint("native-vulkan-loader-dlsym-gdpa-intercept");return reinterpret_cast<void*>(&vkGetDeviceProcAddr);}
 }
 return p;
}
static bool installGtavDlsymCallHook(){
 static std::atomic<bool> installed{false};if(installed.load(std::memory_order_acquire))return true;
 if(!gtavBase)dl_iterate_phdr(findGtav,nullptr);if(!gtavBase)return false;
 // grVulkanNativeDeviceAdapter::Initialize: BL dlsym@plt at +0x622f0fc.
 uintptr_t call=gtavBase+0x622f0fc;
 uint32_t ins=*reinterpret_cast<uint32_t*>(call);
 if((ins&0xfc000000u)!=0x94000000u){gtavdiag::checkpoint("native-vulkan-loader-dlsym-call-mismatch");return false;}
 int64_t imm=(int64_t)(ins&0x03ffffffu);if(imm&0x02000000u)imm|=~0x03ffffffll;
 uintptr_t plt=(uintptr_t)((int64_t)call+(imm<<2));
 // Patch the PLT/GOT target used by this dlsym call, preserving the real resolver.
 // AArch64 standard PLT: ADRP x16; LDR x17,[x16,#imm]; ADD x16,x16,#imm; BR x17.
 uint32_t a=*reinterpret_cast<uint32_t*>(plt),b=*reinterpret_cast<uint32_t*>(plt+4),d=*reinterpret_cast<uint32_t*>(plt+8);
 if((a&0x9f00001fu)!=0x90000010u||(b&0xffc003ffu)!=0xf9400211u||(d&0xffc003ffu)!=0x91000210u){gtavdiag::checkpoint("native-vulkan-loader-dlsym-plt-mismatch");return false;}
 int64_t adrp=((int64_t)((a>>5)&0x7ffff)<<2)|((a>>29)&3);if(adrp&(1ll<<20))adrp|=~((1ll<<21)-1);
 uintptr_t page=(plt&~uintptr_t(0xfff))+(adrp<<12);
 uintptr_t got=page+(((b>>10)&0xfff)<<3);
 auto slot=reinterpret_cast<void**>(got);if(!slot||!*slot)return false;
 origGtavDlsym=reinterpret_cast<GtavDlsymFn>(*slot);
 long ps=sysconf(_SC_PAGESIZE);uintptr_t pg=got&~(uintptr_t(ps)-1);
 if(mprotect((void*)pg,ps,PROT_READ|PROT_WRITE)!=0)return false;
 *slot=reinterpret_cast<void*>(&hookGtavDlsym);__builtin___clear_cache((char*)pg,(char*)(pg+ps));
 mprotect((void*)pg,ps,PROT_READ);
 installed.store(true,std::memory_order_release);gtavdiag::checkpoint("native-vulkan-loader-dlsym-got-patched");return true;
}
using GtavNativeAdapterInitFn=bool(*)();
static GtavNativeAdapterInitFn origGtavNativeAdapterInit=nullptr;
static bool hookGtavNativeAdapterInit(){
 gtavdiag::checkpoint("native-vulkan-loader-init-hook-enter");
 if(!gtavBase)dl_iterate_phdr(findGtav,nullptr);
 // Initialize itself resolves vkGetInstanceProcAddr with dlsym and only then stores
 // it at +0x8aac0b0. Patching that slot on hook entry is therefore too early.
 // Run the original initialization first, then replace the live GTA dispatch slot
 // so every later proc lookup is routed through our selective wrapper.
 bool ok=origGtavNativeAdapterInit?origGtavNativeAdapterInit():false;
 if(gtavBase){
   auto slot=reinterpret_cast<PFN_vkGetInstanceProcAddr*>(gtavBase+0x8aac0b0);
   if(slot&&*slot){
     *slot=&gtavInterceptGIPA;
     gtavdiag::checkpoint("native-vulkan-loader-gipa-slot-patched");
   } else {
     gtavdiag::checkpoint("native-vulkan-loader-gipa-slot-empty");
   }
 }
 return ok;
}
static bool installVulkanLoaderInitHook(){
 static std::atomic<bool> installed{false};if(installed.load(std::memory_order_acquire))return true;
 if(!gtavBase)dl_iterate_phdr(findGtav,nullptr);if(!gtavBase)return false;
 static constexpr uint32_t expected[4]={0xa9ba7bfdu,0xa9016ffcu,0xa90267fau,0xa9035ff8u};
 if(std::memcmp((void*)(gtavBase+0x622f0b8),expected,16)!=0){gtavdiag::checkpoint("native-vulkan-loader-init-prologue-mismatch");return false;}
 uint32_t saved[4]{};void* tramp=nullptr;
 bool ok=patchJump(gtavBase+0x622f0b8,(void*)hookGtavNativeAdapterInit,saved,&tramp);
 if(ok){origGtavNativeAdapterInit=(GtavNativeAdapterInitFn)tramp;installed.store(true,std::memory_order_release);gtavdiag::checkpoint("native-vulkan-loader-init-hook-installed");}
 return ok;
}

extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_install_early_vulkan_hook(){
 return installVulkanLoaderInitHook();
}


extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_install_draw_hooks(){
 installVulkanLoaderInitHook();
 if(!gtavBase)dl_iterate_phdr(findGtav,nullptr);
 if(!gtavBase){gtavdiag::checkpoint("native-hook-libgtav-missing");return false;}
 auto mark=[&](const char* name,bool ok){gtavdiag::checkpoint(ok?name:"native-hook-optional-missing",ok?nullptr:name);return ok;};
 auto install=[&](uintptr_t va,void* hook,void** orig,const char* name){
   uint32_t saved[4]{};void* trampoline=nullptr;
   bool ok=patchJump(gtavBase+va,hook,saved,&trampoline);
   if(ok&&orig)*orig=trampoline;
   return mark(name,ok);
 };
 bool draw=false,drawIndexed=false;
 static constexpr uint32_t expectDraw[4]={0xf9400400u,0xf9400008u,0xf9403503u,0xd61f0060u};
 static constexpr uint32_t expectDrawIndexed[4]={0xf9400400u,0xf9400008u,0xf9403104u,0xd61f0080u};
 if(std::memcmp((void*)(gtavBase+0x61d254c),expectDraw,16)==0)draw=install(0x61d254c,(void*)hookDraw,(void**)&origDraw,"native-hook-draw");
 else mark("native-hook-draw-prologue-mismatch",false);
 if(std::memcmp((void*)(gtavBase+0x61d253c),expectDrawIndexed,16)==0)drawIndexed=install(0x61d253c,(void*)hookDrawIndexed,(void**)&origDrawIndexed,"native-hook-draw-indexed");
 else mark("native-hook-draw-indexed-prologue-mismatch",false);
 bool dispatch=install(0x61d2da8,(void*)hookDispatch,(void**)&origDispatch,"native-hook-dispatch");

 // Submission capture is independent from the D3D-shaped state hooks. A missing
 // optional state hook must never prevent us from observing GTA's recording CB.
 bool submission=false;
 static constexpr uint32_t expectSubmissionBegin[4]={0xd10283ffu,0xa9047bfdu,0xf9002bfbu,0xa90667fau};
 if(std::memcmp((void*)(gtavBase+0x623526c),expectSubmissionBegin,16)==0)
   submission=install(0x623526c,(void*)hookSubmissionBegin,(void**)&origSubmissionBegin,"native-hook-submission-begin");
 else mark("native-hook-submission-prologue-mismatch",false);
 bool passCapture=install(0x6235de0,(void*)hookTransitionOwnedResources,(void**)&origTransitionOwnedResources,"native-hook-pass-transition-owned");
 bool passEnd=install(0x6235f54,(void*)hookPassEndAndSubmit,(void**)&origPassEndAndSubmit,"native-hook-pass-end-submit");

 bool state=true;
 state&=install(0x61d2654,(void*)hookIASetInputLayout,(void**)&origIASetInputLayout,"native-hook-input-layout");
 state&=install(0x61d2698,(void*)hookIASetVertexBuffers,(void**)&origIASetVertexBuffers,"native-hook-vertex-buffers");
 state&=install(0x61d27fc,(void*)hookIASetIndexBuffer,(void**)&origIASetIndexBuffer,"native-hook-index-buffer");
 state&=install(0x61d2968,(void*)hookIASetPrimitiveTopology,(void**)&origIASetPrimitiveTopology,"native-hook-topology");
 state&=install(0x61d252c,(void*)hookVSSetShader,(void**)&origVSSetShader,"native-hook-vs");
 state&=install(0x61d2494,(void*)hookPSSetShader,(void**)&origPSSetShader,"native-hook-ps");
 state&=install(0x61d3134,(void*)hookCSSetShader,(void**)&origCSSetShader,"native-hook-cs");
 state&=install(0x61d2e0c,(void*)hookRSSetViewports,(void**)&origRSSetViewports,"native-hook-viewports");
 state&=install(0x61d2e1c,(void*)hookRSSetScissorRects,(void**)&origRSSetScissorRects,"native-hook-scissors");
 state&=install(0x61d23ac,(void*)hVSCB,(void**)&oVSCB,"native-hook-vscb");
 state&=install(0x61d257c,(void*)hPSCB,(void**)&oPSCB,"native-hook-pscb");
 state&=install(0x61d3154,(void*)hCSCB,(void**)&oCSCB,"native-hook-cscb");
 state&=install(0x61d29ac,(void*)hVSSRV,(void**)&oVSSRV,"native-hook-vssrv");
 state&=install(0x61d2484,(void*)hPSSRV,(void**)&oPSSRV,"native-hook-pssrv");
 state&=install(0x61d3114,(void*)hCSSRV,(void**)&oCSSRV,"native-hook-cssrv");
 state&=install(0x61d2a60,(void*)hVSSamp,(void**)&oVSSamp,"native-hook-vssampler");
 state&=install(0x61d24a4,(void*)hPSSamp,(void**)&oPSSamp,"native-hook-pssampler");
 state&=install(0x61d3144,(void*)hCSSamp,(void**)&oCSSamp,"native-hook-cssampler");
 state&=install(0x61d3124,(void*)hCSUAV,(void**)&oCSUAV,"native-hook-csuav");
 state&=install(0x61d2b48,(void*)hOMRT,(void**)&oOMRT,"native-hook-render-targets");

 // "installed" means the native draw cutover is safe. Submission capture can
 // still remain active on its own and provide diagnostics when another hook moved.
 // The compat ID3D11 context already mirrors IA/shader/resource state before these
 // native hooks are armed. Treat moved optional state veneers as diagnostics rather
 // than blocking the verified Draw/DrawIndexed hooks.
 bool cutover=draw&&drawIndexed;
 if(cutover)gtavdiag::checkpoint(state?"native-hook-cutover-full-state":"native-hook-cutover-compat-state");
 else gtavdiag::checkpoint("native-hook-cutover-unavailable");
 drawHooksInstalled.store(cutover,std::memory_order_release);
 char detail[224];snprintf(detail,sizeof(detail),"draw=%d indexed=%d dispatch=%d submission=%d passCapture=%d passEnd=%d state=%d cutover=%d compat-state-fallback=%d",draw,drawIndexed,dispatch,submission,passCapture,passEnd,state,cutover,state?0:1);
 gtavdiag::checkpoint("native-hook-summary",detail);
 return cutover;
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
 if(!g.device||!layout)return VK_NULL_HANDLE;
 std::lock_guard<std::mutex> l(descriptorPoolMutex);
 for(size_t attempt=0;attempt<descriptorPools.size();++attempt){
   VkDescriptorPool pool=descriptorPools[descriptorPools.size()-1-attempt];
   VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
   ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;
   VkDescriptorSet s=VK_NULL_HANDLE;VkResult vr=vkAllocateDescriptorSets(g.device,&ai,&s);
   if(vr==VK_SUCCESS)return s;
   if(vr!=VK_ERROR_OUT_OF_POOL_MEMORY && vr!=VK_ERROR_FRAGMENTED_POOL){
     char d[64];snprintf(d,sizeof(d),"vkResult=%d",(int)vr);gtavdiag::checkpoint("native-descriptor-alloc-error",d);
   }
 }
 uint32_t scale=std::min<uint32_t>(4u,std::max<uint32_t>(1u,descriptorPoolGeneration));
 VkDescriptorPool extra=createCompatDescriptorPool(scale);
 if(!extra){gtavdiag::checkpoint("native-descriptor-overflow-pool-create-failed");return VK_NULL_HANDLE;}
 descriptorPools.push_back(extra);descriptorPoolGeneration++;
 g.descriptors=extra;
 VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
 ai.descriptorPool=extra;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;
 VkDescriptorSet s=VK_NULL_HANDLE;VkResult vr=vkAllocateDescriptorSets(g.device,&ai,&s);
 if(vr==VK_SUCCESS){gtavdiag::checkpoint("native-descriptor-overflow-pool-grown");return s;}
 char d[64];snprintf(d,sizeof(d),"vkResult=%d",(int)vr);gtavdiag::checkpoint("native-descriptor-overflow-alloc-failed",d);
 return VK_NULL_HANDLE;
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
struct NativeImageMeta {
 VkImage image{};VkFormat format{VK_FORMAT_UNDEFINED};VkImageAspectFlags aspect{};
 VkImageViewType viewType{VK_IMAGE_VIEW_TYPE_2D};uint32_t baseMip{},levelCount{1},baseLayer{},layerCount{1};
 uint32_t width{},height{},dxgiFormat{};VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};VkImageUsageFlags usage{};VkImageLayout observedLayout{VK_IMAGE_LAYOUT_UNDEFINED};
};
static std::mutex imageMetaMutex;
static std::unordered_map<uint64_t,NativeImageMeta> imageMeta;
static NativeImageMeta compatViewMeta(VkImage image,VkFormat resourceFormat,VkImageAspectFlags aspect,void* publish,uint32_t kind,uint32_t resourceMips,uint32_t resourceLayers){
 NativeImageMeta m{};m.image=image;m.format=resourceFormat;m.aspect=aspect;m.levelCount=std::max(1u,resourceMips);m.layerCount=std::max(1u,resourceLayers);
 void* metaRes=compatUnderlyingResource(publish);if(!metaRes)metaRes=publish;
 if(auto* rr=compatResourceObject(metaRes);rr&&rr->vtbl==gCompatTexture2DVtable&&rr->descSize>=44){auto* rd=(uint32_t*)rr->desc;m.width=rd[0];m.height=rd[1];m.dxgiFormat=rd[4];m.samples=(VkSampleCountFlagBits)std::max(1u,rd[5]);}
 auto* v=(CompatViewObject*)publish;if(!publish||v->vtbl!=gCompatViewVtable||v->descSize<8){m.levelCount=1;m.layerCount=1;return m;}
 const uint32_t* d=(const uint32_t*)v->desc;VkFormat vf=compatDxgiFormat(d[0]);if(vf!=VK_FORMAT_UNDEFINED){m.format=vf;if(d[0])m.dxgiFormat=d[0];}uint32_t dim=d[1];
 auto clampMip=[&](uint32_t base,uint32_t count){m.baseMip=std::min(base,resourceMips?resourceMips-1:0u);uint32_t avail=std::max(1u,resourceMips-m.baseMip);m.levelCount=(count==0xffffffffu||count==0)?avail:std::min(count,avail);};
 auto clampLayer=[&](uint32_t base,uint32_t count){m.baseLayer=std::min(base,resourceLayers?resourceLayers-1:0u);uint32_t avail=std::max(1u,resourceLayers-m.baseLayer);m.layerCount=(count==0xffffffffu||count==0)?avail:std::min(count,avail);};
 m.viewType=VK_IMAGE_VIEW_TYPE_2D;m.baseMip=0;m.levelCount=1;m.baseLayer=0;m.layerCount=1;
 if(kind==NR_SRV){
  switch(dim){
   case 4:clampMip(d[2],d[3]);break;
   case 5:clampMip(d[2],d[3]);if(v->descSize>=24)clampLayer(d[4],d[5]);m.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;break;
   case 6:break;
   case 7:if(v->descSize>=16)clampLayer(d[2],d[3]);m.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;break;
   case 9:clampMip(d[2],d[3]);clampLayer(0,6);m.viewType=VK_IMAGE_VIEW_TYPE_CUBE;break;
   case 10:clampMip(d[2],d[3]);if(v->descSize>=24){m.baseLayer=d[4];m.layerCount=std::min(std::max(1u,d[5])*6u,std::max(1u,resourceLayers-m.baseLayer));}m.viewType=VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;break;
   default:break;
  }
 }else if(kind==NR_DSV){
  switch(dim){
   case 3:if(v->descSize>=16)clampMip(d[3],1);break;
   case 4:if(v->descSize>=24){clampMip(d[3],1);clampLayer(d[4],d[5]);}m.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;break;
   case 5:break;
   case 6:if(v->descSize>=20)clampLayer(d[3],d[4]);m.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;break;
   default:break;
  }
 }else{
  switch(dim){
   case 4:clampMip(d[2],1);break;
   case 5:clampMip(d[2],1);if(v->descSize>=20)clampLayer(d[3],d[4]);m.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;break;
   case 6:break;
   case 7:if(v->descSize>=16)clampLayer(d[2],d[3]);m.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;break;
   default:break;
  }
 }
 return m;
}
static std::unordered_map<uint64_t,VkImageView> imageViews;
struct CompatOwnedImage {
 VkImage image{};VkDeviceMemory memory{};VkFormat format{VK_FORMAT_R8G8B8A8_UNORM};VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
 VkBuffer staging{};VkDeviceMemory stagingMemory{};void* stagingMapped{};VkDeviceSize stagingSize{};
 VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};uint64_t uploadedVersion{};uint32_t width{},height{},mips{1},layers{1};
};
static std::unordered_map<uint64_t,CompatOwnedImage> compatOwnedImages;
static uint32_t compatMemoryType(uint32_t bits,VkMemoryPropertyFlags wanted){
 VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(g.physical,&mp);
 for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((bits&(1u<<i))&&((mp.memoryTypes[i].propertyFlags&wanted)==wanted))return i;
 for(uint32_t i=0;i<mp.memoryTypeCount;i++)if(bits&(1u<<i))return i;
 return UINT32_MAX;
}
static bool ensureBlackProbeReadback(){
 if(!g.device||!g.physical)return false;
 for(uint32_t i=0;i<3;i++){
  auto& r=gBlackProbeReadback[i];if(r.buffer&&r.memory&&r.mapped)continue;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=r.size;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  if(vkCreateBuffer(g.device,&bi,nullptr,&r.buffer)!=VK_SUCCESS)return false;
  VkMemoryRequirements mr{};vkGetBufferMemoryRequirements(g.device,r.buffer,&mr);
  uint32_t mt=compatMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);if(mt==UINT32_MAX)return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=mt;
  if(vkAllocateMemory(g.device,&ai,nullptr,&r.memory)!=VK_SUCCESS)return false;
  if(vkBindBufferMemory(g.device,r.buffer,r.memory,0)!=VK_SUCCESS)return false;
  if(vkMapMemory(g.device,r.memory,0,r.size,0,&r.mapped)!=VK_SUCCESS)return false;
  std::memset(r.mapped,0,(size_t)r.size);
 }
 gtavdiag::checkpoint("black-probe-readback-ready");return true;
}
static void recordBlackProbeReadback(VkCommandBuffer cb,VkImage src,VkFormat fmt,uint32_t w,uint32_t h,uint32_t slot){
 if(!cb||!src||slot>=3||!w||!h||!ensureBlackProbeReadback())return;
 auto& r=gBlackProbeReadback[slot];r.recorded=false;r.format=fmt;r.width=std::min(4u,w);r.height=std::min(4u,h);
 VkBufferImageCopy cp{};cp.bufferOffset=0;cp.bufferRowLength=0;cp.bufferImageHeight=0;cp.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.imageOffset={0,0,0};cp.imageExtent={r.width,r.height,1};
 vkCmdCopyImageToBuffer(cb,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.buffer,1,&cp);
 r.recorded=true;
}
static void logBlackProbeReadback(uint32_t slot){
 if(slot>=3)return;auto& r=gBlackProbeReadback[slot];if(!r.recorded||!r.mapped)return;
 const uint8_t* p=(const uint8_t*)r.mapped;uint64_t hash=1469598103934665603ull;uint32_t nonzero=0,nonff=0,sum=0,minv=255,maxv=0;
 for(size_t i=0;i<256&&i<(size_t)r.size;i++){uint8_t v=p[i];hash^=v;hash*=1099511628211ull;if(v)nonzero++;if(v!=0xff)nonff++;sum+=v;minv=std::min<uint32_t>(minv,v);maxv=std::max<uint32_t>(maxv,v);}
 char d[320];snprintf(d,sizeof(d),"slot=%u fmt=%d extent=%ux%u hash=%016llx nz=%u nonff=%u sum=%u min=%u max=%u bytes=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x",slot,(int)r.format,r.width,r.height,(unsigned long long)hash,nonzero,nonff,sum,minv,maxv,p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
 gtavdiag::checkpoint("BLACKSCREEN-PIXEL-PROBE",d);r.recorded=false;
}

static bool createCompatOwnedImage(void* resource,uint32_t kind,void* publish){
 if(!resource||!publish||!g.device||!g.physical)return false;
 const uint64_t key=(uint64_t)(uintptr_t)resource;
 {std::lock_guard<std::mutex> l(imageMetaMutex);auto it=compatOwnedImages.find(key);if(it!=compatOwnedImages.end()){
   gtav_native_renderer_register_resource((uint64_t)(uintptr_t)publish,(uint64_t)(uintptr_t)it->second.image,kind,1);
   imageMeta[(uint64_t)(uintptr_t)publish]=compatViewMeta(it->second.image,it->second.format,it->second.aspect,publish,kind,it->second.mips,it->second.layers);return true;
 }}
 uint32_t w=0,h=0,fmt=28,mips=1,layers=1,samples=1,bindFlags=0x28u,miscFlags=0;
 if(resource==&gCompatBackBuffer){w=gCompatSwapWidth.load();h=gCompatSwapHeight.load();fmt=gCompatSwapFormat.load();bindFlags=0x20u;}
 else {
   auto* r=(CompatResourceObject*)resource;
   if(r->vtbl==gCompatTexture2DVtable&&r->descSize>=44){auto* d=(uint32_t*)r->desc;w=d[0];h=d[1];mips=std::max(1u,d[2]);layers=std::max(1u,d[3]);fmt=d[4];samples=std::max(1u,d[5]);bindFlags=d[8];miscFlags=d[10];}
   else return false;
 }
 if(!w||!h||w>16384||h>16384||mips>16||layers>2048){gtavdiag::checkpoint("native-compat-image-invalid-desc");return false;}
 VkFormat vf=compatDxgiFormat(fmt);if(vf==VK_FORMAT_UNDEFINED){char d[64];snprintf(d,sizeof(d),"dxgi=%u",fmt);gtavdiag::checkpoint("native-compat-image-unsupported-format",d);return false;}
 bool depth=(bindFlags&0x40u)!=0||kind==NR_DSV;VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
 VkFormatFeatureFlags need=0;
 if(bindFlags&0x08u){usage|=VK_IMAGE_USAGE_SAMPLED_BIT;need|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;}
 if(bindFlags&0x20u){usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;need|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;}
 if(bindFlags&0x40u){usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;need|=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;}
 if(bindFlags&0x80u){usage|=VK_IMAGE_USAGE_STORAGE_BIT;need|=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;}
 if(kind==NR_SRV){usage|=VK_IMAGE_USAGE_SAMPLED_BIT;need|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;}
 if(kind==NR_RTV){usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;need|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;}
 if(kind==NR_DSV){usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;need|=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;}
 if(kind==NR_UAV){usage|=VK_IMAGE_USAGE_STORAGE_BIT;need|=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;}
 VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(g.physical,vf,&fp);
 if((fp.optimalTilingFeatures&need)!=need){gtavdiag::checkpoint("native-compat-image-format-unsupported");return false;}
 VkSampleCountFlagBits sc=VK_SAMPLE_COUNT_1_BIT;
 switch(samples){case 1:sc=VK_SAMPLE_COUNT_1_BIT;break;case 2:sc=VK_SAMPLE_COUNT_2_BIT;break;case 4:sc=VK_SAMPLE_COUNT_4_BIT;break;case 8:sc=VK_SAMPLE_COUNT_8_BIT;break;default:gtavdiag::checkpoint("native-compat-image-samples-unsupported");return false;}
 VkImageAspectFlags aspect=depth?(vf==VK_FORMAT_D24_UNORM_S8_UINT?(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT):VK_IMAGE_ASPECT_DEPTH_BIT):VK_IMAGE_ASPECT_COLOR_BIT;
 VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=vf;ci.extent={w,h,1};ci.mipLevels=mips;ci.arrayLayers=layers;ci.samples=sc;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
 if(!depth&&(usage&VK_IMAGE_USAGE_SAMPLED_BIT))ci.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
 if((miscFlags&0x4u)&&layers>=6)ci.flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
 {static std::atomic<uint32_t> mb{128};uint32_t n=mb.fetch_sub(1,std::memory_order_relaxed);if(n>0){char d[192];snprintf(d,sizeof(d),"dxgi=%u vkfmt=%d flags=0x%x usage=0x%x extent=%ux%u mips=%u",fmt,(int)vf,(unsigned)ci.flags,(unsigned)usage,w,h,mips);gtavdiag::checkpoint("native-image-create-format",d);}}
 VkImage img{};VkResult cr=vkCreateImage(g.device,&ci,nullptr,&img);if(cr!=VK_SUCCESS){gtavdiag::checkpoint("native-compat-image-create-failed");return false;}
 VkMemoryRequirements mr{};vkGetImageMemoryRequirements(g.device,img,&mr);uint32_t mt=compatMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(mt==UINT32_MAX){vkDestroyImage(g.device,img,nullptr);return false;}
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=mt;VkDeviceMemory mem{};
 if(vkAllocateMemory(g.device,&ai,nullptr,&mem)!=VK_SUCCESS||vkBindImageMemory(g.device,img,mem,0)!=VK_SUCCESS){if(mem)vkFreeMemory(g.device,mem,nullptr);vkDestroyImage(g.device,img,nullptr);return false;}
 CompatOwnedImage owned{};owned.image=img;owned.memory=mem;owned.format=vf;owned.aspect=aspect;owned.width=w;owned.height=h;owned.mips=mips;owned.layers=layers;
 {static std::atomic<uint32_t> fb{128};uint32_t n=fb.fetch_sub(1,std::memory_order_relaxed);if(n>0){char d[160];snprintf(d,sizeof(d),"resource=%p dxgi=%u baseVk=%d",(void*)resource,fmt,(int)vf);gtavdiag::checkpoint("native-image-resource-format",d);}}
 if(resource!=&gCompatBackBuffer){auto* rr=(CompatResourceObject*)resource;if(!rr->backing.empty()){VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=rr->backing.size();bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
   if(vkCreateBuffer(g.device,&bi,nullptr,&owned.staging)==VK_SUCCESS){VkMemoryRequirements sr{};vkGetBufferMemoryRequirements(g.device,owned.staging,&sr);uint32_t smt=compatMemoryType(sr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(smt!=UINT32_MAX){VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};sai.allocationSize=sr.size;sai.memoryTypeIndex=smt;if(vkAllocateMemory(g.device,&sai,nullptr,&owned.stagingMemory)==VK_SUCCESS&&vkBindBufferMemory(g.device,owned.staging,owned.stagingMemory,0)==VK_SUCCESS&&vkMapMemory(g.device,owned.stagingMemory,0,bi.size,0,&owned.stagingMapped)==VK_SUCCESS)owned.stagingSize=bi.size;}}}}
 {std::lock_guard<std::mutex> l(imageMetaMutex);compatOwnedImages.emplace(key,owned);imageMeta[(uint64_t)(uintptr_t)publish]=compatViewMeta(img,vf,aspect,publish,kind,mips,layers);}
 gtav_native_renderer_register_resource((uint64_t)(uintptr_t)resource,(uint64_t)(uintptr_t)img,kind,1);
 gtav_native_renderer_register_resource((uint64_t)(uintptr_t)publish,(uint64_t)(uintptr_t)img,kind,1);
 gtavdiag::checkpoint("native-compat-image-created");return true;
}

static bool ensureEnginePresentSync(){if(gPresentSyncReady)return true;if(!g.device||!gPresentProbe.swapchain)return false;VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};for(int i=0;i<3;i++)if(vkCreateSemaphore(g.device,&s,nullptr,&gPresentAcquire[i])!=VK_SUCCESS||vkCreateSemaphore(g.device,&s,nullptr,&gPresentDone[i])!=VK_SUCCESS)return false;return gPresentSyncReady=true;}
static bool recordEnginePresentCopy(VkCommandBuffer cb,uint32_t ix){
 if(!cb||ix>=gPresentProbe.images.size())return false;
 VkImage src{};VkImageLayout old{};VkFormat srcFormat=VK_FORMAT_UNDEFINED;uint32_t sw=0,sh=0;void* presentResource=&gCompatBackBuffer;
 uint64_t fs=lastCompatFullSizeSerial.load(std::memory_order_acquire);
 uint64_t ts=lastCompatTransferSerial.load(std::memory_order_acquire);
 uint64_t ds=lastCompatDrawnSerial.load(std::memory_order_acquire);
 void* chosen=nullptr; enum { SRC_NONE, SRC_FULLSIZE, SRC_TRANSFER, SRC_DRAWN } sourceKind=SRC_NONE;
 if(fs>=ts&&fs>=ds&&fs){chosen=lastCompatFullSizeRTV.load(std::memory_order_acquire);sourceKind=SRC_FULLSIZE;}
 else if(ts>=ds&&ts){chosen=lastCompatFinalTransferDst.load(std::memory_order_acquire);sourceKind=SRC_TRANSFER;}
 else if(ds){chosen=lastCompatDrawnRTV.load(std::memory_order_acquire);sourceKind=SRC_DRAWN;}
 if(chosen){
   void* r=compatUnderlyingResource(chosen);if(!r)r=chosen;
   auto* rr=compatResourceObject(r);bool compat=rr&&rr->vtbl==gCompatTexture2DVtable;
   bool mapped=compat?createCompatOwnedImage(r,2u,chosen):mapWrappedImage(r,2u,true);
   if(mapped){
     presentResource=r;
     char sd[160];snprintf(sd,sizeof(sd),"kind=%u serial=%llu full=%llu transfer=%llu drawn=%llu resource=%p",(unsigned)sourceKind,(unsigned long long)std::max(fs,std::max(ts,ds)),(unsigned long long)fs,(unsigned long long)ts,(unsigned long long)ds,r);
     gtavdiag::checkpoint("native-engine-present-newest-written-source",sd);
   }
 } {std::lock_guard<std::mutex> l(imageMetaMutex);auto it=compatOwnedImages.find((uint64_t)(uintptr_t)presentResource);if(it!=compatOwnedImages.end()){src=it->second.image;old=it->second.layout;srcFormat=it->second.format;sw=it->second.width;sh=it->second.height;}else{auto mi=imageMeta.find((uint64_t)(uintptr_t)presentResource);if(mi==imageMeta.end()||!mi->second.image){gtavdiag::checkpoint("native-engine-present-source-missing");return false;}src=mi->second.image;old=mi->second.observedLayout==VK_IMAGE_LAYOUT_UNDEFINED?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:mi->second.observedLayout;srcFormat=mi->second.format;sw=mi->second.width?mi->second.width:gCompatSwapWidth.load();sh=mi->second.height?mi->second.height:gCompatSwapHeight.load();char d[160];snprintf(d,sizeof(d),"resource=%p image=%p fmt=%d extent=%ux%u layout=%d",presentResource,(void*)src,(int)mi->second.format,sw,sh,(int)old);gtavdiag::checkpoint("native-engine-present-rage-meta-source",d);}}
 if(!src||!sw||!sh)return false;
 VkImageMemoryBarrier pre[2]{};
 for(auto& x:pre){x.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;x.srcQueueFamilyIndex=x.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;x.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
 if(old==VK_IMAGE_LAYOUT_UNDEFINED){gtavdiag::checkpoint("native-engine-present-rejected-undefined-layout");return false;}
 pre[0].oldLayout=old;pre[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;pre[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;pre[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;pre[0].image=src;
 pre[1].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;pre[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;pre[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;pre[1].image=gPresentProbe.images[ix];
 vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,pre);
 recordBlackProbeReadback(cb,src,srcFormat,sw,sh,gCompatFrameIndex);
 const bool exactExtent=(sw==gPresentProbe.extent.width&&sh==gPresentProbe.extent.height);
 const bool exactFormat=(srcFormat!=VK_FORMAT_UNDEFINED&&srcFormat==gPresentProbe.format);
 if(exactExtent&&exactFormat){
   VkImageCopy cp{};cp.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.extent={sw,sh,1};
   vkCmdCopyImage(cb,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,gPresentProbe.images[ix],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&cp);
   gtavdiag::checkpoint("native-present-bridge-copy");
 }else{
   const uint32_t dw=gPresentProbe.extent.width,dh=gPresentProbe.extent.height;
   double srcAspect=(double)sw/(double)sh,dstAspect=(double)dw/(double)dh;
   uint32_t fitW=dw,fitH=dh;
   if(srcAspect>dstAspect)fitH=std::max(1u,(uint32_t)llround((double)dw/srcAspect));
   else fitW=std::max(1u,(uint32_t)llround((double)dh*srcAspect));
   int32_t dx=((int32_t)dw-(int32_t)fitW)/2,dy=((int32_t)dh-(int32_t)fitH)/2;
   VkClearColorValue black{};VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
   vkCmdClearColorImage(cb,gPresentProbe.images[ix],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&clearRange);
   VkImageBlit bl{};bl.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};bl.srcOffsets[1]={(int32_t)sw,(int32_t)sh,1};bl.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};bl.dstOffsets[0]={dx,dy,0};bl.dstOffsets[1]={dx+(int32_t)fitW,dy+(int32_t)fitH,1};
   vkCmdBlitImage(cb,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,gPresentProbe.images[ix],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bl,VK_FILTER_LINEAR);
   char bd[224];snprintf(bd,sizeof(bd),"srcfmt=%d dstfmt=%d src=%ux%u swap=%ux%u fit=%ux%u off=%d,%d transform=0x%x",(int)srcFormat,(int)gPresentProbe.format,sw,sh,dw,dh,fitW,fitH,dx,dy,(unsigned)gPresentProbe.surfaceTransform);gtavdiag::checkpoint("native-present-bridge-blit",bd);
 }
 VkImageMemoryBarrier post[2]{};
 for(auto& x:post){x.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;x.srcQueueFamilyIndex=x.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;x.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
 post[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;post[0].newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;post[0].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;post[0].image=gPresentProbe.images[ix];
 post[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;post[1].newLayout=old;post[1].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;post[1].image=src;
 VkPipelineStageFlags restoreStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
 if(old==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL){post[1].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;restoreStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;}
 else if(old==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){post[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;restoreStage=VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;}
 else if(old==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL){post[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;restoreStage=VK_PIPELINE_STAGE_TRANSFER_BIT;}
 else if(old==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL){post[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;restoreStage=VK_PIPELINE_STAGE_TRANSFER_BIT;}
 else if(old==VK_IMAGE_LAYOUT_GENERAL){post[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;}
 else if(old==VK_IMAGE_LAYOUT_UNDEFINED){gtavdiag::checkpoint("native-engine-present-rejected-undefined-layout");return false;}
 vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,restoreStage|VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,2,post);
 VkImageLayout restored=post[1].newLayout;
 {std::lock_guard<std::mutex> l(imageMetaMutex);auto it=compatOwnedImages.find((uint64_t)(uintptr_t)presentResource);if(it!=compatOwnedImages.end())it->second.layout=restored;auto mi=imageMeta.find((uint64_t)(uintptr_t)presentResource);if(mi!=imageMeta.end())mi->second.observedLayout=restored;}
 static std::atomic<uint32_t> liveDiag{0};uint32_t dn=liveDiag.fetch_add(1,std::memory_order_relaxed);if(dn<8||dn%600==0){char d[160];snprintf(d,sizeof(d),"src=%p resource=%p extent=%ux%u old=%d restored=%d swap=%ux%u",(void*)src,presentResource,sw,sh,(int)old,(int)restored,gPresentProbe.extent.width,gPresentProbe.extent.height);gtavdiag::checkpoint("native-engine-present-source-state",d);}
 return true;
}
static bool transitionCompatOwnedImage(VkCommandBuffer cb,void* object,VkImageLayout target){
 if(!cb||!object)return false;void* resource=compatUnderlyingResource(object);if(!resource)resource=object;const uint64_t key=(uint64_t)(uintptr_t)resource;
 std::lock_guard<std::mutex> l(imageMetaMutex);auto it=compatOwnedImages.find(key);if(it==compatOwnedImages.end())return true;auto& o=it->second;if(o.layout==target)return true;
 VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=o.layout;b.newLayout=target;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=o.image;
 b.subresourceRange.aspectMask=o.aspect;b.subresourceRange.baseMipLevel=0;b.subresourceRange.levelCount=o.mips;b.subresourceRange.baseArrayLayer=0;b.subresourceRange.layerCount=o.layers;
 VkPipelineStageFlags src=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,dst=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
 if(o.layout==VK_IMAGE_LAYOUT_UNDEFINED){src=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;b.srcAccessMask=0;}
 else if(o.layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL){src=VK_PIPELINE_STAGE_TRANSFER_BIT;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;}
 else if(o.layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL){src=VK_PIPELINE_STAGE_TRANSFER_BIT;b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;}
 else if(o.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){src=VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;}
 else if(o.layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL){src=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;}
 else if(o.layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL){src=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;b.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;}
 if(target==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL){dst=VK_PIPELINE_STAGE_TRANSFER_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;}
 else if(target==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL){dst=VK_PIPELINE_STAGE_TRANSFER_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;}
 else if(target==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){dst=VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;}
 else if(target==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL){dst=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;b.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;}
 else if(target==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL){dst=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;b.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;}
 vkCmdPipelineBarrier(cb,src,dst,0,0,nullptr,0,nullptr,1,&b);o.layout=target;return true;
}
static bool compatGpuCopyResource(void* dst,void* src){
 if(!dst||!src||!gCompatRecordingCB)return false;void* dr=compatUnderlyingResource(dst);if(!dr)dr=dst;void* sr=compatUnderlyingResource(src);if(!sr)sr=src;
 if(!createCompatOwnedImage(dr,2u,dst)||!createCompatOwnedImage(sr,1u,src))return false;
 if(!transitionCompatOwnedImage(gCompatRecordingCB,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)||!transitionCompatOwnedImage(gCompatRecordingCB,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))return false;
 VkImage di{},si{};uint32_t w=0,h=0;{std::lock_guard<std::mutex> l(imageMetaMutex);auto dit=compatOwnedImages.find((uint64_t)(uintptr_t)dr),sit=compatOwnedImages.find((uint64_t)(uintptr_t)sr);if(dit==compatOwnedImages.end()||sit==compatOwnedImages.end()||dit->second.format!=sit->second.format||dit->second.aspect!=VK_IMAGE_ASPECT_COLOR_BIT||sit->second.aspect!=VK_IMAGE_ASPECT_COLOR_BIT)return false;di=dit->second.image;si=sit->second.image;w=std::min(dit->second.width,sit->second.width);h=std::min(dit->second.height,sit->second.height);}
 VkImageCopy cp{};cp.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.extent={w,h,1};vkCmdCopyImage(gCompatRecordingCB,si,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,di,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&cp);lastCompatFinalTransferDst.store(dst,std::memory_order_release);lastCompatTransferSerial.store(compatPresentWriteSerial.fetch_add(1,std::memory_order_acq_rel)+1,std::memory_order_release);gtavdiag::checkpoint("native-compat-gpu-copy-resource");return true;
}
static bool compatGpuCopySubresource(void* dst,uint32_t ds,uint32_t x,uint32_t y,uint32_t z,void* src,uint32_t ss,const void* box){
 if(box||x||y||z||ds||ss){gtavdiag::checkpoint("native-compat-gpu-copy-subresource-complex");return false;}return compatGpuCopyResource(dst,src);
}
static bool compatGpuResolveResource(void* dst,uint32_t ds,void* src,uint32_t ss){
 if(ds||ss){gtavdiag::checkpoint("native-compat-gpu-resolve-subresource-complex");return false;}void* dr=compatUnderlyingResource(dst);if(!dr)dr=dst;void* sr=compatUnderlyingResource(src);if(!sr)sr=src;auto* d=compatResourceObject(dr);auto* s=compatResourceObject(sr);if(!d||!s||d->vtbl!=gCompatTexture2DVtable||s->vtbl!=gCompatTexture2DVtable||d->descSize<44||s->descSize<44)return false;auto* dd=(uint32_t*)d->desc;auto* sd=(uint32_t*)s->desc;uint32_t dsamp=std::max(1u,dd[5]),ssamp=std::max(1u,sd[5]);if(ssamp==1&&dsamp==1)return compatGpuCopyResource(dst,src);if(ssamp<=1||dsamp!=1||dd[4]!=sd[4])return false;if(!createCompatOwnedImage(dr,2u,dst)||!createCompatOwnedImage(sr,1u,src))return false;if(!transitionCompatOwnedImage(gCompatRecordingCB,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)||!transitionCompatOwnedImage(gCompatRecordingCB,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))return false;VkImage di{},si{};uint32_t w=std::min(dd[0],sd[0]),h=std::min(dd[1],sd[1]);{std::lock_guard<std::mutex> l(imageMetaMutex);auto dit=compatOwnedImages.find((uint64_t)(uintptr_t)dr),sit=compatOwnedImages.find((uint64_t)(uintptr_t)sr);if(dit==compatOwnedImages.end()||sit==compatOwnedImages.end())return false;di=dit->second.image;si=sit->second.image;}VkImageResolve r{};r.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};r.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};r.extent={w,h,1};vkCmdResolveImage(gCompatRecordingCB,si,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,di,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&r);lastCompatFinalTransferDst.store(dst,std::memory_order_release);lastCompatTransferSerial.store(compatPresentWriteSerial.fetch_add(1,std::memory_order_acq_rel)+1,std::memory_order_release);gtavdiag::checkpoint("native-compat-gpu-resolve");return true;
}
static bool syncCompatOwnedImage(VkCommandBuffer cb,void* object){
 if(!cb||!object)return false;void* resource=compatUnderlyingResource(object);if(!resource)resource=object;if(resource==&gCompatBackBuffer)return true;
 auto* rr=(CompatResourceObject*)resource;if(rr->vtbl!=gCompatTexture2DVtable)return true;const uint64_t key=(uint64_t)(uintptr_t)resource;
 uint64_t version=rr->version;{
  std::lock_guard<std::mutex> l(imageMetaMutex);auto it=compatOwnedImages.find(key);if(it==compatOwnedImages.end())return true;if(it->second.uploadedVersion==version)return true;
  if(!it->second.stagingMapped||!it->second.staging||rr->backing.empty()||it->second.stagingSize<rr->backing.size())return false;
  std::memcpy(it->second.stagingMapped,rr->backing.data(),rr->backing.size());
 }
 if(!transitionCompatOwnedImage(cb,object,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))return false;
 std::vector<VkBufferImageCopy> regions;{
  std::lock_guard<std::mutex> l(imageMetaMutex);auto it=compatOwnedImages.find(key);if(it==compatOwnedImages.end())return true;auto& o=it->second;
  if(o.aspect!=VK_IMAGE_ASPECT_COLOR_BIT){o.uploadedVersion=version;return true;}
  const uint32_t* d=(const uint32_t*)rr->desc;uint32_t count=std::min<uint32_t>(o.mips*o.layers,4096u);regions.reserve(count);
  for(uint32_t s=0;s<count;s++){uint32_t row=0,depth=0;size_t off=0;compatTexture2DLayout(d,s,&row,&depth,&off);uint32_t mip=s%o.mips,layer=s/o.mips;
   VkBufferImageCopy x{};x.bufferOffset=off;x.imageSubresource.aspectMask=o.aspect;x.imageSubresource.mipLevel=mip;x.imageSubresource.baseArrayLayer=layer;x.imageSubresource.layerCount=1;
   x.imageExtent={std::max(1u,o.width>>std::min(mip,31u)),std::max(1u,o.height>>std::min(mip,31u)),1};regions.push_back(x);}
  if(!regions.empty())vkCmdCopyBufferToImage(cb,o.staging,o.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,(uint32_t)regions.size(),regions.data());o.uploadedVersion=version;
 }
 return true;
}
static void invalidateImageResource(uint64_t rage){
 std::lock_guard<std::mutex> l(imageMetaMutex);
 auto v=imageViews.find(rage);
 if(v!=imageViews.end()){if(v->second&&g.device)vkDestroyImageView(g.device,v->second,nullptr);imageViews.erase(v);}
 imageMeta.erase(rage);
}
static void registerImageMeta(void* rage,const NativeWrappedImage& w){
 if(!rage||!w.image)return;
 const uint64_t key=(uint64_t)(uintptr_t)rage;
 NativeImageMeta m{};m.image=w.image;m.format=(VkFormat)w.format;m.aspect=(VkImageAspectFlags)w.aspect;m.observedLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; if(auto* rr=compatResourceObject(rage);rr&&rr->vtbl==gCompatTexture2DVtable&&rr->descSize>=44){auto* d=(uint32_t*)rr->desc;m.width=d[0];m.height=d[1];m.samples=(VkSampleCountFlagBits)std::max(1u,d[5]);} else {m.width=gCompatSwapWidth.load();m.height=gCompatSwapHeight.load();}m.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
 VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=m.image;ci.viewType=m.viewType;ci.format=m.format;
 if(m.dxgiFormat==65){
   ci.components.r=VK_COMPONENT_SWIZZLE_ZERO;
   ci.components.g=VK_COMPONENT_SWIZZLE_ZERO;
   ci.components.b=VK_COMPONENT_SWIZZLE_ZERO;
   ci.components.a=VK_COMPONENT_SWIZZLE_R;
   static std::atomic<uint32_t> a8Budget{64};uint32_t n=a8Budget.fetch_sub(1,std::memory_order_relaxed);
   if(n>0){char d[128];snprintf(d,sizeof(d),"resource=%p vkfmt=%d",(void*)rageResource,(int)m.format);gtavdiag::checkpoint("native-a8-alpha-swizzle",d);}
 }
 if(m.dxgiFormat==88||m.dxgiFormat==92||m.dxgiFormat==93){
   ci.components.a=VK_COMPONENT_SWIZZLE_ONE;
   static std::atomic<uint32_t> x8Budget{128};uint32_t n=x8Budget.fetch_sub(1,std::memory_order_relaxed);
   if(n>0){char d[128];snprintf(d,sizeof(d),"resource=%p dxgi=%u vkfmt=%d",(void*)rageResource,m.dxgiFormat,(int)m.format);gtavdiag::checkpoint("native-bgrx-alpha-one",d);}
 }
 ci.subresourceRange.aspectMask=m.aspect;ci.subresourceRange.baseMipLevel=m.baseMip;ci.subresourceRange.levelCount=std::max(1u,m.levelCount);ci.subresourceRange.baseArrayLayer=m.baseLayer;ci.subresourceRange.layerCount=std::max(1u,m.layerCount);
 VkImageView view=VK_NULL_HANDLE;VkResult ivr=vkCreateImageView(g.device,&ci,nullptr,&view);
 if(ivr!=VK_SUCCESS){
   char d[224];snprintf(d,sizeof(d),"resource=%p result=%d dxgi=%u imageFmt=%d viewFmt=%d type=%d mip=%u+%u layer=%u+%u",(void*)rageResource,(int)ivr,m.dxgiFormat,(int)m.format,(int)ci.format,(int)ci.viewType,m.baseMip,m.levelCount,m.baseLayer,m.layerCount);
   gtavdiag::checkpoint("native-image-view-create-failed",d);return VK_NULL_HANDLE;
 }
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
struct CompatShaderDescriptorDecl {uint32_t binding{};VkDescriptorType type{};uint32_t count{1};VkShaderStageFlags stages{};uint32_t scalarType{},space{},reg{},resourceKind{};};
static std::mutex compatShaderDeclMutex;
static std::unordered_map<uint64_t,std::vector<CompatShaderDescriptorDecl>> compatShaderDecls;
static std::vector<CompatShaderDescriptorDecl> getCompatShaderDecls(void* shader){
 std::lock_guard<std::mutex> l(compatShaderDeclMutex);auto it=compatShaderDecls.find((uint64_t)(uintptr_t)shader);return it==compatShaderDecls.end()?std::vector<CompatShaderDescriptorDecl>{}:it->second;
}
#if GTAV_HAVE_DXBC_SPIRV
static uint32_t compatDescriptorBinding(uint32_t stage,uint32_t type,uint32_t space,uint32_t reg){
 uint32_t base=stage*128u+space*512u;
 switch(type){case 23:return base+reg;case 24:return base+32u+reg;case 22:return base+64u+reg;case 25:return base+96u+reg;case 26:return base+112u+reg;default:return base+120u+reg;}
}
class CompatResourceMapping final: public dxbc_spv::spirv::ResourceMapping {
public:
 explicit CompatResourceMapping(uint32_t s):stage(s){}
 dxbc_spv::spirv::DescriptorBinding mapDescriptor(dxbc_spv::ir::ScalarType type,uint32_t space,uint32_t reg) override {
  return {0u,compatDescriptorBinding(stage,(uint32_t)type,space,reg)};
 }
 uint32_t mapPushData(dxbc_spv::ir::ShaderStageMask stages) override {
  uint32_t raw=uint32_t(stages);if(!raw||(raw&(raw-1u))||raw==uint32_t(dxbc_spv::ir::ShaderStage::eCompute))return 0u;
  uint32_t bit=0;while(((raw>>bit)&1u)==0u&&bit<31u)bit++;return 64u+32u*bit;
 }
private:uint32_t stage;
};
static bool compileCompatDxbcToSpirv(const CompatShaderObject* s,uint32_t kind,std::vector<uint32_t>& out,std::vector<CompatShaderDescriptorDecl>& decls){
 if(!s||s->bytecode.empty())return false;
 dxbc_spv::dxbc::Converter::Options co{};co.includeDebugNames=false;co.name="gtav-compat";
 dxbc_spv::ir::CompileOptions io{};io.cseOptions.relocateDescriptorLoad=true;io.descriptorIndexing.optimizeDescriptorIndexing=true;
 auto ir=dxbc_spv::dxbc::compileShaderToLegalizedIr(s->bytecode.data(),s->bytecode.size(),co,io);
 if(!ir){gtavdiag::checkpoint("native-shader-dxbc-convert-failed");return false;}
 uint32_t stage=kind==NR_PS?1u:kind==NR_CS?2u:0u;CompatResourceMapping mapping(stage);
 VkShaderStageFlags vkStage=kind==NR_PS?VK_SHADER_STAGE_FRAGMENT_BIT:kind==NR_CS?VK_SHADER_STAGE_COMPUTE_BIT:VK_SHADER_STAGE_VERTEX_BIT;
 auto range=ir->getDeclarations();
 for(auto it=range.first;it!=range.second;++it){
  const auto& op=*it;uint32_t scalar=0;VkDescriptorType dt=VK_DESCRIPTOR_TYPE_MAX_ENUM;uint32_t rk=0;
  switch(op.getOpCode()){
   case dxbc_spv::ir::OpCode::eDclSampler:scalar=22;dt=VK_DESCRIPTOR_TYPE_SAMPLER;break;
   case dxbc_spv::ir::OpCode::eDclCbv:scalar=23;dt=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;break;
   case dxbc_spv::ir::OpCode::eDclSrv:scalar=24;rk=uint32_t(op.getOperand(4u));dt=(rk==0)?VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:(rk==1||rk==2)?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;break;
   case dxbc_spv::ir::OpCode::eDclUav:scalar=25;rk=uint32_t(op.getOperand(4u));dt=(rk==0)?VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:(rk==1||rk==2)?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;break;
   case dxbc_spv::ir::OpCode::eDclInputTarget:scalar=27;rk=uint32_t(op.getOperand(4u));dt=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;break;
   default:continue;
  }
  uint32_t space=uint32_t(op.getOperand(1u)),reg=uint32_t(op.getOperand(2u)),count=std::max(1u,uint32_t(op.getOperand(3u)));
  decls.push_back({compatDescriptorBinding(stage,scalar,space,reg),dt,count,vkStage,scalar,space,reg,rk});
 }
 dxbc_spv::spirv::SpirvBuilder::Options so{};so.includeDebugNames=false;so.floatControls2=false;
 so.supportedRoundModesF16=so.supportedRoundModesF32=so.supportedRoundModesF64=dxbc_spv::ir::RoundMode::eNearestEven|dxbc_spv::ir::RoundMode::eZero;
 so.supportedDenormModesF16=so.supportedDenormModesF32=so.supportedDenormModesF64=dxbc_spv::ir::DenormMode::eFlush|dxbc_spv::ir::DenormMode::ePreserve;
 dxbc_spv::spirv::SpirvBuilder sb(*ir,mapping,so);sb.buildSpirvBinary();out=sb.getSpirvBinary();
 if(out.empty()){gtavdiag::checkpoint("native-shader-dxbc-spirv-empty");return false;}
 gtavdiag::checkpoint("native-shader-dxbc-spirv-ok");return true;
}
#endif
static std::mutex compatShaderMapMutex;
static std::unordered_map<uint64_t,VkShaderModule> compatShaderModules;
static bool mapCompatShader(void* p,uint32_t kind){
 if(!p||!g.device||(kind!=NR_VS&&kind!=NR_PS&&kind!=NR_CS))return false;
 if(gtav_native_renderer_resolve_resource((uint64_t)(uintptr_t)p,kind))return true;
 if(!compatShaderObject(p)){gtavdiag::checkpoint("native-shader-not-compat-object");return false;}
 auto* s=(CompatShaderObject*)p;
 std::vector<uint32_t> translated;
 const void* moduleCode=s->bytecode.data();size_t moduleBytes=s->bytecode.size();
 if(!compatShaderIsSpirv(s)){
   bool dxbc=s->bytecode.size()>=4&&s->bytecode[0]=='D'&&s->bytecode[1]=='X'&&s->bytecode[2]=='B'&&s->bytecode[3]=='C';
   if(!dxbc){gtavdiag::checkpoint("native-shader-bytecode-not-spirv");return false;}
   gtavdiag::checkpoint("native-shader-bytecode-dxbc");
#if GTAV_HAVE_DXBC_SPIRV
   std::vector<CompatShaderDescriptorDecl> decls;if(!compileCompatDxbcToSpirv(s,kind,translated,decls))return false;
   {std::lock_guard<std::mutex> dl(compatShaderDeclMutex);compatShaderDecls[(uint64_t)(uintptr_t)p]=std::move(decls);}
   moduleCode=translated.data();moduleBytes=translated.size()*sizeof(uint32_t);
#else
   gtavdiag::checkpoint("native-shader-dxbc-compiler-missing");return false;
#endif
 }
 const uint64_t key=(uint64_t)(uintptr_t)p;std::lock_guard<std::mutex> l(compatShaderMapMutex);
 auto it=compatShaderModules.find(key);VkShaderModule m=VK_NULL_HANDLE;
 if(it!=compatShaderModules.end())m=it->second;
 else {
   m=gtav_native_renderer_create_shader_module((const uint32_t*)moduleCode,moduleBytes);
   if(!m){gtavdiag::checkpoint("native-shader-module-create-failed");return false;}
   compatShaderModules[key]=m;gtavdiag::checkpoint("native-shader-module-created");
 }
 return gtav_native_renderer_register_resource(key,(uint64_t)(uintptr_t)m,kind,1);
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
 return m.vertexBuffers[0] && m.vs && m.ps && m.rtvCount>0 && m.rtv[0];
}
struct CompatOwnedBuffer { VkBuffer buffer{}; VkDeviceMemory memory{}; VkDeviceSize size{}; void* mapped{}; };
static std::mutex compatBufferMutex;
static std::unordered_map<uint64_t,CompatOwnedBuffer> compatOwnedBuffers;
static bool mapCompatBuffer(void* p,uint32_t kind){
 if(!p||!g.device||!g.physical)return false;
 auto* r=compatResourceObject(p);if(!r||r->vtbl!=gCompatBufferVtable)return false;
 VkDeviceSize size=r->backing.size();if(r->descSize>=4){uint32_t declared=*(uint32_t*)r->desc;if(declared>size)size=declared;}if(!size)size=256;
 std::lock_guard<std::mutex> l(compatBufferMutex);auto it=compatOwnedBuffers.find((uint64_t)(uintptr_t)p);
 if(it==compatOwnedBuffers.end()){
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
   CompatOwnedBuffer ob{};if(vkCreateBuffer(g.device,&bi,nullptr,&ob.buffer)!=VK_SUCCESS)return false;VkMemoryRequirements mr{};vkGetBufferMemoryRequirements(g.device,ob.buffer,&mr);
   uint32_t mt=compatMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);if(mt==UINT32_MAX){vkDestroyBuffer(g.device,ob.buffer,nullptr);return false;}
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=mt;
   if(vkAllocateMemory(g.device,&ai,nullptr,&ob.memory)!=VK_SUCCESS||vkBindBufferMemory(g.device,ob.buffer,ob.memory,0)!=VK_SUCCESS){if(ob.memory)vkFreeMemory(g.device,ob.memory,nullptr);vkDestroyBuffer(g.device,ob.buffer,nullptr);return false;}
   ob.size=size;if(vkMapMemory(g.device,ob.memory,0,size,0,&ob.mapped)!=VK_SUCCESS)ob.mapped=nullptr;it=compatOwnedBuffers.emplace((uint64_t)(uintptr_t)p,ob).first;gtavdiag::checkpoint("native-compat-buffer-created");
 }
 if(it->second.mapped&&!r->backing.empty()){
   size_t n=std::min<size_t>(r->backing.size(),(size_t)it->second.size);
   std::memcpy(it->second.mapped,r->backing.data(),n);
   static std::atomic<uint32_t> upBudget{256};uint32_t ub=upBudget.fetch_sub(1,std::memory_order_relaxed);
   if(ub>0){const uint8_t* b=r->backing.data();uint64_t h=1469598103934665603ull;uint32_t nz=0;for(size_t i=0;i<std::min<size_t>(n,256);i++){h^=b[i];h*=1099511628211ull;if(b[i])nz++;}char d[192];snprintf(d,sizeof(d),"res=%p kind=%u ver=%llu bytes=%zu nz256=%u hash=%016llx",p,kind,(unsigned long long)r->version,n,nz,(unsigned long long)h);gtavdiag::checkpoint("black-probe-buffer-upload",d);}
 }
 return gtav_native_renderer_register_resource((uint64_t)(uintptr_t)p,(uint64_t)(uintptr_t)it->second.buffer,kind,1);
}
static std::mutex compatSamplerMutex;
static std::unordered_map<uint64_t,VkSampler> compatSamplers;
static bool mapCompatSampler(void* p){
 if(!p||!g.device)return false;if(resolveMapped(p,NR_SAMPLER))return true;auto* s=compatStateObject(p);
 VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
 uint32_t fallbackDesc[13]{};fallbackDesc[0]=0x15;fallbackDesc[1]=fallbackDesc[2]=fallbackDesc[3]=3;fallbackDesc[5]=1;float zero=0.0f,maxlod=1000.0f;std::memcpy(&fallbackDesc[4],&zero,4);std::memcpy(&fallbackDesc[11],&zero,4);std::memcpy(&fallbackDesc[12],&maxlod,4);
 const uint32_t* d=(s&&s->descSize>=52)?(const uint32_t*)s->desc:fallbackDesc;
 if(!s){static std::atomic<uint32_t> fb{64};uint32_t n=fb.fetch_sub(1,std::memory_order_relaxed);if(n>0){char q[96];snprintf(q,sizeof(q),"sampler=%p",p);gtavdiag::checkpoint("native-foreign-sampler-fallback",q);}}
 uint32_t filter=d[0];ci.magFilter=(filter&0x4)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;ci.minFilter=(filter&0x10)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
 ci.mipmapMode=(filter&0x1)?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
 auto addr=[](uint32_t a){switch(a){case 1:return VK_SAMPLER_ADDRESS_MODE_REPEAT;case 2:return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;case 3:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;case 4:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;default:return VK_SAMPLER_ADDRESS_MODE_REPEAT;}};
 ci.addressModeU=addr(d[1]);ci.addressModeV=addr(d[2]);ci.addressModeW=addr(d[3]);std::memcpy(&ci.mipLodBias,&d[4],4);
 ci.anisotropyEnable=(filter&0x55)==0x55?VK_TRUE:VK_FALSE;VkPhysicalDeviceProperties pp{};vkGetPhysicalDeviceProperties(g.physical,&pp);ci.maxAnisotropy=ci.anisotropyEnable?std::min<float>((float)std::max(1u,d[5]),pp.limits.maxSamplerAnisotropy):1.0f;
 ci.compareEnable=(filter&0x80)?VK_TRUE:VK_FALSE;switch(d[6]){case 1:ci.compareOp=VK_COMPARE_OP_NEVER;break;case 2:ci.compareOp=VK_COMPARE_OP_LESS;break;case 3:ci.compareOp=VK_COMPARE_OP_EQUAL;break;case 4:ci.compareOp=VK_COMPARE_OP_LESS_OR_EQUAL;break;case 5:ci.compareOp=VK_COMPARE_OP_GREATER;break;case 6:ci.compareOp=VK_COMPARE_OP_NOT_EQUAL;break;case 7:ci.compareOp=VK_COMPARE_OP_GREATER_OR_EQUAL;break;default:ci.compareOp=VK_COMPARE_OP_ALWAYS;break;}
 std::memcpy(&ci.minLod,&d[11],4);std::memcpy(&ci.maxLod,&d[12],4);ci.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
 std::lock_guard<std::mutex> l(compatSamplerMutex);auto it=compatSamplers.find((uint64_t)(uintptr_t)p);VkSampler sm=VK_NULL_HANDLE;
 if(it!=compatSamplers.end())sm=it->second;else{if(vkCreateSampler(g.device,&ci,nullptr,&sm)!=VK_SUCCESS)return false;compatSamplers[(uint64_t)(uintptr_t)p]=sm;}
 return gtav_native_renderer_register_resource((uint64_t)(uintptr_t)p,(uint64_t)(uintptr_t)sm,NR_SAMPLER,1);
}
static std::mutex compatBufferViewMutex;
static std::unordered_map<uint64_t,VkBufferView> compatBufferViews;
static VkBufferView mapCompatBufferView(void* view,bool storage){
 if(!view||!g.device)return VK_NULL_HANDLE;
 auto* v=compatViewObject(view);if(!v||!v->resource)return VK_NULL_HANDLE;
 if(!mapCompatBuffer(v->resource,storage?NR_UAV:NR_SRV))return VK_NULL_HANDLE;
 uint64_t key=((uint64_t)(uintptr_t)view<<1)|(storage?1ull:0ull);{std::lock_guard<std::mutex> l(compatBufferViewMutex);auto it=compatBufferViews.find(key);if(it!=compatBufferViews.end())return it->second;}
 uint32_t fmt=0,first=0,count=0;if(v->descSize>=16){const uint32_t* d=(const uint32_t*)v->desc;fmt=d[0];first=d[2];count=d[3];}
 VkFormat vf=compatDxgiFormat(fmt);if(vf==VK_FORMAT_UNDEFINED)return VK_NULL_HANDLE;uint32_t bw=1,bh=1,bpe=4;compatFormatLayout(fmt,bw,bh,bpe);if(bw!=1||bh!=1||!bpe)return VK_NULL_HANDLE;
 VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};ci.buffer=(VkBuffer)(uintptr_t)resolveMapped(v->resource,storage?NR_UAV:NR_SRV);ci.format=vf;ci.offset=(VkDeviceSize)first*bpe;
 VkDeviceSize total=v->resource->backing.size();ci.range=count?(VkDeviceSize)count*bpe:(total>ci.offset?total-ci.offset:VK_WHOLE_SIZE);if(ci.range==0)return VK_NULL_HANDLE;
 VkBufferView bv=VK_NULL_HANDLE;if(vkCreateBufferView(g.device,&ci,nullptr,&bv)!=VK_SUCCESS)return VK_NULL_HANDLE;std::lock_guard<std::mutex> l(compatBufferViewMutex);compatBufferViews[key]=bv;return bv;
}
static VkPrimitiveTopology compatVkTopology(uint32_t t){
 switch(t){case 1:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;case 2:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;case 3:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;case 4:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;case 5:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;default:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;}
}
static bool updateCompatGraphicsDescriptors(const RageMirrorState& m,VkDescriptorSet desc){
 if(!desc)return false;
 std::vector<VkWriteDescriptorSet> writes;std::vector<VkDescriptorBufferInfo> bis;std::vector<VkDescriptorImageInfo> iis;std::vector<VkBufferView> bvs;
 bis.reserve(64);iis.reserve(96);bvs.reserve(32);writes.reserve(160);
 uint32_t expectedCbv=0,boundCbv=0,missingCbv=0,expectedSrv=0,boundSrv=0,missingSrv=0,expectedSampler=0,boundSampler=0,missingSampler=0;
 auto processStage=[&](void* sh,uint32_t stage,void* const* cbs,void* const* srvs,void* const* samplers){
  auto decls=getCompatShaderDecls(sh);
  for(const auto& d:decls){
   if(d.space!=0)continue;
   for(uint32_t e=0;e<d.count;e++){
    uint32_t reg=d.reg+e;void* p=nullptr;
    if(d.scalarType==23&&reg<16){expectedCbv++;p=cbs?cbs[reg]:nullptr;if(p)boundCbv++;else missingCbv++;}
    else if(d.scalarType==24&&reg<32){expectedSrv++;p=srvs?srvs[reg]:nullptr;if(p)boundSrv++;else missingSrv++;}
    else if(d.scalarType==22&&reg<16){expectedSampler++;p=samplers?samplers[reg]:nullptr;if(p)boundSampler++;else missingSampler++;}
    else continue;
    {char md[320];snprintf(md,sizeof(md),"draw=%llu stage=%u scalar=%u reg=%u binding=%u dtype=%d ptr=%p",(unsigned long long)tlsMegaDrawId,stage,d.scalarType,reg,d.binding,(int)d.type,p);gtavdiag::checkpoint("MEGA-DESC",md);}
    if(!p){static std::atomic<uint32_t> missBudget{256};uint32_t mb=missBudget.fetch_sub(1,std::memory_order_relaxed);if(mb>0){char md[160];snprintf(md,sizeof(md),"stage=%u type=%u reg=%u binding=%u",stage,d.scalarType,reg,d.binding);gtavdiag::checkpoint("black-probe-descriptor-missing",md);}continue;}
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=desc;w.dstBinding=d.binding;w.dstArrayElement=e;w.descriptorCount=1;w.descriptorType=d.type;
    if(d.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||d.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){
      void* resource=p;if(auto* v=compatViewObject(p);v&&v->resource)resource=v->resource;
      uint32_t role=d.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?NR_CBUFFER:NR_SRV;
      if(!mapCompatBuffer(resource,role)){gMegaDescriptorFail.fetch_add(1);char z[256];snprintf(z,sizeof(z),"draw=%llu stage=%u reg=%u buffer-map-fail ptr=%p resource=%p role=%u",(unsigned long long)tlsMegaDrawId,stage,reg,p,resource,role);gtavdiag::checkpoint("MEGA-DESC-FAIL",z);return false;}
      VkBuffer mapped=(VkBuffer)(uintptr_t)resolveMapped(resource,role);if(!mapped){gMegaDescriptorFail.fetch_add(1);char z[256];snprintf(z,sizeof(z),"draw=%llu stage=%u reg=%u buffer-resolve-fail resource=%p role=%u",(unsigned long long)tlsMegaDrawId,stage,reg,resource,role);gtavdiag::checkpoint("MEGA-DESC-FAIL",z);return false;}
      auto* rr=compatResourceObject(resource);
      VkDeviceSize range=(rr&&!rr->backing.empty())?(VkDeviceSize)rr->backing.size():VK_WHOLE_SIZE;
      VkDescriptorBufferInfo bi{mapped,0,range};bis.push_back(bi);w.pBufferInfo=&bis.back();
    }else if(d.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE||d.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE){
      uint32_t kind=d.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?NR_UAV:NR_SRV;if(!mapWrappedImage(p,kind,false)){gMegaDescriptorFail.fetch_add(1);gMegaImageFail.fetch_add(1);char z[256];snprintf(z,sizeof(z),"draw=%llu stage=%u reg=%u image-map-fail ptr=%p kind=%u",(unsigned long long)tlsMegaDrawId,stage,reg,p,kind);gtavdiag::checkpoint("MEGA-DESC-FAIL",z);return false;}VkImageView v=gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)p);if(!v){gMegaDescriptorFail.fetch_add(1);gMegaImageFail.fetch_add(1);char z[256];snprintf(z,sizeof(z),"draw=%llu stage=%u reg=%u image-view-fail ptr=%p kind=%u",(unsigned long long)tlsMegaDrawId,stage,reg,p,kind);gtavdiag::checkpoint("MEGA-DESC-FAIL",z);return false;}VkDescriptorImageInfo ii{};ii.imageView=v;ii.imageLayout=d.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;iis.push_back(ii);w.pImageInfo=&iis.back();
    }else if(d.type==VK_DESCRIPTOR_TYPE_SAMPLER){
      if(!mapCompatSampler(p)){gMegaDescriptorFail.fetch_add(1);char z[256];snprintf(z,sizeof(z),"draw=%llu stage=%u reg=%u sampler-map-fail ptr=%p",(unsigned long long)tlsMegaDrawId,stage,reg,p);gtavdiag::checkpoint("MEGA-DESC-FAIL",z);return false;}VkDescriptorImageInfo ii{};ii.sampler=(VkSampler)(uintptr_t)resolveMapped(p,NR_SAMPLER);iis.push_back(ii);w.pImageInfo=&iis.back();
    }else if(d.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||d.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER){
      VkBufferView bv=mapCompatBufferView(p,d.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);if(!bv)return false;bvs.push_back(bv);w.pTexelBufferView=&bvs.back();
    }else continue;
    writes.push_back(w);
   }
  }
 };
 processStage(m.vs,0,m.vsCB,m.vsSRV,m.vsSampler);processStage(m.ps,1,m.psCB,m.psSRV,m.psSampler);
 gBlackProbeExpectedCbv.fetch_add(expectedCbv,std::memory_order_relaxed);gBlackProbeBoundCbv.fetch_add(boundCbv,std::memory_order_relaxed);gBlackProbeMissingCbv.fetch_add(missingCbv,std::memory_order_relaxed);
 gBlackProbeExpectedSrv.fetch_add(expectedSrv,std::memory_order_relaxed);gBlackProbeBoundSrv.fetch_add(boundSrv,std::memory_order_relaxed);gBlackProbeMissingSrv.fetch_add(missingSrv,std::memory_order_relaxed);
 gBlackProbeExpectedSampler.fetch_add(expectedSampler,std::memory_order_relaxed);gBlackProbeBoundSampler.fetch_add(boundSampler,std::memory_order_relaxed);gBlackProbeMissingSampler.fetch_add(missingSampler,std::memory_order_relaxed);
 gBlackProbeDescriptorWrites.fetch_add((uint32_t)writes.size(),std::memory_order_relaxed);
 {static std::atomic<uint32_t> sumBudget{256};uint32_t sb=sumBudget.fetch_sub(1,std::memory_order_relaxed);if(sb>0){char sd[224];snprintf(sd,sizeof(sd),"cbv=%u/%u miss=%u srv=%u/%u miss=%u samp=%u/%u miss=%u writes=%zu",boundCbv,expectedCbv,missingCbv,boundSrv,expectedSrv,missingSrv,boundSampler,expectedSampler,missingSampler,writes.size());gtavdiag::checkpoint("black-probe-descriptor-summary",sd);}}
 if(!writes.empty())vkUpdateDescriptorSets(g.device,(uint32_t)writes.size(),writes.data(),0,nullptr);return true;
}
static bool updateCompatComputeDescriptors(const RageMirrorState& m,VkDescriptorSet desc){
 if(!m.cs||!desc)return false;std::vector<VkWriteDescriptorSet> writes;std::vector<VkDescriptorBufferInfo> bis;std::vector<VkDescriptorImageInfo> iis;std::vector<VkBufferView> bvs;
 bis.reserve(48);iis.reserve(64);bvs.reserve(24);writes.reserve(128);
 for(const auto& d:getCompatShaderDecls(m.cs)){
  if(d.space!=0)continue;
  for(uint32_t e=0;e<d.count;e++){
   uint32_t reg=d.reg+e;void* p=nullptr;if(d.scalarType==23&&reg<16)p=m.csCB[reg];else if(d.scalarType==24&&reg<32)p=m.csSRV[reg];else if(d.scalarType==22&&reg<16)p=m.csSampler[reg];else if(d.scalarType==25&&reg<16)p=m.csUAV[reg];else continue;if(!p)continue;
   VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=desc;w.dstBinding=d.binding;w.dstArrayElement=e;w.descriptorCount=1;w.descriptorType=d.type;
   if(d.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||d.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){
    void* resource=p;if(auto* v=compatViewObject(p);v&&v->resource)resource=v->resource;uint32_t role=d.scalarType==23?NR_CBUFFER:(d.scalarType==25?NR_UAV:NR_SRV);
    if(!mapCompatBuffer(resource,role))return false;
    VkBuffer mapped=(VkBuffer)(uintptr_t)resolveMapped(resource,role);if(!mapped)return false;
    auto* rr=compatResourceObject(resource);
    VkDeviceSize range=(rr&&!rr->backing.empty())?(VkDeviceSize)rr->backing.size():VK_WHOLE_SIZE;
    VkDescriptorBufferInfo bi{mapped,0,range};bis.push_back(bi);w.pBufferInfo=&bis.back();
   }else if(d.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE||d.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE){
    uint32_t role=d.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?NR_UAV:NR_SRV;if(!mapWrappedImage(p,role,false))return false;VkImageView v=gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)p);if(!v)return false;VkDescriptorImageInfo ii{};ii.imageView=v;ii.imageLayout=d.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;iis.push_back(ii);w.pImageInfo=&iis.back();
   }else if(d.type==VK_DESCRIPTOR_TYPE_SAMPLER){
    if(!mapCompatSampler(p))return false;VkDescriptorImageInfo ii{};ii.sampler=(VkSampler)(uintptr_t)resolveMapped(p,NR_SAMPLER);iis.push_back(ii);w.pImageInfo=&iis.back();
   }else if(d.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||d.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER){
    VkBufferView bv=mapCompatBufferView(p,d.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);if(!bv)return false;bvs.push_back(bv);w.pTexelBufferView=&bvs.back();
   }else continue;
   writes.push_back(w);
  }
 }
 if(!writes.empty())vkUpdateDescriptorSets(g.device,(uint32_t)writes.size(),writes.data(),0,nullptr);return true;
}
static bool ensureCompatComputeState(const RageMirrorState& m){
 if(!g.device||!m.cs)return false;uint64_t key=hashMix((uint64_t)(uintptr_t)m.cs,0x43534e4154495645ull);
 if(gtav_native_renderer_resolve_resource(key,NR_GRAPHICS_PIPELINE))return true;if(!mapCompatShader(m.cs,NR_CS))return false;VkShaderModule cs=(VkShaderModule)(uintptr_t)resolveMapped(m.cs,NR_CS);if(!cs)return false;
 std::vector<VkDescriptorSetLayoutBinding> bindings;for(const auto& d:getCompatShaderDecls(m.cs)){VkDescriptorSetLayoutBinding b{};b.binding=d.binding;b.descriptorType=d.type;b.descriptorCount=d.count;b.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;bindings.push_back(b);}
 VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dci.bindingCount=(uint32_t)bindings.size();dci.pBindings=bindings.empty()?nullptr:bindings.data();VkDescriptorSetLayout dsl=VK_NULL_HANDLE;
 if(vkCreateDescriptorSetLayout(g.device,&dci,nullptr,&dsl)!=VK_SUCCESS){gtavdiag::checkpoint("native-compute-descriptor-layout-failed");return false;}
 VkPushConstantRange push{};push.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;push.offset=0;push.size=128;VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};lci.setLayoutCount=1;lci.pSetLayouts=&dsl;lci.pushConstantRangeCount=1;lci.pPushConstantRanges=&push;VkPipelineLayout layout=VK_NULL_HANDLE;
 if(vkCreatePipelineLayout(g.device,&lci,nullptr,&layout)!=VK_SUCCESS){vkDestroyDescriptorSetLayout(g.device,dsl,nullptr);return false;}VkDescriptorSet desc=gtav_native_renderer_alloc_descriptor_set(dsl);if(!desc){vkDestroyPipelineLayout(g.device,layout,nullptr);vkDestroyDescriptorSetLayout(g.device,dsl,nullptr);return false;}
 if(!updateCompatComputeDescriptors(m,desc)){/* descriptor set reclaimed with owning pool at shutdown */vkDestroyPipelineLayout(g.device,layout,nullptr);vkDestroyDescriptorSetLayout(g.device,dsl,nullptr);return false;}
 VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;stage.module=cs;stage.pName="main";VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage=stage;ci.layout=layout;VkPipeline pipe=VK_NULL_HANDLE;VkResult vr=vkCreateComputePipelines(g.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipe);
 if(vr!=VK_SUCCESS){char d[64];snprintf(d,sizeof(d),"vkResult=%d",(int)vr);gtavdiag::checkpoint("native-compute-pipeline-create-failed",d);/* descriptor set reclaimed with owning pool at shutdown */vkDestroyPipelineLayout(g.device,layout,nullptr);vkDestroyDescriptorSetLayout(g.device,dsl,nullptr);return false;}
 if(!gtav_native_renderer_register_compute_state((uint64_t)(uintptr_t)m.cs,pipe,layout,desc)){vkDestroyPipeline(g.device,pipe,nullptr);return false;}gtavdiag::checkpoint("native-compute-pipeline-created");return true;
}

static int32_t compatDxbcInputLocation(const CompatShaderObject* sh,const std::string& semantic,uint32_t semanticIndex){
 if(!sh||semantic.empty()||sh->bytecode.size()<36)return -1;
 const uint8_t* b=sh->bytecode.data();size_t n=sh->bytecode.size();
 if(std::memcmp(b,"DXBC",4)!=0)return -1;
 uint32_t chunkCount=0;std::memcpy(&chunkCount,b+28,4);
 if(chunkCount>256||32ull+4ull*chunkCount>n)return -1;
 auto eq=[](const std::string& a,const char* p){
   if(!p)return false;size_t m=strnlen(p,64);if(m!=a.size())return false;
   for(size_t i=0;i<m;i++){char x=a[i],y=p[i];if(x>='a'&&x<='z')x-=32;if(y>='a'&&y<='z')y-=32;if(x!=y)return false;}return true;
 };
 for(uint32_t ci=0;ci<chunkCount;ci++){
   uint32_t off=0;std::memcpy(&off,b+32+ci*4,4);if((size_t)off+16>n)continue;
   const uint8_t* ch=b+off;uint32_t four=0,sz=0;std::memcpy(&four,ch,4);std::memcpy(&sz,ch+4,4);
   // ISGN / ISG1 input-signature chunks.
   if(four!=0x4e475349u && four!=0x31475349u)continue;
   size_t end=std::min<size_t>(n,(size_t)off+(size_t)8+(size_t)sz);const uint8_t* d=ch+8;if(d+8>b+end)continue;
   uint32_t cnt=0;std::memcpy(&cnt,d,4);if(cnt>256)continue;
   // SM4/5 ISGN entries are 24 bytes. ISG1 may carry a 32-byte entry; the
   // first semantic/name/index/register fields stay at the same offsets.
   size_t stride=(four==0x31475349u)?32u:24u;
   if((size_t)(d-b)+8ull+stride*cnt>end) { if(four==0x31475349u)stride=24u; }
   for(uint32_t i=0;i<cnt;i++){
     const uint8_t* e=d+8+i*stride;if(e+24>b+end)break;
     uint32_t nameOff=0,idx=0,reg=0;std::memcpy(&nameOff,e+0,4);std::memcpy(&idx,e+4,4);std::memcpy(&reg,e+16,4);
     const char* name=nullptr;
     if((size_t)(d-b)+nameOff<end)name=(const char*)(d+nameOff);
     if(idx==semanticIndex&&name&&eq(semantic,name))return (int32_t)reg;
   }
 }
 return -1;
}
static bool ensureCompatGraphicsState(const RageMirrorState& m){
 if(!g.device||!m.vs||!m.ps||!m.rtvCount||!m.rtv[0])return false;
 const uint64_t key=graphicsStateKey(m);
 if(gtav_native_renderer_resolve_resource(key,NR_GRAPHICS_PIPELINE))return true;
 const VkShaderModule vs=(VkShaderModule)(uintptr_t)resolveMapped(m.vs,NR_VS);
 const VkShaderModule ps=(VkShaderModule)(uintptr_t)resolveMapped(m.ps,NR_PS);
 if(!vs||!ps){gtavdiag::checkpoint("native-pipeline-missing-shader");return false;}
 NativeImageMeta rt{};VkFormat colorFormats[8]{};uint32_t colorCount=m.rtvCount>8?8:m.rtvCount;
 {std::lock_guard<std::mutex> l(imageMetaMutex);for(uint32_t i=0;i<colorCount;i++){if(!m.rtv[i]){colorFormats[i]=VK_FORMAT_UNDEFINED;continue;}auto it=imageMeta.find((uint64_t)(uintptr_t)m.rtv[i]);if(it==imageMeta.end()){gtavdiag::checkpoint("native-pipeline-missing-rt-meta");return false;}if(i==0)rt=it->second;colorFormats[i]=it->second.format;}}
 std::vector<VkDescriptorSetLayoutBinding> bindings;
 auto addDecls=[&](void* sh){for(const auto& d:getCompatShaderDecls(sh)){VkDescriptorSetLayoutBinding b{};b.binding=d.binding;b.descriptorType=d.type;b.descriptorCount=d.count;b.stageFlags=d.stages;bindings.push_back(b);}};
 addDecls(m.vs);addDecls(m.ps);
 if(bindings.empty()){
  auto addBinding=[&](uint32_t binding,VkDescriptorType type,VkShaderStageFlags stages){VkDescriptorSetLayoutBinding b{};b.binding=binding;b.descriptorType=type;b.descriptorCount=1;b.stageFlags=stages;bindings.push_back(b);};
  for(uint32_t i=0;i<16;i++){if(m.vsCB[i])addBinding(compatDescriptorBinding(0,23,0,i),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_SHADER_STAGE_VERTEX_BIT);if(m.psCB[i])addBinding(compatDescriptorBinding(1,23,0,i),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT);}
  for(uint32_t i=0;i<32;i++){if(m.vsSRV[i])addBinding(compatDescriptorBinding(0,24,0,i),VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_SHADER_STAGE_VERTEX_BIT);if(m.psSRV[i])addBinding(compatDescriptorBinding(1,24,0,i),VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_SHADER_STAGE_FRAGMENT_BIT);}
  for(uint32_t i=0;i<16;i++){if(m.vsSampler[i])addBinding(compatDescriptorBinding(0,22,0,i),VK_DESCRIPTOR_TYPE_SAMPLER,VK_SHADER_STAGE_VERTEX_BIT);if(m.psSampler[i])addBinding(compatDescriptorBinding(1,22,0,i),VK_DESCRIPTOR_TYPE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT);}
 }
 VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dci.bindingCount=(uint32_t)bindings.size();dci.pBindings=bindings.empty()?nullptr:bindings.data();VkDescriptorSetLayout dsl{};
 if(vkCreateDescriptorSetLayout(g.device,&dci,nullptr,&dsl)!=VK_SUCCESS){gtavdiag::checkpoint("native-pipeline-descriptor-layout-failed");return false;}
 VkPushConstantRange push{};push.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;push.offset=0;push.size=128;
 VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};lci.setLayoutCount=1;lci.pSetLayouts=&dsl;lci.pushConstantRangeCount=1;lci.pPushConstantRanges=&push;VkPipelineLayout layout{};
 if(vkCreatePipelineLayout(g.device,&lci,nullptr,&layout)!=VK_SUCCESS){vkDestroyDescriptorSetLayout(g.device,dsl,nullptr);gtavdiag::checkpoint("native-pipeline-layout-create-failed");return false;}
 VkDescriptorSet desc=gtav_native_renderer_alloc_descriptor_set(dsl);if(!desc){vkDestroyPipelineLayout(g.device,layout,nullptr);vkDestroyDescriptorSetLayout(g.device,dsl,nullptr);gtavdiag::checkpoint("native-pipeline-descriptor-alloc-failed");return false;}
 if(!updateCompatGraphicsDescriptors(m,desc)){/* descriptor set reclaimed with owning pool at shutdown */vkDestroyPipelineLayout(g.device,layout,nullptr);vkDestroyDescriptorSetLayout(g.device,dsl,nullptr);gtavdiag::checkpoint("native-pipeline-descriptor-update-failed");return false;}
 VkPipelineShaderStageCreateInfo stages[2]{};
 stages[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[0].pName="main";
 stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=ps;stages[1].pName="main";
 VkVertexInputBindingDescription vbDesc[16]{};VkVertexInputAttributeDescription vaDesc[32]{};uint32_t vbCount=0,vaCount=0;bool slotUsed[16]{};
 if(auto* il=compatResolveInputLayout(m.inputLayout,m.vs)){
   uint32_t appendOffset[16]{};
   for(size_t i=0;i<il->elements.size()&&vaCount<32;i++){
     const auto& e=il->elements[i];if(e.slot>=16){char d[128];snprintf(d,sizeof(d),"semantic=%s%u slot=%u fmt=%u",e.semantic.c_str(),e.semanticIndex,e.slot,e.format);gtavdiag::checkpoint("native-input-layout-slot-unsupported",d);continue;}VkFormat vf=VK_FORMAT_UNDEFINED;uint32_t sz=0;
     switch(e.format){
      case 2:vf=VK_FORMAT_R32G32B32A32_SFLOAT;sz=16;break;
      case 3:vf=VK_FORMAT_R32G32B32A32_UINT;sz=16;break;
      case 4:vf=VK_FORMAT_R32G32B32A32_SINT;sz=16;break;
      case 6:vf=VK_FORMAT_R32G32B32_SFLOAT;sz=12;break;
      case 7:vf=VK_FORMAT_R32G32B32_UINT;sz=12;break;
      case 8:vf=VK_FORMAT_R32G32B32_SINT;sz=12;break;
      case 10:vf=VK_FORMAT_R16G16B16A16_SFLOAT;sz=8;break;
      case 11:vf=VK_FORMAT_R16G16B16A16_UNORM;sz=8;break;
      case 12:vf=VK_FORMAT_R16G16B16A16_UINT;sz=8;break;
      case 13:vf=VK_FORMAT_R16G16B16A16_SNORM;sz=8;break;
      case 14:vf=VK_FORMAT_R16G16B16A16_SINT;sz=8;break;
      case 16:vf=VK_FORMAT_R32G32_SFLOAT;sz=8;break;
      case 17:vf=VK_FORMAT_R32G32_UINT;sz=8;break;
      case 18:vf=VK_FORMAT_R32G32_SINT;sz=8;break;
      case 24:vf=VK_FORMAT_A2B10G10R10_UNORM_PACK32;sz=4;break;
      case 25:vf=VK_FORMAT_A2B10G10R10_UINT_PACK32;sz=4;break;
      case 28:vf=VK_FORMAT_R8G8B8A8_UNORM;sz=4;break;
      case 29:vf=VK_FORMAT_R8G8B8A8_SRGB;sz=4;break;
      case 30:vf=VK_FORMAT_R8G8B8A8_UINT;sz=4;break;
      case 31:vf=VK_FORMAT_R8G8B8A8_SNORM;sz=4;break;
      case 32:vf=VK_FORMAT_R8G8B8A8_SINT;sz=4;break;
      case 34:vf=VK_FORMAT_R16G16_SFLOAT;sz=4;break;
      case 35:vf=VK_FORMAT_R16G16_UNORM;sz=4;break;
      case 36:vf=VK_FORMAT_R16G16_UINT;sz=4;break;
      case 37:vf=VK_FORMAT_R16G16_SNORM;sz=4;break;
      case 38:vf=VK_FORMAT_R16G16_SINT;sz=4;break;
      case 41:vf=VK_FORMAT_R32_SFLOAT;sz=4;break;
      case 42:vf=VK_FORMAT_R32_UINT;sz=4;break;
      case 43:vf=VK_FORMAT_R32_SINT;sz=4;break;
      case 49:vf=VK_FORMAT_R8G8_UNORM;sz=2;break;
      case 50:vf=VK_FORMAT_R8G8_UINT;sz=2;break;
      case 51:vf=VK_FORMAT_R8G8_SNORM;sz=2;break;
      case 52:vf=VK_FORMAT_R8G8_SINT;sz=2;break;
      case 54:vf=VK_FORMAT_R16_SFLOAT;sz=2;break;
      case 56:vf=VK_FORMAT_R16_UNORM;sz=2;break;
      case 57:vf=VK_FORMAT_R16_UINT;sz=2;break;
      case 58:vf=VK_FORMAT_R16_SNORM;sz=2;break;
      case 59:vf=VK_FORMAT_R16_SINT;sz=2;break;
      case 61:vf=VK_FORMAT_R8_UNORM;sz=1;break;
      case 62:vf=VK_FORMAT_R8_UINT;sz=1;break;
      case 63:vf=VK_FORMAT_R8_SNORM;sz=1;break;
      case 64:vf=VK_FORMAT_R8_SINT;sz=1;break;
      default:break;
     }
     if(vf==VK_FORMAT_UNDEFINED){gtavdiag::checkpoint("native-input-layout-format-unsupported");continue;}
     VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(g.physical,vf,&fp);
     if(!(fp.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)){
       char d[128];snprintf(d,sizeof(d),"semantic=%s%u format=%d",e.semantic.c_str(),e.semanticIndex,(int)vf);
       gtavdiag::checkpoint("native-input-format-no-vertex-support",d);continue;
     }
     uint32_t off=e.offset==0xffffffffu?appendOffset[e.slot]:e.offset;appendOffset[e.slot]=off+sz;
     auto& a=vaDesc[vaCount];int32_t loc=-1;if(auto* vsh=(CompatShaderObject*)m.vs;compatShaderObject(vsh))loc=compatDxbcInputLocation(vsh,e.semantic,e.semanticIndex);a.location=loc>=0?(uint32_t)loc:vaCount;a.binding=e.slot;a.format=vf;a.offset=off;{char id[160];snprintf(id,sizeof(id),"semantic=%s%u location=%u slot=%u offset=%u",e.semantic.c_str(),e.semanticIndex,a.location,e.slot,off);gtavdiag::checkpoint(loc>=0?"native-input-semantic-mapped":"native-input-semantic-fallback",id);}vaCount++;slotUsed[e.slot]=true;
   }
   for(uint32_t slot=0;slot<16;slot++)if(slotUsed[slot]){auto& b=vbDesc[vbCount++];b.binding=slot;b.stride=m.strides[slot]?m.strides[slot]:appendOffset[slot];bool inst=false;for(const auto& e:il->elements)if(e.slot==slot&&e.inputClass==1){inst=true;break;}b.inputRate=inst?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX;}
 }
 {char d[128];snprintf(d,sizeof(d),"layout=%p attrs=%u bindings=%u",m.inputLayout,vaCount,vbCount);gtavdiag::checkpoint("native-input-pipeline-ready",d);}
 VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=vbCount;vi.pVertexBindingDescriptions=vbCount?vbDesc:nullptr;vi.vertexAttributeDescriptionCount=vaCount;vi.pVertexAttributeDescriptions=vaCount?vaDesc:nullptr;
 VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=compatVkTopology(m.topology);
 VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.scissorCount=1;
 VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1.0f;
 if(auto* s=compatStateObject(m.rasterState);s&&s->descSize>=40){const uint32_t* d=(const uint32_t*)s->desc;rs.polygonMode=d[0]==2?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_FILL;rs.cullMode=d[1]==2?VK_CULL_MODE_FRONT_BIT:d[1]==3?VK_CULL_MODE_BACK_BIT:VK_CULL_MODE_NONE;// Negative-height Vulkan viewport flips framebuffer winding, so invert
 // D3D11 FrontCounterClockwise when mapping the rasterizer state.
 rs.frontFace=d[2]?VK_FRONT_FACE_CLOCKWISE:VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.depthBiasEnable=d[3]!=0||d[4]!=0||d[5]!=0;std::memcpy(&rs.depthBiasConstantFactor,&d[3],4);std::memcpy(&rs.depthBiasClamp,&d[4],4);std::memcpy(&rs.depthBiasSlopeFactor,&d[5],4);rs.depthClampEnable=VK_FALSE;}
 VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;VkSampleMask compatSampleMask=m.sampleMask;ms.pSampleMask=&compatSampleMask;
 VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
 auto cmp=[](uint32_t x){switch(x){case 1:return VK_COMPARE_OP_NEVER;case 2:return VK_COMPARE_OP_LESS;case 3:return VK_COMPARE_OP_EQUAL;case 4:return VK_COMPARE_OP_LESS_OR_EQUAL;case 5:return VK_COMPARE_OP_GREATER;case 6:return VK_COMPARE_OP_NOT_EQUAL;case 7:return VK_COMPARE_OP_GREATER_OR_EQUAL;default:return VK_COMPARE_OP_ALWAYS;}};
 if(auto* s=compatStateObject(m.depthState);s&&s->descSize>=52){
 const uint8_t* q=s->desc;const uint32_t* d=(const uint32_t*)q;
 auto sop=[](uint32_t x){switch(x){case 2:return VK_STENCIL_OP_ZERO;case 3:return VK_STENCIL_OP_REPLACE;case 4:return VK_STENCIL_OP_INCREMENT_AND_CLAMP;case 5:return VK_STENCIL_OP_DECREMENT_AND_CLAMP;case 6:return VK_STENCIL_OP_INVERT;case 7:return VK_STENCIL_OP_INCREMENT_AND_WRAP;case 8:return VK_STENCIL_OP_DECREMENT_AND_WRAP;default:return VK_STENCIL_OP_KEEP;}};
 ds.depthTestEnable=d[0]?VK_TRUE:VK_FALSE;ds.depthWriteEnable=d[1]?VK_TRUE:VK_FALSE;ds.depthCompareOp=cmp(d[2]);ds.stencilTestEnable=d[3]?VK_TRUE:VK_FALSE;
 uint32_t readMask=q[16],writeMask=q[17];
 auto fillStencil=[&](VkStencilOpState& o,const uint32_t* z){o.failOp=sop(z[0]);o.depthFailOp=sop(z[1]);o.passOp=sop(z[2]);o.compareOp=cmp(z[3]);o.compareMask=readMask;o.writeMask=writeMask;o.reference=m.stencilRef;};
 fillStencil(ds.front,(const uint32_t*)(q+20));fillStencil(ds.back,(const uint32_t*)(q+36));
}
 auto bf=[](uint32_t x){switch(x){case 1:return VK_BLEND_FACTOR_ZERO;case 2:return VK_BLEND_FACTOR_ONE;case 3:return VK_BLEND_FACTOR_SRC_COLOR;case 4:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;case 5:return VK_BLEND_FACTOR_SRC_ALPHA;case 6:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;case 7:return VK_BLEND_FACTOR_DST_ALPHA;case 8:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;case 9:return VK_BLEND_FACTOR_DST_COLOR;case 10:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;case 11:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;case 14:return VK_BLEND_FACTOR_CONSTANT_COLOR;case 15:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;case 16:return VK_BLEND_FACTOR_SRC1_COLOR;case 17:return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;case 18:return VK_BLEND_FACTOR_SRC1_ALPHA;case 19:return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;default:return VK_BLEND_FACTOR_ONE;}};
 auto bop=[](uint32_t x){switch(x){case 2:return VK_BLEND_OP_SUBTRACT;case 3:return VK_BLEND_OP_REVERSE_SUBTRACT;case 4:return VK_BLEND_OP_MIN;case 5:return VK_BLEND_OP_MAX;default:return VK_BLEND_OP_ADD;}};
 VkPipelineColorBlendAttachmentState cba[8]{};for(uint32_t i=0;i<colorCount;i++)cba[i].colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
 if(auto* s=compatStateObject(m.blendState);s&&s->descSize>=264){const uint8_t* d=s->desc;bool independent=*(const uint32_t*)(d+4)!=0;for(uint32_t i=0;i<colorCount;i++){const uint8_t* r=d+8+(independent?i:0)*32;auto& a=cba[i];a.blendEnable=*(const uint32_t*)(r+0)?VK_TRUE:VK_FALSE;a.srcColorBlendFactor=bf(*(const uint32_t*)(r+4));a.dstColorBlendFactor=bf(*(const uint32_t*)(r+8));a.colorBlendOp=bop(*(const uint32_t*)(r+12));a.srcAlphaBlendFactor=bf(*(const uint32_t*)(r+16));a.dstAlphaBlendFactor=bf(*(const uint32_t*)(r+20));a.alphaBlendOp=bop(*(const uint32_t*)(r+24));uint8_t mask=*(r+28);a.colorWriteMask=((mask&1)?VK_COLOR_COMPONENT_R_BIT:0)|((mask&2)?VK_COLOR_COMPONENT_G_BIT:0)|((mask&4)?VK_COLOR_COMPONENT_B_BIT:0)|((mask&8)?VK_COLOR_COMPONENT_A_BIT:0);}}
 VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=colorCount;cb.pAttachments=colorCount?cba:nullptr;
 VkDynamicState dyns[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_REFERENCE};VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dyn.dynamicStateCount=4;dyn.pDynamicStates=dyns;
 VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=colorCount;rendering.pColorAttachmentFormats=colorFormats;
 NativeImageMeta depth{};if(m.dsv){std::lock_guard<std::mutex> l(imageMetaMutex);auto it=imageMeta.find((uint64_t)(uintptr_t)m.dsv);if(it!=imageMeta.end()){depth=it->second;rendering.depthAttachmentFormat=depth.format;if(depth.aspect&VK_IMAGE_ASPECT_STENCIL_BIT)rendering.stencilAttachmentFormat=depth.format;}}
 VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pci.pNext=&rendering;pci.stageCount=2;pci.pStages=stages;pci.pVertexInputState=&vi;pci.pInputAssemblyState=&ia;pci.pViewportState=&vp;pci.pRasterizationState=&rs;pci.pMultisampleState=&ms;pci.pDepthStencilState=&ds;pci.pColorBlendState=&cb;pci.pDynamicState=&dyn;pci.layout=layout;
 VkPipeline pipe{};VkResult pr=VK_ERROR_INITIALIZATION_FAILED;
 {
   std::lock_guard<std::mutex> createLock(graphicsPipelineCreateMutex);
   char d[192];snprintf(d,sizeof(d),"attrs=%u bindings=%u colors=%u depthFmt=%d topology=%d",vaCount,vbCount,colorCount,(int)rendering.depthAttachmentFormat,(int)ia.topology);
   gtavdiag::checkpoint("native-pipeline-create-enter",d);
   pr=vkCreateGraphicsPipelines(g.device,VK_NULL_HANDLE,1,&pci,nullptr,&pipe);
   gtavdiag::checkpoint("native-pipeline-create-return");
 }
 if(pr!=VK_SUCCESS){char d[64];snprintf(d,sizeof(d),"vkResult=%d",(int)pr);gtavdiag::checkpoint("native-pipeline-create-failed",d);/* descriptor set reclaimed with owning pool at shutdown */vkDestroyPipelineLayout(g.device,layout,nullptr);vkDestroyDescriptorSetLayout(g.device,dsl,nullptr);return false;}
 if(!gtav_native_renderer_register_graphics_state(key,pipe,layout,desc)){vkDestroyPipeline(g.device,pipe,nullptr);/* descriptor set reclaimed with owning pool at shutdown */vkDestroyPipelineLayout(g.device,layout,nullptr);vkDestroyDescriptorSetLayout(g.device,dsl,nullptr);return false;}
 gtavdiag::checkpoint("native-pipeline-created");return true;
}
static bool buildMappedDrawState(void* ctx,GtavNativeDrawState* s){
 tlsMegaDrawId=gMegaDrawSerial.fetch_add(1,std::memory_order_relaxed)+1;
 if(!s){gMegaDrawFail.fetch_add(1);char d[96];snprintf(d,sizeof(d),"id=%llu null-state",(unsigned long long)tlsMegaDrawId);gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;}
 if(!g.device || !g.queue){gMegaDrawFail.fetch_add(1);char d[96];snprintf(d,sizeof(d),"id=%llu no-runtime",(unsigned long long)tlsMegaDrawId);gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;}
 RageMirrorState m{};
 { std::lock_guard<std::mutex> l(mirrorMutex);
   auto it=mirrorStates.find(ctx); if(it==mirrorStates.end()){gMegaDrawFail.fetch_add(1);char d[128];snprintf(d,sizeof(d),"id=%llu no-mirror ctx=%p",(unsigned long long)tlsMegaDrawId,ctx);gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;} m=mergeCompatAliasState(it->second); }
 {uint32_t vm=0,pm=0,sm=0,tm=0,vbm=0;for(uint32_t i=0;i<16;i++){if(m.vsCB[i])vm|=1u<<i;if(m.psCB[i])pm|=1u<<i;if(m.vsSampler[i]||m.psSampler[i])sm|=1u<<i;if(m.vertexBuffers[i])vbm|=1u<<i;}for(uint32_t i=0;i<32;i++)if(m.vsSRV[i]||m.psSRV[i])tm|=1u<<(i&31);char d[512];snprintf(d,sizeof(d),"id=%llu ctx=%p il=%p vs=%p ps=%p topo=%u vbMask=0x%x ib=%p ifmt=%u ioff=%u cbV=0x%x cbP=0x%x srv=0x%x samp=0x%x rtvN=%u rtv0=%p dsv=%p vp=%u sc=%u blend=%p depth=%p rast=%p",(unsigned long long)tlsMegaDrawId,ctx,m.inputLayout,m.vs,m.ps,m.topology,vbm,m.indexBuffer,m.indexFormat,m.indexOffset,vm,pm,tm,sm,m.rtvCount,m.rtv[0],m.dsv,m.viewportCount,m.scissorCount,m.blendState,m.depthState,m.rasterState);gtavdiag::checkpoint("MEGA-DRAW-BEGIN",d);
 for(uint32_t i=0;i<16;i++)if(m.vertexBuffers[i]){auto* rr=compatResourceObject(m.vertexBuffers[i]);char b[256];snprintf(b,sizeof(b),"id=%llu slot=%u ptr=%p stride=%u off=%u mapped=0x%llx bytes=%zu ver=%llu",(unsigned long long)tlsMegaDrawId,i,m.vertexBuffers[i],m.strides[i],m.offsets[i],(unsigned long long)resolveMapped(m.vertexBuffers[i],NR_VERTEX_BUFFER),rr?rr->backing.size():0,(unsigned long long)(rr?rr->version:0));gtavdiag::checkpoint("MEGA-VB",b);}
}
 if(!m.inputLayout){
   void* fallback=gCompatLastInputLayout.load(std::memory_order_acquire);
   if(fallback&&compatInputLayoutObject(fallback)){m.inputLayout=fallback;gtavdiag::checkpoint("native-input-layout-context-fallback");}
 }
 // ID3D11InputLayout may legally be NULL. In that case the Vulkan pipeline uses
 // zero vertex attributes; shaders relying on SV_VertexID remain valid.
 CompatInputLayoutObject* activeInputLayout=compatResolveInputLayout(m.inputLayout,m.vs);
 const bool needsVertexInput=activeInputLayout&&!activeInputLayout->elements.empty();
 if(needsVertexInput){
   bool anyVB=false;
   for(const auto& e:activeInputLayout->elements)if(e.slot<16&&m.vertexBuffers[e.slot]){anyVB=true;break;}
   if(!anyVB){gMegaDrawFail.fetch_add(1);char d[192];snprintf(d,sizeof(d),"id=%llu required-vb-missing il=%p elems=%zu",(unsigned long long)tlsMegaDrawId,m.inputLayout,activeInputLayout?activeInputLayout->elements.size():0);gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;}
 }
 if(!m.vs){gMegaDrawFail.fetch_add(1);char d[96];snprintf(d,sizeof(d),"id=%llu no-vs",(unsigned long long)tlsMegaDrawId);gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;}
 if(!m.ps){gMegaDrawFail.fetch_add(1);char d[96];snprintf(d,sizeof(d),"id=%llu no-ps",(unsigned long long)tlsMegaDrawId);gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;}
 if(!m.rtvCount || !m.rtv[0]){gMegaDrawFail.fetch_add(1);char d[128];snprintf(d,sizeof(d),"id=%llu no-rtv count=%u",(unsigned long long)tlsMegaDrawId,m.rtvCount);gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;}
 // Use GTA's own native wrappers to obtain real Vulkan images and materialize
 // reusable views for every active render target / depth target / sampled image.
 for(unsigned i=0;i<m.rtvCount&&i<8;i++){
   if(!m.rtv[i])continue;
   if(!mapWrappedImage(m.rtv[i],NR_RTV,true)){gtavdiag::checkpoint("native-draw-fail-map-rtv");return false;}
   if(!gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)m.rtv[i])){gtavdiag::checkpoint("native-draw-fail-view-rtv");return false;}
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
 mapCompatShader(m.vs,NR_VS);mapCompatShader(m.ps,NR_PS);if(m.cs)mapCompatShader(m.cs,NR_CS);
 if(!ensureCompatGraphicsState(m)){gMegaDrawFail.fetch_add(1);char d[160];snprintf(d,sizeof(d),"id=%llu pipeline-build-fail key=0x%llx",(unsigned long long)tlsMegaDrawId,(unsigned long long)graphicsStateKey(m));gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;}
 for(unsigned i=0;i<16;i++)if(m.vertexBuffers[i])mapCompatBuffer(m.vertexBuffers[i],NR_VERTEX_BUFFER);
 if(m.indexBuffer)mapCompatBuffer(m.indexBuffer,NR_INDEX_BUFFER);
 for(unsigned i=0;i<16;i++){if(m.vsCB[i])mapCompatBuffer(m.vsCB[i],NR_CBUFFER);if(m.psCB[i])mapCompatBuffer(m.psCB[i],NR_CBUFFER);if(m.csCB[i])mapCompatBuffer(m.csCB[i],NR_CBUFFER);}
 captureMappedState(ctx,m);
 uint64_t vb=m.vertexBuffers[0]?resolveMapped(m.vertexBuffers[0],NR_VERTEX_BUFFER):0;
 uint64_t vs=resolveMapped(m.vs,NR_VS), ps=resolveMapped(m.ps,NR_PS);
 uint64_t rt=resolveMapped(m.rtv[0],NR_RTV);
 uint64_t stateKey=graphicsStateKey(m);
 uint64_t pipe=gtav_native_renderer_resolve_resource(stateKey,NR_GRAPHICS_PIPELINE);
 uint64_t layout=gtav_native_renderer_resolve_resource(stateKey,NR_PIPELINE_LAYOUT);
 uint64_t desc=gtav_native_renderer_resolve_resource(stateKey,NR_DESCRIPTOR_SET);
 // Do not enter native draw until every GPU object required by that draw has a real Vulkan mapping.
 // This deliberately prevents raw RAGE/D3D pointers from ever reaching vkCmd*.
 if(needsVertexInput){
   for(const auto& e:activeInputLayout->elements){
     if(e.slot>=16||!m.vertexBuffers[e.slot]||!resolveMapped(m.vertexBuffers[e.slot],NR_VERTEX_BUFFER)){
       gMegaDrawFail.fetch_add(1);char d[256];snprintf(d,sizeof(d),"id=%llu required-vb-map-fail semantic=%s%u slot=%u vb=%p stride=%u off=%u mapped=0x%llx",(unsigned long long)tlsMegaDrawId,e.semantic.c_str(),e.semanticIndex,e.slot,e.slot<16?m.vertexBuffers[e.slot]:nullptr,e.slot<16?m.strides[e.slot]:0,e.slot<16?m.offsets[e.slot]:0,(unsigned long long)(e.slot<16?resolveMapped(m.vertexBuffers[e.slot],NR_VERTEX_BUFFER):0));gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;
     }
   }
 }
 if(!vs){gtavdiag::checkpoint("native-draw-fail-map-vs");return false;}
 if(!ps){gtavdiag::checkpoint("native-draw-fail-map-ps");return false;}
 if(!rt){gtavdiag::checkpoint("native-draw-fail-map-rt");return false;}
 if(!pipe){gtavdiag::checkpoint("native-draw-fail-pipeline");return false;}
 if(!layout){gtavdiag::checkpoint("native-draw-fail-pipeline-layout");return false;}
 if(!desc){gtavdiag::checkpoint("native-draw-fail-descriptor");return false;}
 for(uint32_t i=0;i<32;i++){void* p=m.psSRV[i]?m.psSRV[i]:m.vsSRV[i];if(!p)continue;NativeImageMeta mm{};bool have=false;{std::lock_guard<std::mutex> q(imageMetaMutex);auto it=imageMeta.find((uint64_t)(uintptr_t)p);if(it!=imageMeta.end()){mm=it->second;have=true;}}void* ur=compatUnderlyingResource(p);auto* rr=compatResourceObject(ur?ur:p);uint64_t hh=1469598103934665603ull;uint32_t nz=0;if(rr){size_t lim=std::min<size_t>(rr->backing.size(),4096);for(size_t bi=0;bi<lim;bi++){uint8_t v=rr->backing[bi];hh^=v;hh*=1099511628211ull;if(v)nz++;}}char z[448];snprintf(z,sizeof(z),"draw=%llu slot=%u ptr=%p res=%p haveMeta=%d img=%p vkfmt=%d dxgi=%u mip=%u+%u layer=%u+%u size=%ux%u backing=%zu ver=%llu nz4k=%u hash=%016llx",(unsigned long long)tlsMegaDrawId,i,p,ur?ur:p,have?1:0,(void*)mm.image,(int)mm.format,mm.dxgiFormat,mm.baseMip,mm.levelCount,mm.baseLayer,mm.layerCount,mm.width,mm.height,rr?rr->backing.size():0,(unsigned long long)(rr?rr->version:0),nz,(unsigned long long)hh);gtavdiag::checkpoint("MEGA-SRV",z);}
 if(!updateCompatGraphicsDescriptors(m,(VkDescriptorSet)(uintptr_t)desc)){gMegaDrawFail.fetch_add(1);gMegaDescriptorFail.fetch_add(1);char d[160];snprintf(d,sizeof(d),"id=%llu descriptor-update-fail desc=0x%llx",(unsigned long long)tlsMegaDrawId,(unsigned long long)desc);gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;}
 VkCommandBuffer cb=currentNativeCommandBuffer();
 if(cb==VK_NULL_HANDLE){gMegaDrawFail.fetch_add(1);char d[96];snprintf(d,sizeof(d),"id=%llu no-command-buffer",(unsigned long long)tlsMegaDrawId);gtavdiag::checkpoint("MEGA-DRAW-FAIL",d);return false;}

 if(m.indexBuffer&&!resolveMapped(m.indexBuffer,NR_INDEX_BUFFER)){gtavdiag::checkpoint("native-draw-fail-map-ib");return false;}
 // Descriptor update above validates and materializes only resources actually declared
 // by the active shaders. Extra bound D3D slots are legal and must not kill the draw.

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
 gMegaDrawOk.fetch_add(1,std::memory_order_relaxed);{char d[256];snprintf(d,sizeof(d),"id=%llu pipe=%p layout=%p desc=%p cb=%p vb0=%p ib=%p",(unsigned long long)tlsMegaDrawId,s->pipeline,s->pipeline_layout,s->descriptor_set,s->command_buffer,s->vertex_buffer,s->index_buffer);gtavdiag::checkpoint("MEGA-DRAW-STATE-OK",d);}
 return true;
}
static bool getDrawState(void* ctx,GtavNativeDrawState* s){
 if(!s || !g.device || !g.queue) return false;
 if(drawStateProvider && drawStateProvider(ctx,s) && s->command_buffer!=VK_NULL_HANDLE) return true;
 return buildMappedDrawState(ctx,s);
}
static void applyMirroredDynamicState(void* ctx,VkCommandBuffer cb){
 RageMirrorState m{};
 {std::lock_guard<std::mutex> l(mirrorMutex);auto it=mirrorStates.find(ctx);if(it==mirrorStates.end())return;m=mergeCompatAliasState(it->second);}
 if(m.viewportCount){
   uint32_t n=m.viewportCount>4?4:m.viewportCount;
   VkViewport vps[4]{};
   const VkViewport* src=reinterpret_cast<const VkViewport*>(m.viewports);
   for(uint32_t i=0;i<n;i++){
     vps[i]=src[i];
     // D3D11 and the translated DXBC shaders use the D3D framebuffer Y
     // convention. Vulkan needs a negative-height viewport to preserve it.
     vps[i].y=src[i].y+src[i].height;
     vps[i].height=-src[i].height;
   }
   static std::atomic<uint32_t> vpBudget{128};uint32_t vb=vpBudget.fetch_sub(1,std::memory_order_relaxed);
   if(vb>0){char d[192];snprintf(d,sizeof(d),"x=%.1f y=%.1f w=%.1f h=%.1f -> vkY=%.1f vkH=%.1f",src[0].x,src[0].y,src[0].width,src[0].height,vps[0].y,vps[0].height);gtavdiag::checkpoint("native-viewport-yflip",d);}
   vkCmdSetViewport(cb,0,n,vps);
 }
 bool scissorEnabled=false;
 if(auto* rs=compatStateObject(m.rasterState);rs&&rs->descSize>=40){const uint32_t* rd=(const uint32_t*)rs->desc;scissorEnabled=rd[7]!=0;}
 if(scissorEnabled&&m.scissorCount){
   uint32_t n=m.scissorCount>16?16:m.scissorCount;
   VkRect2D rects[16]{};
   const int32_t* d3d=reinterpret_cast<const int32_t*>(m.scissors);
   for(uint32_t i=0;i<n;i++){
     int32_t left=d3d[i*4+0],top=d3d[i*4+1],right=d3d[i*4+2],bottom=d3d[i*4+3];
     rects[i].offset={left,top};
     rects[i].extent={(uint32_t)std::max(0,right-left),(uint32_t)std::max(0,bottom-top)};
   }
   vkCmdSetScissor(cb,0,n,rects);
 }else{
   VkRect2D full{};uint32_t w=1,h=1;
   if(m.viewportCount){const VkViewport* v=reinterpret_cast<const VkViewport*>(m.viewports);w=(uint32_t)std::max(1.0f,std::abs(v[0].width));h=(uint32_t)std::max(1.0f,std::abs(v[0].height));}
   full.extent={w,h};vkCmdSetScissor(cb,0,1,&full);
 }
 vkCmdSetBlendConstants(cb,m.blendFactor);
 vkCmdSetStencilReference(cb,VK_STENCIL_FACE_FRONT_AND_BACK,m.stencilRef);
}
static bool beginCompatRendering(void* ctx,VkCommandBuffer cb){
 if(!cb){gtavdiag::checkpoint("native-render-scope-no-command-buffer");return false;}
 auto beginRendering=resolveBeginRenderingNow();
 auto endRendering=resolveEndRenderingNow();
 if(!beginRendering||!endRendering){gtavdiag::checkpoint("native-render-scope-no-dynamic-rendering");return false;}
 RageMirrorState m{};{std::lock_guard<std::mutex> l(mirrorMutex);auto it=mirrorStates.find(ctx);if(it==mirrorStates.end()){gtavdiag::checkpoint("native-render-scope-no-mirror");return false;}m=mergeCompatAliasState(it->second);}
 for(uint32_t i=0;i<32;i++){
   if(m.vsSRV[i]){
     if(!syncCompatOwnedImage(cb,m.vsSRV[i])){char d[48];snprintf(d,sizeof(d),"vs-srv=%u",i);gtavdiag::checkpoint("native-render-scope-sync-srv-failed",d);return false;}
     transitionCompatOwnedImage(cb,m.vsSRV[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
   }
   if(m.psSRV[i]){
     if(!syncCompatOwnedImage(cb,m.psSRV[i])){char d[48];snprintf(d,sizeof(d),"ps-srv=%u",i);gtavdiag::checkpoint("native-render-scope-sync-srv-failed",d);return false;}
     transitionCompatOwnedImage(cb,m.psSRV[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
   }
 }
 VkRenderingAttachmentInfo colors[8]{};uint32_t colorCount=m.rtvCount>8?8:m.rtvCount;
 for(uint32_t i=0;i<colorCount;i++){
   if(!m.rtv[i])continue;
   if(!syncCompatOwnedImage(cb,m.rtv[i])){char d[48];snprintf(d,sizeof(d),"rtv=%u",i);gtavdiag::checkpoint("native-render-scope-sync-rtv-failed",d);return false;}
   transitionCompatOwnedImage(cb,m.rtv[i],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
   VkImageView v=gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)m.rtv[i]);
   if(!v){char d[48];snprintf(d,sizeof(d),"rtv=%u",i);gtavdiag::checkpoint("native-render-scope-rtv-view-failed",d);return false;}
   colors[i].sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;colors[i].imageView=v;colors[i].imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;colors[i].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;colors[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
   if(void* u=compatUnderlyingResource(m.rtv[i])){
     if(auto* rr=compatResourceObject(u);rr&&(rr->pendingClearFlags&0x100u)){
       colors[i].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;for(int k=0;k<4;k++)colors[i].clearValue.color.float32[k]=rr->pendingClearColor[k];rr->pendingClearFlags&=~0x100u;
     }else{
       std::lock_guard<std::mutex> cl(gCompatPendingClearMutex);
       auto pc=gCompatPendingNativeColorClears.find(u);
       if(pc!=gCompatPendingNativeColorClears.end()){
         colors[i].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
         for(int k=0;k<4;k++)colors[i].clearValue.color.float32[k]=pc->second.color[k];
         gCompatPendingNativeColorClears.erase(pc);
         gtavdiag::checkpoint("native-foreign-rtv-clear-applied");
       }
     }
   }
 }
 VkRenderingAttachmentInfo depth{},stencilAtt{};VkRenderingAttachmentInfo* dp=nullptr;VkRenderingAttachmentInfo* sp=nullptr;bool hasStencil=false;
 if(m.dsv){
   transitionCompatOwnedImage(cb,m.dsv,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
   VkImageView v=gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)m.dsv);
   if(!v){gtavdiag::checkpoint("native-render-scope-dsv-view-failed");return false;}
   {std::lock_guard<std::mutex> l(imageMetaMutex);auto it=imageMeta.find((uint64_t)(uintptr_t)m.dsv);if(it!=imageMeta.end())hasStencil=(it->second.aspect&VK_IMAGE_ASPECT_STENCIL_BIT)!=0;}
   depth.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;depth.imageView=v;depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;depth.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE;stencilAtt=depth;dp=&depth;if(hasStencil)sp=&stencilAtt;
   if(void* u=compatUnderlyingResource(m.dsv)){auto* rr=compatResourceObject(u);if(rr){depth.clearValue.depthStencil.depth=rr->pendingClearDepth;depth.clearValue.depthStencil.stencil=rr->pendingClearStencil;stencilAtt.clearValue=depth.clearValue;if(rr->pendingClearFlags&1u)depth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;if(hasStencil&&(rr->pendingClearFlags&2u))stencilAtt.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;rr->pendingClearFlags&=~3u;}}
 }
 uint32_t rw=gCompatSwapWidth.load(),rh=gCompatSwapHeight.load();int32_t rx=0,ry=0;if(m.rtvCount&&m.rtv[0]){void* u=compatUnderlyingResource(m.rtv[0]);if(u){auto* rr=(CompatResourceObject*)u;if(rr->vtbl==gCompatTexture2DVtable&&rr->descSize>=8){rw=((uint32_t*)rr->desc)[0];rh=((uint32_t*)rr->desc)[1];}}}
 if(m.viewportCount){const VkViewport* v=reinterpret_cast<const VkViewport*>(m.viewports);rx=(int32_t)std::max(0.0f,v[0].x);ry=(int32_t)std::max(0.0f,v[0].y);float vw=v[0].width<0.0f?-v[0].width:v[0].width;float vh=v[0].height<0.0f?-v[0].height:v[0].height;rw=std::min(rw,(uint32_t)std::max(1.0f,vw));rh=std::min(rh,(uint32_t)std::max(1.0f,vh));}
 VkRect2D area{{rx,ry},{std::max(1u,rw),std::max(1u,rh)}};VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea=area;ri.layerCount=1;ri.colorAttachmentCount=colorCount;ri.pColorAttachments=colorCount?colors:nullptr;ri.pDepthAttachment=dp;ri.pStencilAttachment=sp;
 beginRendering(cb,&ri);
 gtavdiag::checkpoint("native-render-scope-begun");
 return true;
}
static bool refreshCompatDescriptors(const RageMirrorState& m,VkDescriptorSet desc){
 if(!g.device||!desc)return false;
 // If shader reflection produced descriptor declarations, the descriptor set
 // layout was created from those exact declarations. Updating every mirrored
 // D3D slot would write bindings that do not exist in that layout and is
 // invalid Vulkan (Turnip can fault inside vkUpdateDescriptorSets). Reuse the
 // declaration-driven updater so every write matches the active layout.
 if(!getCompatShaderDecls(m.vs).empty() || !getCompatShaderDecls(m.ps).empty())
   return updateCompatGraphicsDescriptors(m,desc);
 std::vector<VkWriteDescriptorSet> writes;std::vector<VkDescriptorBufferInfo> bis;std::vector<VkDescriptorImageInfo> iis;
 bis.reserve(32);iis.reserve(96);writes.reserve(128);
 auto wb=[&](uint32_t binding,void* p){
   if(!p)return;if(!mapCompatBuffer(p,NR_CBUFFER))return;uint64_t h=resolveMapped(p,NR_CBUFFER);if(!h)return;
   auto* rr=(CompatResourceObject*)p;VkDescriptorBufferInfo bi{(VkBuffer)(uintptr_t)h,0,rr->backing.empty()?VK_WHOLE_SIZE:(VkDeviceSize)rr->backing.size()};
   bis.push_back(bi);VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=desc;w.dstBinding=binding;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w.pBufferInfo=&bis.back();writes.push_back(w);
 };
 auto wi=[&](uint32_t binding,void* p){
   if(!p)return;if(!mapWrappedImage(p,NR_SRV,false))return;VkImageView v=gtav_native_renderer_create_image_view((uint64_t)(uintptr_t)p);if(!v)return;
   VkDescriptorImageInfo ii{};ii.imageView=v;ii.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;iis.push_back(ii);
   VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=desc;w.dstBinding=binding;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&iis.back();writes.push_back(w);
 };
 auto ws=[&](uint32_t binding,void* p){
   if(!p)return;if(!mapCompatSampler(p))return;VkSampler sm=(VkSampler)(uintptr_t)resolveMapped(p,NR_SAMPLER);if(!sm)return;
   VkDescriptorImageInfo ii{};ii.sampler=sm;iis.push_back(ii);
   VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=desc;w.dstBinding=binding;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;w.pImageInfo=&iis.back();writes.push_back(w);
 };
 for(uint32_t i=0;i<16;i++){if(m.vsCB[i])wb(compatDescriptorBinding(0,23,0,i),m.vsCB[i]);if(m.psCB[i])wb(compatDescriptorBinding(1,23,0,i),m.psCB[i]);}
 for(uint32_t i=0;i<32;i++){if(m.vsSRV[i])wi(compatDescriptorBinding(0,24,0,i),m.vsSRV[i]);if(m.psSRV[i])wi(compatDescriptorBinding(1,24,0,i),m.psSRV[i]);}
 for(uint32_t i=0;i<16;i++){if(m.vsSampler[i])ws(compatDescriptorBinding(0,22,0,i),m.vsSampler[i]);if(m.psSampler[i])ws(compatDescriptorBinding(1,22,0,i),m.psSampler[i]);}
 if(!writes.empty())vkUpdateDescriptorSets(g.device,(uint32_t)writes.size(),writes.data(),0,nullptr);
 return true;
}
static bool bindMappedGraphicsState(void* ctx,const GtavNativeDrawState& s,bool indexed){
 if(!s.command_buffer||!s.pipeline||!s.pipeline_layout||!s.descriptor_set)return false;
 if(indexed&&!s.index_buffer)return false;
 vkCmdBindPipeline(s.command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,s.pipeline);
 // Bind every active mirrored vertex stream, not only slot 0. This keeps native
 // multi-stream vertex input identical to the RAGE/D3D state before a draw.
 RageMirrorState m{};
 {std::lock_guard<std::mutex> l(mirrorMutex);auto it=mirrorStates.find(ctx);if(it==mirrorStates.end())return false;m=mergeCompatAliasState(it->second);}
 if(!refreshCompatDescriptors(m,s.descriptor_set)){gtavdiag::checkpoint("native-draw-fail-descriptor-refresh");return false;}
 VkBuffer vbs[16]{}; VkDeviceSize offsets[16]{};
 uint32_t last=0;
 for(uint32_t i=0;i<16;i++){
   if(!m.vertexBuffers[i])continue;
   uint64_t mapped=resolveMapped(m.vertexBuffers[i],NR_VERTEX_BUFFER);
   if(!mapped)return false;
   vbs[i]=(VkBuffer)(uintptr_t)mapped; offsets[i]=m.offsets[i]; last=i+1;
 }
 auto* il=compatResolveInputLayout(m.inputLayout,m.vs);
 bool needsVertexInput=il&&!il->elements.empty();
 if(needsVertexInput){
   for(const auto& e:il->elements){
     if(e.slot>=16||!vbs[e.slot]){gtavdiag::checkpoint("native-bind-missing-layout-stream");return false;}
   }
 }
 if(!last&&needsVertexInput)return false;
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
extern "C" bool gtavnative_compat_draw(void* c,uint32_t n,uint32_t f){return gtav_native_renderer_rage_draw(c,n,f);}
extern "C" bool gtavnative_compat_draw_indexed(void* c,uint32_t n,uint32_t f,int32_t v){return gtav_native_renderer_rage_draw_indexed(c,n,f,v);}
extern "C" bool gtavnative_compat_dispatch(void* c,uint32_t x,uint32_t y,uint32_t z){return gtav_native_renderer_rage_dispatch(c,x,y,z);}
static void rememberDrawnPrimaryRTV(void* ctx){
 std::lock_guard<std::mutex> l(mirrorMutex);auto it=mirrorStates.find(ctx);if(it==mirrorStates.end())return;RageMirrorState merged=mergeCompatAliasState(it->second);if(!merged.rtvCount||!merged.rtv[0])return;
 void* v=merged.rtv[0];
 uint64_t serial=compatPresentWriteSerial.fetch_add(1,std::memory_order_acq_rel)+1;
 lastCompatDrawnRTV.store(v,std::memory_order_release);
 lastCompatDrawnSerial.store(serial,std::memory_order_release);
 uint32_t w=0,h=0;void* r=compatUnderlyingResource(v);if(!r)r=v;auto* rr=compatResourceObject(r);
 if(r==&gCompatBackBuffer){w=gCompatSwapWidth.load(std::memory_order_relaxed);h=gCompatSwapHeight.load(std::memory_order_relaxed);}
 else if(rr&&rr->vtbl==gCompatTexture2DVtable&&rr->descSize>=8){w=((uint32_t*)rr->desc)[0];h=((uint32_t*)rr->desc)[1];}
 else {
   if(mapWrappedImage(r,2u,true)){std::lock_guard<std::mutex> ml(imageMetaMutex);auto mi=imageMeta.find((uint64_t)(uintptr_t)v);if(mi==imageMeta.end())mi=imageMeta.find((uint64_t)(uintptr_t)r);if(mi!=imageMeta.end()){w=mi->second.width;h=mi->second.height;}}
 }
 uint32_t tw=gCompatSwapWidth.load(),th=gCompatSwapHeight.load();if(w&&h&&tw&&th&&w*4>=tw*3&&h*4>=th*3){lastCompatFullSizeRTV.store(v,std::memory_order_release);lastCompatFullSizeSerial.store(serial,std::memory_order_release);static std::atomic<uint32_t> fsn{0};uint32_t fn=fsn.fetch_add(1,std::memory_order_relaxed);if(fn<8||fn%512==0){char fd[128];snprintf(fd,sizeof(fd),"view=%p resource=%p size=%ux%u swap=%ux%u",v,r,w,h,tw,th);gtavdiag::checkpoint("native-fullsize-rtv-selected",fd);}}
 static std::atomic<uint32_t> dn{0};uint32_t n=dn.fetch_add(1,std::memory_order_relaxed);if(n<12||n%512==0){char d[192];snprintf(d,sizeof(d),"view=%p resource=%p compat=%u size=%ux%u fmt=%u",v,r,(rr&&rr->vtbl==gCompatTexture2DVtable)?1u:0u,w,h,(rr&&rr->descSize>=20)?((uint32_t*)rr->desc)[4]:0u);gtavdiag::checkpoint("native-last-drawn-rtv",d);}
}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_draw(void* ctx,uint32_t vc,uint32_t first){GtavNativeDrawState s{};if(!getDrawState(ctx,&s)||!bindMappedGraphicsState(ctx,s,false))return false;if(!beginCompatRendering(ctx,s.command_buffer)){gtavdiag::checkpoint("native-draw-fail-render-scope");return false;}applyMirroredDynamicState(ctx,s.command_buffer);vkCmdDraw(s.command_buffer,vc,1,first,0);gBlackProbeDraws.fetch_add(1,std::memory_order_relaxed);rememberDrawnPrimaryRTV(ctx);gtavdiag::checkpoint("native-vkcmd-draw");endCompatRenderingNow(s.command_buffer);return true;}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_draw_indexed(void* ctx,uint32_t ic,uint32_t first,int32_t vo){GtavNativeDrawState s{};if(!getDrawState(ctx,&s)||!bindMappedGraphicsState(ctx,s,true))return false;if(!beginCompatRendering(ctx,s.command_buffer)){gtavdiag::checkpoint("native-draw-fail-render-scope");return false;}applyMirroredDynamicState(ctx,s.command_buffer);vkCmdDrawIndexed(s.command_buffer,ic,1,first,vo,0);gBlackProbeDraws.fetch_add(1,std::memory_order_relaxed);rememberDrawnPrimaryRTV(ctx);gtavdiag::checkpoint("native-vkcmd-draw-indexed");endCompatRenderingNow(s.command_buffer);return true;}
extern "C" __attribute__((visibility("default"))) bool gtav_native_renderer_rage_dispatch(void* ctx,uint32_t x,uint32_t y,uint32_t z){
 if(!ctx||!x||!y||!z||!g.device)return false;RageMirrorState m{};{std::lock_guard<std::mutex> l(mirrorMutex);auto it=mirrorStates.find(ctx);if(it==mirrorStates.end())return false;m=it->second;}if(!m.cs)return false;
 if(!ensureCompatComputeState(m)){gtavdiag::checkpoint("native-dispatch-fail-pipeline");return false;}VkCommandBuffer cb=currentNativeCommandBuffer();if(!cb){gtavdiag::checkpoint("native-dispatch-fail-command-buffer");return false;}
 for(uint32_t i=0;i<32;i++)if(m.csSRV[i]){if(!syncCompatOwnedImage(cb,m.csSRV[i]))return false;transitionCompatOwnedImage(cb,m.csSRV[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
 for(uint32_t i=0;i<16;i++)if(m.csUAV[i]){void* u=compatUnderlyingResource(m.csUAV[i]);auto* ur=compatResourceObject(u);if(ur&&ur->vtbl==gCompatTexture2DVtable){if(!syncCompatOwnedImage(cb,m.csUAV[i]))return false;transitionCompatOwnedImage(cb,m.csUAV[i],VK_IMAGE_LAYOUT_GENERAL);}}
 uint64_t key=hashMix((uint64_t)(uintptr_t)m.cs,0x43534e4154495645ull);VkPipeline pipe=(VkPipeline)(uintptr_t)gtav_native_renderer_resolve_resource(key,NR_GRAPHICS_PIPELINE);VkPipelineLayout layout=(VkPipelineLayout)(uintptr_t)gtav_native_renderer_resolve_resource(key,NR_PIPELINE_LAYOUT);VkDescriptorSet desc=(VkDescriptorSet)(uintptr_t)gtav_native_renderer_resolve_resource(key,NR_DESCRIPTOR_SET);
 if(!pipe||!layout||!desc||!updateCompatComputeDescriptors(m,desc))return false;vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&desc,0,nullptr);vkCmdDispatch(cb,x,y,z);return true;
}


using OrigGrvkSwapchainPresent=bool(*)(void*,uint32_t,VkSemaphore);
static OrigGrvkSwapchainPresent origGrvkSwapchainPresent{};
struct PresentBridgeSlot{VkCommandBuffer cb{VK_NULL_HANDLE};VkFence fence{VK_NULL_HANDLE};VkSemaphore done{VK_NULL_HANDLE};bool inFlight{false};};
static PresentBridgeSlot gPresentBridge[3]{};
static uint32_t gPresentBridgeCursor=0;
static std::atomic<bool> gPresentBridgeHookInstalled{false};
static std::atomic<bool> gPresentBridgeReady{false};

static bool ensurePresentBridgeResources(){
 if(gPresentBridgeReady.load(std::memory_order_acquire))return true;
 gtavdiag::checkpoint("native-present-bridge-resource-init");
 if(!g.device){gtavdiag::checkpoint("native-present-bridge-no-device");return false;}
 if(!g.commands){gtavdiag::checkpoint("native-present-bridge-no-command-pool");return false;}
 VkCommandBuffer bufs[3]{};
 VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
 ai.commandPool=g.commands;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=3;
 VkResult ar=vkAllocateCommandBuffers(g.device,&ai,bufs);
 if(ar!=VK_SUCCESS){gtavdiag::checkpoint("native-present-bridge-command-alloc-failed");__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE-PRESENT","bridge command alloc failed=%d",(int)ar);return false;}
 for(uint32_t i=0;i<3;i++){
   gPresentBridge[i].cb=bufs[i];
   VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkResult fr=vkCreateFence(g.device,&fi,nullptr,&gPresentBridge[i].fence);
   if(fr!=VK_SUCCESS){gtavdiag::checkpoint("native-present-bridge-fence-create-failed");__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE-PRESENT","bridge fence create slot=%u result=%d",i,(int)fr);return false;}
   VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkResult sr=vkCreateSemaphore(g.device,&si,nullptr,&gPresentBridge[i].done);
   if(sr!=VK_SUCCESS){gtavdiag::checkpoint("native-present-bridge-semaphore-create-failed");__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE-PRESENT","bridge semaphore create slot=%u result=%d",i,(int)sr);return false;}
 }
 gPresentBridgeReady.store(true,std::memory_order_release);
 gtavdiag::checkpoint("native-present-bridge-ready");
 return true;
}

static bool hookGrvkSwapchainPresent(void* self,uint32_t imageIndex,VkSemaphore waitSemaphore){
 static std::atomic<uint32_t> bridgeCalls{0};
 uint32_t call=bridgeCalls.fetch_add(1,std::memory_order_relaxed)+1;
 if(call<=8 || (call%120)==0){
   gtavdiag::checkpoint("native-present-bridge-enter");
   __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-PRESENT","bridge enter=%u self=%p image=%u wait=%p orig=%p device=%p queue=%p",call,self,imageIndex,(void*)waitSemaphore,(void*)origGrvkSwapchainPresent,(void*)g.device,(void*)g.queue);
 }
 if(!origGrvkSwapchainPresent){gtavdiag::checkpoint("native-present-bridge-no-original");return false;}
 if(!self){gtavdiag::checkpoint("native-present-bridge-no-self");return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);}
 if(!g.device){gtavdiag::checkpoint("native-present-bridge-no-device");return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);}
 if(!g.queue){gtavdiag::checkpoint("native-present-bridge-no-queue");return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);}
 if(!ensurePresentBridgeResources()){gtavdiag::checkpoint("native-present-bridge-resources-failed");return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);}

 VkImage src=VK_NULL_HANDLE;uint32_t sw=0,sh=0;
 {
   std::lock_guard<std::mutex> l(imageMetaMutex);
   auto it=compatOwnedImages.find((uint64_t)(uintptr_t)&gCompatBackBuffer);
   if(it!=compatOwnedImages.end()){src=it->second.image;sw=it->second.width;sh=it->second.height;}
 }
 if(!src||!sw||!sh){
   gtavdiag::checkpoint("native-present-bridge-no-backbuffer");
   return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);
 }

 // grvk::Swapchain layout verified against this libgtav build:
 // +0x48 std::vector<VkImage>, +0x30 VkExtent2D.
 auto* base=reinterpret_cast<uint8_t*>(self);
 VkImage* images=*reinterpret_cast<VkImage**>(base+0x48);
 VkImage* imagesEnd=*reinterpret_cast<VkImage**>(base+0x50);
 uint32_t count=(images&&imagesEnd&&imagesEnd>=images)?uint32_t(imagesEnd-images):0;
 if(!count||imageIndex>=count||!images[imageIndex]){
   gtavdiag::checkpoint("native-present-bridge-bad-image-index");
   return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);
 }
 VkImage dst=images[imageIndex];
 uint32_t dw=*reinterpret_cast<uint32_t*>(base+0x30);
 uint32_t dh=*reinterpret_cast<uint32_t*>(base+0x34);
 if(!dw||!dh){dw=sw;dh=sh;}

 PresentBridgeSlot& slot=gPresentBridge[gPresentBridgeCursor++%3];
 if(slot.inFlight){
   if(vkWaitForFences(g.device,1,&slot.fence,VK_TRUE,1000000000ull)!=VK_SUCCESS){
     gtavdiag::checkpoint("native-present-bridge-fence-failed");
     return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);
   }
   vkResetFences(g.device,1,&slot.fence);slot.inFlight=false;
 }
 VkResult rr=vkResetCommandBuffer(slot.cb,0);
 if(rr!=VK_SUCCESS){gtavdiag::checkpoint("native-present-bridge-reset-command-failed");__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE-PRESENT","bridge reset command failed=%d",(int)rr);return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);}
 VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
 VkResult br=vkBeginCommandBuffer(slot.cb,&bi);
 if(br!=VK_SUCCESS){gtavdiag::checkpoint("native-present-bridge-begin-command-failed");__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE-PRESENT","bridge begin command failed=%d",(int)br);return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);}
 gtavdiag::checkpoint("native-present-bridge-recording");

 transitionCompatOwnedImage(slot.cb,&gCompatBackBuffer,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
 VkImageMemoryBarrier db{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
 db.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;db.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
 db.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;db.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
 db.image=dst;db.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
 db.subresourceRange.baseMipLevel=0;db.subresourceRange.levelCount=1;db.subresourceRange.baseArrayLayer=0;db.subresourceRange.layerCount=1;
 db.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT;db.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
 vkCmdPipelineBarrier(slot.cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&db);

 VkImageBlit blit{};
 blit.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;blit.srcSubresource.layerCount=1;
 blit.srcOffsets[1]=VkOffset3D{(int32_t)sw,(int32_t)sh,1};
 blit.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;blit.dstSubresource.layerCount=1;
 blit.dstOffsets[1]=VkOffset3D{(int32_t)dw,(int32_t)dh,1};
 gtavdiag::checkpoint("native-present-bridge-blit-record");
 vkCmdBlitImage(slot.cb,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);

 db.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;db.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
 db.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;db.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;
 vkCmdPipelineBarrier(slot.cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&db);
 transitionCompatOwnedImage(slot.cb,&gCompatBackBuffer,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
 VkResult er=vkEndCommandBuffer(slot.cb);
 if(er!=VK_SUCCESS){gtavdiag::checkpoint("native-present-bridge-end-command-failed");__android_log_print(ANDROID_LOG_ERROR,"GTAV-NATIVE-PRESENT","bridge end command failed=%d",(int)er);return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);}

 VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_TRANSFER_BIT;
 VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
 if(waitSemaphore){si.waitSemaphoreCount=1;si.pWaitSemaphores=&waitSemaphore;si.pWaitDstStageMask=&waitStage;}
 si.commandBufferCount=1;si.pCommandBuffers=&slot.cb;si.signalSemaphoreCount=1;si.pSignalSemaphores=&slot.done;
 gtavdiag::checkpoint("native-present-bridge-submit");
 VkResult sr=vkQueueSubmit(g.queue,1,&si,slot.fence);
 if(sr!=VK_SUCCESS){
   gtavdiag::checkpoint("native-present-bridge-submit-failed");
   return origGrvkSwapchainPresent(self,imageIndex,waitSemaphore);
 }
 slot.inFlight=true;
 gtavdiag::checkpoint("native-present-bridge-blit");
 return origGrvkSwapchainPresent(self,imageIndex,slot.done);
}

static bool ensurePresentBridgeHook(){
 if(gPresentBridgeHookInstalled.load(std::memory_order_acquire))return true;
 if(!gtavBase)return false;
 static constexpr uint32_t expected[4]={0xd10243ffu,0xa9067bfdu,0xa90757f6u,0xa9084ff4u};
 uintptr_t target=gtavBase+0x6242688;
 if(std::memcmp((void*)target,expected,16)!=0){gtavdiag::checkpoint("native-present-bridge-prologue-mismatch");return false;}
 uint32_t saved[4]{};void* tramp=nullptr;
 if(!patchJump(target,(void*)hookGrvkSwapchainPresent,saved,&tramp)){gtavdiag::checkpoint("native-present-bridge-hook-failed");return false;}
 origGrvkSwapchainPresent=reinterpret_cast<OrigGrvkSwapchainPresent>(tramp);
 gPresentBridgeHookInstalled.store(true,std::memory_order_release);
 gtavdiag::checkpoint("native-present-bridge-hooked");
 return true;
}

extern "C" __attribute__((visibility("default"))) void gtav_native_renderer_begin_frame(){
 bool hadDevice=!!g.device; bool attached=hadDevice||attachFromGtavRuntime();
 uint64_t frame=g.frame.fetch_add(1,std::memory_order_relaxed)+1;
 if(attached){
   probeGameAndroidSurface();
   // This function already closes/submits the previously recorded frame before
   // opening the next ring command buffer. Calling a second submit helper here
   // was both undefined and redundant.
   gtavdiag::checkpoint("native-active-present-submit");
   submitAndBeginCompatFrameCommand();
 }

 // Hook installation was deliberately removed from the ELF constructor because
 // libgtav's graphics bootstrap is not ready there. Present is the first verified
 // late point where the native Vulkan runtime is alive, so arm the draw cutover here.
 static std::atomic<uint32_t> hookAttempts{0};
 // Retry a failed late install a few times: bootstrap code may still be changing
 // protections/state on the first Present, and optional hooks no longer gate cutover.
 uint32_t ha=hookAttempts.load(std::memory_order_relaxed);
 if(attached && !drawHooksInstalled.load(std::memory_order_acquire) && ha<3 &&
    hookAttempts.compare_exchange_strong(ha,ha+1,std::memory_order_acq_rel)){
   gtavdiag::checkpoint("native-draw-hooks-install-attempt");
   bool hooks=gtav_native_renderer_install_draw_hooks();
   gtavdiag::checkpoint(hooks?"native-draw-hooks-installed":"native-draw-hooks-failed");
   __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-PRESENT","late draw hooks installed=%d",hooks?1:0);
 }

 if(frame<=8 || (frame%120)==0){
   uint64_t native=nativeDraws.load(std::memory_order_relaxed)+nativeIndexedDraws.load(std::memory_order_relaxed)+nativeDispatches.load(std::memory_order_relaxed);
   uint64_t fallback=fallbackDraws.load(std::memory_order_relaxed);
   VkCommandBuffer cb=observedNativeCommandBuffer.load(std::memory_order_acquire);
   gtavdiag::checkpoint(attached?"native-frame-attached":"native-frame-unattached");
   if(drawHooksInstalled.load(std::memory_order_acquire))
     gtavdiag::checkpoint(cb?"native-draw-path-command-buffer":"native-draw-path-waiting-command-buffer");
   __android_log_print(ANDROID_LOG_INFO,"GTAV-NATIVE-PRESENT",
     "frame=%llu attached=%d device=%p queue=%p hooks=%d cb=%p native=%llu fallback=%llu",
     (unsigned long long)frame,attached?1:0,(void*)g.device,(void*)g.queue,
     drawHooksInstalled.load(std::memory_order_acquire)?1:0,(void*)cb,
     (unsigned long long)native,(unsigned long long)fallback);
 }
}
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
 {
   std::lock_guard<std::mutex> l(descriptorPoolMutex);
   for(VkDescriptorPool p:descriptorPools)if(p)vkDestroyDescriptorPool(g.device,p,nullptr);
   descriptorPools.clear();g.descriptors=VK_NULL_HANDLE;descriptorPoolGeneration=0;
 }
 for(auto& f:gCompatFrameCmd){if(f.fence)vkDestroyFence(g.device,f.fence,nullptr);f.fence=VK_NULL_HANDLE;f.cb=VK_NULL_HANDLE;f.inFlight=false;}
 gCompatFrameCmdReady=false;gCompatRecordingCB=VK_NULL_HANDLE;
 for(auto& p:gPresentBridge){if(p.fence)vkDestroyFence(g.device,p.fence,nullptr);if(p.done)vkDestroySemaphore(g.device,p.done,nullptr);p.fence=VK_NULL_HANDLE;p.done=VK_NULL_HANDLE;p.cb=VK_NULL_HANDLE;p.inFlight=false;}
 gPresentBridgeReady.store(false,std::memory_order_release);
 profileDestroy();
 if(g.commands)vkDestroyCommandPool(g.device,g.commands,nullptr);
 g.descriptors=VK_NULL_HANDLE;g.commands=VK_NULL_HANDLE;g.device=VK_NULL_HANDLE;g.queue=VK_NULL_HANDLE;
}
extern "C" __attribute__((visibility("default"))) const GtavNativeDispatch* gtav_native_renderer_get_dispatch(){static const GtavNativeDispatch d{2,gtav_native_renderer_ready,gtav_native_renderer_begin_frame,gtav_native_renderer_register_resource,gtav_native_renderer_resolve_resource,gtav_native_renderer_unregister_resource,gtav_native_renderer_bind_vertex_buffer,gtav_native_renderer_bind_index_buffer,gtav_native_renderer_set_viewport,gtav_native_renderer_set_scissor,gtav_native_renderer_draw,gtav_native_renderer_draw_indexed,gtav_native_renderer_dispatch,gtav_native_renderer_set_draw_state_provider,gtav_native_renderer_rage_draw,gtav_native_renderer_rage_draw_indexed,gtav_native_renderer_rage_dispatch};return &d;}
}
