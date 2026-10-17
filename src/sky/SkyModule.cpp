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

using RenderLevelFn = void (*)(void*, void*, void*);

RenderLevelFn g_renderLevelOriginal = nullptr;
bedrocktools::hooks::Handle g_renderLevelHandle = nullptr;
std::atomic<std::uint32_t> g_renderCalls{0};

struct RenderSnapshot {
    std::uintptr_t tessellator = 0;
    std::uintptr_t levelRendererPlayer = 0;
    float cam[3] = {0, 0, 0};
};
RenderSnapshot g_snapshot;

void renderLevelDetour(void* self, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(self, screenContext, a3);

    const auto n = g_renderCalls.fetch_add(1, std::memory_order_relaxed);
    if (n % 120 != 0 || !self || !screenContext) return;

    RenderSnapshot snap;
    snap.tessellator = *reinterpret_cast<std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(screenContext) + offsets::ScreenContext::mTessellator);
    snap.levelRendererPlayer = *reinterpret_cast<std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(self) + offsets::LevelRenderer::mLevelRendererPlayer);
    if (snap.levelRendererPlayer >= 0x1000) {
        const auto* cam = reinterpret_cast<const float*>(
            snap.levelRendererPlayer + offsets::LevelRendererPlayer::mCamPos);
        snap.cam[0] = cam[0];
        snap.cam[1] = cam[1];
        snap.cam[2] = cam[2];
    }
    g_snapshot = snap;
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
    out << "screenContext tessellator: 0x" << std::hex << snap.tessellator << "\n"
        << "levelRendererPlayer:       0x" << snap.levelRendererPlayer << std::dec << "\n"
        << "camPos: " << snap.cam[0] << ", " << snap.cam[1] << ", " << snap.cam[2] << "\n";

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
    if (frame == 900 || frame % 1800 == 0) writeReport();
}

void SkyModule::onEnable() { g_active.store(true, std::memory_order_relaxed); }

void SkyModule::onDisable() { g_active.store(false, std::memory_order_relaxed); }
