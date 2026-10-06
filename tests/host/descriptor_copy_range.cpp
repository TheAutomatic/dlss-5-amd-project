#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/resource_tracking/DescriptorCopyRange.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <shared_mutex>
#include <vector>
#include <algorithm>
struct Heap
{
    std::atomic<bool> active{true};
    size_t cpuStart, cpuEnd;
    std::vector<int> data;
    Heap(size_t base,size_t n):cpuStart(base),cpuEnd(base+4*n),data(n){}
};
struct Fixture
{
    std::vector<std::shared_ptr<Heap>> heaps;
    std::shared_mutex mutex;
    std::weak_ptr<Heap> previous;
    size_t lookups=0;
    Fixture()
    {
        for(size_t base:{100u,164u,1000u,1064u})
        { auto h=std::make_shared<Heap>(base,16); for(int i=0;i<16;i++)h->data[i]=int(base)+i; heaps.push_back(h); }
    }
    std::shared_ptr<Heap> Find(size_t handle)
    {
        ++lookups;
        auto h=previous.lock();
        if(h && h->active.load() && handle>=h->cpuStart && handle<h->cpuEnd)return h;
        std::shared_lock lock(mutex);
        for(auto& candidate:heaps)
            if(candidate->active.load() && handle>=candidate->cpuStart && handle<candidate->cpuEnd)
            {previous=candidate;return candidate;}
        previous.reset();return {};
    }
    void Run(bool cached,size_t count,size_t src,size_t dst,bool retire=false)
    {
        size_t index=0;
        auto copy=[&](const auto& source,size_t sh,const auto& dest,size_t dh) {
            if(dest && dest->active.load())dest->data[(dh-dest->cpuStart)/4]=source && source->active.load()?source->data[(sh-source->cpuStart)/4]:0;
            if(retire && index++==0)
            { auto old=heaps[0]; old->active.store(false);auto next=std::make_shared<Heap>(old->cpuStart,16);std::fill(next->data.begin(),next->data.end(),77);heaps[0]=next; }
        };
        if(cached && count != 1)DescriptorTracking::CopyRange(count,src,dst,4,[&](size_t h){return Find(h);},copy);
        else for(size_t i=0;i<count;i++) {auto sh=src?src+4*i:0;auto source=src?Find(sh):nullptr;auto dh=dst+4*i;auto dest=Find(dh);copy(source,sh,dest,dh);}
    }
};
int main()
{
    for(size_t count:{0,1,2,16,20,32})for(size_t src:{0,100,148,164,800})for(size_t dst:{100,108,1000,1048,800})
    {
        Fixture old,fast; old.Run(false,count,src,dst);fast.Run(true,count,src,dst);
        for(size_t i=0;i<old.heaps.size();i++)assert(old.heaps[i]->data==fast.heaps[i]->data);
    }
    { Fixture old,fast; old.Run(false,16,100,1000,true);fast.Run(true,16,100,1000,true);
      for(size_t i=0;i<old.heaps.size();i++)assert(old.heaps[i]->data==fast.heaps[i]->data);
      // A second call/device must never inherit a prior call's cache.
      Fixture other; other.Run(true,16,164,1064); assert(other.heaps[3]->data[0]==164); }
    volatile int sink=0;
    for(size_t count:{1,16})
    {
        for(bool cached:{false,true})
        {
            Fixture f; auto start=std::chrono::steady_clock::now();
            for(int i=0;i<200000;i++)f.Run(cached,count,100,1000);
            auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            sink=f.heaps[2]->data[0];
            std::cout<<"count="<<count<<" cached="<<cached<<" lookups="<<f.lookups<<" ms="<<elapsed<<'\n';
            assert(f.lookups==200000ull*(cached?2:count*2));
        }
    }
    (void)sink;
    std::cout<<"Descriptor range: PASS (mapping parity, boundaries, unknown/clear, overlap, retire/recreate, independent calls)\n";
}
