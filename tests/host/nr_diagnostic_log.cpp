#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/DiagnosticLog.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    assert(argc == 2);
    using namespace DlssNr::Diagnostics;
    RepeatGate gate;
    assert(gate.Allow(0, 1));
    for (uint64_t i = 1; i < 5000; ++i) assert(!gate.Allow(i, 1));
    assert(gate.Allow(5000, 1));
    assert(gate.TakeSuppressed() == 4999);
    assert(gate.TakeSuppressed() == 0);
    assert(gate.Allow(5001, 2)); // A different fault remains visible immediately.
    assert(!gate.Allow(5002, 2));
    gate.Reset();
    assert(gate.Allow(5003, 2)); // A new episode is not hidden by the previous one.
    assert(gate.TakeSuppressed() == 1);
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    const auto path = dir / "bounded.log";
    std::filesystem::remove(path);
    std::filesystem::remove(dir / "bounded.log.1");
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t)
        writers.emplace_back([&] { for (int i = 0; i < 1000; ++i) Append(path, "complete-line", 128); });
    for (auto& t : writers) t.join();
    for (const auto& p : {path, dir / "bounded.log.1"})
    {
        assert(std::filesystem::file_size(p) <= 128);
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) assert(line == "complete-line");
    }
    const auto before = std::filesystem::file_size(path);
    Append(path, std::string(129, 'x'), 128);
    assert(std::filesystem::file_size(path) == before);
    Append(dir, "invalid destination"); // No exception escapes an I/O failure.
    std::cout << "nr diagnostic logging: PASS\n";
}
