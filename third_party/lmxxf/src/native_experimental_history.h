#pragma once
#include <cstdlib>
#include <cstring>
#include <cstdio>
// 0.41-a regular pre-upscale experiment. Not a new default or a general temporal API.
inline bool NativeExperimentalTemporalRequested(){const char*v=std::getenv("DLSS5_TEMPORAL_HISTORY_EXPERIMENT");return v&&!std::strcmp(v,"1");}
inline bool NativeExperimentalTemporalCompatible(){
 auto off=[](const char*k){const char*v=std::getenv(k);return !v||!*v||!std::strcmp(v,"0");};
 const char*mp=std::getenv("DLSS5_MULTI_PASS");
 return NativeExperimentalTemporalRequested()&&std::getenv("DLSS5_TEMPORAL_MV_UNJITTERED")&&!std::strcmp(std::getenv("DLSS5_TEMPORAL_MV_UNJITTERED"),"1")&&(!mp||!*mp||!std::strcmp(mp,"1"))&&off("DLSS5_MULTI_PASS_SKIN_PROTECT")&&off("DLSS5_HIP_GRAPH")&&off("DLSS5_OVERLAP")&&off("DLSS5_VIT_ADAPTIVE")&&off("DLSS5_VIT_REUSE_HOTKEY")&&off("DLSS5_FAST_TEMPORAL")&&off("DLSS5_HISTORY_GUARD")&&off("DLSS5_OUTPUT_SMOOTH");
}
