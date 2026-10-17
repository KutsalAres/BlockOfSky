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

// ------------------------------------------- RenderLevel (sadece gozlem)

// Tum tamsayi/float register'lari aynen iletilir; imza farkli olsa bile arguman kaybolmaz.
using RenderLevelFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*,
                                float, float, float, float);

RenderLevelFn g_renderLevelOriginal = nullptr;
bedrocktools::hooks::Handle g_renderLevelHandle = nullptr;
std::atomic<std::uint32_t> g_renderCalls{0};

struct RenderSnapshot {
    std::uintptr_t arg0 = 0, arg1 = 0, arg2 = 0;
    std::uintptr_t tessellator = 0;
    std::uintptr_t levelRendererPlayer = 0;
    float cam[3] = {0, 0, 0};
    bool valid = false;
};
RenderSnapshot g_snapshot;

void* renderLevelDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6,
                        void* a7, float f0, float f1, float f2, float f3) {
    void* result = g_renderLevelOriginal
                       ? g_renderLevelOriginal(a0, a1, a2, a3, a4, a5, a6, a7, f0, f1, f2, f3)
                       : nullptr;

    const auto n = g_renderCalls.fetch_add(1, std::memory_order_relaxed);
    if (n % 120 != 0) return result;

    RenderSnapshot snap;
    snap.arg0 = reinterpret_cast<std::uintptr_t>(a0);
    snap.arg1 = reinterpret_cast<std::uintptr_t>(a1);
    snap.arg2 = reinterpret_cast<std::uintptr_t>(a2);

    const auto self = snap.arg0;
    const auto screenContext = snap.arg1;

    const auto tessAddr = screenContext + offsets::ScreenContext::mTessellator;
    if (isReadable(tessAddr, 8)) snap.tessellator = *reinterpret_cast<const std::uintptr_t*>(tessAddr);

    const auto lrpAddr = self + offsets::LevelRenderer::mLevelRendererPlayer;
    if (isReadable(lrpAddr, 8)) {
        snap.levelRendererPlayer = *reinterpret_cast<const std::uintptr_t*>(lrpAddr);
        const auto camAddr = snap.levelRendererPlayer + offsets::LevelRendererPlayer::mCamPos;
        if (isReadable(camAddr, 12)) {
            const auto* cam = reinterpret_cast<const float*>(camAddr);
            snap.cam[0] = cam[0];
            snap.cam[1] = cam[1];
            snap.cam[2] = cam[2];
            snap.valid = true;
        }
    }
    g_snapshot = snap;
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

    const auto snap = g_snapshot;
    out << "args: 0x" << std::hex << snap.arg0 << " 0x" << snap.arg1 << " 0x" << snap.arg2 << "\n"
        << "screenContext tessellator: 0x" << snap.tessellator << "\n"
        << "levelRendererPlayer:       0x" << snap.levelRendererPlayer << std::dec << "\n"
        << "camPos: " << snap.cam[0] << ", " << snap.cam[1] << ", " << snap.cam[2]
        << (snap.valid ? "" : "  (not read)") << "\n";

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
