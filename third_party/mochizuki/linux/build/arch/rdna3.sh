# RDNA3 (RX 7000, gfx11): no FP8 matrix instructions, so every e4m3 value travels as FP16
# through the FP16 WMMA with FP32 accumulation, wave32. The network in linux/shaders/rdna3/.
# Sourced by build_package.sh, build_optiscaler_nr.sh and assemble_product_data.sh.
#
# The shaders and the host must agree: nr_graph refuses a shader directory whose
# shader-constants.txt disagrees with the defines below. The shaders are built by
# `python3 linux/build/build_network.py rdna3` from linux/shaders/rdna3/pipelines.json.
NR_GPU_ARCH=rdna3
# The int4 mixed option's calibration tables (linux/data/int4/): they are calibrated on the
# FP8 network, whose e4m3 values this build reproduces, so the rdna4 tables apply as they
# are and the weights made from them have the same SHA-256.
NR_INT4_DATA=rdna4
NR_PRODUCT_SPV=build/linux/rdna3/network
NR_PRODUCT_DEFINES=(
    -DNR_ARCH_RDNA3=1
    -DNR_GEMM_WIDE_MT=64 -DNR_GEMM_WIDE_NT=256
    -DNR_GEMM_PROJ_MT=32 -DNR_GEMM_PROJ_NT=128
    -DNR_PROJW_MIN_TOKENS=768 -DNR_GEMM_REMAP_HOST=1
    -DNR_FFWD_WGW=4 -DNR_FFWD_GMAJOR=1 -DNR_UPSVIEW_VEC=1 -DNR_REPACK_VEC=1
    -DNR_WIDE_UPS_MASK=7 -DNR_PERSIST_DF=1 -DNR_DS_FUSE=15
    -DNR_DECUPS_VEC=16 -DNR_DECQ_SMALL=1
    -DNR_PERSIST_DS_MASK=7 -DNR_PERSIST_UPS_MASK=7 -DNR_REPACK_FOLD=1 -DNR_STRAG_PCT=25
    -DNR_PERSIST_ONE_MASK=6 -DNR_NOISE_FIELD=1 -DNR_FFWD_FM2_MIN_TOKENS=2560 -DNR_POST_ALPHA=1
    -DNR_TCHAIN_TILES=1
)
# The model pack's file name inside dlssnr-amd/ (linux/src/core/nr_runtime.cpp).
NR_MODEL_NAME=dlssnr.bin
