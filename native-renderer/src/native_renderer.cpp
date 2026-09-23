#include <vulkan/vulkan.h>
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
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <cstdio>
#include <cstdlib>



namespace gtavdiag {
static const char* kPath="/storage/emulated/0/Games/GTAV/Config/gtav-native-crash.txt";
static std::atomic<uint32_t> seq{0};
static std::atomic<const char*> last{"native-renderer-loaded"};
static void ensureDir(){ mkdir("/storage/emulated/0/Games",0775); mkdir("/storage/emulated/0/Games/GTAV",0775); mkdir("/storage/emulated/0/Games/GTAV/Config",0775); }
static void append(const char* s){ ensureDir(); int fd=open(kPath,O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC,0664); if(fd>=0){write(fd,s,strlen(s));close(fd);} }
static void checkpoint(const char* name,const char* detail=nullptr){
 last.store(name,std::memory_order_relaxed); char b[768];
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
 const char* x=" last=";memcpy(p,x,strlen(x));p+=strlen(x);const char* z=last.load(std::memory_order_relaxed);size_t zn=strlen(z);memcpy(p,z,zn);p+=zn;*p++='\n';
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
static CompatDXGIFactory gCompatFactory{};
static CompatDXGIAdapter gCompatAdapter{};
static void* gFactoryVtable[8]{};
static void* gAdapterVtable[10]{};

static int32_t compatQueryInterface(void* self, const void*, void** out) {
  gtavdiag::checkpoint("compat-dxgi-query-interface");
  if (!out) return (int32_t)0x80004003u;
  // Do not claim arbitrary DXGI interfaces on the adapter. SuppressAltEnter
  // probes adapter -> QI -> GetParent(+0x30); our minimal adapter does not
  // implement that queried interface. Returning E_NOINTERFACE makes the
  // engine take its safe non-Windows fullscreen fallback.
  if (self == &gCompatAdapter) {
    *out=nullptr;
    gtavdiag::checkpoint("compat-dxgi-adapter-qi-unsupported");
    return (int32_t)0x80004002u;
  }
  *out=self;
  return 0;
}
static uint32_t compatAddRef(void*) { return 2; }
static uint32_t compatRelease(void*) { return 1; }
static int32_t compatEnumAdapters(void*, uint32_t index, void** out) {
  gtavdiag::checkpoint("compat-dxgi-enum-adapters");
  if (!out) return (int32_t)0x80004003u;
  if (index != 0) { *out=nullptr; return (int32_t)0x887A0002u; } // DXGI_ERROR_NOT_FOUND
  *out=&gCompatAdapter; return 0;
}
struct CompatDXGIOutput { void** vtbl; uint8_t pad[8]; uint32_t modeCount; };
static CompatDXGIOutput gCompatOutput{};
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
  gtavdiag::checkpoint("compat-dxgi-output-unsupported");
  return (int32_t)0x80004001u;
}
static int32_t compatEnumOutputs(void*, uint32_t index, void** out) {
  gtavdiag::checkpoint("compat-dxgi-enum-outputs");
  if (!out) return (int32_t)0x80004003u;
  if(index!=0){*out=nullptr;return (int32_t)0x887A0002u;}
  static bool once=false;
  if(!once){ once=true; for(void*& p:gOutputVtable)p=(void*)compatOutputUnsupported;
    gOutputVtable[0]=(void*)compatOutputQueryInterface; gOutputVtable[1]=(void*)compatOutputAddRef; gOutputVtable[2]=(void*)compatOutputRelease;
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
  gFactoryVtable[7]=(void*)compatEnumAdapters;
  gCompatFactory.vtbl=gFactoryVtable;
  // Adapter slots observed by grcAdapterD3D11:
  // Release @ +0x10, EnumOutputs @ +0x38, GetDesc @ +0x40.
  gAdapterVtable[0]=(void*)compatQueryInterface;
  gAdapterVtable[1]=(void*)compatAddRef;
  gAdapterVtable[2]=(void*)compatRelease;
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

static int32_t compatD3DQueryInterface(void* self,const void*,void** out) {
  gtavdiag::checkpoint("compat-d3d11-query-interface");
  if(!out) return (int32_t)0x80004003u;
  // grcDevice::RetrieveVideoMemory queries the temporary ID3D11Device for a
  // DXGI device interface, then immediately calls IDXGIObject::GetParent (+0x30).
  // Returning the D3D device itself here gives that call the wrong vtable.
  if(self==&gCompatD3DDevice) *out=&gCompatDXGIDevice;
  else *out=self;
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
static void compatContextNoop(void*,...){gtavdiag::checkpoint("compat-context-noop");}
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
  extern bool compatMapResourceBackingSubresource(void*, uint32_t, CompatMappedSubresource*);\n  (void)compatMapResourceBackingSubresource(resource,subresource,mapped);
  return 0;
}
static void compatD3DUnmap(void*, void*, uint32_t) {
  gtavdiag::checkpoint("compat-d3d11-unmap");
}
// Shader creation is consumed as an object pointer by grcProgram::CreateShader.
// Returning E_NOTIMPL through the generic stub leaves the out-object undefined
// and later crashes on Release. Return a tiny COM object instead; actual shader
// execution is intercepted by the native Vulkan renderer hooks.
struct CompatShaderObject { void** vtbl; };
static CompatShaderObject gCompatShader{};
static void* gCompatShaderVtable[8]{};
static int32_t compatShaderQI(void* self,const void*,void** out){
  if(!out)return (int32_t)0x80004003u; *out=self; return 0;
}
static uint32_t compatShaderAddRef(void*){return 2;}
static uint32_t compatShaderRelease(void*){return 1;}
static int32_t compatSetPrivateData(void*, const void*, uint32_t, const void*);
struct CompatDeviceChildObject { void** vtbl; };
static CompatDeviceChildObject gCompatDeviceChild{};
static void* gCompatDeviceChildVtable[8]{};
static int32_t compatDeviceChildQI(void* self,const void*,void** out){ if(!out)return (int32_t)0x80004003u; *out=self; return 0; }
static uint32_t compatDeviceChildAddRef(void*){return 2;}
static uint32_t compatDeviceChildRelease(void*){return 1;}
static int32_t compatCreateInputLayout(void*,const void*,size_t,const void*,uint32_t,void** out){
  gtavdiag::checkpoint("compat-d3d11-create-input-layout");
  if(!out)return (int32_t)0x80004003u;
  gCompatDeviceChildVtable[0]=(void*)compatDeviceChildQI;
  gCompatDeviceChildVtable[1]=(void*)compatDeviceChildAddRef;
  gCompatDeviceChildVtable[2]=(void*)compatDeviceChildRelease;
  gCompatDeviceChildVtable[5]=(void*)compatSetPrivateData;
  gCompatDeviceChild.vtbl=gCompatDeviceChildVtable;
  *out=&gCompatDeviceChild;
  return 0;
}
static int32_t compatCreateShader(void*, const void*, size_t, void*, void** out){
  gtavdiag::checkpoint("compat-d3d11-create-shader");
  if(!out)return (int32_t)0x80004003u;
  gCompatShaderVtable[0]=(void*)compatShaderQI;
  gCompatShaderVtable[1]=(void*)compatShaderAddRef;
  gCompatShaderVtable[2]=(void*)compatShaderRelease;
  // ID3D11DeviceChild: GetDevice=3, GetPrivateData=4, SetPrivateData=5,
  // SetPrivateDataInterface=6. PIX labels use slot 5 / +0x28.
  gCompatShaderVtable[5]=(void*)compatSetPrivateData;
  gCompatShader.vtbl=gCompatShaderVtable;
  *out=&gCompatShader;
  return 0;
}


// Minimal D3D11 resource/view shells used only for the engine bootstrap ABI.
// They keep valid COM objects and descriptors alive while the actual draw path is
// migrated to Vulkan. Returning E_NOTIMPL with a null out pointer here is unsafe:
// GTA consumes the created RTV/DSV/SRV objects immediately.
struct CompatResourceObject { void** vtbl; size_t descSize; uint8_t desc[64]; std::vector<uint8_t> backing; };
struct CompatViewObject { void** vtbl; CompatResourceObject* resource; size_t descSize; uint8_t desc[32]; };
static void* gCompatBufferVtable[16]{};
static void* gCompatTexture1DVtable[16]{};
static void* gCompatTexture2DVtable[16]{};
static void* gCompatTexture3DVtable[16]{};
static void* gCompatViewVtable[16]{};
static std::mutex gCompatObjectMutex;
static std::vector<CompatResourceObject*> gCompatResources;
static std::vector<CompatViewObject*> gCompatViews;
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
  for(void** t:tables){for(int i=0;i<16;i++)t[i]=(void*)compatD3DUnsupported;t[0]=(void*)compatChildQI;t[1]=(void*)compatChildAddRef;t[2]=(void*)compatChildRelease;t[3]=(void*)compatChildGetDevice;t[4]=(void*)compatChildGetPrivateData;t[5]=(void*)compatSetPrivateData;t[6]=(void*)compatChildSetPrivateDataInterface;t[7]=(void*)compatResourceGetType;t[8]=(void*)compatResourceSetEvictionPriority;t[9]=(void*)compatResourceGetEvictionPriority;}
  // ID3D11Buffer::GetDesc slot 10; Texture1D/2D/3D GetDesc slots 10/10/10.
  gCompatBufferVtable[10]=(void*)compatResourceGetDesc;
  gCompatTexture1DVtable[10]=(void*)compatResourceGetDesc;
  gCompatTexture2DVtable[10]=(void*)compatResourceGetDesc;
  gCompatTexture3DVtable[10]=(void*)compatResourceGetDesc;
  for(void*& p:gCompatViewVtable)p=(void*)compatD3DUnsupported;
  gCompatViewVtable[0]=(void*)compatChildQI; gCompatViewVtable[1]=(void*)compatChildAddRef; gCompatViewVtable[2]=(void*)compatChildRelease;
  gCompatViewVtable[3]=(void*)compatChildGetDevice; gCompatViewVtable[4]=(void*)compatChildGetPrivateData; gCompatViewVtable[5]=(void*)compatSetPrivateData; gCompatViewVtable[6]=(void*)compatChildSetPrivateDataInterface; gCompatViewVtable[7]=(void*)compatViewGetResource; gCompatViewVtable[8]=(void*)compatViewGetDesc;
}
static CompatResourceObject* makeCompatResource(const void* desc,size_t bytes,const char* checkpoint,void** vtbl){
  gtavdiag::checkpoint(checkpoint);initCompatResourceVtables();
  auto* o=new CompatResourceObject{};o->vtbl=vtbl;o->descSize=std::min(bytes,sizeof(o->desc));
  if(desc)std::memcpy(o->desc,desc,std::min(bytes,sizeof(o->desc)));
  size_t storage=0; if(desc){const uint32_t* d=(const uint32_t*)desc; if(vtbl==gCompatBufferVtable)storage=d[0]; else if(vtbl==gCompatTexture1DVtable)storage=(size_t)d[0]*4u; else if(vtbl==gCompatTexture2DVtable)storage=(size_t)d[0]*std::max(1u,d[1])*4u; else if(vtbl==gCompatTexture3DVtable)storage=(size_t)d[0]*std::max(1u,d[1])*std::max(1u,d[2])*4u;} if(storage)o->backing.resize(std::min<size_t>(storage,256u*1024u*1024u));
  std::lock_guard<std::mutex> l(gCompatObjectMutex);gCompatResources.push_back(o);return o;
}
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
    case 70:case 71:case 72:case 79:case 80: bw=bh=4; bytes=8; break;
    case 73:case 74:case 75:case 76:case 77:case 78:case 81:case 82:case 83:case 84:case 94:case 95:case 96:case 97:case 98:case 99: bw=bh=4; bytes=16; break;
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
  auto* o=(CompatResourceObject*)resource;
  if(o->vtbl!=gCompatBufferVtable&&o->vtbl!=gCompatTexture1DVtable&&o->vtbl!=gCompatTexture2DVtable&&o->vtbl!=gCompatTexture3DVtable)return false;
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
static int32_t compatCreateBuffer(void*,const void* desc,const void*,void** out){
  if(!out)return (int32_t)0x80004003u;*out=makeCompatResource(desc,24,"compat-d3d11-create-buffer",gCompatBufferVtable);return 0;
}
static int32_t compatCreateTexture1D(void*,const void* desc,const void*,void** out){
  if(!out)return (int32_t)0x80004003u;*out=makeCompatResource(desc,32,"compat-d3d11-create-texture1d",gCompatTexture1DVtable);return 0;
}
static int32_t compatCreateTexture2D(void*,const void* desc,const void*,void** out){
  if(!out)return (int32_t)0x80004003u;*out=makeCompatResource(desc,44,"compat-d3d11-create-texture2d",gCompatTexture2DVtable);return 0;
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
  for(void*& p:gCompatStateVtable)p=(void*)compatD3DUnsupported;
  for(void*& p:gCompatQueryVtable)p=(void*)compatD3DUnsupported;
  for(void** t:{gCompatStateVtable,gCompatQueryVtable}){t[0]=(void*)compatChildQI;t[1]=(void*)compatChildAddRef;t[2]=(void*)compatChildRelease;t[5]=(void*)compatSetPrivateData;}
  gCompatStateVtable[7]=(void*)compatStateGetDesc;
  gCompatQueryVtable[7]=(void*)compatQueryGetDataSize;
  gCompatQueryVtable[8]=(void*)compatQueryGetDesc;
}
static int32_t makeCompatState(const void* desc,size_t bytes,void** out,const char* cp){
  gtavdiag::checkpoint(cp); if(!out)return (int32_t)0x80004003u; initCompatStateVtables();
  auto* o=new CompatStateObject{};o->vtbl=gCompatStateVtable;o->descSize=std::min(bytes,sizeof(o->desc));if(desc)std::memcpy(o->desc,desc,o->descSize);
  {std::lock_guard<std::mutex> l(gCompatObjectMutex);gCompatStates.push_back(o);}*out=o;return 0;
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
static int32_t compatCheckFormatSupport(void*,uint32_t,uint32_t* out){
  gtavdiag::checkpoint("compat-d3d11-check-format-support");if(!out)return (int32_t)0x80004003u;*out=0xffffffffu;return 0;
}
static int32_t compatCheckMSAA(void*,uint32_t,uint32_t samples,uint32_t* out){
  gtavdiag::checkpoint("compat-d3d11-check-msaa");if(!out)return (int32_t)0x80004003u;*out=samples?1u:0u;return 0;
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
static int32_t compatDXGIDeviceGetParent(void*, const void*, void** out) {
  gtavdiag::checkpoint("compat-dxgi-device-get-parent");
  if(!out) return (int32_t)0x80004003u;
  // RetrieveVideoMemory is the only bootstrap consumer that genuinely needs
  // the adapter parent. SuppressAltEnter later asks for a different DXGI
  // parent/interface chain that is Windows-only. Distinguish the calls by
  // sequence: the first parent query supplies the adapter; later parent
  // probes fail cleanly so the engine takes its fallback instead of invoking
  // an unimplemented adapter vtable slot.
  static uint32_t parentCalls=0;
  ++parentCalls;
  if(parentCalls > 1) {
    *out=nullptr;
    gtavdiag::checkpoint("compat-dxgi-device-get-parent-fallback");
    return (int32_t)0x80004002u;
  }
  initCompatDXGI();
  *out=&gCompatAdapter;
  return 0;
}
static int32_t compatDXGIDeviceGetAdapter(void*, void** out) {
  gtavdiag::checkpoint("compat-dxgi-device-get-adapter");
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
  gD3DContextVtable[7]=(void*)compatContextNoop;
  gD3DContextVtable[8]=(void*)compatContextNoop;
  gD3DContextVtable[9]=(void*)compatContextNoop;
  gD3DContextVtable[10]=(void*)compatContextNoop;
  gD3DContextVtable[11]=(void*)compatContextNoop;
  gD3DContextVtable[12]=(void*)compatContextNoop;
  gD3DContextVtable[13]=(void*)compatContextNoop;
  gD3DContextVtable[14]=(void*)compatContextSlot14;
  gD3DContextVtable[15]=(void*)compatContextSlot15;
  gD3DContextVtable[16]=(void*)compatContextNoop;
  gD3DContextVtable[17]=(void*)compatContextNoop;
  gD3DContextVtable[18]=(void*)compatContextNoop;
  gD3DContextVtable[19]=(void*)compatContextNoop;
  gD3DContextVtable[20]=(void*)compatContextNoop;
  gD3DContextVtable[21]=(void*)compatContextNoop;
  gD3DContextVtable[22]=(void*)compatContextNoop;
  gD3DContextVtable[23]=(void*)compatContextNoop;
  gD3DContextVtable[24]=(void*)compatContextNoop;
  gD3DContextVtable[25]=(void*)compatContextNoop;
  gD3DContextVtable[26]=(void*)compatContextNoop;
  gD3DContextVtable[27]=(void*)compatContextBegin;
  gD3DContextVtable[28]=(void*)compatContextEnd;
  gD3DContextVtable[29]=(void*)compatContextGetData;
  gD3DContextVtable[30]=(void*)compatContextNoop;
  gD3DContextVtable[31]=(void*)compatContextNoop;
  gD3DContextVtable[32]=(void*)compatContextNoop;
  gD3DContextVtable[33]=(void*)compatContextNoop;
  gD3DContextVtable[34]=(void*)compatContextNoop;
  gD3DContextVtable[35]=(void*)compatContextNoop;
  gD3DContextVtable[36]=(void*)compatContextNoop;
  gD3DContextVtable[37]=(void*)compatContextNoop;
  gD3DContextVtable[38]=(void*)compatContextNoop;
  gD3DContextVtable[39]=(void*)compatContextNoop;
  gD3DContextVtable[40]=(void*)compatContextNoop;
  gD3DContextVtable[41]=(void*)compatContextNoop;
  gD3DContextVtable[42]=(void*)compatContextNoop;
  gD3DContextVtable[43]=(void*)compatContextNoop;
  gD3DContextVtable[44]=(void*)compatContextNoop;
  gD3DContextVtable[45]=(void*)compatContextNoop;
  gD3DContextVtable[46]=(void*)compatContextNoop;
  gD3DContextVtable[47]=(void*)compatContextNoop;
  gD3DContextVtable[48]=(void*)compatContextNoop;
  gD3DContextVtable[49]=(void*)compatContextNoop;
  gD3DContextVtable[50]=(void*)compatContextNoop;
  gD3DContextVtable[51]=(void*)compatContextNoop;
  gD3DContextVtable[52]=(void*)compatContextNoop;
  gD3DContextVtable[53]=(void*)compatContextNoop;
  gD3DContextVtable[54]=(void*)compatContextNoop;
  gD3DContextVtable[55]=(void*)compatContextNoop;
  gD3DContextVtable[56]=(void*)compatContextNoop;
  gD3DContextVtable[57]=(void*)compatContextNoop;
  gD3DContextVtable[58]=(void*)compatContextNoop;
  gD3DContextVtable[59]=(void*)compatContextNoop;
  gD3DContextVtable[60]=(void*)compatContextNoop;
  gD3DContextVtable[61]=(void*)compatContextNoop;
  gD3DContextVtable[62]=(void*)compatContextNoop;
  gD3DContextVtable[63]=(void*)compatContextNoop;
  for(int i=64;i<128;i++) gD3DContextVtable[i]=(void*)compatContextNoop;

  // ID3D11DeviceContext: Map=14, Unmap=15.
  gD3DContextVtable[14]=(void*)compatD3DMap;
  gD3DContextVtable[15]=(void*)compatD3DUnmap;
  gD3DDeviceVtable[0]=(void*)compatD3DQueryInterface;
  gD3DDeviceVtable[1]=(void*)compatD3DAddRef;
  gD3DDeviceVtable[2]=(void*)compatD3DRelease;
  // RetrieveVideoMemory exact trace:
  // device QI -> returned interface slot 6/+0x30 GetParent -> adapter slot 8/+0x40 GetDesc.
  for(void*& p:gDXGIDeviceVtable) p=(void*)compatD3DUnsupported;
  gDXGIDeviceVtable[0]=(void*)compatD3DQueryInterface;
  gDXGIDeviceVtable[1]=(void*)compatD3DAddRef;
  gDXGIDeviceVtable[2]=(void*)compatD3DRelease;
  gDXGIDeviceVtable[3]=(void*)compatDXGIGetParentUnsupported;
  gDXGIDeviceVtable[4]=(void*)compatDXGIGetParentUnsupported;
  gDXGIDeviceVtable[5]=(void*)compatDXGIGetParentUnsupported;
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
static int32_t compatSwapPresent(void*,uint32_t,uint32_t) {
  static std::atomic<uint32_t> presents{0};
  uint32_t n=presents.fetch_add(1,std::memory_order_relaxed)+1;
  if(n<=8 || (n%120)==0) gtavdiag::checkpoint("compat-swapchain-present");
  // The native renderer records into GTA's Vulkan runtime. Advance its frame
  // epoch here instead of returning E_NOTIMPL every frame.
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
static int32_t compatSwapResizeBuffers(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){
  gtavdiag::checkpoint("compat-swapchain-resize-buffers"); return 0;
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
  auto* p=(uint8_t*)desc; *(uint32_t*)(p+0)=1920; *(uint32_t*)(p+4)=1080;
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
  *(uint32_t*)(p+0)=1920; *(uint32_t*)(p+4)=1080;
  *(uint32_t*)(p+8)=1; *(uint32_t*)(p+12)=1;
  *(uint32_t*)(p+16)=28; // DXGI_FORMAT_R8G8B8A8_UNORM
  *(uint32_t*)(p+20)=1; // sample count
  *(uint32_t*)(p+28)=0; // D3D11_USAGE_DEFAULT
  *(uint32_t*)(p+32)=0x28; // RENDER_TARGET | SHADER_RESOURCE
}
static void initCompatBackBuffer(){
  static bool once=false;if(once)return;once=true;
  for(void*& p:gBackBufferVtable)p=(void*)compatD3DUnsupported;
  gBackBufferVtable[0]=(void*)compatBackBufferQI;
  gBackBufferVtable[1]=(void*)compatBackBufferAddRef;
  gBackBufferVtable[2]=(void*)compatBackBufferRelease;
  gBackBufferVtable[5]=(void*)compatBackBufferSetPrivateData;
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
 gtavdiag::checkpoint("attach-attempt");
 const uint32_t attempt=attachAttempts.fetch_add(1,std::memory_order_relaxed)+1;
 if(!gtavBase) dl_iterate_phdr(findGtav,nullptr);
 if(!gtavBase){ if(attempt<=8) __android_log_print(ANDROID_LOG_WARN,"GTAV-NATIVE","ATTACH wait: libgtav base unavailable attempt=%u",attempt); return false; }
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
