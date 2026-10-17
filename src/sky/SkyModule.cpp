#include "SkyModule.hpp"

#include "config/ConfigManager.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/render/Block.hpp>

#include <android/log.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace {

using bedrocktools::memory::SignatureId;

constexpr std::string_view kSkyBlockName = "bos:block_of_sky";

// (tessellator, meshTessellator, block, position, texture)
using FaceFn = void (*)(void*, void*, const void*, const void*, const void*);

std::array<FaceFn, 6> g_original{};
std::array<bedrocktools::hooks::Handle, 6> g_handles{};

// teshis verileri
std::array<std::atomic<uint32_t>, 6> g_calls{};
std::atomic<uint32_t> g_skyHits{0};
std::atomic<uint32_t> g_unreadable{0};
std::mutex g_namesMutex;
std::set<std::string> g_names;
std::filesystem::path g_dir;

thread_local const void* t_lastBlock = nullptr;
thread_local bool t_lastIsSky = false;

std::string sanitize(std::string_view s) {
    std::string out;
    for (char c : s.substr(0, 96)) out += (c >= 32 && c < 127) ? c : '?';
    return out;
}

bool inspectBlock(const void* block) {
    if (!block) return false;
    const auto* obj = static_cast<const bedrocktools::sdk::Block*>(block);
    const std::string* name = obj->fullName();
    if (!name || name->size() > 256 || name->data() == nullptr) {
        g_unreadable.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::string_view view(name->data(), name->size());
    {
        std::lock_guard lock(g_namesMutex);
        if (g_names.size() < 120) g_names.insert(sanitize(view));
    }
    return view == kSkyBlockName;
}

template <size_t Face>
void faceDetour(void* tess, void* meshTess, const void* block, const void* pos, const void* tex) {
    g_calls[Face].fetch_add(1, std::memory_order_relaxed);

    bool sky;
    if (block == t_lastBlock) {
        sky = t_lastIsSky;
    } else {
        sky = inspectBlock(block);
        t_lastBlock = block;
        t_lastIsSky = sky;
    }

    if (sky) {
        g_skyHits.fetch_add(1, std::memory_order_relaxed);
        return;  // test: sky block yuzleri cizilmez
    }
    if (g_original[Face]) g_original[Face](tess, meshTess, block, pos, tex);
}

template <size_t... I>
constexpr std::array<FaceFn, 6> makeDetours(std::index_sequence<I...>) {
    return {&faceDetour<I>...};
}

constexpr std::array<FaceFn, 6> kDetours = makeDetours(std::make_index_sequence<6>{});

constexpr std::array<SignatureId, 6> kFaceIds = {
    SignatureId::BlockTessellatorTessellateFaceDown,
    SignatureId::BlockTessellatorTessellateFaceUp,
    SignatureId::BlockTessellatorTessellateFaceNorth,
    SignatureId::BlockTessellatorTessellateFaceSouth,
    SignatureId::BlockTessellatorTessellateFaceWest,
    SignatureId::BlockTessellatorTessellateFaceEast,
};

void writeDebug() {
    std::ofstream out(g_dir / "sky_debug.txt");
    if (!out) return;
    static const char* faceNames[6] = {"Down", "Up", "North", "South", "West", "East"};
    for (size_t i = 0; i < 6; ++i)
        out << "calls " << faceNames[i] << ": " << g_calls[i].load() << "\n";
    out << "sky block hits: " << g_skyHits.load() << "\n";
    out << "unreadable names: " << g_unreadable.load() << "\n";
    out << "--- seen block names ---\n";
    std::lock_guard lock(g_namesMutex);
    for (const auto& n : g_names) out << n << "\n";
}

}

void SkyModule::onInit() {
    setMasterEnabled(true);

    g_dir = std::filesystem::path(
        bedrocktools::config::ConfigManager::get().getConfigPath()).parent_path();
    std::ofstream out(g_dir / "sky_hooks.txt");

    for (size_t i = 0; i < kFaceIds.size(); ++i) {
        const auto address = bedrocktools::memory::resolve(kFaceIds[i]);
        bool ok = false;
        if (address) {
            g_handles[i] = bedrocktools::hooks::install(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(kDetours[i]),
                reinterpret_cast<void**>(&g_original[i]));
            ok = g_handles[i] != nullptr;
        }
        __android_log_print(ANDROID_LOG_INFO, "BlockOfSky", "face hook %zu: %s", i, ok ? "OK" : "FAILED");
        if (out) out << "face hook " << i << ": " << (ok ? "OK" : "FAILED") << "\n";
    }
}

void SkyModule::onFrame() {
    static unsigned frame = 0;
    ++frame;
    if (frame == 180 || frame % 900 == 0) writeDebug();
}
