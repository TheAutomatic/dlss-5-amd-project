# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)
project(opennr_rr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
find_package(Vulkan REQUIRED)

find_program(GLSLANG glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
file(GLOB SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp)
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/include/*.glsl)
set(SPV_DIR ${CMAKE_BINARY_DIR}/shaders)
set(SPVS)
# what OpenNrRrRuntime.dll loads (fast mode only); the other kernels (k8, _x, _e, probes) stay for the harness
set(RUNTIME_SPVS)
foreach(n ingest pack selftest selftest_tex pooled_history2 ae downsample downsample_tl post_tile dfn_export dfn_import)
    list(APPEND RUNTIME_SPVS ${SPV_DIR}/${n}.spv)
endforeach()
set(FLOW_KERNELS enc_stage_96_128_128 enc_stage_128_128_160 enc_stage_160_256_160 enc_stage_160_256_0 dec_up_160_256_160
    dec_dfn_160_256 dec_up_128_128_160 dec_dfn_128_128 dec_up_96_128_128 dec_dfn_96_128)
# the user's "Fast mode" (docs/FASE24-FASTMODE.md): each listed kernel again with these defines, suffix _lt beside the file
# it replaces. Engine::set_lite loads whichever exist, so removing a file from the stage stops the ladder at that cut.
include(${CMAKE_CURRENT_SOURCE_DIR}/fastmode.cmake)
function(lite_spv out_name src)
    set(spvl ${SPV_DIR}/${out_name}_lt.spv)
    add_custom_command(OUTPUT ${spvl}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
        COMMAND ${GLSLANG} -V --target-env vulkan1.3 ${ARGN} -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spvl} ${src}
        DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
    set(LITE_SPVS ${LITE_SPVS} ${spvl} PARENT_SCOPE)
endfunction()
foreach(src ${SHADERS})
    get_filename_component(name ${src} NAME_WE)
    if(DEFINED LT_DEFS_${name})
        set(ltd)
        foreach(x ${LT_DEFS_${name}})
            list(APPEND ltd -D${x})
        endforeach()
        if(name STREQUAL "enc0_attr")
            lite_spv(enc0_attr_f ${src} -DENC_F32=1 ${ltd})
            lite_spv(enc0_attr_f_w ${src} -DENC_F32=1 -DATTR_WIDE=1 ${ltd})
            lite_spv(d3d_enc0_attr_f ${src} -DD3D_GUIDES=1 -DENC_F32=1 ${ltd})
            lite_spv(d3d_enc0_attr_f_w ${src} -DD3D_GUIDES=1 -DENC_F32=1 -DATTR_WIDE=1 ${ltd})
        elseif(name STREQUAL "dfn_export" OR name STREQUAL "dfn_import")
            lite_spv(${name} ${src} ${ltd})
        elseif(name STREQUAL "pooled_history2")
            lite_spv(pooled_history2 ${src} ${ltd})
        elseif(name STREQUAL "downsample")
            lite_spv(downsample ${src} ${ltd})
            lite_spv(d3d_downsample ${src} -DD3D_TEX=1 -DD3D_UAV=1 ${ltd})
        elseif(name STREQUAL "downsample_tl")
            lite_spv(downsample_tl ${src} ${ltd})
            lite_spv(d3d_downsample_tl ${src} -DD3D_TEX=1 -DD3D_UAV=1 ${ltd})
        else()
            lite_spv(${name}_f ${src} -DENC_F32=1 ${ltd})
            if(name STREQUAL "enc0_stage_32_64")
                lite_spv(${name}_f_w ${src} -DENC_F32=1 -DATTR_WIDE=1 ${ltd})
            endif()
            list(FIND FLOW_KERNELS ${name} slot)
            if(slot GREATER_EQUAL 0)
                if(slot GREATER 0)
                    set(fw 1)
                else()
                    set(fw 0)
                endif()
                lite_spv(${name}_fl ${src} -DENC_F32=1 -DFLOW=1 -DFLOW_SLOT=${slot} -DFLOW_WAIT=${fw} ${ltd})
            endif()
        endif()
    endif()
    set(spv ${SPV_DIR}/${name}.spv)
    add_custom_command(OUTPUT ${spv}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
        COMMAND ${GLSLANG} -V --target-env vulkan1.3 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spv} ${src}
        DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
    list(APPEND SPVS ${spv})
    # k8 network kernels also built with the exact scalar P.V chain (ENC_PVX), suffix _x
    if(name MATCHES "^(enc_block_.*_k8|enc_stage_|enc0_stage_|dec_)")
        set(spvx ${SPV_DIR}/${name}_x.spv)
        add_custom_command(OUTPUT ${spvx}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
            COMMAND ${GLSLANG} -V --target-env vulkan1.3 -DENC_PVX=1 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spvx} ${src}
            DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
        list(APPEND SPVS ${spvx})
        # and with every product as the exact scalar HMMA chain (ENC_EXACT, validation only), suffix _e
        set(spve ${SPV_DIR}/${name}_e.spv)
        add_custom_command(OUTPUT ${spve}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
            COMMAND ${GLSLANG} -V --target-env vulkan1.3 -DENC_EXACT=1 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spve} ${src}
            DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
        list(APPEND SPVS ${spve})
        # and the fast variant: fp32 accumulation over the whole K of each product (ENC_F32), suffix _f
        set(spvf ${SPV_DIR}/${name}_f.spv)
        add_custom_command(OUTPUT ${spvf}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
            COMMAND ${GLSLANG} -V --target-env vulkan1.3 -DENC_F32=1 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spvf} ${src}
            DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
        list(APPEND SPVS ${spvf})
        if(NOT name MATCHES "^enc_block_")
            list(APPEND RUNTIME_SPVS ${spvf})
        endif()
        # the small levels in order (enc2 .. dec2_1): the fast variant without barriers between them, row flags (FLOW,
        # onr_flow.glsl), slot = position, every one but enc2 waits on the previous; suffix _fl
        list(FIND FLOW_KERNELS ${name} slot)
        if(slot GREATER_EQUAL 0)
            if(slot GREATER 0)
                set(fw 1)
            else()
                set(fw 0)
            endif()
            set(spvl ${SPV_DIR}/${name}_fl.spv)
            add_custom_command(OUTPUT ${spvl}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
                COMMAND ${GLSLANG} -V --target-env vulkan1.3 -DENC_F32=1 -DFLOW=1 -DFLOW_SLOT=${slot} -DFLOW_WAIT=${fw} -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spvl} ${src}
                DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
            list(APPEND SPVS ${spvl})
            list(APPEND RUNTIME_SPVS ${spvl})
        endif()
    endif()
    # the D3D12 path's ingest / pack / downsample, reading and writing the game's textures through set 0 (d3d_ prefix: built
    # with the descriptor layout of Device::pipeline_elf)
    if(name MATCHES "^(ingest|pack|downsample|downsample_tl)$")
        set(spvd ${SPV_DIR}/d3d_${name}.spv)
        add_custom_command(OUTPUT ${spvd}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
            COMMAND ${GLSLANG} -V --target-env vulkan1.3 -DD3D_TEX=1 -DD3D_UAV=1 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spvd} ${src}
            DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
        list(APPEND SPVS ${spvd})
        list(APPEND RUNTIME_SPVS ${spvd})
    endif()
    # the D3D12 path's exposure and fast enc0_attr reading the game's guides through set 0 (no ingest for them)
    if(name STREQUAL "ae" OR name STREQUAL "enc0_attr")
        if(name STREQUAL "ae")
            set(spvd ${SPV_DIR}/d3d_ae.spv)
        else()
            set(spvd ${SPV_DIR}/d3d_enc0_attr_f.spv)
        endif()
        add_custom_command(OUTPUT ${spvd}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
            COMMAND ${GLSLANG} -V --target-env vulkan1.3 -DD3D_GUIDES=1 -DENC_F32=1 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spvd} ${src}
            DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
        list(APPEND SPVS ${spvd})
        list(APPEND RUNTIME_SPVS ${spvd})
    endif()
    # kernels outside the network with a fast variant too (_f: not bit-exact to the reference filtering)
    if(name MATCHES "^(enc0_attr|post|post_dlt)$")
        set(spvf ${SPV_DIR}/${name}_f.spv)
        add_custom_command(OUTPUT ${spvf}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
            COMMAND ${GLSLANG} -V --target-env vulkan1.3 -DENC_F32=1 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spvf} ${src}
            DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
        list(APPEND SPVS ${spvf})
        list(APPEND RUNTIME_SPVS ${spvf})
    endif()
    # the fast enc0 halves with the 64-byte attribute record (ATTR_WIDE, suffix _f_w): a frame with a depth of field
    # guide or a responsivity mask, whose attributes 32 / 33 the 48-byte record has no room for
    if(name MATCHES "^(enc0_attr|enc0_stage_32_64)$")
        set(spvw ${SPV_DIR}/${name}_f_w.spv)
        add_custom_command(OUTPUT ${spvw}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
            COMMAND ${GLSLANG} -V --target-env vulkan1.3 -DENC_F32=1 -DATTR_WIDE=1 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spvw} ${src}
            DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
        list(APPEND SPVS ${spvw})
        list(APPEND RUNTIME_SPVS ${spvw})
        if(name STREQUAL "enc0_attr")
            set(spvw ${SPV_DIR}/d3d_enc0_attr_f_w.spv)
            add_custom_command(OUTPUT ${spvw}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
                COMMAND ${GLSLANG} -V --target-env vulkan1.3 -DD3D_GUIDES=1 -DENC_F32=1 -DATTR_WIDE=1 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include -o ${spvw} ${src}
                DEPENDS ${src} ${SHADER_INCLUDES} VERBATIM)
            list(APPEND SPVS ${spvw})
            list(APPEND RUNTIME_SPVS ${spvw})
        endif()
    endif()
endforeach()
add_custom_target(shaders ALL DEPENDS ${SPVS} ${LITE_SPVS})

add_library(opennr_rr STATIC src/vk.cpp src/d3d12.cpp src/kernels.cpp src/weights.cpp src/engine.cpp)
target_include_directories(opennr_rr PUBLIC include)
target_link_libraries(opennr_rr PUBLIC Vulkan::Vulkan d3d12 dxgi)
add_dependencies(opennr_rr shaders)

file(GLOB HARNESS_SRC CONFIGURE_DEPENDS harness/*.cpp)
if(HARNESS_SRC)
    add_executable(opennr_rr_harness ${HARNESS_SRC})
    target_link_libraries(opennr_rr_harness PRIVATE opennr_rr d3d12 dxgi)
    target_compile_definitions(opennr_rr_harness PRIVATE ONR_SPV_DIR="${SPV_DIR}")
    if(MSVC)
        target_compile_options(opennr_rr_harness PRIVATE /W4 /permissive-)
    endif()
endif()

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    target_compile_options(opennr_rr PRIVATE /W4 /permissive-)
endif()

# runtime DLL (opennr_rr_api.h) and its D3D12 validation driver
add_library(OpenNrRrRuntime SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/runtime.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/vk_host.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/version.rc)
target_include_directories(OpenNrRrRuntime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include)
target_link_libraries(OpenNrRrRuntime PRIVATE opennr_rr d3d12)
if(MSVC)
    target_compile_options(OpenNrRrRuntime PRIVATE /W4 /permissive-)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/validate/main.cpp")
    add_executable(opennr_rr_validate ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/validate/main.cpp)
    target_include_directories(opennr_rr_validate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include)
    target_link_libraries(opennr_rr_validate PRIVATE d3d12 dxgi)
    if(MSVC)
        target_compile_options(opennr_rr_validate PRIVATE /W4 /permissive-)
    endif()
endif()
# and its Vulkan host driver (ABI 5): a device of its own made as a Vulkan game's, RecordVk on its command buffer
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/validate/vk_main.cpp")
    add_executable(opennr_rr_validate_vk ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/validate/vk_main.cpp)
    target_include_directories(opennr_rr_validate_vk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include)
    target_link_libraries(opennr_rr_validate_vk PRIVATE Vulkan::Vulkan)
    if(MSVC)
        target_compile_options(opennr_rr_validate_vk PRIVATE /W4 /permissive-)
    endif()
endif()

# what ships: the DLL and the kernels it loads, in <build>/stage (OpenNrRrRuntime.dll, opennr-rr/shaders/*.spv)
set(STAGE_DIR ${CMAKE_BINARY_DIR}/stage)
add_custom_target(runtime_stage
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE_DIR}/opennr-rr/shaders
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:OpenNrRrRuntime> ${STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${RUNTIME_SPVS} ${LITE_SPVS} ${STAGE_DIR}/opennr-rr/shaders
    DEPENDS OpenNrRrRuntime shaders VERBATIM)
