#include "SkyModule.hpp"

#include "config/ConfigManager.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/render/Block.hpp>

#include <android/log.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace {

using bedrocktools::memory::SignatureId;

constexpr std::string_view kSkyBlockName = "bos:block_of_sky";
constexpr std::size_t kFaceCount = 6;

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
bool g_installed = false;

// ---------------------------------------------------------------- hooks

bool isSkyBlock(const void* block) {
    if (!block) return false;
    const auto* obj = static_cast<const bedrocktools::sdk::Block*>(block);
    const std::string* name = obj->fullName();
    if (!name || name->data() == nullptr || name->size() != kSkyBlockName.size()) return false;
    return std::string_view(name->data(), name->size()) == kSkyBlockName;
}

template <std::size_t Face>
void faceDetour(void* tess, void* meshTess, const void* block, const void* pos, const void* tex) {
    // Sky blogunun yuzleri chunk'ta cizilmez.
    if (g_active.load(std::memory_order_relaxed) && isSkyBlock(block)) return;
    if (g_original[Face]) g_original[Face](tess, meshTess, block, pos, tex);
}

template <std::size_t... I>
constexpr std::array<FaceFn, kFaceCount> makeDetours(std::index_sequence<I...>) {
    return {&faceDetour<I>...};
}

constexpr auto kDetours = makeDetours(std::make_index_sequence<kFaceCount>{});

void installFaceHooks() {
    if (g_installed) return;
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
    __android_log_print(ANDROID_LOG_INFO, "BlockOfSky", "face hooks: %zu/%zu", installed, kFaceCount);
}

// ------------------------------------------------- vertex function scanner

void scanFaceFunctions() {
    const auto dir = std::filesystem::path(
        bedrocktools::config::ConfigManager::get().getConfigPath()).parent_path();
    std::ofstream out(dir / "sky_scan2.txt");
    if (!out) return;

    const auto begin = bedrocktools::memory::resolve(SignatureId::TessellatorBegin);
    Dl_info info{};
    if (!begin || !dladdr(reinterpret_cast<void*>(begin), &info) || !info.dli_fbase) {
        out << "base not found\n";
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const std::uintptr_t lo = begin - 0x4000;
    const std::uintptr_t hi = begin + 0x4000;

    auto dumpBytes = [&](std::uintptr_t addr, int n) {
        char buf[8];
        const auto* b = reinterpret_cast<const std::uint8_t*>(addr);
        for (int i = 0; i < n; ++i) {
            std::snprintf(buf, sizeof buf, " %02X", b[i]);
            out << buf;
        }
    };

    // A) yuz fonksiyonlarinin Tessellator bolgesindeki cagri hedefleri
    std::map<std::uintptr_t, std::array<int, kFaceCount>> targets;
    for (std::size_t f = 0; f < kFaceCount; ++f) {
        const auto start = bedrocktools::memory::resolve(kFaceIds[f]);
        if (!start) continue;
        const auto* code = reinterpret_cast<const std::uint32_t*>(start);
        for (int k = 0; k < 1200; ++k) {
            const std::uint32_t ins = code[k];
            if ((ins & 0xFC000000u) != 0x94000000u) continue;  // BL
            const std::int32_t imm = static_cast<std::int32_t>(ins << 6) >> 6;
            const auto target = start + static_cast<std::intptr_t>(k) * 4 +
                                static_cast<std::intptr_t>(imm) * 4;
            if (target >= lo && target < hi) ++targets[target][f];
        }
    }

    out << "== A: face callees in tessellator region\n";
    for (const auto& [target, counts] : targets) {
        out << "0x" << std::hex << (target - base) << std::dec << " counts:";
        for (int c : counts) out << ' ' << c;
        out << " bytes:";
        dumpBytes(target, 32);
        out << "\n";
    }

    // B) basinda TLS (TPIDR_EL0) okuyan fonksiyonlar
    out << "== B: prologue reads TPIDR_EL0\n";
    int lines = 0;
    for (std::uintptr_t a = lo; a < hi && lines < 80; a += 4) {
        const auto ins = *reinterpret_cast<const std::uint32_t*>(a);
        const bool isSub = (ins & 0xFF0003FFu) == 0xD10003FFu;  // SUB SP, SP, #imm
        const bool isStp = (ins & 0xFFC07FFFu) == 0xA9807BFDu;  // STP X29, X30, [SP, #imm]!
        if (!isSub && !isStp) continue;
        for (int k = 1; k < 24; ++k) {
            const auto n = *reinterpret_cast<const std::uint32_t*>(a + k * 4);
            if ((n & 0xFFFFFFE0u) != 0xD53BD040u) continue;  // MRS Xt, TPIDR_EL0
            int calls = 0;
            const auto it = targets.find(a);
            if (it != targets.end()) {
                for (int c : it->second) calls += c;
            }
            out << "0x" << std::hex << (a - base) << std::dec << " mrs@" << k
                << " faceCalls:" << calls << " bytes:";
            dumpBytes(a, 64);
            out << "\n";
            ++lines;
            break;
        }
    }
}

}  // namespace

void SkyModule::onInit() {
    installFaceHooks();
    scanFaceFunctions();
    setMasterEnabled(true);
}

void SkyModule::onEnable() { g_active.store(true, std::memory_order_relaxed); }

void SkyModule::onDisable() { g_active.store(false, std::memory_order_relaxed); }
