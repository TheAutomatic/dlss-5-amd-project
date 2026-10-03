// Sample the GPU's own clock, power and temperature while a timed region runs.
//
// Lifted out of the kernel benchmark, which had it first and for the same reason: this
// part idles near 1 GHz and boosts past 2.9, so the identical kernel measures
// 2.5x apart for reasons that have nothing to do with the kernel. Two ablations
// of gemm1x1 that *removed* work came out slower than the full kernel before
// this was wired in, which is not a thing that can happen and was entirely the
// clock. Report FLOP/clk, or cycles per WMMA - the clock-independent quantity
// the hardware ceiling is expressed in - and the numbers become comparable.
#pragma once
#include <dirent.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace nrvk {

struct Telemetry {
    std::string dir;
    std::atomic<bool> running{false};
    std::thread worker;
    std::vector<double> mhz, watts, celsius, elapsed;
    std::chrono::steady_clock::time_point origin;

    double elapsed_ms() const {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - origin).count();
    }

    static std::string find(uint32_t vendor, uint32_t device) {
        for (int card = 0; card < 8; ++card) {
            const std::string base = "/sys/class/drm/card" + std::to_string(card) + "/device/";
            std::ifstream v(base + "vendor"), d(base + "device");
            std::string vs, ds;
            if (!(v >> vs) || !(d >> ds)) continue;
            if (std::stoul(vs, nullptr, 16) != vendor || std::stoul(ds, nullptr, 16) != device) continue;
            DIR* dp = opendir((base + "hwmon").c_str());
            if (!dp) continue;
            std::string found;
            while (dirent* e = readdir(dp))
                if (std::string(e->d_name).rfind("hwmon", 0) == 0)
                    found = base + "hwmon/" + e->d_name + "/";
            closedir(dp);
            if (!found.empty()) return found;
        }
        return {};
    }
    static double read(const std::string& path, double scale) {
        std::ifstream f(path);
        double v;
        return (f >> v) ? v * scale : -1.0;
    }
    void start() {
        mhz.clear(); watts.clear(); celsius.clear(); elapsed.clear();
        origin = std::chrono::steady_clock::now();
        if (dir.empty()) return;
        running = true;
        worker = std::thread([this] {
            while (running) {
                elapsed.push_back(elapsed_ms());
                mhz.push_back(read(dir + "freq1_input", 1e-6));
                watts.push_back(read(dir + "power1_average", 1e-6));
                celsius.push_back(read(dir + "temp1_input", 1e-3));
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    void stop() { if (!dir.empty() && worker.joinable()) { running = false; worker.join(); } }
    static double mean(const std::vector<double>& v) {
        double s = 0; size_t n = 0;
        for (double x : v) if (x >= 0) { s += x; ++n; }
        return n ? s / n : 0.0;
    }
};

}  // namespace nrvk
