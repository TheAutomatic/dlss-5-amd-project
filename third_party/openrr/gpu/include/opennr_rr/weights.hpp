// SPDX-License-Identifier: MIT
// rr2 (preset F) weights rebuilt from the player's nvngx_dlssd.dll, in the logical order the kernels read
// (FASE3A section 2, ref-impl/weights.py). All matrices and vectors are fp16 bit patterns.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace onr {

using H16 = std::vector<uint16_t>;

struct BlockWeights {
    int C = 0, X = 0;
    H16 in_a, in_b, out, out_b, fc1, fc1_b, fc2, fc2_b, bias;   // bias: [H][64][64] as the kernels read it
    uint32_t ls = 0;                                            // ls1 | ls2 << 16 (fp16 bits)
};

struct StageWeights {
    BlockWeights b;
    H16 dfn_w, dfn_b;        // [16][C] / [16], rows past the DFN outputs zero (empty for dec0_1)
    H16 mrg_w, mrg_b;        // encoders: merge_out [Co][4C] (16 zeros without one), enc1 followed by merge_in
    H16 up_w, up_b;          // decN_0: [4][C][Cp], [4][C]
    int Co = 0, Cp = 0;
    uint16_t s_dfn = 0;      // param offset 92
    uint16_t s98 = 0;        // decN_0: param offset 98 (skip scale)
};

struct Weights {
    StageWeights enc[6];     // enc0 .. enc5 (enc0: block, DFN head)
    StageWeights dec0[5];    // decN_0 by level N
    StageWeights dec1[5];    // decN_1 by level N (dec1[0] = dec0_1, the output head)
    H16 embed_w, embed_b;    // enc0 embed MLP: fc1 [64][64] (K 43 -> 64) then fc2 [32][64]; fc1_b [64] then fc2_b [32]
    H16 lut;                 // 2048 noise codes
    float enc0_scalars[4] = {};   // enc0 parameter offsets 572, 576, 580, 584
    float head_k_scale = 8.0f, head_k_tanh = 0;   // dec0_1 offsets 208, 212
    float post_gate = 0;     // post offset 128
    std::string version;     // DLL file version
};

// Reads the DLL (never loads it) and rebuilds every tensor. False with err when the file is not a build whose
// layout is known or its bytes there do not match.
bool build_weights(const std::wstring& dll_path, Weights& w, std::string& err);

// fp16 helpers (round to nearest even)
float h2f(uint16_t h);
uint16_t f2h(float f);

}  // namespace onr
