#pragma once
#include <array>
#include <cstddef>

// Ops must outlive the lease. All Ops methods are noexcept and return HIP-like
// status codes (0 success). Handle is pointer-like and null-initializable.
// select_owner() selects the Network's device on the current host thread.
// drain() waits only the owned stream; record() records only that stream.
// log(operation,status) must be noexcept. Retained handles on cleanup failure
// intentionally remain allocated: destroying potentially live events is unsafe.
namespace hip_reference {
template<class Ops> class SubmitPulseLease {
public:
 using Handle=typename Ops::Handle;
 explicit SubmitPulseLease(Ops& ops) noexcept: ops_(ops) {}
 SubmitPulseLease(const SubmitPulseLease&)=delete;
 SubmitPulseLease& operator=(const SubmitPulseLease&)=delete;
 ~SubmitPulseLease() noexcept { Close(); }
 bool Configure(bool requested,bool eligible,unsigned count=1,unsigned flags=0) noexcept {
  if(configured_)return active_;
  configured_=true;
  if(!requested||!eligible)return false;
  if(count<1||count>2){ops_.log("invalid_count",-1);return false;}
  int status=ops_.select_owner();
  if(status){ops_.log("create_owner",status);return false;}
  status=ops_.supported(flags);
  if(status){ops_.log("unsupported",status);return false;}
  for(unsigned i=0;i<count;i++){
   status=ops_.create(&handles_[i],flags);
   if(status){ops_.log("create",status);Close();return false;}
  }
  count_=count;active_=true;return true;
 }
 // Call only after the caller has selected the owning device. No device change,
 // allocation, query or synchronization occurs in this hot path.
 void Record() noexcept {
  if(!active_)return;
  for(unsigned i=0;i<count_;i++){
   // A failed API call may have partially queued work. Conservatively require
   // drain even if the first record returned failure.
   needs_drain_=true;
   const int status=ops_.record(handles_[i]);
   if(status){ops_.log("record",status);active_=false;return;}
  }
 }
 // Safe to retry manually while Ops is alive. A destructor logs but cannot
 // transfer ownership after an owner/drain/destroy failure.
 bool Close() noexcept {
  active_=false;
  if(!HasHandles())return true;
  int status=ops_.select_owner();
  if(status){ops_.log("cleanup_owner_retained",status);return false;}
  if(needs_drain_){status=ops_.drain();if(status){ops_.log("drain_retained",status);return false;}needs_drain_=false;}
  bool released=true;
  for(auto& handle:handles_)if(handle){
   status=ops_.destroy(handle);
   if(status){ops_.log("destroy_retained",status);released=false;}
   else handle=Handle{};
  }
  return released;
 }
 bool Active()const noexcept{return active_;}
 bool HasHandles()const noexcept{for(auto h:handles_)if(h)return true;return false;}
private:
 Ops& ops_;std::array<Handle,2> handles_{};unsigned count_=0;
 bool configured_=false,active_=false,needs_drain_=false;
};
}
