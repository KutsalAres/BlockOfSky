#include "SkyModule.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/render/Block.hpp>

#include <android/log.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace {

using bedrocktools::memory::SignatureId;

constexpr std::string_view kSkyBlockName = "bos:block_of_sky";
constexpr std::size_t kFaceCount = 6;

// (tessellator, meshTessellator, block, position, texture)
using FaceFn = void (*)(void*, void*, const void*, const void*, const void*);

std::array<FaceFn, kFaceCount> g_original{};
std::array<bedrocktools::hooks::Handle, kFaceCount> g_handles{};
std::atomic_bool g_active{true};
bool g_installed = false;

bool isSkyBlock(const void* block) {
    if (!block) return false;
    const auto* obj = static_cast<const bedrocktools::sdk::Block*>(block);
    const std::string* name = obj->fullName();
    if (!name || name->size() != kSkyBlockName.size() || name->data() == nullptr) return false;
    return std::string_view(name->data(), name->size()) == kSkyBlockName;
}

template <std::size_t Face>
void faceDetour(void* tess, void* meshTess, const void* block, const void* pos, const void* tex) {
    // Sky blogunun yuzleri cizilmez: delikten oyunun kendi gokyuzu gorunur.
    if (g_active.load(std::memory_order_relaxed) && isSkyBlock(block)) return;
    if (g_original[Face]) g_original[Face](tess, meshTess, block, pos, tex);
}

template <std::size_t... I>
constexpr std::array<FaceFn, kFaceCount> makeDetours(std::index_sequence<I...>) {
    return {&faceDetour<I>...};
}

constexpr auto kDetours = makeDetours(std::make_index_sequence<kFaceCount>{});

constexpr std::array<SignatureId, kFaceCount> kFaceIds = {
    SignatureId::BlockTessellatorTessellateFaceDown,
    SignatureId::BlockTessellatorTessellateFaceUp,
    SignatureId::BlockTessellatorTessellateFaceNorth,
    SignatureId::BlockTessellatorTessellateFaceSouth,
    SignatureId::BlockTessellatorTessellateFaceWest,
    SignatureId::BlockTessellatorTessellateFaceEast,
};

}

void SkyModule::onInit() {
    if (!g_installed) {
        std::size_t installed = 0;
        for (std::size_t i = 0; i < kFaceCount; ++i) {
            const auto address = bedrocktools::memory::resolve(kFaceIds[i]);
            if (!address) continue;
            g_handles[i] = bedrocktools::hooks::install(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(kDetours[i]),
                reinterpret_cast<void**>(&g_original[i]));
            if (g_handles[i]) ++installed;
        }
        g_installed = installed == kFaceCount;
        __android_log_print(ANDROID_LOG_INFO, "BlockOfSky",
                            "face hooks installed: %zu/%zu", installed, kFaceCount);
    }
    setMasterEnabled(true);
}

void SkyModule::onEnable() { g_active.store(true, std::memory_order_relaxed); }

void SkyModule::onDisable() { g_active.store(false, std::memory_order_relaxed); }
