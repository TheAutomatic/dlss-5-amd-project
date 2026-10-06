#pragma once
#include "submit_pulse.h"
#include <cstdio>

namespace hip_reference {
// Borrowed ABI entries; the Network owns stream and keeps the HIP DLL loaded.
// This adapter intentionally supports timed flag0 only. No API export lookup
// or owner selection occurs in Record. The bridge selects owner before enqueue.
struct HipSubmitPulseOps {
 using Handle=void*;
 int owner{};Handle* stream{};
 int(*set_device)(int){};int(*event_create)(Handle*){};
 int(*event_record)(Handle,Handle){};int(*stream_sync)(Handle){};
 int(*event_destroy)(Handle){};unsigned long long create_calls{},create_ok{},record_calls{},record_ok{},destroy_calls{},destroy_ok{},drain_calls{};
 int select_owner()noexcept{return set_device?set_device(owner):801;}
 int supported(unsigned flags)noexcept{return !flags&&event_create&&event_record&&stream_sync&&event_destroy&&stream&&*stream?0:801;}
 int create(Handle*handle,unsigned flags)noexcept{if(flags||!event_create)return 801;++create_calls;int result=event_create(handle);create_ok+=result==0;return result;}
 int record(Handle handle)noexcept{if(!event_record||!stream)return 801;++record_calls;int result=event_record(handle,*stream);record_ok+=result==0;return result;}
 int drain()noexcept{if(!stream_sync||!stream)return 801;++drain_calls;return stream_sync(*stream);}
 int destroy(Handle handle)noexcept{if(!event_destroy)return 801;++destroy_calls;int result=event_destroy(handle);destroy_ok+=result==0;return result;}
 void log(const char*operation,int status)noexcept{std::fprintf(stderr,"submit_pulse operation=%s status=%d owner=%d\n",operation,status,owner);}
};
}
