#include "SkyModule.hpp"

#include "config/ConfigManager.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/render/Block.hpp>

#include <algorithm>
#include <android/log.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using bedrocktools::memory::SignatureId;

constexpr std::string_view kSkyBlockName = "bos:block_of_sky";
constexpr std::size_t kFaceCount = 6;

std::filesystem::path g_dir;
std::uintptr_t g_base = 0;

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
    // Sky blogunun yuzleri chunk'ta cizilmez.
    if (g_active.load(std::memory_order_relaxed) && isSkyBlock(block)) return;
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

// ------------------------------------------------- Tessellator::vertex probes
//
// sky_scan2.txt'deki adaylari hook'layip kac kez cagrildiklarini ve
// ilk cagrilarin argumanlarini kaydeder. Gercek vertex fonksiyonu cok sik
// cagrilir ve argumanlari dunya koordinatlari (ya da bir Vec3 isaretcisi) olur.

struct ProbeSpec {
    std::uintptr_t offset;
    const char* name;
};

constexpr std::array<ProbeSpec, 7> kProbeSpecs = {{
    {0xa7717f4, "cand_a7717f4 (loads 3 floats from x1)"},
    {0xa771f84, "cand_a771f84 (TLS x24, like old signature)"},
    {0xa76f048, "cand_a76f048"},
    {0xa7710f8, "cand_a7710f8"},
    {0xa76e14c, "cand_a76e14c"},
    {0xa76deb8, "cand_a76deb8"},
    {0xa76ef7c, "reference_a76ef7c (tex-like, 20 calls per face)"},
}};
constexpr std::size_t kProbeCount = kProbeSpecs.size();
constexpr std::size_t kSampleCount = 8;

using ProbeFn = void* (*)(void*, void*, void*, void*, float, float, float, float);

struct Sample {
    std::uintptr_t x1 = 0;
    std::uintptr_t x2 = 0;
    float f0 = 0, f1 = 0, f2 = 0;
};

struct ProbeState {
    ProbeFn original = nullptr;
    bedrocktools::hooks::Handle handle = nullptr;
    std::atomic<std::uint32_t> calls{0};
    std::array<Sample, kSampleCount> samples{};
};

std::array<ProbeState, kProbeCount> g_probes;

template <std::size_t P>
void* probeDetour(void* a0, void* a1, void* a2, void* a3, float f0, float f1, float f2, float f3) {
    auto& st = g_probes[P];
    const auto n = st.calls.fetch_add(1, std::memory_order_relaxed);
    if (n < kSampleCount) {
        st.samples[n] = Sample{reinterpret_cast<std::uintptr_t>(a1),
                               reinterpret_cast<std::uintptr_t>(a2), f0, f1, f2};
    }
    return st.original ? st.original(a0, a1, a2, a3, f0, f1, f2, f3) : nullptr;
}

template <std::size_t... I>
constexpr std::array<ProbeFn, kProbeCount> makeProbeDetours(std::index_sequence<I...>) {
    return {&probeDetour<I>...};
}

constexpr auto kProbeDetours = makeProbeDetours(std::make_index_sequence<kProbeCount>{});

void installProbes() {
    if (!g_base) return;
    std::size_t installed = 0;
    for (std::size_t i = 0; i < kProbeCount; ++i) {
        const auto address = g_base + kProbeSpecs[i].offset;
        g_probes[i].handle = bedrocktools::hooks::install(
            reinterpret_cast<void*>(address),
            reinterpret_cast<void*>(kProbeDetours[i]),
            reinterpret_cast<void**>(&g_probes[i].original));
        if (g_probes[i].handle) ++installed;
    }
    __android_log_print(ANDROID_LOG_INFO, "BlockOfSky", "probes: %zu/%zu", installed, kProbeCount);
}

void writeProbeReport() {
    std::ofstream out(g_dir / "sky_probe.txt");
    if (!out) return;
    for (std::size_t i = 0; i < kProbeCount; ++i) {
        const auto& st = g_probes[i];
        const auto calls = st.calls.load(std::memory_order_relaxed);
        out << "0x" << std::hex << kProbeSpecs[i].offset << std::dec << " "
            << kProbeSpecs[i].name << " hooked=" << (st.handle ? "yes" : "NO")
            << " calls=" << calls << "\n";
        const auto shown = std::min<std::uint32_t>(calls, kSampleCount);
        for (std::uint32_t s = 0; s < shown; ++s) {
            const auto& sm = st.samples[s];
            out << "    x1=0x" << std::hex << sm.x1 << " x2=0x" << sm.x2 << std::dec
                << "  f=[" << sm.f0 << ", " << sm.f1 << ", " << sm.f2 << "]\n";
        }
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
    installProbes();
    setMasterEnabled(true);
}

void SkyModule::onFrame() {
    static unsigned frame = 0;
    ++frame;
    if (frame == 600 || frame % 1800 == 0) writeProbeReport();
}

void SkyModule::onEnable() { g_active.store(true, std::memory_order_relaxed); }

void SkyModule::onDisable() { g_active.store(false, std::memory_order_relaxed); }
