// mz_interpose.cpp: the core's pipeline creation seen from outside, the prewarm and the pipeline binary probe
// (mz_interpose.h). Built without the build script's redirects, so every vk* call here is the real entry point.
#include "mz_interpose.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nr_log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The core's calls, renamed by the build script. The declarations it sees are vulkan_core.h's, so these must keep
// their exact types.
extern "C"
{
    VKAPI_ATTR VkResult VKAPI_CALL mzi_vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                                uint32_t createInfoCount,
                                                                const VkComputePipelineCreateInfo* pCreateInfos,
                                                                const VkAllocationCallbacks* pAllocator,
                                                                VkPipeline* pPipelines);
    VKAPI_ATTR VkResult VKAPI_CALL mzi_vkCreateShaderModule(VkDevice device,
                                                            const VkShaderModuleCreateInfo* pCreateInfo,
                                                            const VkAllocationCallbacks* pAllocator,
                                                            VkShaderModule* pShaderModule);
    VKAPI_ATTR VkResult VKAPI_CALL mzi_vkCreateDescriptorSetLayout(VkDevice device,
                                                                   const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                                                   const VkAllocationCallbacks* pAllocator,
                                                                   VkDescriptorSetLayout* pSetLayout);
    VKAPI_ATTR VkResult VKAPI_CALL mzi_vkCreatePipelineLayout(VkDevice device,
                                                              const VkPipelineLayoutCreateInfo* pCreateInfo,
                                                              const VkAllocationCallbacks* pAllocator,
                                                              VkPipelineLayout* pPipelineLayout);
    VKAPI_ATTR VkResult VKAPI_CALL mzi_vkCreatePipelineCache(VkDevice device,
                                                             const VkPipelineCacheCreateInfo* pCreateInfo,
                                                             const VkAllocationCallbacks* pAllocator,
                                                             VkPipelineCache* pPipelineCache);
    VKAPI_ATTR void VKAPI_CALL mzi_vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache,
                                                          const VkAllocationCallbacks* pAllocator);
}
static_assert(std::is_same_v<decltype(&mzi_vkCreateComputePipelines), PFN_vkCreateComputePipelines>);
static_assert(std::is_same_v<decltype(&mzi_vkCreateShaderModule), PFN_vkCreateShaderModule>);
static_assert(std::is_same_v<decltype(&mzi_vkCreateDescriptorSetLayout), PFN_vkCreateDescriptorSetLayout>);
static_assert(std::is_same_v<decltype(&mzi_vkCreatePipelineLayout), PFN_vkCreatePipelineLayout>);
static_assert(std::is_same_v<decltype(&mzi_vkCreatePipelineCache), PFN_vkCreatePipelineCache>);
static_assert(std::is_same_v<decltype(&mzi_vkDestroyPipelineCache), PFN_vkDestroyPipelineCache>);

namespace
{
// ---- a compute pipeline's create info, as the manifest describes it ----

struct Binding
{
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLER;
    uint32_t count = 0;
    VkShaderStageFlags stages = 0;
};

struct SetLayoutInfo
{
    VkDescriptorSetLayoutCreateFlags flags = 0;
    std::vector<Binding> bindings;
};

struct Layout
{
    VkPipelineLayoutCreateFlags flags = 0;
    std::vector<SetLayoutInfo> sets; // in set order
    std::vector<VkPushConstantRange> push;
};

// A SPIR-V module by its content: Hash of its bytes, and their number.
struct Module
{
    uint64_t hash = 0, bytes = 0;
};

// Everything vkCreateComputePipelines is given. What this cannot describe (another pNext than the required subgroup
// size, specialization constants, immutable samplers, a derivative pipeline) is refused when it is recorded.
struct Pipeline
{
    Module module;
    std::string entry;
    VkPipelineCreateFlags flags = 0;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
    VkPipelineShaderStageCreateFlags stageFlags = 0;
    uint32_t subgroup = 0; // VkPipelineShaderStageRequiredSubgroupSizeCreateInfo's; 0 without one
    Layout layout;
};

// A module's or a file's identity, not a secret: 64-bit words through a multiply and a shift, the tail byte by byte.
uint64_t Hash(const void* data, size_t n)
{
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9e3779b97f4a7c15ull;
        h ^= h >> 32;
    }
    for (; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// The pipeline as one line of the manifest, which is also its identity (Capture's deduplication):
// m=<hash>:<bytes> e=<entry> f=<flags> st=<stage> sf=<stage flags> sg=<subgroup> lf=<layout flags>, then one
// set=<flags>[:<binding>.<type>.<count>.<stages>]... per set layout and one pc=<stages>.<offset>.<size> per push
// constant range. Flags in hex, the rest in decimal.
std::string Describe(const Pipeline& p)
{
    char one[128];
    std::snprintf(one, sizeof one, "m=%016llx:%llu e=", static_cast<unsigned long long>(p.module.hash),
                  static_cast<unsigned long long>(p.module.bytes));
    std::string out = one;
    out += p.entry;
    std::snprintf(one, sizeof one, " f=%x st=%x sf=%x sg=%u lf=%x", unsigned(p.flags), unsigned(p.stage),
                  unsigned(p.stageFlags), p.subgroup, unsigned(p.layout.flags));
    out += one;
    for (const SetLayoutInfo& set : p.layout.sets)
    {
        std::snprintf(one, sizeof one, " set=%x", unsigned(set.flags));
        out += one;
        for (const Binding& b : set.bindings)
        {
            std::snprintf(one, sizeof one, ":%u.%u.%u.%x", b.binding, unsigned(b.type), b.count, unsigned(b.stages));
            out += one;
        }
    }
    for (const VkPushConstantRange& r : p.layout.push)
    {
        std::snprintf(one, sizeof one, " pc=%x.%u.%u", unsigned(r.stageFlags), r.offset, r.size);
        out += one;
    }
    return out;
}

// An entry point name the manifest can carry: an identifier.
bool Identifier(const char* name)
{
    if (!name || !*name || std::strlen(name) > 64)
        return false;
    for (const char* c = name; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_'))
            return false;
    return true;
}

// Describe's inverse. Only its own output is accepted: what does not print back to the same text is refused.
bool Parse(const std::string& text, Pipeline& p)
{
    std::vector<std::string> tokens;
    for (size_t at = 0;;)
    {
        const size_t space = text.find(' ', at);
        tokens.push_back(text.substr(at, space == std::string::npos ? std::string::npos : space - at));
        if (space == std::string::npos)
            break;
        at = space + 1;
    }
    if (tokens.size() < 7)
        return false;
    int used = 0;
    auto whole = [&used](const std::string& t) { return used == int(t.size()); };
    unsigned long long hash = 0, bytes = 0;
    if (std::sscanf(tokens[0].c_str(), "m=%16llx:%llu%n", &hash, &bytes, &used) != 2 || !whole(tokens[0]))
        return false;
    p.module = { hash, bytes };
    if (tokens[1].compare(0, 2, "e=") || !Identifier(tokens[1].c_str() + 2))
        return false;
    p.entry = tokens[1].substr(2);
    unsigned flags = 0, stage = 0, stageFlags = 0, subgroup = 0, layoutFlags = 0;
    const struct
    {
        const char* format;
        unsigned* value;
    } fields[] = { { "f=%x%n", &flags },
                   { "st=%x%n", &stage },
                   { "sf=%x%n", &stageFlags },
                   { "sg=%u%n", &subgroup },
                   { "lf=%x%n", &layoutFlags } };
    for (size_t i = 0; i < std::size(fields); ++i)
    {
        used = 0;
        if (std::sscanf(tokens[2 + i].c_str(), fields[i].format, fields[i].value, &used) != 1 || !whole(tokens[2 + i]))
            return false;
    }
    p.flags = flags;
    p.stage = VkShaderStageFlagBits(stage);
    p.stageFlags = stageFlags;
    p.subgroup = subgroup;
    p.layout = {};
    p.layout.flags = layoutFlags;
    size_t i = 7;
    for (; i < tokens.size() && !tokens[i].compare(0, 4, "set="); ++i)
    {
        const char* c = tokens[i].c_str() + 4;
        SetLayoutInfo set;
        unsigned setFlags = 0;
        int n = 0;
        if (std::sscanf(c, "%x%n", &setFlags, &n) != 1)
            return false;
        set.flags = setFlags;
        for (c += n; *c == ':'; c += n)
        {
            unsigned binding = 0, type = 0, count = 0, stages = 0;
            n = 0;
            if (std::sscanf(c, ":%u.%u.%u.%x%n", &binding, &type, &count, &stages, &n) != 4)
                return false;
            set.bindings.push_back({ binding, VkDescriptorType(type), count, stages });
        }
        if (*c)
            return false;
        p.layout.sets.push_back(std::move(set));
    }
    for (; i < tokens.size() && !tokens[i].compare(0, 3, "pc="); ++i)
    {
        unsigned stages = 0, offset = 0, size = 0;
        used = 0;
        if (std::sscanf(tokens[i].c_str(), "pc=%x.%u.%u%n", &stages, &offset, &size, &used) != 3 || !whole(tokens[i]))
            return false;
        p.layout.push.push_back({ stages, offset, size });
    }
    return i == tokens.size() && Describe(p) == text;
}

// ---- files ----

std::string Utf8(const std::wstring& text)
{
    const int n = text.empty()
                      ? 0
                      : WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(std::max(n, 0)), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool ReadAll(const std::wstring& path, std::vector<char>& out)
{
    out.clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f)
        return false;
    bool ok = !_fseeki64(f, 0, SEEK_END);
    const long long n = ok ? _ftelli64(f) : -1;
    ok = n >= 0 && !_fseeki64(f, 0, SEEK_SET);
    if (ok)
    {
        out.resize(size_t(n));
        ok = !n || std::fread(out.data(), 1, out.size(), f) == out.size();
    }
    std::fclose(f);
    if (!ok)
        out.clear();
    return ok;
}

uint64_t FileBytes(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return UINT64_MAX;
    return (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

// Through a temporary file and a rename that replaces the file, so that no reader, in this process or another, ever
// sees half of it. Each call has a temporary of its own (the process id and a number no other call of this process
// gets): two sessions can write the same file at once (one's Prewarm and another's Finish both write pipeline.cache,
// and nothing serializes them), and then each writes a whole file of its own, and the last rename wins with it.
bool WriteReplace(const std::wstring& path, const void* data, size_t bytes)
{
    static std::atomic<uint32_t> serial { 0 };
    const std::wstring tmp =
        path + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++serial) + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f)
        return false;
    bool ok = !bytes || std::fwrite(data, 1, bytes, f) == bytes;
    ok = !std::fclose(f) && ok;
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        return true;
    DeleteFileW(tmp.c_str());
    return false;
}

// Every file under the shader folders, by path relative to dlssnr-amd\shaders ('/'-separated, UTF-8) in byte order,
// and a hash of the list with the sizes.
struct File
{
    std::string rel;
    std::wstring path;
    uint64_t bytes = 0;
};

struct Folder
{
    std::vector<File> files;
    uint64_t hash = 0;

    const File* Find(const std::string& rel) const
    {
        const auto at = std::lower_bound(files.begin(), files.end(), rel,
                                         [](const File& f, const std::string& r) { return f.rel < r; });
        return at != files.end() && at->rel == rel ? &*at : nullptr;
    }
};

void Walk(const std::wstring& dir, const std::string& rel, std::vector<File>& out, int depth)
{
    WIN32_FIND_DATAW fd {};
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L".."))
            continue;
        const std::wstring path = dir + L"\\" + fd.cFileName;
        const std::string name = rel + Utf8(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && depth < 8)
                Walk(path, name + "/", out, depth + 1);
        }
        else
            out.push_back({ name, path, (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

Folder Scan(const std::wstring& shaders)
{
    Folder folder;
    Walk(shaders, "", folder.files, 0);
    std::sort(folder.files.begin(), folder.files.end(), [](const File& a, const File& b) { return a.rel < b.rel; });
    std::string list;
    for (const File& f : folder.files)
        list += f.rel + "\t" + std::to_string(f.bytes) + "\n";
    folder.hash = Hash(list.data(), list.size());
    return folder;
}

std::string Hex(const uint8_t* bytes, size_t n)
{
    std::string out;
    char two[3];
    for (size_t i = 0; i < n; ++i)
    {
        std::snprintf(two, sizeof two, "%02x", bytes[i]);
        out += two;
    }
    return out;
}

// ---- the manifest (dlssnr-amd\prewarm\manifest.txt) ----
//
//   mochizuki-prewarm 1
//   shaders <Folder hash> <files>
//   cache-uuid <the pipelineCacheUUID of the device that recorded it>
//   pipelines <n>
//   p <path relative to dlssnr-amd\shaders, or - when no file there has the module's bytes> <Describe>   (n lines)
//   end <Hash of every byte before this line>
//
// It describes layouts and names files; it holds nothing a driver made. The cache UUID is only a note: another
// driver compiles the same create infos.
constexpr const char* kManifestVersion = "mochizuki-prewarm 1";

struct Manifest
{
    uint64_t folderHash = 0;
    uint32_t files = 0;
    uint8_t uuid[VK_UUID_SIZE] {};
    std::vector<std::pair<std::string, Pipeline>> entries;
};

bool ValidPath(const std::string& rel)
{
    if (rel.empty() || rel.find(' ') != std::string::npos)
        return false;
    return rel == "-" || (rel.find("..") == std::string::npos && rel.find(':') == std::string::npos &&
                          rel.find('\\') == std::string::npos && rel[0] != '/');
}

std::string Write(const Manifest& m)
{
    char line[160];
    std::string text = std::string(kManifestVersion) + "\n";
    std::snprintf(line, sizeof line, "shaders %016llx %u\n", static_cast<unsigned long long>(m.folderHash), m.files);
    text += line;
    text += "cache-uuid " + Hex(m.uuid, sizeof m.uuid) + "\n";
    std::snprintf(line, sizeof line, "pipelines %zu\n", m.entries.size());
    text += line;
    for (const auto& [path, p] : m.entries)
        text += "p " + path + " " + Describe(p) + "\n";
    std::snprintf(line, sizeof line, "end %016llx\n", static_cast<unsigned long long>(Hash(text.data(), text.size())));
    return text + line;
}

// Only the file itself: whole (its end line and checksum), this version, every line well formed. `missing` tells a
// file that is not there from one that is wrong; `why` says what is wrong.
bool Read(const std::wstring& path, Manifest& m, std::string& why, bool& missing)
{
    std::vector<char> bytes;
    missing = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (missing || !ReadAll(path, bytes))
    {
        why = missing ? "there is none" : "it cannot be read";
        return false;
    }
    const std::string text(bytes.begin(), bytes.end());
    if (text.size() < 2 || text.back() != '\n')
    {
        why = "it does not end with a whole line (truncated?)";
        return false;
    }
    const size_t newline = text.rfind('\n', text.size() - 2);
    const size_t last = newline == std::string::npos ? 0 : newline + 1;
    const std::string end = text.substr(last, text.size() - 1 - last);
    unsigned long long sum = 0;
    int used = 0;
    if (std::sscanf(end.c_str(), "end %16llx%n", &sum, &used) != 1 || used != int(end.size()) || end.size() != 20)
    {
        why = "its end line is missing (truncated?)";
        return false;
    }
    if (Hash(text.data(), last) != sum)
    {
        why = "its checksum does not match (truncated or edited?)";
        return false;
    }
    std::vector<std::string> lines;
    for (size_t at = 0; at < last;)
    {
        const size_t eol = text.find('\n', at);
        lines.push_back(text.substr(at, eol - at));
        at = eol + 1;
    }
    if (lines.empty() || lines[0] != kManifestVersion)
    {
        why = "it is not a \"" + std::string(kManifestVersion) + "\" manifest";
        return false;
    }
    unsigned long long folderHash = 0;
    unsigned files = 0, count = 0;
    char uuid[2 * VK_UUID_SIZE + 1] {};
    int shadersUsed = 0, countUsed = 0;
    if (lines.size() < 4 ||
        std::sscanf(lines[1].c_str(), "shaders %16llx %u%n", &folderHash, &files, &shadersUsed) != 2 ||
        shadersUsed != int(lines[1].size()) || lines[2].size() != 11 + 2 * VK_UUID_SIZE ||
        std::sscanf(lines[2].c_str(), "cache-uuid %32[0-9a-f]", uuid) != 1 || std::strlen(uuid) != 2 * VK_UUID_SIZE ||
        std::sscanf(lines[3].c_str(), "pipelines %u%n", &count, &countUsed) != 1 || countUsed != int(lines[3].size()) ||
        lines.size() != 4 + size_t(count))
    {
        why = "its header is malformed";
        return false;
    }
    m = {};
    m.folderHash = folderHash;
    m.files = files;
    for (size_t i = 0; i < VK_UUID_SIZE; ++i)
    {
        unsigned b = 0;
        std::sscanf(uuid + 2 * i, "%2x", &b);
        m.uuid[i] = uint8_t(b);
    }
    for (size_t i = 4; i < lines.size(); ++i)
    {
        const std::string& l = lines[i];
        const size_t space = l.find(' ', 2);
        Pipeline p;
        if (l.compare(0, 2, "p ") || space == std::string::npos || !ValidPath(l.substr(2, space - 2)) ||
            !Parse(l.substr(space + 1), p))
        {
            why = "line " + std::to_string(i + 1) + " is malformed";
            return false;
        }
        m.entries.emplace_back(l.substr(2, space - 2), std::move(p));
    }
    return true;
}

// The manifest against the shaders as they are now: made for the same folders (file list and sizes), and every file
// it names still has the module's bytes. With `code`, those bytes are kept, by path.
bool Check(const Manifest& m, const Folder& folder, std::string& why,
           std::map<std::string, std::vector<uint32_t>>* code)
{
    if (m.folderHash != folder.hash || m.files != folder.files.size())
    {
        why = "it was made for other shaders (the file list or sizes of dlssnr-amd\\shaders differ)";
        return false;
    }
    std::map<std::string, std::vector<uint32_t>> read;
    for (const auto& [path, p] : m.entries)
    {
        if (path == "-" || read.count(path))
            continue;
        const File* f = folder.Find(path);
        std::vector<char> bytes;
        if (!f || f->bytes != p.module.bytes || p.module.bytes < 20 || p.module.bytes % 4 || !ReadAll(f->path, bytes) ||
            Hash(bytes.data(), bytes.size()) != p.module.hash)
        {
            why = "shaders/" + path + " is not the module it lists";
            return false;
        }
        std::vector<uint32_t>& words = read[path];
        if (code)
        {
            words.resize(bytes.size() / 4);
            std::memcpy(words.data(), bytes.data(), bytes.size());
        }
    }
    if (code)
        *code = std::move(read);
    return true;
}

bool SameUuid(const uint8_t* a, const uint8_t* b) { return !std::memcmp(a, b, VK_UUID_SIZE); }

void CacheUuid(VkPhysicalDevice physical, uint8_t* uuid)
{
    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(physical, &props);
    std::memcpy(uuid, props.pipelineCacheUUID, VK_UUID_SIZE);
}

// MZ_COMPILE_THREADS, or min(8, logical processors - 2).
uint32_t CompileThreads()
{
    const unsigned hw = std::thread::hardware_concurrency();
    uint32_t n = hw > 3 ? std::min(8u, hw - 2) : 1;
    if (const char* value = std::getenv("MZ_COMPILE_THREADS"); value && *value)
    {
        char* end = nullptr;
        const unsigned long wanted = std::strtoul(value, &end, 10);
        if (end != value && !*end && wanted >= 1)
            n = uint32_t(std::min(wanted, 64ul));
        else
            nr::logf("[mochizuki] MZ_COMPILE_THREADS=%s is not a thread count; %u threads are used", value, n);
    }
    return n;
}

// The processor time a thread has used, in 100 ns units, counted per clock tick (about 15.6 ms); 0 if unknown.
uint64_t CpuTime(HANDLE thread)
{
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user))
        return 0;
    return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
           (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
}

// One manifest pipeline, made into `cache` and destroyed again: what the core will make, down to the create info.
VkResult Compile(VkDevice device, VkPipelineCache cache, const Pipeline& p, const std::vector<uint32_t>& code)
{
    std::vector<VkDescriptorSetLayout> sets(p.layout.sets.size(), VK_NULL_HANDLE);
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = VK_SUCCESS;
    for (size_t s = 0; s < sets.size() && r == VK_SUCCESS; ++s)
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const Binding& b : p.layout.sets[s].bindings)
            bindings.push_back({ b.binding, b.type, b.count, b.stages, nullptr });
        VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.flags = p.layout.sets[s].flags;
        li.bindingCount = uint32_t(bindings.size());
        li.pBindings = bindings.empty() ? nullptr : bindings.data();
        r = vkCreateDescriptorSetLayout(device, &li, nullptr, &sets[s]);
    }
    if (r == VK_SUCCESS)
    {
        VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.flags = p.layout.flags;
        pli.setLayoutCount = uint32_t(sets.size());
        pli.pSetLayouts = sets.empty() ? nullptr : sets.data();
        pli.pushConstantRangeCount = uint32_t(p.layout.push.size());
        pli.pPushConstantRanges = p.layout.push.empty() ? nullptr : p.layout.push.data();
        r = vkCreatePipelineLayout(device, &pli, nullptr, &layout);
    }
    if (r == VK_SUCCESS)
    {
        VkShaderModuleCreateInfo smi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = code.size() * 4;
        smi.pCode = code.data();
        r = vkCreateShaderModule(device, &smi, nullptr, &module);
    }
    if (r == VK_SUCCESS)
    {
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO
        };
        required.requiredSubgroupSize = p.subgroup;
        VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.flags = p.flags;
        cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        cpi.stage.pNext = p.subgroup ? &required : nullptr;
        cpi.stage.flags = p.stageFlags;
        cpi.stage.stage = p.stage;
        cpi.stage.module = module;
        cpi.stage.pName = p.entry.c_str();
        cpi.layout = layout;
        r = vkCreateComputePipelines(device, cache, 1, &cpi, nullptr, &pipeline);
    }
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, module, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    for (VkDescriptorSetLayout set : sets)
        vkDestroyDescriptorSetLayout(device, set, nullptr);
    return r;
}

template <class T> uint64_t Key(T handle) { return reinterpret_cast<uint64_t>(handle); }
} // namespace

struct mzi::Capture::State
{
    VkDevice device {};
    VkPhysicalDevice physical {};
    std::wstring data; // root\dlssnr-amd
    State* outer = nullptr;

    // What the core made on this thread since the Capture opened.
    std::unordered_map<uint64_t, Module> modules;
    std::unordered_map<uint64_t, SetLayoutInfo> setLayouts;
    std::unordered_map<uint64_t, Layout> layouts;
    std::vector<Pipeline> pipelines;
    std::unordered_set<std::string> described; // Describe of each in pipelines
    std::string refused;                       // the first create info the manifest cannot describe
    VkPipelineCache cache {};                  // the core's, until it destroys it
    uint32_t created = 0;                      // the core's vkCreateComputePipelines calls' pipelines...
    double createSeconds = 0;                  // ... and the time in them: a miss in the cache is a compile

    // Prewarm's reading of the manifest, which Finish uses rather than reading it again.
    bool checked = false;
    Manifest manifest;

    std::wstring ManifestPath() const { return data + L"\\prewarm\\manifest.txt"; }
    std::wstring CachePath() const { return data + L"\\pipeline.cache"; }

    void Refuse(const char* what)
    {
        if (refused.empty())
            refused = what;
    }

    void Prewarm(const std::atomic<bool>* stop);
    void SaveCache();
    void WriteManifest();
};

namespace
{
thread_local mzi::Capture::State* t_capture = nullptr;

// Each Record* runs in an mzi_* call from the core: nothing may escape it.
template <class F> void Record(F&& record) noexcept
{
    try
    {
        record(*t_capture);
    }
    catch (...)
    {
        t_capture->Refuse("out of memory while recording");
    }
}
} // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL mzi_vkCreateShaderModule(VkDevice device,
                                                                   const VkShaderModuleCreateInfo* pCreateInfo,
                                                                   const VkAllocationCallbacks* pAllocator,
                                                                   VkShaderModule* pShaderModule)
{
    const VkResult r = vkCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
    if (r == VK_SUCCESS && t_capture)
        Record(
            [&](mzi::Capture::State& s)
            {
                if (pCreateInfo->pNext || pCreateInfo->flags)
                    s.Refuse("a shader module with flags or a pNext chain");
                s.modules[Key(*pShaderModule)] = { Hash(pCreateInfo->pCode, pCreateInfo->codeSize),
                                                   pCreateInfo->codeSize };
            });
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
mzi_vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pSetLayout)
{
    const VkResult r = vkCreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pSetLayout);
    if (r == VK_SUCCESS && t_capture)
        Record(
            [&](mzi::Capture::State& s)
            {
                if (pCreateInfo->pNext)
                    s.Refuse("a descriptor set layout with a pNext chain");
                SetLayoutInfo set;
                set.flags = pCreateInfo->flags;
                for (uint32_t i = 0; i < pCreateInfo->bindingCount; ++i)
                {
                    const VkDescriptorSetLayoutBinding& b = pCreateInfo->pBindings[i];
                    if (b.pImmutableSamplers && b.descriptorCount &&
                        (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                         b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
                        s.Refuse("immutable samplers");
                    set.bindings.push_back({ b.binding, b.descriptorType, b.descriptorCount, b.stageFlags });
                }
                s.setLayouts[Key(*pSetLayout)] = std::move(set);
            });
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL mzi_vkCreatePipelineLayout(VkDevice device,
                                                                     const VkPipelineLayoutCreateInfo* pCreateInfo,
                                                                     const VkAllocationCallbacks* pAllocator,
                                                                     VkPipelineLayout* pPipelineLayout)
{
    const VkResult r = vkCreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout);
    if (r == VK_SUCCESS && t_capture)
        Record(
            [&](mzi::Capture::State& s)
            {
                if (pCreateInfo->pNext)
                    s.Refuse("a pipeline layout with a pNext chain");
                Layout layout;
                layout.flags = pCreateInfo->flags;
                for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; ++i)
                {
                    const auto set = s.setLayouts.find(Key(pCreateInfo->pSetLayouts[i]));
                    if (set == s.setLayouts.end())
                    {
                        s.Refuse("a pipeline layout over a set layout made before the capture");
                        return;
                    }
                    layout.sets.push_back(set->second);
                }
                layout.push.assign(pCreateInfo->pPushConstantRanges,
                                   pCreateInfo->pPushConstantRanges + pCreateInfo->pushConstantRangeCount);
                s.layouts[Key(*pPipelineLayout)] = std::move(layout);
            });
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL mzi_vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                                       uint32_t createInfoCount,
                                                                       const VkComputePipelineCreateInfo* pCreateInfos,
                                                                       const VkAllocationCallbacks* pAllocator,
                                                                       VkPipeline* pPipelines)
{
    const auto t0 = t_capture ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
    const VkResult r =
        vkCreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    if (t_capture)
    {
        t_capture->createSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        t_capture->created += createInfoCount;
    }
    if (r == VK_SUCCESS && t_capture)
        Record(
            [&](mzi::Capture::State& s)
            {
                for (uint32_t i = 0; i < createInfoCount; ++i)
                {
                    const VkComputePipelineCreateInfo& ci = pCreateInfos[i];
                    Pipeline p;
                    p.flags = ci.flags;
                    p.stage = ci.stage.stage;
                    p.stageFlags = ci.stage.flags;
                    for (auto* next = static_cast<const VkBaseInStructure*>(ci.stage.pNext); next; next = next->pNext)
                    {
                        if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO &&
                            !p.subgroup)
                            p.subgroup =
                                reinterpret_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*>(next)
                                    ->requiredSubgroupSize;
                        else
                            s.Refuse("a shader stage with another pNext than its required subgroup size");
                    }
                    const auto module = s.modules.find(Key(ci.stage.module));
                    const auto layout = s.layouts.find(Key(ci.layout));
                    if (ci.pNext)
                        s.Refuse("a compute pipeline with a pNext chain");
                    else if (ci.stage.pSpecializationInfo)
                        s.Refuse("specialization constants");
                    else if (ci.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT)
                        s.Refuse("a derivative pipeline");
                    else if (!Identifier(ci.stage.pName))
                        s.Refuse("an entry point name the manifest cannot carry");
                    else if (module == s.modules.end() || layout == s.layouts.end())
                        s.Refuse("a pipeline whose module or layout was made before the capture");
                    if (!s.refused.empty())
                        return;
                    p.module = module->second;
                    p.entry = ci.stage.pName;
                    p.layout = layout->second;
                    if (s.described.insert(Describe(p)).second)
                        s.pipelines.push_back(std::move(p));
                }
            });
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL mzi_vkCreatePipelineCache(VkDevice device,
                                                                    const VkPipelineCacheCreateInfo* pCreateInfo,
                                                                    const VkAllocationCallbacks* pAllocator,
                                                                    VkPipelineCache* pPipelineCache)
{
    const VkResult r = vkCreatePipelineCache(device, pCreateInfo, pAllocator, pPipelineCache);
    if (r == VK_SUCCESS && t_capture && device == t_capture->device)
        t_capture->cache = *pPipelineCache;
    return r;
}

extern "C" VKAPI_ATTR void VKAPI_CALL mzi_vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache,
                                                                 const VkAllocationCallbacks* pAllocator)
{
    if (t_capture && pipelineCache && pipelineCache == t_capture->cache)
        t_capture->cache = VK_NULL_HANDLE;
    vkDestroyPipelineCache(device, pipelineCache, pAllocator);
}

namespace mzi
{
Capture::Capture(VkDevice device, VkPhysicalDevice physical, const std::wstring& root) noexcept
{
    try
    {
        const std::wstring data = root + L"\\dlssnr-amd";
        const DWORD attributes = root.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(data.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            return;
        state = std::make_unique<State>();
        state->device = device;
        state->physical = physical;
        state->data = data;
        state->outer = t_capture;
        t_capture = state.get();
    }
    catch (...)
    {
        state.reset();
    }
}

Capture::~Capture()
{
    if (state && t_capture == state.get())
        t_capture = state->outer;
}

void Capture::Prewarm(const std::atomic<bool>* stop) noexcept
{
    if (!state)
        return;
    try
    {
        state->Prewarm(stop);
    }
    catch (const std::exception& e)
    {
        nr::logf("[mochizuki] prewarm failed (%s); the pipelines are built serially", e.what());
    }
    catch (...)
    {
        nr::logf("[mochizuki] prewarm failed; the pipelines are built serially");
    }
}

void Capture::Finish() noexcept
{
    if (!state)
        return;
    if (state->created)
        nr::logf("[mochizuki] the core made %u pipelines (%zu distinct) in %.2f s", state->created,
                 state->pipelines.size(), state->createSeconds);
    try
    {
        state->SaveCache();
    }
    catch (...)
    {
        nr::logf("[mochizuki] pipeline.cache could not be written after the adapters (out of memory)");
    }
    try
    {
        state->WriteManifest();
    }
    catch (...)
    {
        nr::logf("[mochizuki] the prewarm manifest could not be written (out of memory)");
    }
}

void Capture::State::Prewarm(const std::atomic<bool>* stop)
{
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t threads = CompileThreads();
    if (threads <= 1)
    {
        nr::logf("[mochizuki] prewarm off (one compile thread); the pipelines are built serially");
        return;
    }
    const Folder folder = Scan(data + L"\\shaders");
    std::string why;
    bool missing = false;
    std::map<std::string, std::vector<uint32_t>> code;
    if (!Read(ManifestPath(), manifest, why, missing) || !Check(manifest, folder, why, &code))
    {
        if (missing)
            nr::logf("[mochizuki] no prewarm manifest yet; the pipelines are built serially");
        else
            nr::logf("[mochizuki] prewarm manifest ignored: %s; the pipelines are built serially", why.c_str());
        manifest = {};
        return;
    }
    checked = true;

    // Largest module first: the longest compiles start at once, and the small ones fill in behind them.
    struct Job
    {
        const Pipeline* pipeline;
        const std::vector<uint32_t>* code;
    };
    std::vector<Job> jobs;
    for (const auto& [path, p] : manifest.entries)
        if (path != "-")
            jobs.push_back({ &p, &code.at(path) });
    std::stable_sort(jobs.begin(), jobs.end(),
                     [](const Job& a, const Job& b) { return a.pipeline->module.bytes > b.pipeline->module.bytes; });
    // Allocated before the cache is made, so that nothing between its creation and its destruction throws.
    const size_t wanted = std::min<size_t>(threads, jobs.size());
    std::vector<std::thread> pool;
    pool.reserve(wanted);
    std::vector<char> done(wanted, 0), raised(wanted, 0);
    std::vector<uint64_t> used(wanted, 0);
    uint8_t uuid[VK_UUID_SIZE];
    CacheUuid(physical, uuid);
    const char* otherDriver = SameUuid(uuid, manifest.uuid) ? "" : " (recorded with another driver)";

    // Into a cache loaded from pipeline.cache, so that what it holds already is found rather than compiled again.
    const std::wstring cachePath = CachePath();
    std::vector<char> initial;
    if (!ReadAll(cachePath, initial) || initial.size() < 32)
        initial.clear();
    VkPipelineCacheCreateInfo ci { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = initial.size();
    ci.pInitialData = initial.empty() ? nullptr : initial.data();
    VkPipelineCache cache {};
    if (vkCreatePipelineCache(device, &ci, nullptr, &cache) != VK_SUCCESS)
    {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        if (vkCreatePipelineCache(device, &ci, nullptr, &cache) != VK_SUCCESS)
        {
            nr::logf("[mochizuki] prewarm skipped: no pipeline cache could be made; the pipelines are built serially");
            return;
        }
    }

    std::atomic<size_t> next { 0 };
    std::atomic<uint32_t> made { 0 }, failed { 0 };
    std::atomic<int32_t> firstError { VK_SUCCESS };
    // The next pipeline to compile; false once none is left to hand out, or when the build is being stopped.
    auto take = [&](size_t& i) noexcept
    {
        if (stop && stop->load(std::memory_order_relaxed))
            return false;
        i = next++;
        return i < jobs.size();
    };
    auto compile = [&](size_t i) noexcept
    {
        VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
        try
        {
            r = Compile(device, cache, *jobs[i].pipeline, *jobs[i].code);
        }
        catch (...)
        {
        }
        if (r == VK_SUCCESS)
            ++made;
        else
        {
            ++failed;
            int32_t none = VK_SUCCESS;
            firstError.compare_exchange_strong(none, r);
        }
    };

    // The workers compile below normal priority, so that they give way to the game's threads. They get no processor
    // at all while normal-priority threads keep every one busy (a game compiling its own shaders), so this thread, the
    // one the serial path compiles on, watches the processor time they use. When, over a quarter of a second, they
    // together got less than half a processor, it compiles the next pipeline itself at its own priority, as the serial
    // path would; once none is left to hand out, it raises the starved workers to its own priority for the pipeline
    // each is still compiling, since the build waits for nothing else. With a processor to spare, everything is
    // compiled below normal.
    int own = GetThreadPriority(GetCurrentThread());
    if (own == THREAD_PRIORITY_ERROR_RETURN)
        own = THREAD_PRIORITY_NORMAL;
    const int below = std::min(own, int(THREAD_PRIORITY_BELOW_NORMAL));
    std::mutex mutex;
    std::condition_variable ended;
    for (size_t k = 0; k < wanted; ++k)
    {
        try
        {
            pool.emplace_back(
                [&, k]() noexcept
                {
                    for (size_t i; take(i);)
                        compile(i);
                    std::lock_guard lock(mutex);
                    done[k] = 1;
                    ended.notify_one();
                });
        }
        catch (...)
        {
            break;
        }
        SetThreadPriority(pool.back().native_handle(), below);
    }
    const uint64_t ownStart = CpuTime(GetCurrentThread());
    uint32_t helped = 0, lifted = 0;
    {
        const auto window = std::chrono::milliseconds(250);
        auto mark = std::chrono::steady_clock::now();
        for (size_t k = 0; k < pool.size(); ++k)
            used[k] = CpuTime(pool[k].native_handle());
        auto allDone = [&]
        { return std::all_of(done.begin(), done.begin() + pool.size(), [](char d) { return d != 0; }); };
        std::unique_lock lock(mutex);
        while (!ended.wait_until(lock, mark + window, allDone))
        {
            lock.unlock();
            const auto now = std::chrono::steady_clock::now();
            uint64_t got = 0;
            for (size_t k = 0; k < pool.size(); ++k)
            {
                const uint64_t t = CpuTime(pool[k].native_handle());
                got += t > used[k] ? t - used[k] : 0;
                used[k] = t;
            }
            const auto span = std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count() / 100;
            mark = now;
            bool lift = false;
            if (2 * got < uint64_t(span))
            {
                size_t i;
                if (take(i))
                {
                    compile(i);
                    ++helped;
                }
                else
                    lift = true;
            }
            lock.lock();
            if (lift)
                for (size_t k = 0; k < pool.size(); ++k)
                    if (!done[k] && !raised[k])
                    {
                        SetThreadPriority(pool[k].native_handle(), own);
                        raised[k] = 1;
                        ++lifted;
                    }
        }
    }
    uint64_t cpu = 0;
    for (std::thread& t : pool)
        cpu += CpuTime(t.native_handle());
    for (std::thread& t : pool)
        t.join();
    // Without a single worker, this thread compiles them all.
    for (size_t i; take(i);)
    {
        compile(i);
        ++helped;
    }
    if (const uint64_t ownEnd = CpuTime(GetCurrentThread()); ownEnd > ownStart)
        cpu += ownEnd - ownStart;

    size_t bytes = 0;
    bool written = false;
    if (made && vkGetPipelineCacheData(device, cache, &bytes, nullptr) == VK_SUCCESS && bytes &&
        bytes != initial.size())
    {
        try
        {
            std::vector<char> blob(bytes);
            written = vkGetPipelineCacheData(device, cache, &bytes, blob.data()) == VK_SUCCESS &&
                      WriteReplace(cachePath, blob.data(), bytes);
        }
        catch (...)
        {
        }
        if (!written)
            nr::logf("[mochizuki] prewarm: pipeline.cache could not be written");
    }
    vkDestroyPipelineCache(device, cache, nullptr);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char starved[128] = {};
    if (!pool.empty() && (helped || lifted))
        std::snprintf(starved, sizeof starved,
                      "; starved below normal: %u compiled on the builder thread, %u raised to finish", helped, lifted);
    char failures[96] = {};
    if (failed)
        std::snprintf(failures, sizeof failures, "; %u failed (first VkResult %d)", failed.load(),
                      int(firstError.load()));
    nr::logf("[mochizuki] prewarm %u pipelines on %zu threads in %.2f s%s (pipeline.cache %s, %.1f MB; %.1f s of "
             "processor time)%s%s",
             made.load(), std::max<size_t>(pool.size(), 1), seconds, otherDriver, written ? "written" : "unchanged",
             double(written ? bytes : initial.size()) / 1e6, double(cpu) / 1e7, starved, failures);
}

void Capture::State::SaveCache()
{
    if (!cache)
        return;
    const std::wstring path = CachePath();
    size_t bytes = 0;
    if (vkGetPipelineCacheData(device, cache, &bytes, nullptr) != VK_SUCCESS || !bytes || bytes == FileBytes(path))
        return;
    std::vector<char> blob(bytes);
    if (vkGetPipelineCacheData(device, cache, &bytes, blob.data()) == VK_SUCCESS &&
        WriteReplace(path, blob.data(), bytes))
        nr::logf("[mochizuki] pipeline.cache written after the adapter and temporal pipelines (%.1f MB)",
                 double(bytes) / 1e6);
    else
        nr::logf("[mochizuki] pipeline.cache could not be written after the adapter and temporal pipelines");
}

void Capture::State::WriteManifest()
{
    if (!refused.empty())
    {
        nr::logf("[mochizuki] prewarm manifest not written: the build made %s", refused.c_str());
        return;
    }
    if (pipelines.empty())
        return;
    const Folder folder = Scan(data + L"\\shaders");
    uint8_t uuid[VK_UUID_SIZE];
    CacheUuid(physical, uuid);

    // Up to date: whole, for these shaders, from this driver, and it lists every pipeline of this build.
    Manifest old;
    std::string why;
    bool missing = false;
    const bool whole = checked || Read(ManifestPath(), old, why, missing);
    if (checked)
        old = manifest;
    const bool current = whole && (checked || Check(old, folder, why, nullptr));
    std::unordered_set<std::string> listed;
    for (const auto& entry : old.entries)
        listed.insert(Describe(entry.second));
    if (current && SameUuid(uuid, old.uuid) &&
        std::all_of(described.begin(), described.end(), [&](const std::string& d) { return listed.count(d); }))
        return;

    // The union of what it lists for these shaders and this build's pipelines, each named by the file that has its
    // module's bytes now.
    std::map<std::pair<uint64_t, uint64_t>, std::string> byModule; // the first file in byte order wins
    for (const File& f : folder.files)
    {
        if (f.rel.size() < 4 || _stricmp(f.rel.c_str() + f.rel.size() - 4, ".spv") || !ValidPath(f.rel))
            continue;
        std::vector<char> bytes;
        if (ReadAll(f.path, bytes))
            byModule.emplace(std::make_pair(Hash(bytes.data(), bytes.size()), uint64_t(bytes.size())), f.rel);
    }
    auto fileOf = [&](const Module& m)
    {
        const auto at = byModule.find({ m.hash, m.bytes });
        return at == byModule.end() ? std::string("-") : at->second;
    };
    Manifest m;
    m.folderHash = folder.hash;
    m.files = uint32_t(folder.files.size());
    std::memcpy(m.uuid, uuid, sizeof uuid);
    std::unordered_set<std::string> seen;
    uint32_t added = 0, unnamed = 0;
    if (whole && old.folderHash == folder.hash)
        for (auto& [path, p] : old.entries)
        {
            const std::string file = fileOf(p.module);
            if (file != "-" && seen.insert(Describe(p)).second)
                m.entries.emplace_back(file, p);
        }
    for (const Pipeline& p : pipelines)
        if (seen.insert(Describe(p)).second)
        {
            m.entries.emplace_back(fileOf(p.module), p);
            ++added;
            unnamed += m.entries.back().first == "-";
        }
    std::stable_sort(m.entries.begin(), m.entries.end(),
                     [](const auto& a, const auto& b) { return a.second.module.bytes > b.second.module.bytes; });
    CreateDirectoryW((data + L"\\prewarm").c_str(), nullptr);
    const std::string text = Write(m);
    if (!WriteReplace(ManifestPath(), text.data(), text.size()))
    {
        nr::logf("[mochizuki] the prewarm manifest could not be written");
        return;
    }
    char note[96] = {};
    if (unnamed)
        std::snprintf(note, sizeof note, "; %u have no file in dlssnr-amd\\shaders and are not prewarmed", unnamed);
    nr::logf("[mochizuki] prewarm manifest written: %zu pipelines, %u of them new%s", m.entries.size(), added, note);
}

namespace
{
bool ProbeWanted()
{
    static const bool wanted = []
    {
        const char* value = std::getenv("MZ_PROBE_PIPELINE_BINARY");
        return value && !std::strcmp(value, "1");
    }();
    return wanted;
}
} // namespace

void* BinaryProbe::Prepare(VkPhysicalDevice physical, const std::vector<VkExtensionProperties>& offered,
                           std::vector<const char*>& extensions, void* next)
{
    if (!ProbeWanted())
        return next;
    auto has = [&](const char* name)
    {
        return std::any_of(offered.begin(), offered.end(),
                           [name](const VkExtensionProperties& p) { return !std::strcmp(p.extensionName, name); });
    };
    for (const char* e : { VK_KHR_MAINTENANCE_5_EXTENSION_NAME, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME })
        if (!has(e))
        {
            nr::logf("[mochizuki] pipeline binary probe: the device does not offer %s; not probed", e);
            return next;
        }
    VkPhysicalDevicePipelineBinaryFeaturesKHR hasBinaries {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR
    };
    VkPhysicalDeviceMaintenance5FeaturesKHR hasMaintenance5 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &hasBinaries
    };
    VkPhysicalDeviceFeatures2 features { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &hasMaintenance5 };
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!hasMaintenance5.maintenance5 || !hasBinaries.pipelineBinaries)
    {
        nr::logf("[mochizuki] pipeline binary probe: maintenance5 %u, pipelineBinaries %u; not probed",
                 hasMaintenance5.maintenance5, hasBinaries.pipelineBinaries);
        return next;
    }
    extensions.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    extensions.push_back(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME);
    maintenance5.maintenance5 = VK_TRUE;
    binaries.pipelineBinaries = VK_TRUE;
    maintenance5.pNext = &binaries;
    binaries.pNext = next;
    enabled = true;
    return &maintenance5;
}

void BinaryProbe::Report(VkDevice device, VkPhysicalDevice physical) const
{
    if (!enabled)
        return;
    std::wstring exe(MAX_PATH, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size())));
    VkPhysicalDevicePipelineBinaryPropertiesKHR binary {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_PROPERTIES_KHR
    };
    VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &binary };
    vkGetPhysicalDeviceProperties2(physical, &props);
    const auto getKey = reinterpret_cast<PFN_vkGetPipelineKeyKHR>(vkGetDeviceProcAddr(device, "vkGetPipelineKeyKHR"));
    VkPipelineBinaryKeyKHR key { VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
    const VkResult r = getKey ? getKey(device, nullptr, &key) : VK_ERROR_EXTENSION_NOT_PRESENT;
    nr::logf("[mochizuki] pipeline binary probe: exe %s, global key %s (%u bytes, VkResult %d), pipelineCacheUUID %s, "
             "driver version 0x%08x; internal cache %u, control %u, prefers internal %u, precompiled %u, "
             "compressed %u",
             Utf8(exe).c_str(), r == VK_SUCCESS ? Hex(key.key, std::min<uint32_t>(key.keySize, 32)).c_str() : "-",
             key.keySize, int(r), Hex(props.properties.pipelineCacheUUID, VK_UUID_SIZE).c_str(),
             props.properties.driverVersion, binary.pipelineBinaryInternalCache,
             binary.pipelineBinaryInternalCacheControl, binary.pipelineBinaryPrefersInternalCache,
             binary.pipelineBinaryPrecompiledInternalCache, binary.pipelineBinaryCompressedData);
}
} // namespace mzi
