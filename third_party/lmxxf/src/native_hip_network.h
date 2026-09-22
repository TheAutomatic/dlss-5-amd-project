#pragma once
#include "../Development/HIP/hip_d3d12_bridge.h"
#include "native_network_geometry.h"
#include "native_lab_paths.h"
// Experimental compile-time backend. Ordinary D3D12 codec and temporal passes stay in NativeGameFrame.
class NativeHipNetwork {
 hip_reference::D3D12Bridge bridge;ID3D12Resource*color{};ID3D12Resource*history{};
 static std::string Utf8(const std::wstring&s){int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);if(!n&&!s.empty())throw std::runtime_error("HIP path encoding");std::string r(n,'\0');if(n)WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),r.data(),n,nullptr,nullptr);return r;}
public:
 NativeHipNetwork()=default;NativeHipNetwork(const NativeHipNetwork&)=delete;
 ~NativeHipNetwork(){if(!bridge.WaitForSubmittedWork())return;if(color)color->Release();if(history)history->Release();}
 void Create(ID3D12CommandQueue*q,ID3D12Resource*rgb,const std::vector<float>&noise,const std::wstring&directory,ID3D12Resource*temporal,UINT post_shift){
  if(color||!rgb)throw std::runtime_error("HIP network initialization");auto g=NativeCurrentNetworkGeometry();hip_reference::Options o;o.width=g.processing_width;o.height=g.processing_height;o.post_shift=post_shift;o.fast_vit=true;o.wmma=o.wave=o.tiled=o.pooled=true;o.assets=Utf8(directory);if(const char*skip=std::getenv("DLSS5_SKIP_BLOCKS"))o.skip_blocks=hip_reference::ParseSkipBlocks(skip);
  const bool fast=hip_reference::FastPrefixFromEnvironment();
  if(fast)o.fast_c32=o.fused_c32=o.fused_ffn=o.fast_mh=o.fused_mh=o.mh_wave=o.fast_deep=o.fast_prefix=o.packed_weights=o.packed_c32=o.fp8_normalized=o.fp8_ffn=o.fp8_av=o.fp8_deep=o.fp8_middle=o.half_c32=o.crop_c32=o.fused_qkv_norm=o.fused_mh_ffn=o.tiled_mh_ffn=o.mapped_c32=o.vit_blocked=o.vit_contract_blocked=true;
  o.tiled_ffn_min_c=256;
  if(fast){o.vit_weight_mask=1;o.vit_pack_input=true;o.elide_identity_shift=true;o.raw_chain=true;o.pre_main8=true;o.post_merge_fold=true;o.fused_ffn_project=true;o.split_ffn_fused=true;o.split_mix_blocked=true;o.split_project_blocked=true;o.vit_qkv_blocked=true;o.vit_qkv_fused=true;o.mh_project_crop=true;o.mh_input_mapped=true;o.prefix_fused=true;o.direct_prefix_input=true;o.grouped_mh_contract=true;o.ffn_qkv=true;o.ffn_qkv_max_c=256;o.vit_attn_fused=true;o.vit_qkv_fp8=true;o.vit_expand_frag=true;/* 2026-09-16: fused ViT attention, byte ViT QKV, fragment-native expand weights (-0.23ms, bit-exact) */o.split_mix_h16w=true;/* 2026-09-16 21:50: C512 mix weights prepacked half (-0.12ms, bit-exact) */o.pool_project_h16w=true;/* 2026-09-16 22:20: downsample projection weights prepacked half/E4M3 (-0.21ms, bit-exact) */o.decoder_h16w=true;/* 2026-09-16 22:40: decoder up-projection weights prepacked half (-0.07ms, bit-exact) */o.c512_qkv_frag=true;/* 2026-09-16 20:45: C512 QKV from projection E4M3 tiles + fragment weights, no LDS (-0.37ms, bit-exact) */o.c512_proj_frag=true;o.c512_proj_tiles=true;o.mh_proj_diag=true;o.post_head_fused=true;o.c32_finish_fused=true;o.down_crop_fused=true;o.pool32_h16w=true;o.pool_project_group=true;o.vit_proj_frag=true;o.vit_qkv_frag=true;o.vit_contract_frag=true;/* 2026-09-16 21:55: C512 attention projection + split projection in the same shape (-0.31ms together, bit-exact) */o.prefix_inline=true;/* 2026-09-17 02:25: null on 09-17 00:21 while the block-0 kernel was issue-bound; -0.27ms once its F()/load serialization was fixed, bit-exact */}
  if(const char*v=std::getenv("DLSS5_HIP_GRAPH")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("DLSS5_HIP_GRAPH must be 0 or 1");o.graph=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_PREFIX_FUSED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("prefix fused must be 0 or 1");o.prefix_fused=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_DIRECT_INPUT")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("direct input must be 0 or 1");o.direct_prefix_input=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_GROUPED_CONTRACT")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("grouped contract flag");o.grouped_mh_contract=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_ATTN_FUSED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT attention fused flag");o.vit_attn_fused=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_EXPAND_FRAG")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT expand fragment flag");o.vit_expand_frag=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_EXPAND_M4")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT expand M4 flag");o.vit_expand_m4=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_MH_BYTE_STREAM")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("MH byte stream flag");o.mh_byte_stream=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_BYTE_STREAM")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT byte stream flag");o.vit_byte_stream=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_HALF_STREAM")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT half stream flag");o.vit_half_stream=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_QKV_N4")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT QKV N4 flag");o.vit_qkv_n4=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_QKV_FP8")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT byte QKV flag");o.vit_qkv_fp8=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_QKV_FUSED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT QKV fused flag");o.vit_qkv_fused=!strcmp(v,"1");}
  o.ffn_qkv=fast&&o.grouped_mh_contract;
  if(const char*v=std::getenv("DLSS5_HIP_FFN_QKV")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("FFN/QKV flag");o.ffn_qkv=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_DUP_PREFIX"))o.dup_prefix=v;
  if(const char*v=std::getenv("DLSS5_HIP_DUP_COUNT")){o.dup_count=unsigned(strtoul(v,nullptr,10));if(o.dup_count<1||o.dup_count>8)throw std::runtime_error("dup count 1..8");}
  if(const char*v=std::getenv("DLSS5_HIP_MH_FFN_FRAG256")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("C256 FFN fragment flag");o.mh_ffn_frag256=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_MH_FFN_FRAG")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("MH FFN frag flag");o.mh_ffn_frag=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_CONTRACT_FRAG")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT contract frag flag");o.vit_contract_frag=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_PROJ_FRAG")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT projection frag flag");o.vit_proj_frag=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_QKV_FRAG")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT QKV frag flag");o.vit_qkv_frag=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_MH_WINDOW_FUSED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("MH window fused flag");o.mh_window_fused=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_MH_PROJ_DIAG_FB")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("MH projection diagonal (byte feature) flag");o.mh_proj_diag_fb=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_SPLIT_K")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT split-K flag");o.vit_split_k=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_SPARSE_WEIGHTS")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("sparse weights flag");o.sparse_weights=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_PREFIX_INLINE")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("prefix inline flag");o.prefix_inline=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_POOL_PROJECT_GROUP")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("pool project group flag");o.pool_project_group=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_TILED_FFN_SMALL")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("tiled FFN small flag");o.tiled_ffn_small=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_DOWN_CROP_FUSED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("down crop fused flag");o.down_crop_fused=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_POOL32_H16W")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("pool32 h16w flag");o.pool32_h16w=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_C32_FINISH_FUSED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("C32 finish fused flag");o.c32_finish_fused=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_POST_HEAD_FUSED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("post head fused flag");o.post_head_fused=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_MH_PROJ_DIAG")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("MH projection diagonal flag");o.mh_proj_diag=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_MH_FEATURE_BYTE")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("MH feature byte flag");o.mh_feature_byte=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_C512_PROJ_TILES")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("C512 tiled projection flag");o.c512_proj_tiles=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_C512_PROJ_FRAG")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("C512 fragment projection flag");o.c512_proj_frag=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_C512_QKV_FRAG")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("C512 fragment QKV flag");o.c512_qkv_frag=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_MH_ATTN_W16")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("MH attention w16 flag");o.mh_attn_w16=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_DECODER_H16W")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("decoder half weight flag");o.decoder_h16w=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_POOL_PROJECT_H16W")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("pool project half weight flag");o.pool_project_h16w=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_SPLIT_MIX_H16W")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("split mix half weight flag");o.split_mix_h16w=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_SPLIT_MIX_FUSED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("split mix fused flag");o.split_mix_fused=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_FFN_FUSED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT fused FFN flag");o.vit_ffn_fused=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_INPUT_TILED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT tiled input flag");o.vit_input_tiled=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_VIT_EXPAND_M2")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("ViT expand M2 flag");o.vit_expand_m2=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_QKV_WAVE_C512")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("C512 wave QKV flag");o.qkv_norm_wave_c512=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_POOL_PROJECT_FUSED")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("pool project fused flag");o.pool_project_fused=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_FFN_QKV_BN")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("FFN/QKV batched norm flag");o.ffn_qkv_batched_norm=!strcmp(v,"1");}
  if(const char*v=std::getenv("DLSS5_HIP_FFN_QKV_MAX_C")){char*end=nullptr;auto c=strtoul(v,&end,10);if(*end||(c!=64&&c!=128&&c!=256))throw std::runtime_error("FFN/QKV channel limit");o.ffn_qkv_max_c=unsigned(c);}
  if(const char*v=std::getenv("DLSS5_HIP_DECODER_BYTE")){if(strcmp(v,"0")&&strcmp(v,"1"))throw std::runtime_error("decoder byte flag");o.decoder_byte=!strcmp(v,"1");}
  const wchar_t*modules=_wgetenv(L"DLSS5_HIP_MODULES");o.modules=modules&&*modules?Utf8(modules):Utf8(directory+L"\\HIP");try{bridge.Create(q,o,noise);}catch(...){LogDevice();throw;}LogDevice();
  if(FILE*f=_wfopen(NativeLabPath(L"logs\\native-hip.txt").c_str(),L"ab")){fprintf(f,"pid=%lu runtime=7 fast=%u packed_weights=%u packed_c32=%u fp8_normalized=%u fp8_ffn=%u fp8_av=%u fp8_deep=%u fp8_middle=%u half_c32=%u crop_c32=%u fused_qkv_norm=%u fused_mh_ffn=%u tiled_mh_ffn=%u tiled_ffn_min_c=%u mapped_c32=%u vit_blocked=%u vit_contract_blocked=%u vit_split_k=%u vit_weight_mask=%u vit_pack_input=%u elide_identity_shift=%u vit_qkv_fused=%u ffn_qkv=%u ffn_qkv_max_c=%u grouped_mh_contract=%u direct_prefix_input=%u prefix_fused=%u mh_input_mapped=%u mh_project_crop=%u vit_qkv_blocked=%u split_project_blocked=%u split_mix_blocked=%u split_ffn_fused=%u fused_ffn_project=%u post_merge_fold=%u pre_main8=%u raw_chain=%u graph=%u sparse_weights=%u processing=%ux%u modules=%s\n",GetCurrentProcessId(),unsigned(fast),unsigned(o.packed_weights),unsigned(o.packed_c32),unsigned(o.fp8_normalized),unsigned(o.fp8_ffn),unsigned(o.fp8_av),unsigned(o.fp8_deep),unsigned(o.fp8_middle),unsigned(o.half_c32),unsigned(o.crop_c32),unsigned(o.fused_qkv_norm),unsigned(o.fused_mh_ffn),unsigned(o.tiled_mh_ffn),o.tiled_ffn_min_c,unsigned(o.mapped_c32),unsigned(o.vit_blocked),unsigned(o.vit_contract_blocked),unsigned(o.vit_split_k),o.vit_weight_mask,unsigned(o.vit_pack_input),unsigned(o.elide_identity_shift),unsigned(o.vit_qkv_fused),unsigned(o.ffn_qkv),o.ffn_qkv_max_c,unsigned(o.grouped_mh_contract),unsigned(o.direct_prefix_input),unsigned(o.prefix_fused),unsigned(o.mh_input_mapped),unsigned(o.mh_project_crop),unsigned(o.vit_qkv_blocked),unsigned(o.split_project_blocked),unsigned(o.split_mix_blocked),unsigned(o.split_ffn_fused),unsigned(o.fused_ffn_project),unsigned(o.post_merge_fold),unsigned(o.pre_main8),unsigned(o.raw_chain),unsigned(o.graph),unsigned(o.sparse_weights),o.width,o.height,o.modules.c_str());fprintf(f,"pid=%lu hip_device=%d\n",GetCurrentProcessId(),bridge.hip_device);fclose(f);}
color=rgb;color->AddRef();history=temporal;if(history)history->AddRef();
 }
 // The host submits both sides on the same queue. See D3D12Bridge's stage contract.
 void RecordInputCopy(ID3D12GraphicsCommandList*c,bool use_history=false){if(use_history&&!history)throw std::runtime_error("HIP history not bound");bridge.RecordInputCopy(c,color,use_history?history:nullptr);}
 void EnqueueAfterProducer(ID3D12CommandQueue*q,UINT seed,bool use_history=false){if(use_history&&!history)throw std::runtime_error("HIP history not bound");bridge.EnqueueAfterProducer(q,seed,use_history);}
 void RecordOutputReadable(ID3D12GraphicsCommandList*c){bridge.RecordOutputReadable(c);}
 void NotifyOutputSubmitted(ID3D12CommandQueue*q){bridge.NotifyOutputSubmitted(q);FrameSubmitted();}
 template<class Submission>void Run(Submission&submit,UINT seed,bool use_history=false){if(use_history&&!history)throw std::runtime_error("HIP history not bound");bridge.Run(submit,color,use_history?history:nullptr,seed);FrameSubmitted();}
private:
 void FrameSubmitted(){
  if(++frames==3){if(const char*v=std::getenv("DLSS5_HIP_MEMORY");v&&!strcmp(v,"1"))if(FILE*f=_wfopen(NativeLabPath(L"logs\\native-hip.txt").c_str(),L"ab")){bridge.MemoryReport(f);fclose(f);}}
 }
public:
 void LogDevice(){if(FILE*f=_wfopen(NativeLabPath(L"logs\\native-hip-device.txt").c_str(),L"ab")){fprintf(f,"pid=%lu device=%s arch=%s modules=%s match=%s runtime=%d\n",GetCurrentProcessId(),bridge.adapter_name.c_str(),bridge.architecture.c_str(),bridge.module_directory.c_str(),bridge.device_match.c_str(),bridge.runtime_version);fclose(f);}}
 unsigned frames{};
 ID3D12Resource*Output()const{return bridge.Output();}
#ifdef DLSS5_BENCH_BRIDGE_ISOLATE
 hip_reference::Network& DiagnosticNetwork(){return bridge.DiagnosticNetwork();}
#endif
};
