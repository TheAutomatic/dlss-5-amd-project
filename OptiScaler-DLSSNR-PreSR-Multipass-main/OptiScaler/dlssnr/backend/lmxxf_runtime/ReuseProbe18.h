#pragma once
#include "../../../../../third_party/lmxxf/Development/HIP/hip_api.h"
#include <array>
#include <sstream>
// Test-only nonblocking state readback. Resources are retired after GPU idle.
class ReuseProbe18 {
    struct Slot { void* host=nullptr;void* event=nullptr;bool pending=false,poisoned=false;
        uint64_t frame=0;unsigned mode=0; };
    std::array<Slot,16> slots{};
    hip_probe::Api* api=nullptr;
    int (*query)(void*)=nullptr;int (*hostFree)(void*)=nullptr;int (*eventDestroy)(void*)=nullptr;
    std::ostringstream rows;
public:
    unsigned requested=0,completed=0,failed=0;
    void Start(){rows.str("");rows.clear();rows<<"frame,mode,reuse,age,initialized,decisions,global_delta,local_delta,reason,image_delta\n";requested=completed=failed=0;}
    bool Pending()const{for(auto&s:slots)if(s.pending)return true;return false;}
    void Poll(){if(!api||!query)return;for(auto&s:slots)if(s.pending){int rc=query(s.event);if(rc==600)continue;
        if(rc){s.pending=false;s.poisoned=true;++failed;continue;}
        auto*v=static_cast<unsigned*>(s.host);float g,l,im;memcpy(&g,v+4,4);memcpy(&l,v+5,4);memcpy(&im,v+7,4);
        rows<<s.frame<<','<<s.mode<<','<<v[0]<<','<<v[1]<<','<<v[2]<<','<<v[3]<<','<<g<<','<<l<<','<<v[6]<<','<<im<<'\n';
        s.pending=false;++completed;
    }}
    void Record(hip_probe::Api&a,void*stream,const void*state,unsigned mode,uint64_t frame){
        Poll();++requested;
        if(!state){rows<<frame<<','<<mode<<",0,0,0,0,0,0,9,0\n";++completed;return;}
        if(!api){api=&a;eventDestroy=a.hipEventDestroy;query=reinterpret_cast<int(*)(void*)>(GetProcAddress(a.dll,"hipEventQuery"));hostFree=reinterpret_cast<int(*)(void*)>(GetProcAddress(a.dll,"hipHostFree"));}
        if(api!=&a||!query||!hostFree){++failed;return;}
        for(auto&s:slots)if(!s.pending&&!s.poisoned){
            if(!s.host&&a.hipHostMalloc(&s.host,32,0)){++failed;return;}
            if(!s.event&&a.hipEventCreate(&s.event)){++failed;return;}
            s.frame=frame;s.mode=mode;
            if(a.hipMemcpyAsync(s.host,state,32,2,stream)||a.hipEventRecord(s.event,stream)){s.poisoned=true;++failed;return;}
            s.pending=true;return;
        }++failed;
    }
    std::string Text(){Poll();return rows.str();}
    void ReleaseAfterGpuIdle(){Poll();if(api)for(auto&s:slots){if(s.event)eventDestroy(s.event);if(s.host&&hostFree)hostFree(s.host);s={};}api=nullptr;}
};
