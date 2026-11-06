// SPDX-License-Identifier: MIT
#include "opennr_rr/kernels.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace onr {

static uint32_t subgroup_of(const std::string& n) {
    if (getenv("ONR_W32") && n.find(getenv("ONR_W32")) != std::string::npos) return 32;   // measurements
    return n.rfind("enc_", 0) == 0 || n.rfind("enc0_stage", 0) == 0 || n.rfind("dec_", 0) == 0 || n.rfind("probe_wmma", 0) == 0 || n.rfind("probe_rate", 0) == 0 ? 32 : 0;
}

// every compiled kernel of the directory; the network kernels need 32-lane subgroups (cooperative matrices)
void Kernels::load(Gpu& dev, const std::string& dir) {
    for (auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".spv") continue;
        std::string n = e.path().stem().string();
        k_[n] = dev.load(e.path().string(), subgroup_of(n));
    }
}

void Kernels::load(Gpu& dev, const std::string& dir, const std::vector<std::string>& names) {
    for (auto& n : names)
        if (!k_.count(n)) k_[n] = dev.load((std::filesystem::path(dir) / (n + ".spv")).string(), subgroup_of(n));
}

void Kernels::load_lite(Gpu& dev, const std::string& dir, const std::vector<std::string>& names) {
    for (auto& n : names) {
        auto p = std::filesystem::path(dir) / (n + "_lt.spv");
        if (!k_.count(n) && std::filesystem::exists(p)) k_[n] = dev.load(p.string(), subgroup_of(n));
    }
}

std::vector<std::string> Kernels::names() const {
    std::vector<std::string> v;
    for (auto& [n, k] : k_) v.push_back(n);
    return v;
}

void Kernels::run(Gpu& d, const std::string& n, const void* pc, uint32_t size, uint32_t groups) const {
    d.dispatch(k_.at(n), pc, size, groups);
}

void Kernels::unload(Gpu& dev) {
    for (auto& [n, k] : k_) dev.destroy(k);
    k_.clear();
}

void Kernels::clear_range(Gpu& d, const ClearRangePC& p) const {
    d.dispatch(k_.at("clear_range"), &p, sizeof p, div_up(div_up(p.count, 4) + 1, 256));
}

void Kernels::pooled_history2(Gpu& d, const PooledHistory2PC& p) const {
    d.dispatch(k_.at("pooled_history2"), &p, sizeof p, div_up(p.out_size.x, 16), div_up(p.out_size.y, 16));
}

void Kernels::auto_exposure(Gpu& d, const AeSumPC& s, const AeFinalPC& f, I2 grid) const {
    d.dispatch(k_.at("ae_sum"), &s, sizeof s, grid.x, grid.y);
    d.barrier();
    d.dispatch(k_.at("ae_final"), &f, sizeof f, 1);
}

void Kernels::downsample(Gpu& d, const DownsamplePC& p) const {
    d.dispatch(k_.at("downsample"), &p, sizeof p, div_up(p.out_end.x - p.out_origin.x, 16),
               div_up(p.out_end.y - p.out_origin.y, 16));
}

void Kernels::post_tile(Gpu& d, const PostTilePC& p) const {
    d.dispatch(k_.at("post_tile"), &p, sizeof p, div_up(p.lr_size.x, 16), div_up(p.lr_size.y, 16));
}

static void post_pass(Gpu& d, const std::map<std::string, Kernel>& k, const std::string& n, const PostPC& p, const void* pc,
                      uint32_t size, const Buffer& fix, bool fast) {
    const uint32_t gx = div_up(p.end.x - p.origin.x, 16), gy = div_up(p.end.y - p.origin.y, 32);
    if (fast) {
        d.dispatch(k.at(n + "_f"), pc, size, gx, gy);
        return;
    }
    d.fill(fix, 0, 4, 0);
    d.fill(fix, 4, 8, 1);
    d.fill(fix, 12, 4, 0);
    d.barrier();
    d.dispatch(k.at(n), pc, size, gx, gy);
    d.barrier();
    d.dispatch_indirect(k.at(n == "post" ? "post_fixup" : "post_fixup_dlt"), pc, size, fix);
}

void Kernels::post(Gpu& d, const PostPC& p, const Buffer& fix, bool fast) const {
    post_pass(d, k_, "post", p, &p, sizeof p, fix, fast);
}

void Kernels::post_dlt(Gpu& d, const PostDltPC& p, const Buffer& fix, bool fast) const {
    post_pass(d, k_, "post_dlt", p.p, &p, sizeof p, fix, fast);
}

void Kernels::enc_block(Gpu& d, const EncBlockPC& p, uint32_t windows, int C, int X, bool k8) const {
    d.dispatch(k_.at("enc_block_" + std::to_string(C) + "_" + std::to_string(X) + (k8 ? "_k8" : "")), &p, sizeof p, windows);
}

void Kernels::enc_stage(Gpu& d, const EncStagePC& p, uint32_t windows, int C, int X, int Co) const {
    d.dispatch(k_.at("enc_stage_" + std::to_string(C) + "_" + std::to_string(X) + "_" + std::to_string(Co)), &p, sizeof p, windows);
}

void Kernels::enc_qv_fp8(Gpu& d, const EncQvFp8PC& p, uint32_t windows) const {
    d.dispatch(k_.at("enc_qv_fp8"), &p, sizeof p, windows);
}

}  // namespace onr
