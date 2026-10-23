#include "np/touch_diag.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#if defined(__linux__)
#include "../vita/src/plat_realclock.h"
#endif
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#endif
#if defined(__APPLE__)
extern "C" uint64_t np_time_real_ns(void) __attribute__((weak_import));
#else
extern "C" uint64_t np_time_real_ns(void) __attribute__((weak));
#endif

#ifdef NP_TITLE_ID
#define NP_DIAG_TITLE NP_TITLE_ID
#else
#define NP_DIAG_TITLE "MSCN00003"
#endif
namespace {
constexpr uint32_t kCapacity=16384, kBanks=4;
struct Event { uint64_t ns,id,a,b,c,d,e; uint32_t kind,flags; };
static_assert(sizeof(Event)==64,"Trace schema");
static_assert(__atomic_always_lock_free(4,nullptr),"ARM lock-free atomics required");
struct Bank {
    alignas(64) std::atomic<uint32_t> state{0};
    alignas(64) std::atomic<uint32_t> writers{0};
    alignas(64) std::atomic<uint32_t> next{0};
    std::atomic<uint32_t> overflow{0}, raced{0};
    uint32_t mode=0, number=0, clocks[4]={};
    uint64_t start=0, stop=0;
    Event events[kCapacity];
    std::atomic<uint32_t> committed[kCapacity];
};
// A retired bank is never reopened. A delayed producer can only drop its event.
// (23 Oct: the 4.4 MiB of banks are allocated on first use, i.e. only when MSC_TOUCH_DIAG=1 runs a recorder; before, every
//  build reserved them as static memory although the recorder is off by default.)
struct BankStore { Bank b[kBanks]; };
BankStore* Store(){static BankStore* s=new BankStore();return s;}
#define banks (Store()->b)
std::atomic<Bank*> active{nullptr};
std::atomic<uint32_t> frame{0}, step{0}, poll{0}, usedPoll{0}, sendTag{0}, mega{0};
std::atomic<uint32_t> outside{0};
struct Thread { std::atomic<uint32_t> tid{0},role{0}; } threads[32];
std::atomic<uint32_t> threadCount{0};
Event history[120];
unsigned historyNext=0,historyCount=0,used[3]={};
uint32_t lastMode=0,stadium=~0u,simFrame=0,game=0;
float simTime=0,timeScale=0;
uint64_t session=0,lastContext=0,lastMega=0,lastSample=0,taskStart[256]={};
uint64_t taskTag[256]={};
uint32_t clockCost=0,statusSerial=0,statusState=~0u;
bool initialized=false,clockFailed=false;
int directoryResult=0;
struct Profile { unsigned char value,coerced; };
const Profile& Detail() {
    static const Profile p=[] {
        const char* v=getenv("MSC_PERF_DETAIL");
        return Profile{(unsigned char)(v&&!strcmp(v,"summary")?2:1),
            (unsigned char)(v&&strcmp(v,"summary")&&strcmp(v,"full")?1:0)};
    }();
    return p;
}
struct Phase {
    uint32_t local=0,state=0,frame=0,step=0;
    bool known=false,mixed=false;
    uint64_t contextNs=0;
} phases[2]; // Main-thread state only; render/display threads record immutable frame tags.
struct PhaseCounts { uint32_t calls=0,local=0,local5=0,other5=0; } phaseCounts[2];
uint32_t phaseRevision=0,statusPhaseRevision=0,invalidPhase=0,carriedPhase=0;
uint32_t lastCaptureMode=0,pendingMode=0,lastScene=~0u,lastStadium=~0u,lastGame=0;
uint64_t tailStart=0;
bool KeepSummary(uint32_t kind) {
    switch(kind) {
    case NPD_POLL: case NPD_CONTACT: case NPD_PANEL: case NPD_SAMPLE: case NPD_GESTURE:
    case NPD_POINTER_BEFORE: case NPD_POINTER_AFTER: case NPD_CTRL: case NPD_KEEPER:
    case NPD_TASK: case NPD_COMMAND: return false;
    default: return true;
    }
}

void Clocks(uint32_t* p) {
#if defined(__vita__)
    p[0]=scePowerGetArmClockFrequency();p[1]=scePowerGetGpuClockFrequency();
    p[2]=scePowerGetBusClockFrequency();p[3]=scePowerGetGpuXbarClockFrequency();
#else
    memset(p,0,16);
#endif
}
const char* Directory() {
#if !defined(__vita__)
    const char* p=getenv("MSC_DIAG_DIR");if(p&&*p)return p;
#endif
    return "ux0:data/mscharged";
}
uint32_t Crc(uint32_t crc,const void* bytes,size_t size) {
    static uint32_t table[256];static bool ready=false;   // (built once, not on every call)
    if(!ready){for(unsigned i=0;i<256;i++){uint32_t v=i;for(unsigned j=0;j<8;j++)v=(v>>1)^((v&1)?0xedb88320u:0);table[i]=v;}ready=true;}
    const auto* p=static_cast<const unsigned char*>(bytes);
    for(size_t i=0;i<size;i++)crc=(crc>>8)^table[(crc^p[i])&255];
    return crc;
}
void U32(unsigned char* h,unsigned off,uint32_t v){memcpy(h+off,&v,4);}
void U64(unsigned char* h,unsigned off,uint64_t v){memcpy(h+off,&v,8);}
void Close(Bank* b,uint32_t reason,uint64_t now) {
    if(!b)return;
    uint32_t collecting=1;b->state.compare_exchange_strong(collecting,reason,std::memory_order_seq_cst);
    b->stop=now;
    active.store(nullptr,std::memory_order_seq_cst);
}
bool Persist(Bank& b) {
    const uint32_t s=b.state.load(std::memory_order_seq_cst);
    if((s<2||s>4)&&s!=8)return false;
    // No waiting and no concurrent reads of uncommitted slots. Retry at a later safe frame.
    if(b.writers.load(std::memory_order_seq_cst)!=0)return false;
    uint32_t count=b.next.load();if(count>kCapacity)count=kCapacity;
    for(uint32_t i=0;i<count;i++)if(b.committed[i].load(std::memory_order_acquire)!=i+1){b.state=6;return true;}
    unsigned char h[128]={};memcpy(h,"MSCTD02",7);
    U32(h,8,2);U32(h,12,sizeof(Event));U32(h,16,count);U32(h,20,kCapacity);
    U32(h,24,b.next);U32(h,28,b.overflow);U32(h,32,0);U32(h,36,s);
    U64(h,40,b.start);U64(h,48,b.stop);
    memcpy(h+56,b.clocks,16);uint32_t clocks[4];Clocks(clocks);memcpy(h+72,clocks,16);
    U32(h,88,1);U32(h,92,b.mode);U32(h,96,b.number);U32(h,100,b.raced);U32(h,104,clockCost);
    U32(h,108,0x01020304);{const char* title=NP_DIAG_TITLE;memcpy(h+112,title,strlen(title)<9?strlen(title):9);}U32(h,124,sizeof banks);
    h[121]=Detail().value;h[122]=Detail().coerced;
    char file[256],tmp[264];
    snprintf(file,sizeof file,"%s/perf-%llu-%u-%u.bin",Directory(),(unsigned long long)session,b.number,b.mode);
    snprintf(tmp,sizeof tmp,"%s.tmp",file);
    struct stat st;const int fd=stat(file,&st)==0?-1:open(tmp,O_WRONLY|O_CREAT|O_EXCL,0666);
    FILE* f=fd<0?nullptr:fdopen(fd,"wb");if(fd>=0&&!f)close(fd);
    bool ok=f!=nullptr;
    if(f){
        uint32_t crc=Crc(0xffffffffu,h,sizeof h);crc=Crc(crc,b.events,count*sizeof(Event))^0xffffffffu;
        ok=fwrite(h,1,sizeof h,f)==sizeof h;
        if(ok)ok=fwrite(b.events,sizeof(Event),count,f)==count;
        if(ok)ok=fwrite(&crc,1,4,f)==4;
        if(fclose(f)!=0)ok=false;
        if(ok)ok=rename(tmp,file)==0;
    }
    b.state.store(ok?5:6,std::memory_order_seq_cst);return true;
}
void Status() {
    uint32_t bits=clockFailed?0x10000000u:0;
    for(unsigned i=0;i<kBanks;i++)bits|=banks[i].state.load()<< (i*4);
    if(bits==statusState&&phaseRevision==statusPhaseRevision)return;
    statusState=bits;
    statusPhaseRevision=phaseRevision;
    char p[256];snprintf(p,sizeof p,"%s/perf-status-%llu-%u.txt",Directory(),(unsigned long long)session,statusSerial++);
    const int fd=open(p,O_WRONLY|O_CREAT|O_EXCL,0666);if(fd<0)return;
    FILE* f=fdopen(fd,"wb");if(!f){close(fd);return;}
    fprintf(f,"build=perf-active-phase-20261022\nversion=2\ntitle=MSCN00003\nenabled=1\nclock_failed=%u\nrecorder_bytes=%lu\nclock_cost_ns=%u\nordinary_remaining=%u\nmega_remaining=%u\noutside_events=%u\n",
        clockFailed,(unsigned long)sizeof banks,clockCost,2-used[1],2-used[2],outside.load());
    fprintf(f,"directory_errno=%d\n",directoryResult);
    fprintf(f,"detail=%s detail_coerced=%u\n",Detail().value==2?"summary":"full",Detail().coerced);
    for(unsigned side=0;side<2;++side){const PhaseCounts& c=phaseCounts[side];
        fprintf(f,"side%u_calls=%u local=%u local5=%u other5=%u\n",side,c.calls,c.local,c.local5,c.other5);}
    fprintf(f,"phase_invalidated=%u phase_carried=%u\n",invalidPhase,carriedPhase);
    for(unsigned i=0;i<kBanks;i++)fprintf(f,"bank%u_state=%u mode=%u events=%u overflow=%u raced=%u\n",i,banks[i].state.load(),banks[i].mode,banks[i].next.load(),banks[i].overflow.load(),banks[i].raced.load());
    fprintf(f,"states=0:unused,1:recording,2:capacity,3:time,4:boundary,5:saved,6:write_error,8:tail_deadline\n");
    fclose(f);
}
void SampleSystem(uint64_t now) {
#if defined(__vita__)
    SceKernelFreeMemorySizeInfo mem={};mem.size=sizeof mem;
    const int rc=sceKernelGetFreeMemorySize(&mem);
    NpDiagEvent(NPD_MEMORY,(uint32_t)rc,NpDiagFrame(),mem.size_user,mem.size_cdram,mem.size_phycont,sizeof banks,0);
    uint32_t count=threadCount.load();if(count>32)count=32;
    for(unsigned i=0;i<count;i++){
        const uint32_t tid=threads[i].tid.load(std::memory_order_acquire);if(!tid)continue;
        SceKernelThreadInfo info={};info.size=sizeof info;
        const int r=sceKernelGetThreadInfo(tid,&info);
        NpDiagEvent(NPD_THREAD,threads[i].role,tid,NpDiagInts(r,info.currentPriority),
            NpDiagInts(info.currentCpuAffinityMask,info.currentCpuId),info.runClocks,
            NpDiagInts(info.intrPreemptCount,info.threadPreemptCount),NpDiagInts(info.waitType,info.status));
    }
#endif
    NpDiagEvent(NPD_THREAD_SAMPLE_COST,0,NpDiagFrame(),now,NpDiagNow(),0,0,0);
}
}
extern "C" int NpDiagEnabled(){static const bool on=[]{const char* p=getenv("MSC_TOUCH_DIAG");return p&&!strcmp(p,"1");}();return on;}
extern "C" int NpDiagActive(){Bank* b=active.load(std::memory_order_acquire);return b&&b->state.load(std::memory_order_relaxed)==1;}
extern "C" uint64_t NpDiagNow(){
#if defined(__linux__)
    return PlatRealMonotonicUs()*1000ull;
#else
    return np_time_real_ns?np_time_real_ns():0;
#endif
}
extern "C" void NpDiagEvent(uint32_t kind,uint32_t flags,uint64_t id,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e){
    Bank* bank=active.load(std::memory_order_acquire);
    if(!bank)return;
    if(Detail().value==2&&!KeepSummary(kind))return;
    const uint64_t ns=NpDiagNow();
    bank->writers.fetch_add(1,std::memory_order_seq_cst);
    if(bank->state.load(std::memory_order_seq_cst)!=1){bank->raced.fetch_add(1);bank->writers.fetch_sub(1,std::memory_order_seq_cst);return;}
    const uint32_t i=bank->next.fetch_add(1,std::memory_order_relaxed);
    if(i<kCapacity){bank->events[i]={ns,id,a,b,c,d,e,kind,flags};bank->committed[i].store(i+1,std::memory_order_release);}
    else {bank->overflow.fetch_add(1);uint32_t s=1;bank->state.compare_exchange_strong(s,2,std::memory_order_seq_cst);}
    bank->writers.fetch_sub(1,std::memory_order_seq_cst);
}
extern "C" void NpDiagIdentity(uint32_t st,uint32_t sf,float tm,float scale,uint32_t g){stadium=st;simFrame=sf;simTime=tm;timeScale=scale;game=g;}
extern "C" void NpDiagContext(uint32_t mode,uint32_t scene,uint32_t task,uint32_t paused,uint32_t gs){
    if(!NpDiagEnabled())return;
    const bool summary=Detail().value==2;
    const uint64_t now=NpDiagNow();
    if(!initialized){initialized=true;session=now/1000;clockFailed=now==0;
        directoryResult=mkdir(Directory(),0777)==0||errno==EEXIST?0:errno;
        if(!clockFailed){const uint64_t begin=NpDiagNow();for(int i=0;i<128;i++)NpDiagNow();clockCost=(uint32_t)((NpDiagNow()-begin)/129);}
        Status();}
    if(clockFailed){if(mode==0)Status();return;}
    const bool keeper=mega.exchange(0)!=0;
    for(Phase& p:phases) {
        if(p.known&&(p.step!=(uint32_t)NpDiagStepId()||timeScale<=0||
            (p.frame!=(uint32_t)NpDiagFrame()&&now-p.contextNs>100000000ull))) {
            p=Phase{};++invalidPhase;++phaseRevision;
        }
    }
    if(mode && keeper)mode=2;
    if(summary&&mode==1)for(const Phase& p:phases)if(p.known&&p.state<=9)mode=2;
    if(mode==2)lastMega=now;
    Bank* b=active.load(std::memory_order_acquire);
    const bool safe=mode==0&&(paused||task==1||task==4);
    const bool suspend=lastContext&&now-lastContext>10000000000ull;
    if(mode!=2||paused||suspend||(lastContext&&(scene!=lastScene||stadium!=lastStadium||game!=lastGame))) {
        for(Phase& p:phases)p=Phase{};
    }
    bool catching=false;
    for(Phase& p:phases) {
        if(p.known&&p.local&&p.state==5)catching=true;
    }
    const uint32_t captureMode=summary&&mode==2&&!catching?0:mode;
    if(summary&&captureMode!=lastCaptureMode)pendingMode=captureMode==1||captureMode==2?captureMode:0;
    if(summary&&b&&b->mode==captureMode)pendingMode=0;
    if(summary&&b&&b->mode==2) {
        if(catching)tailStart=0;
        else if(!tailStart)tailStart=now;
    }
    const bool tailExpired=summary&&tailStart&&now-tailStart>=1000000000ull;
    if(b&&(b->state.load()!=1||safe||suspend||now-b->start> (b->mode==1?30000000000ull:60000000000ull)||
        (b->mode==1&&mode==2)||tailExpired||(!summary&&b->mode==2&&mode==1&&now-lastMega>5000000000ull))){
        Close(b,safe||suspend?4:tailExpired?8:now-b->start>(b->mode==1?30000000000ull:60000000000ull)?3:4,now);b=nullptr;tailStart=0;
    }
    const bool entering=summary?(captureMode!=lastCaptureMode||pendingMode==captureMode):mode!=lastMode;
    if(!b&&!suspend&&(captureMode==1||captureMode==2)&&entering&&used[captureMode]<2){
        const unsigned index=(mode-1)*2+used[mode]++;
        b=&banks[index];b->mode=mode;b->number=index;b->start=now;Clocks(b->clocks);
        // Pre-roll contains context only; flag bit 31 excludes it from complete pipeline samples.
        if(mode==2)for(unsigned i=0;i<historyCount;i++){
            Event e=history[(historyNext+120-historyCount+i)%120];e.flags|=0x80000000u;
            b->events[i]=e;b->committed[i].store(i+1,std::memory_order_release);
        }
        if(mode==2)b->next=historyCount;
        b->state.store(1,std::memory_order_seq_cst);active.store(b,std::memory_order_release);
        pendingMode=0;
        NpDiagEvent(NPD_START,mode,NpDiagFrame(),stadium,game,simFrame,sizeof banks,clockCost);
        lastSample=0;
    }
    Event context={now,NpDiagFrame(),NpDiagInts(scene,task),NpDiagInts(gs,paused),NpDiagInts(stadium,game),NpDiagInts(simFrame,(uint32_t)NpDiagStepId()),NpDiagFloats(simTime,timeScale),NPD_CONTEXT,mode};
    history[historyNext]=context;historyNext=(historyNext+1)%120;if(historyCount<120)++historyCount;
    for(Phase& p:phases)if(p.known){
        if(p.frame==(uint32_t)NpDiagFrame())p.contextNs=now;
        else {++carriedPhase;++phaseRevision;}
    }
    if(b){NpDiagEvent(context.kind,context.flags,context.id,context.a,context.b,context.c,context.d,context.e);
        for(unsigned side=0;side<2;++side) {
            const Phase& p=phases[side];
            if(p.known)NpDiagEvent(NPD_MEGA_PHASE,(p.frame==(uint32_t)NpDiagFrame()?(p.mixed?1u:0u):2u),NpDiagFrame(),side,p.local,p.state,p.step,0);
        }
        if(now-lastSample>=1000000000ull){lastSample=now;SampleSystem(now);}}
    if(safe){for(Bank& bank:banks)if(Persist(bank))break;Status();}
    lastMode=mode;lastCaptureMode=captureMode;lastContext=now;lastScene=scene;lastStadium=stadium;lastGame=game;
}
extern "C" void NpDiagMega(){mega.store(1,std::memory_order_relaxed);}
extern "C" void NpDiagMegaPhase(uint32_t side,uint32_t local,uint32_t state) {
    if(!NpDiagEnabled()||side>=2)return;
    PhaseCounts& counts=phaseCounts[side];++counts.calls;++phaseRevision;
    if(local)++counts.local;
    if(state==5){if(local)++counts.local5;else ++counts.other5;}
    Phase& p=phases[side];
    const uint32_t tag=(uint32_t)NpDiagFrame();
    const bool mixed=p.known&&p.frame==tag&&(p.mixed||p.local!=local||p.state!=state);
    p.local=local;p.state=state;p.frame=tag;p.step=(uint32_t)NpDiagStepId();
    p.known=true;p.mixed=mixed;p.contextNs=0;
}
extern "C" void NpDiagRegisterThread(uint32_t role,uint32_t tid){
    const unsigned i=threadCount.fetch_add(1);if(i>=32)return;
    threads[i].role=role;threads[i].tid.store(tid,std::memory_order_release);
}
extern "C" void NpDiagTaskBegin(uint32_t p){if(p<256&&NpDiagActive()){taskTag[p]=NpDiagFrame();taskStart[p]=NpDiagNow();}}
extern "C" void NpDiagTaskEnd(uint32_t p){if(p<256&&taskStart[p]){const uint64_t t=taskStart[p];taskStart[p]=0;NpDiagEvent(NPD_TASK,p,taskTag[p],t,NpDiagNow(),0,0,0);}}
extern "C" uint64_t NpDiagFrame(){return (uint64_t)frame.load()+1;}
extern "C" uint64_t NpDiagPollNext(){return ++poll;}
extern "C" uint64_t NpDiagPollId(){return poll;}
extern "C" void NpDiagUsePoll(uint64_t id){usedPoll=(uint32_t)id;}
extern "C" uint64_t NpDiagUsedPoll(){return usedPoll;}
extern "C" uint64_t NpDiagStep(){++step;NpDiagEvent(NPD_SIM_STEP,0,NpDiagFrame(),step,usedPoll,0,0,0);return step;}
extern "C" uint64_t NpDiagStepId(){return step;}
extern "C" uint64_t NpDiagLatch(){
    const uint32_t tag=++frame;NpDiagEvent(NPD_HANDOVER,0,tag,step,usedPoll,0,0,0);
    const uint32_t old=sendTag.exchange(tag);if(old)NpDiagEvent(NPD_FIFO_ERROR,3,tag,old,0,0,0,0);return tag;
}
extern "C" uint64_t NpDiagTake(){return sendTag.exchange(0);}
