// Included from native_renderer.cpp after the compat command-ring globals.
// Low-overhead GPU timestamp profiler for GTA V Android native Vulkan.
// Samples the first 5 frames and then every 30th frame.

static constexpr uint32_t kProfileQueryCount=2048;
static constexpr uint32_t kProfileEventCount=512;
static constexpr const char* kGpuProfilePath="/storage/emulated/0/Games/GTAV/gtav-gpu-profile.txt";

struct ProfileEvent {
  uint32_t kind{},q0{UINT32_MAX},q1{UINT32_MAX};
  uint32_t draws{},indexedDraws{},dispatches{};
  uint64_t elements{};
  uintptr_t a{},b{};
};

struct ProfileFrame {
  VkQueryPool pool{VK_NULL_HANDLE};
  uint64_t frameNo{};
  uint32_t used{},eventCount{};
  int32_t activePass{-1};
  bool sampled{};
  uint32_t qFrameStart{UINT32_MAX},qFrameEnd{UINT32_MAX};
  ProfileEvent events[kProfileEventCount]{};
};

static ProfileFrame gProfileFrames[3]{};
static bool gProfileEnabled=false;
static float gProfileTimestampPeriod=1.0f;
static uint32_t gProfileTimestampBits=64;
static std::atomic<uint64_t> gProfileFrameCounter{0};

static void profileAppend(const char* s,size_t n){
  if(!s||!n)return;
  gtavdiag::ensureDir();
  int fd=open(kGpuProfilePath,O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC,0664);
  if(fd>=0){write(fd,s,n);close(fd);}
}

static uint64_t profileDelta(uint64_t a,uint64_t b){
  if(gProfileTimestampBits>=64)return b-a;
  const uint64_t mask=(1ull<<gProfileTimestampBits)-1ull;
  return (b-a)&mask;
}

static double profileMs(uint64_t a,uint64_t b){
  return double(profileDelta(a,b))*double(gProfileTimestampPeriod)/1000000.0;
}

static bool profileInit(){
  if(gProfileEnabled)return true;
  if(!g.device||!g.physical)return false;

  VkPhysicalDeviceProperties prop{};
  vkGetPhysicalDeviceProperties(g.physical,&prop);

  uint32_t qc=0;
  vkGetPhysicalDeviceQueueFamilyProperties(g.physical,&qc,nullptr);
  if(g.family>=qc)return false;

  std::vector<VkQueueFamilyProperties> qp(qc);
  vkGetPhysicalDeviceQueueFamilyProperties(g.physical,&qc,qp.data());
  if(!qp[g.family].timestampValidBits)return false;

  gProfileTimestampPeriod=prop.limits.timestampPeriod;
  gProfileTimestampBits=qp[g.family].timestampValidBits;

  for(uint32_t i=0;i<3;i++){
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType=VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount=kProfileQueryCount;
    if(vkCreateQueryPool(g.device,&qi,nullptr,&gProfileFrames[i].pool)!=VK_SUCCESS)
      return false;
  }

  gtavdiag::ensureDir();
  int fd=open(kGpuProfilePath,O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,0664);
  if(fd>=0){
    char h[768];
    int n=snprintf(h,sizeof(h),
      "GTAV GPU profiler v1\n"
      "GPU=%s timestampPeriod=%.6fns validBits=%u\n"
      "Output: GPU timestamps from GTA native graphics queue.\n"
      "Sample cadence: first 5 frames, then every 30th frame.\n"
      "PASS = render-target interval. DISPATCH/COPY/BLIT/RESOLVE can be nested in a PASS and are not additive to pass_ms.\n"
      "rtv/dsv values are stable runtime identities useful for matching repeated expensive passes.\n\n",
      prop.deviceName,(double)gProfileTimestampPeriod,gProfileTimestampBits);
    if(n>0)write(fd,h,(size_t)n);
    close(fd);
  }

  gProfileEnabled=true;
  gtavdiag::checkpoint("gpu-profiler-ready");
  return true;
}

static uint32_t profileAllocQuery(ProfileFrame& p){
  if(!p.sampled||p.used>=kProfileQueryCount)return UINT32_MAX;
  return p.used++;
}

static void profileWrite(VkCommandBuffer cb,ProfileFrame& p,uint32_t q,VkPipelineStageFlagBits stage){
  if(cb&&p.sampled&&p.pool&&q!=UINT32_MAX)
    vkCmdWriteTimestamp(cb,stage,p.pool,q);
}

static void profileBeginFrame(uint32_t slot,VkCommandBuffer cb){
  if(slot>=3||!profileInit())return;
  auto& p=gProfileFrames[slot];
  p.frameNo=gProfileFrameCounter.fetch_add(1,std::memory_order_relaxed)+1;
  p.sampled=(p.frameNo<=5)||((p.frameNo%30)==0);
  p.used=0;
  p.eventCount=0;
  p.activePass=-1;
  p.qFrameStart=p.qFrameEnd=UINT32_MAX;

  if(!p.sampled)return;

  vkCmdResetQueryPool(cb,p.pool,0,kProfileQueryCount);
  p.qFrameStart=profileAllocQuery(p);
  profileWrite(cb,p,p.qFrameStart,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
}

static void profileClosePass(ProfileFrame& p,VkCommandBuffer cb){
  if(p.activePass<0||uint32_t(p.activePass)>=p.eventCount)return;
  auto& e=p.events[p.activePass];
  if(e.q1==UINT32_MAX){
    e.q1=profileAllocQuery(p);
    profileWrite(cb,p,e.q1,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  }
  p.activePass=-1;
}

static void profilePassSwitch(void* rtv0,void* dsv){
  if(!gCompatRecordingCB||gCompatFrameIndex>=3)return;
  auto& p=gProfileFrames[gCompatFrameIndex];
  if(!p.sampled)return;

  profileClosePass(p,gCompatRecordingCB);
  if(p.eventCount>=kProfileEventCount)return;

  auto& e=p.events[p.eventCount];
  e={};
  e.kind=PROFILE_PASS;
  e.a=(uintptr_t)rtv0;
  e.b=(uintptr_t)dsv;
  e.q0=profileAllocQuery(p);
  e.q1=UINT32_MAX;
  if(e.q0==UINT32_MAX)return;

  p.activePass=(int32_t)p.eventCount++;
  profileWrite(gCompatRecordingCB,p,e.q0,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
}

static void profileCountDraw(bool indexed,uint32_t elements){
  if(gCompatFrameIndex>=3)return;
  auto& p=gProfileFrames[gCompatFrameIndex];
  if(!p.sampled||p.activePass<0||uint32_t(p.activePass)>=p.eventCount)return;
  auto& e=p.events[p.activePass];
  if(indexed)e.indexedDraws++;
  else e.draws++;
  e.elements+=elements;
}

static uint32_t profileBeginExact(uint32_t kind,uintptr_t a=0,uintptr_t b=0){
  if(!gCompatRecordingCB||gCompatFrameIndex>=3)return UINT32_MAX;
  auto& p=gProfileFrames[gCompatFrameIndex];
  if(!p.sampled||p.eventCount>=kProfileEventCount)return UINT32_MAX;

  uint32_t idx=p.eventCount++;
  auto& e=p.events[idx];
  e={};
  e.kind=kind;
  e.a=a;
  e.b=b;
  e.q0=profileAllocQuery(p);
  e.q1=UINT32_MAX;

  if(e.q0==UINT32_MAX){
    e.kind=0;
    return UINT32_MAX;
  }

  profileWrite(gCompatRecordingCB,p,e.q0,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
  return idx;
}

static void profileEndExact(uint32_t token){
  if(token==UINT32_MAX||gCompatFrameIndex>=3)return;
  auto& p=gProfileFrames[gCompatFrameIndex];
  if(!p.sampled||token>=p.eventCount)return;
  auto& e=p.events[token];
  if(!e.kind||e.q1!=UINT32_MAX)return;

  e.q1=profileAllocQuery(p);
  profileWrite(gCompatRecordingCB,p,e.q1,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

static void profileCancelExact(uint32_t token){
  if(token==UINT32_MAX||gCompatFrameIndex>=3)return;
  auto& p=gProfileFrames[gCompatFrameIndex];
  if(token<p.eventCount)p.events[token].kind=0;
}

static void profileEndFrame(uint32_t slot,VkCommandBuffer cb){
  if(slot>=3)return;
  auto& p=gProfileFrames[slot];
  if(!p.sampled)return;

  profileClosePass(p,cb);
  p.qFrameEnd=profileAllocQuery(p);
  profileWrite(cb,p,p.qFrameEnd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

static const char* profileKindName(uint32_t k){
  switch(k){
    case PROFILE_PASS:return "PASS";
    case PROFILE_DISPATCH:return "DISPATCH";
    case PROFILE_COPY_BUFFER:return "COPY_BUFFER";
    case PROFILE_COPY_IMAGE:return "COPY_IMAGE";
    case PROFILE_BLIT:return "BLIT";
    case PROFILE_RESOLVE:return "RESOLVE";
    default:return "UNKNOWN";
  }
}

static void profileReadAndLog(uint32_t slot){
  if(slot>=3)return;
  auto& p=gProfileFrames[slot];
  if(!p.sampled||!p.pool||!p.used)return;

  std::vector<uint64_t> ts(p.used);
  VkResult r=vkGetQueryPoolResults(
    g.device,p.pool,0,p.used,
    ts.size()*sizeof(uint64_t),ts.data(),sizeof(uint64_t),
    VK_QUERY_RESULT_64_BIT);
  if(r!=VK_SUCCESS)return;

  char out[32768];
  size_t n=0;
  auto put=[&](const char* fmt,auto... args){
    if(n>=sizeof(out)-256)return;
    int k=snprintf(out+n,sizeof(out)-n,fmt,args...);
    if(k>0)n+=std::min<size_t>((size_t)k,sizeof(out)-n-1);
  };

  double frameMs=(p.qFrameStart<p.used&&p.qFrameEnd<p.used)
    ?profileMs(ts[p.qFrameStart],ts[p.qFrameEnd]):0.0;

  uint64_t draws=0,indexed=0,elements=0;
  uint32_t passes=0;
  double passMs=0,dispatchMs=0,copyMs=0,blitMs=0,resolveMs=0;

  for(uint32_t i=0;i<p.eventCount;i++){
    auto& e=p.events[i];
    if(!e.kind||e.q0>=p.used||e.q1>=p.used)continue;
    double ms=profileMs(ts[e.q0],ts[e.q1]);

    if(e.kind==PROFILE_PASS){
      passes++;
      passMs+=ms;
      draws+=e.draws;
      indexed+=e.indexedDraws;
      elements+=e.elements;
    }else if(e.kind==PROFILE_DISPATCH){
      dispatchMs+=ms;
    }else if(e.kind==PROFILE_COPY_BUFFER||e.kind==PROFILE_COPY_IMAGE){
      copyMs+=ms;
    }else if(e.kind==PROFILE_BLIT){
      blitMs+=ms;
    }else if(e.kind==PROFILE_RESOLVE){
      resolveMs+=ms;
    }
  }

  put("FRAME %llu gpu_ms=%.3f pass_ms=%.3f dispatch_ms=%.3f copy_ms=%.3f blit_ms=%.3f resolve_ms=%.3f passes=%u draws=%llu indexed=%llu elements=%llu\n",
    (unsigned long long)p.frameNo,frameMs,passMs,dispatchMs,copyMs,blitMs,resolveMs,passes,
    (unsigned long long)draws,(unsigned long long)indexed,(unsigned long long)elements);

  for(uint32_t i=0;i<p.eventCount;i++){
    auto& e=p.events[i];
    if(!e.kind||e.q0>=p.used||e.q1>=p.used)continue;
    double ms=profileMs(ts[e.q0],ts[e.q1]);

    if(ms<0.010&&e.kind!=PROFILE_PASS)continue;

    if(e.kind==PROFILE_PASS){
      put("  PASS %03u ms=%7.3f rtv=0x%llx dsv=0x%llx draws=%u indexed=%u elements=%llu\n",
        i,ms,(unsigned long long)e.a,(unsigned long long)e.b,
        e.draws,e.indexedDraws,(unsigned long long)e.elements);
    }else{
      put("  %-11s %03u ms=%7.3f a=0x%llx b=0x%llx\n",
        profileKindName(e.kind),i,ms,
        (unsigned long long)e.a,(unsigned long long)e.b);
    }
  }

  put("%s","\n");
  profileAppend(out,n);
}

static void profileDestroy(){
  if(!g.device)return;
  for(auto& p:gProfileFrames){
    if(p.pool)vkDestroyQueryPool(g.device,p.pool,nullptr);
    p.pool=VK_NULL_HANDLE;
  }
  gProfileEnabled=false;
}
