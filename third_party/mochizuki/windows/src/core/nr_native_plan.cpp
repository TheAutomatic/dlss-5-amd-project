#include "nr_native_plan.hpp"
#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace nr {
namespace {
enum class Kind { Plain, Pre, Post, Down, SwinUp, DecoderUp, FinalHead };
struct Record { const char* row; Kind kind; int skip; bool original; int sx, sy; };
#include "nr_native_plan_data.hpp"
struct Shape { uint32_t w, h; }; // host tensor scalars, not raster axes
struct LayerShape { Shape before, after; };
struct Walk { std::string text; uint32_t nx{}, ny{}; };
uint32_t up(uint32_t n, uint32_t alignment) { return (n + alignment - 1) / alignment * alignment; }
Walk walk(uint32_t source_width, uint32_t source_height, bool emit) {
    Shape current{source_height, source_width}, previous = current;
    std::vector<LayerShape> shapes;
    int last_block = -1;
    std::ostringstream text;
    Walk result;
    for (auto& record : records) {
        std::istringstream input(record.row);
        std::vector<std::string> row; std::string word;
        while (input >> word) row.push_back(word);
        if (row.size() != 19) throw std::logic_error("invalid compiled plan row");
        const int block = std::stoi(row[0]);
        if (block != last_block) {
            result.nx += current.h < previous.h;
            result.ny += current.w < previous.w;
            previous = current; last_block = block;
        }
        Shape out = current;
        switch (record.kind) {
        case Kind::Pre: case Kind::Down: case Kind::FinalHead:
            out = {up((current.w + 1) / 2, 4), up((current.h + 1) / 2, 4)}; break;
        case Kind::SwinUp: case Kind::DecoderUp: {
            if (record.skip < 0 || size_t(record.skip) >= shapes.size()) throw std::logic_error("invalid compiled skip edge");
            out = record.original ? shapes[record.skip].before : shapes[record.skip].after;
            if (record.kind == Kind::SwinUp && row[12] == "32") out = {up(out.w, 8), up(out.h, 8)};
            break;
        }
        case Kind::Post: out = {source_height, source_width}; break;
        case Kind::Plain: break;
        }
        if (emit) {
            Shape plan = current;
            if (record.kind == Kind::FinalHead) plan = out;
            if (record.kind == Kind::DecoderUp) plan = {up(out.w, 4), up(out.h, 4)};
            if (record.kind == Kind::Post) plan = {source_height / 2, source_width / 2};
            const uint64_t tokens = block >= 31 && block <= 38
                ? uint64_t(plan.w) * plan.h : uint64_t(up(plan.w, 8)) * up(plan.h, 8);
            row[3] = std::to_string(plan.w); row[4] = std::to_string(plan.h); row[5] = std::to_string(tokens);
            Shape grid = record.kind == Kind::Post || record.kind == Kind::SwinUp ? out : plan;
            row[6] = std::to_string((int64_t(grid.h) - 4 * record.sx + 7) / 8);
            row[7] = std::to_string((int64_t(grid.w) - 4 * record.sy + 7) / 8); row[8] = "1";
            for (size_t i = 0; i < row.size(); ++i) text << (i ? " " : "") << row[i];
            text << ' ' << record.sx << ' ' << record.sy << '\n';
        }
        shapes.push_back({current, out}); current = out;
    }
    result.text = text.str(); return result;
}
}
NativePlan make_native_plan(uint32_t width, uint32_t height) {
    if (!width || !height || width > 16384 || height > 16384) throw std::invalid_argument("invalid source dimensions");
    const auto probe = walk(width, height, false);
    if (probe.nx >= 16 || probe.ny >= 16) throw std::logic_error("invalid native reduction count");
    const uint32_t ax = 1u << probe.nx, ay = 1u << probe.ny;
    NativePlan result;
    result.reductions_x = probe.nx; result.reductions_y = probe.ny;
    result.width = std::max(320u, up(width, ax)); result.height = std::max(320u, up(height, ay));
    if (!(result.height % (ay * 4)) && !(result.width % (ax * 4))) result.width += ax;
    result.text = walk(result.width, result.height, true).text;
    result.model = model_fingerprint;
    return result;
}
} // namespace nr
