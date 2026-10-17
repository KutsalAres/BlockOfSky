#include "SkyModule.hpp"

#include "config/ConfigManager.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/Block.hpp>

#include <android/log.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bedrocktools::memory::SignatureId;
namespace offsets = bedrocktools::sdk::offsets;

constexpr std::string_view kSkyBlockName = "bos:block_of_sky";
constexpr std::size_t kFaceCount = 6;

// 1.26.52.3: probe sonucuyla bulunan Tessellator::vertex(float x, float y, float z)
constexpr std::uintptr_t kVertexOffset = 0xa76f048;
constexpr std::array<std::uint8_t, 16> kVertexBytes = {
    0xFF, 0xC3, 0x02, 0xD1, 0xEA, 0x1B, 0x00, 0xFD,
    0xE9, 0x23, 0x04, 0x6D, 0xFD, 0x7B, 0x05, 0xA9};

std::filesystem::path g_dir;
std::uintptr_t g_base = 0;

// Bellek okunabilir mi? (mincore haritalanmamis sayfada hata doner, segfault vermez)
bool isReadable(std::uintptr_t addr, std::size_t len) {
    const std::uintptr_t untagged = addr & 0x00FFFFFFFFFFFFFFull;
    if (untagged < 0x10000) return false;
    const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    const auto start = untagged & ~(page - 1);
    const auto end = (untagged + len + page - 1) & ~(page - 1);
    const std::size_t pages = (end - start) / page;
    if (pages == 0 || pages > 4) return false;
    unsigned char vec[4];
    return mincore(reinterpret_cast<void*>(start), end - start, vec) == 0;
}

// ------------------------------------------------- sky blok konumlari

struct SkyPos {
    float rx, ry, rz;  // oyunun verdigi ham konum
    int x, y, z;       // floor
};

std::mutex g_posMutex;
std::vector<SkyPos> g_skyPositions;

void recordSkyPosition(const void* pos) {
    if (!pos) return;
    const auto* p = static_cast<const float*>(pos);
    SkyPos s{p[0], p[1], p[2],
             static_cast<int>(std::floor(p[0])),
             static_cast<int>(std::floor(p[1])),
             static_cast<int>(std::floor(p[2]))};
    std::lock_guard lock(g_posMutex);
    if (g_skyPositions.size() >= 256) return;
    for (const auto& e : g_skyPositions) {
        if (e.x == s.x && e.y == s.y && e.z == s.z) return;
    }
    g_skyPositions.push_back(s);
}

// ---------------------------------------------------------------- face hooks

// (tessellator, meshTessellator, block, position, texture)
using FaceFn = void (*)(void*, void*, const void*, const void*, const void*);

constexpr std::array<SignatureId, kFaceCount> kFaceIds = {
    SignatureId::BlockTessellatorTessellateFaceDown,
    SignatureId::BlockTessellatorTessellateFaceUp,
    SignatureId::BlockTessellatorTessellateFaceNorth,
    SignatureId::BlockTessellatorTessellateFaceSouth,
    SignatureId::BlockTessellatorTessellateFaceWest,
    SignatureId::BlockTessellatorTessellateFaceEast,
};

std::array<FaceFn, kFaceCount> g_original{};
std::array<bedrocktools::hooks::Handle, kFaceCount> g_handles{};
std::atomic_bool g_active{true};
bool g_facesInstalled = false;

bool isSkyBlock(const void* block) {
    if (!block) return false;
    const auto* obj = static_cast<const bedrocktools::sdk::Block*>(block);
    const std::string* name = obj->fullName();
    if (!name || name->data() == nullptr || name->size() != kSkyBlockName.size()) return false;
    return std::string_view(name->data(), name->size()) == kSkyBlockName;
}

template <std::size_t Face>
void faceDetour(void* tess, void* meshTess, const void* block, const void* pos, const void* tex) {
    if (g_active.load(std::memory_order_relaxed) && isSkyBlock(block)) {
        recordSkyPosition(pos);
        return;  // yuzler chunk'ta cizilmez
    }
    if (g_original[Face]) g_original[Face](tess, meshTess, block, pos, tex);
}

template <std::size_t... I>
constexpr std::array<FaceFn, kFaceCount> makeFaceDetours(std::index_sequence<I...>) {
    return {&faceDetour<I>...};
}

constexpr auto kFaceDetours = makeFaceDetours(std::make_index_sequence<kFaceCount>{});

void installFaceHooks() {
    if (g_facesInstalled) return;
    std::size_t installed = 0;
    for (std::size_t i = 0; i < kFaceCount; ++i) {
        const auto address = bedrocktools::memory::resolve(kFaceIds[i]);
        if (!address) continue;
        g_handles[i] = bedrocktools::hooks::install(
            reinterpret_cast<void*>(address),
            reinterpret_cast<void*>(kFaceDetours[i]),
            reinterpret_cast<void**>(&g_original[i]));
        if (g_handles[i]) ++installed;
    }
    g_facesInstalled = installed == kFaceCount;
    __android_log_print(ANDROID_LOG_INFO, "BlockOfSky", "face hooks: %zu/%zu", installed, kFaceCount);
}

// ------------------------------- Tessellator::begin (RenderLevel icindeki tessellator)

using BeginFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*);

BeginFn g_beginOriginal = nullptr;
bedrocktools::hooks::Handle g_beginHandle = nullptr;
thread_local bool t_inRenderLevel = false;
std::array<std::atomic<std::uintptr_t>, 16> g_renderTess{};

void noteRenderTessellator(std::uintptr_t p) {
    for (auto& slot : g_renderTess) {
        const auto cur = slot.load(std::memory_order_relaxed);
        if (cur == p) return;
        if (cur == 0) {
            std::uintptr_t expected = 0;
            if (slot.compare_exchange_strong(expected, p) || expected == p) return;
        }
    }
}

void* beginDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    if (t_inRenderLevel) noteRenderTessellator(reinterpret_cast<std::uintptr_t>(a0));
    return g_beginOriginal ? g_beginOriginal(a0, a1, a2, a3, a4, a5, a6, a7) : nullptr;
}

void installBeginHook() {
    const auto address = bedrocktools::memory::resolve(SignatureId::TessellatorBegin);
    if (!address) return;
    g_beginHandle = bedrocktools::hooks::install(
        reinterpret_cast<void*>(address),
        reinterpret_cast<void*>(&beginDetour),
        reinterpret_cast<void**>(&g_beginOriginal));
}

// ------------------------------------------------ ofset kesfi (sadece okuma)

std::mutex g_scanMutex;
std::string g_scanText;
int g_scanRuns = 0;

std::vector<SkyPos> copySkyPositions() {
    std::lock_guard lock(g_posMutex);
    return g_skyPositions;
}

bool nearSkyBlock(const float* f, const std::vector<SkyPos>& blocks) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(f[i]) || std::fabs(f[i]) > 30000.f) return false;
    }
    if (f[0] == 0.f && f[1] == 0.f && f[2] == 0.f) return false;
    for (const auto& b : blocks) {
        if (std::fabs(f[0] - static_cast<float>(b.x)) < 24.f &&
            std::fabs(f[1] - static_cast<float>(b.y)) < 24.f &&
            std::fabs(f[2] - static_cast<float>(b.z)) < 24.f) {
            return true;
        }
    }
    return false;
}

// [region, region+size) icinde sky bloklarina yakin 3'lu float ara (kamera konumu adayi)
void scanRegion(std::uintptr_t region, std::size_t size, const char* label, std::uintptr_t via,
                const std::vector<SkyPos>& blocks, std::string& text, int& found) {
    if (found >= 40 || !isReadable(region, size)) return;
    for (std::size_t off = 0; off + 12 <= size && found < 40; off += 4) {
        const auto* f = reinterpret_cast<const float*>(region + off);
        if (!nearSkyBlock(f, blocks)) continue;
        char line[160];
        std::snprintf(line, sizeof line, "  cam candidate %s+0x%zx -> +0x%zx: (%.2f, %.2f, %.2f)\n",
                      label, static_cast<std::size_t>(via), off, f[0], f[1], f[2]);
        text += line;
        ++found;
    }
}

void scanForCamera(std::uintptr_t root, const char* label, const std::vector<SkyPos>& blocks,
                   std::string& text, int& found) {
    if (!isReadable(root, 0x800)) return;
    scanRegion(root, 0x800, label, 0, blocks, text, found);  // dogrudan
    for (std::size_t off = 0; off < 0x800 && found < 40; off += 8) {
        const auto ptr = *reinterpret_cast<const std::uintptr_t*>(root + off);
        if ((ptr & 0x00FFFFFFFFFFFFFFull) < 0x10000 || ptr == root) continue;
        scanRegion(ptr, 0x1000, label, off, blocks, text, found);  // 1 seviye isaretci
    }
}

void markTessellatorOffsets(std::uintptr_t root, const char* label,
                            const std::array<std::uintptr_t, 16>& tess, std::string& text) {
    if (!isReadable(root, 0x400)) return;
    for (std::size_t off = 0; off < 0x400; off += 8) {
        const auto word = *reinterpret_cast<const std::uintptr_t*>(root + off);
        if (word == 0) continue;
        for (const auto t : tess) {
            if (t != 0 && word == t) {
                char line[160];
                std::snprintf(line, sizeof line,
                              "  %s+0x%zx == tessellator used inside RenderLevel (0x%llx)\n", label,
                              off, static_cast<unsigned long long>(t));
                text += line;
            }
        }
    }
}

void runDiscovery(std::uintptr_t levelRenderer, std::uintptr_t screenContext) {
    const auto blocks = copySkyPositions();
    std::array<std::uintptr_t, 16> tess{};
    for (std::size_t i = 0; i < tess.size(); ++i) tess[i] = g_renderTess[i].load();

    std::string text;
    char head[96];
    text += "tessellators seen inside RenderLevel:";
    for (const auto t : tess) {
        if (!t) continue;
        std::snprintf(head, sizeof head, " 0x%llx", static_cast<unsigned long long>(t));
        text += head;
    }
    text += "\n";
    markTessellatorOffsets(screenContext, "ScreenContext", tess, text);
    markTessellatorOffsets(levelRenderer, "LevelRenderer", tess, text);

    int found = 0;
    if (!blocks.empty()) {
        scanForCamera(levelRenderer, "LevelRenderer", blocks, text, found);
        scanForCamera(screenContext, "ScreenContext", blocks, text, found);
    } else {
        text += "  (no sky blocks seen yet, camera scan skipped)\n";
    }

    std::lock_guard lock(g_scanMutex);
    g_scanText = std::move(text);
}

// ------------------------------------------------- RenderLevel (sadece gozlem)

// Tum tamsayi/float register'lari aynen iletilir; imza farkli olsa bile arguman kaybolmaz.
using RenderLevelFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*,
                                float, float, float, float);

RenderLevelFn g_renderLevelOriginal = nullptr;
bedrocktools::hooks::Handle g_renderLevelHandle = nullptr;
std::atomic<std::uint32_t> g_renderCalls{0};
std::atomic<std::uintptr_t> g_arg0{0}, g_arg1{0}, g_arg2{0};

void* renderLevelDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6,
                        void* a7, float f0, float f1, float f2, float f3) {
    t_inRenderLevel = true;
    void* result = g_renderLevelOriginal
                       ? g_renderLevelOriginal(a0, a1, a2, a3, a4, a5, a6, a7, f0, f1, f2, f3)
                       : nullptr;
    t_inRenderLevel = false;

    const auto n = g_renderCalls.fetch_add(1, std::memory_order_relaxed);
    g_arg0.store(reinterpret_cast<std::uintptr_t>(a0), std::memory_order_relaxed);
    g_arg1.store(reinterpret_cast<std::uintptr_t>(a1), std::memory_order_relaxed);
    g_arg2.store(reinterpret_cast<std::uintptr_t>(a2), std::memory_order_relaxed);

    if (n % 600 == 300 && g_scanRuns < 6) {
        ++g_scanRuns;
        runDiscovery(reinterpret_cast<std::uintptr_t>(a0), reinterpret_cast<std::uintptr_t>(a1));
    }
    return result;
}

void installRenderLevelHook() {
    const auto address = bedrocktools::memory::resolve(SignatureId::RenderLevel);
    if (!address) return;
    g_renderLevelHandle = bedrocktools::hooks::install(
        reinterpret_cast<void*>(address),
        reinterpret_cast<void*>(&renderLevelDetour),
        reinterpret_cast<void**>(&g_renderLevelOriginal));
    __android_log_print(ANDROID_LOG_INFO, "BlockOfSky", "RenderLevel hook: %s",
                        g_renderLevelHandle ? "OK" : "FAILED");
}

// ------------------------------------------------------------------ rapor

void writeReport() {
    std::ofstream out(g_dir / "sky_render.txt");
    if (!out) return;

    bool vertexOk = false;
    if (g_base) {
        vertexOk = std::memcmp(reinterpret_cast<const void*>(g_base + kVertexOffset),
                               kVertexBytes.data(), kVertexBytes.size()) == 0;
    }
    out << "vertex bytes at base+0x" << std::hex << kVertexOffset << std::dec << ": "
        << (vertexOk ? "MATCH" : "MISMATCH") << "\n";
    out << "RenderLevel hook: " << (g_renderLevelHandle ? "yes" : "NO")
        << " calls=" << g_renderCalls.load() << "\n";

    out << "begin hook: " << (g_beginHandle ? "yes" : "NO") << "\n";
    out << "RenderLevel args: 0x" << std::hex << g_arg0.load() << " 0x" << g_arg1.load()
        << " 0x" << g_arg2.load() << std::dec << "\n";
    {
        std::lock_guard scanLock(g_scanMutex);
        out << "discovery runs: " << g_scanRuns << "\n" << g_scanText;
    }

    std::lock_guard lock(g_posMutex);
    out << "sky blocks seen: " << g_skyPositions.size() << "\n";
    std::size_t shown = 0;
    for (const auto& s : g_skyPositions) {
        if (shown++ >= 16) break;
        out << "  raw=(" << s.rx << ", " << s.ry << ", " << s.rz << ")  floor=("
            << s.x << ", " << s.y << ", " << s.z << ")\n";
    }
}

}  // namespace

void SkyModule::onInit() {
    g_dir = std::filesystem::path(
        bedrocktools::config::ConfigManager::get().getConfigPath()).parent_path();

    Dl_info info{};
    const auto begin = bedrocktools::memory::resolve(SignatureId::TessellatorBegin);
    if (begin && dladdr(reinterpret_cast<void*>(begin), &info) && info.dli_fbase) {
        g_base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    }

    installFaceHooks();
    installBeginHook();
    installRenderLevelHook();
    setMasterEnabled(true);
}

void SkyModule::onFrame() {
    static unsigned frame = 0;
    ++frame;
    if (frame % 600 == 0) writeReport();
}

void SkyModule::onEnable() { g_active.store(true, std::memory_order_relaxed); }

void SkyModule::onDisable() { g_active.store(false, std::memory_order_relaxed); }
