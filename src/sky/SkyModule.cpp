#include "SkyModule.hpp"

#include "config/ConfigManager.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/render/Block.hpp>

#include <android/log.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

namespace {

using bedrocktools::memory::SignatureId;

constexpr std::string_view kSkyBlockName = "bos:block_of_sky";

// (tessellator, meshTessellator, block, position, texture)
using FaceFn = void (*)(void*, void*, const void*, const void*, const void*);

std::array<FaceFn, 6> g_original{};
std::array<bedrocktools::hooks::Handle, 6> g_handles{};

bool isSkyBlock(const void* block) {
    if (!block) return false;
    const auto* obj = static_cast<const bedrocktools::sdk::Block*>(block);
    const std::string* name = obj->fullName();
    if (!name || name->size() > 256 || name->data() == nullptr) return false;
    return std::string_view(name->data(), name->size()) == kSkyBlockName;
}

template <size_t Face>
void faceDetour(void* tess, void* meshTess, const void* block, const void* pos, const void* tex) {
    if (isSkyBlock(block)) return;  // test: sky block yuzleri cizilmez
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

}

void SkyModule::onInit() {
    setMasterEnabled(true);

    const auto configPath = std::filesystem::path(
        bedrocktools::config::ConfigManager::get().getConfigPath());
    std::ofstream out(configPath.parent_path() / "sky_hooks.txt");

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
