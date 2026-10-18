#include "SkyModule.hpp"

#include "config/ConfigManager.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/Events.hpp>
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
#include <set>
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

struct Vec3f {
    float x = 0, y = 0, z = 0;
};

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

// ------------------------------------------- riskli adimlar icin asama isareti
//
// Riskli bir adimdan once sky_stage.txt'e adim adi yazilir. Oyun cokerse bir
// sonraki acilista bu adim "basarisiz" sayilir ve bir daha denenmez.

std::set<std::string> g_failedStages;

void writeStageFile(const std::string& text) {
    std::ofstream out(g_dir / "sky_stage.txt", std::ios::trunc);
    out << text;
}

void loadStages() {
    {
        std::ifstream in(g_dir / "sky_failed.txt");
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) g_failedStages.insert(line);
        }
    }
    std::string armed;
    {
        std::ifstream in(g_dir / "sky_stage.txt");
        std::getline(in, armed);
    }
    if (!armed.empty()) {
        g_failedStages.insert(armed);
        std::ofstream out(g_dir / "sky_failed.txt", std::ios::app);
        out << armed << "\n";
    }
    writeStageFile("");
}

// Yeni derlemede eski basarisizliklar gecersiz olsun diye adimlar derleme etiketiyle saklanir.
constexpr const char* kBuildTag = "r3:";
std::string tagged(const char* name) { return std::string(kBuildTag) + name; }

bool stageAllowed(const char* name) { return g_failedStages.count(tagged(name)) == 0; }
void armStage(const char* name) { writeStageFile(tagged(name)); }
void disarmStage() { writeStageFile(""); }

// ------------------------------------------------- ayarlar (sky_params.txt)

struct Params {
    float eye = 1.62f;     // oyuncu konumuna eklenen goz yuksekligi
    bool draw = true;      // cizimi ac/kapat
    float r = 0.45f, g = 0.70f, b = 1.0f;
    float maxDist = 64.f;
};
Params g_params;

void loadParams() {
    const auto path = g_dir / "sky_params.txt";
    std::ifstream in(path);
    if (!in) {
        std::ofstream out(path);
        out << "eye 1.62\ndraw 1\nr 0.45\ng 0.70\nb 1.0\nmaxdist 64\n";
        return;
    }
    Params p;
    std::string key;
    float v = 0;
    while (in >> key >> v) {
        if (key == "eye") p.eye = v;
        else if (key == "draw") p.draw = v != 0.f;
        else if (key == "r") p.r = v;
        else if (key == "g") p.g = v;
        else if (key == "b") p.b = v;
        else if (key == "maxdist") p.maxDist = v;
    }
    g_params = p;
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

std::vector<SkyPos> copySkyPositions() {
    std::lock_guard lock(g_posMutex);
    return g_skyPositions;
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

// ------------------------- Tessellator::begin: RenderLevel icindeki tessellator'i yakala

using BeginFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*);

BeginFn g_beginOriginal = nullptr;
bedrocktools::hooks::Handle g_beginHandle = nullptr;
thread_local bool t_inRenderLevel = false;
std::atomic<std::uintptr_t> g_renderTess{0};

void* beginDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    if (t_inRenderLevel && g_renderTess.load(std::memory_order_relaxed) == 0) {
        g_renderTess.store(reinterpret_cast<std::uintptr_t>(a0), std::memory_order_relaxed);
    }
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

// ------------------------------------------------------- oyuncu konumu

std::atomic<std::uintptr_t> g_player{0};

void installPlayerTracking() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](bedrocktools::events::LocalPlayerTickEvent& e) {
            g_player.store(reinterpret_cast<std::uintptr_t>(e.player), std::memory_order_relaxed);
        });
}

bool readPlayerPosition(Vec3f& out) {
    const auto player = g_player.load(std::memory_order_relaxed);
    if (!player || !isReadable(player + offsets::Actor::mStateVectorComponent, 8)) return false;
    const auto svc = *reinterpret_cast<const std::uintptr_t*>(player + offsets::Actor::mStateVectorComponent);
    if (!isReadable(svc, 12)) return false;
    const auto* p = reinterpret_cast<const float*>(svc);
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(p[i]) || std::fabs(p[i]) > 1.0e6f) return false;
    }
    out = {p[0], p[1], p[2]};
    return true;
}

// Oyuncu konumu eski ofsetten okunamazsa: oyuncu nesnesinde sky bloklarina yakin 3'lu float ara.
bool looksLikePosition(const float* f, const std::vector<SkyPos>& blocks) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(f[i]) || std::fabs(f[i]) > 30000.f) return false;
    }
    if (std::fabs(f[0]) < 0.05f && std::fabs(f[2]) < 0.05f) return false;
    for (const auto& b : blocks) {
        if (std::fabs(f[1] - static_cast<float>(b.y)) < 10.f &&
            std::fabs(f[0] - static_cast<float>(b.x)) < 30.f &&
            std::fabs(f[2] - static_cast<float>(b.z)) < 30.f) {
            return true;
        }
    }
    return false;
}

std::string scanPlayerForPosition() {
    std::string text;
    const auto player = g_player.load();
    const auto blocks = copySkyPositions();
    if (!player || blocks.empty() || !isReadable(player, 0x800)) return "  (player scan skipped)\n";
    int found = 0;
    auto scan = [&](std::uintptr_t region, std::size_t size, std::uintptr_t via) {
        if (!isReadable(region, size)) return;
        int local = 0;
        for (std::size_t off = 0; off + 12 <= size && found < 20 && local < 2; off += 4) {
            const auto* f = reinterpret_cast<const float*>(region + off);
            if (!looksLikePosition(f, blocks)) continue;
            char line[160];
            std::snprintf(line, sizeof line, "  pos? player+0x%zx -> +0x%zx: (%.2f, %.2f, %.2f)\n",
                          static_cast<std::size_t>(via), off, f[0], f[1], f[2]);
            text += line;
            ++found;
            ++local;
        }
    };
    scan(player, 0x800, 0);
    for (std::size_t off = 0; off < 0x800 && found < 20; off += 8) {
        const auto ptr = *reinterpret_cast<const std::uintptr_t*>(player + off);
        if ((ptr & 0x00FFFFFFFFFFFFFFull) < 0x10000 || ptr == player) continue;
        scan(ptr, 0x400, off);
    }
    if (found == 0) text += "  (no position candidates)\n";
    return text;
}

// ---------------------------------------------------------------- materyal

struct HashedString {
    std::uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }

private:
    static std::uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        std::uint64_t hash = kOffset;
        for (char ch : str) hash = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;
    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }
    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }
    ~MaterialPtr() {}
    explicit operator bool() const { return sharedPtrData[0] != nullptr; }
};

std::uintptr_t resolveADRP(const std::uint32_t* insns, std::size_t count, std::uint32_t targetReg) {
    for (std::size_t i = 0; i < count; i++) {
        const std::uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;
        if ((insn & 0x9F000000) == 0x90000000) {
            const std::uintptr_t page =
                (reinterpret_cast<std::uintptr_t>(&insns[i]) & ~0xFFFULL) +
                static_cast<std::uintptr_t>(
                    static_cast<std::int64_t>(
                        static_cast<std::uint64_t>(((insn >> 3) & 0x1FFFFC) | ((insn >> 29) & 3)) << 43) >> 31);
            for (std::size_t j = i + 1; j < count; j++) {
                const std::uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 && ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    std::uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
    }
    return 0;
}

MaterialPtr g_material;
const char* g_materialName = "none";
bool g_materialTried = false;

void acquireMaterial() {
    if (g_materialTried) return;
    g_materialTried = true;
    if (!stageAllowed("material")) {
        g_materialName = "skipped (crashed last time)";
        return;
    }
    const auto rmg = bedrocktools::memory::resolve(SignatureId::RenderMaterialGroupCommon);
    if (!rmg) {
        g_materialName = "signature missing";
        return;
    }
    armStage("material");
    const auto groupAddr = resolveADRP(reinterpret_cast<const std::uint32_t*>(rmg), 2, 0);
    std::uintptr_t group = groupAddr ? groupAddr + offsets::MaterialGroup::mRenderMaterialGroupOffset : 0;
    if (!group || !isReadable(group, 8)) {
        g_materialName = "group not readable";
        disarmStage();
        return;
    }
    const auto vtable = *reinterpret_cast<void***>(group);
    if (!isReadable(reinterpret_cast<std::uintptr_t>(vtable), 8 * 8)) {
        g_materialName = "vtable not readable";
        disarmStage();
        return;
    }
    void* fn = vtable[offsets::VTable::RenderMaterialGroup_getMaterial];
    if (!fn || !isReadable(reinterpret_cast<std::uintptr_t>(fn), 4)) {
        g_materialName = "getMaterial not readable";
        disarmStage();
        return;
    }
    using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);
    static const char* kNames[] = {"selection_box", "selection_overlay"};
    for (const char* name : kNames) {
        HashedString hs(name);
        MaterialPtr m = reinterpret_cast<GetMaterialFn>(fn)(reinterpret_cast<void*>(group), &hs);
        if (m) {
            g_material = std::move(m);
            g_materialName = name;
            break;
        }
    }
    if (!g_material) g_materialName = "not found (null)";
    disarmStage();
}

// ------------------------------------------------------------------ cizim

using TessBeginFn = void (*)(void*, void*, int, int, int);
using TessColorFn = void (*)(void*, float, float, float, float);
using TessVertexFn = void (*)(void*, float, float, float);
using RenderMeshFn = void (*)(void*, void*, void*, char*);

TessBeginFn g_tessBegin = nullptr;
TessColorFn g_tessColor = nullptr;
TessVertexFn g_tessVertex = nullptr;
std::array<RenderMeshFn, 2> g_renderMeshes{};
constexpr const char* kRenderStages[2] = {"draw:render:0", "draw:render:1"};

std::atomic<std::uint32_t> g_drawFrames{0};
std::atomic<std::uint32_t> g_lastQuadCount{0};
std::atomic<int> g_renderIdxUsed{-1};
std::atomic<int> g_drawState{0};  // 0=hazir degil 1=cizdi 2=atlandi
std::string g_drawNote;
std::mutex g_noteMutex;

void setNote(const std::string& note) {
    std::lock_guard lock(g_noteMutex);
    g_drawNote = note;
}

void resolveDrawFunctions() {
    g_tessBegin = reinterpret_cast<TessBeginFn>(bedrocktools::memory::resolve(SignatureId::TessellatorBegin));
    g_tessColor = reinterpret_cast<TessColorFn>(bedrocktools::memory::resolve(SignatureId::TessellatorColor));
    if (g_base &&
        std::memcmp(reinterpret_cast<const void*>(g_base + kVertexOffset), kVertexBytes.data(),
                    kVertexBytes.size()) == 0) {
        g_tessVertex = reinterpret_cast<TessVertexFn>(g_base + kVertexOffset);
    }
    g_renderMeshes[0] = reinterpret_cast<RenderMeshFn>(
        bedrocktools::memory::resolve(SignatureId::MeshHelpersRenderMeshImmediately2));
    g_renderMeshes[1] = reinterpret_cast<RenderMeshFn>(
        bedrocktools::memory::resolve(SignatureId::MeshHelpersRenderMeshImmediately));
}

// Her yuz 4 kose: 0 -> -eps, 1 -> 1+eps. Iki sarim yonu da cizilir (culling bilinmiyor).
constexpr int kFaceCorners[6][4][3] = {
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},  // alt
    {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},  // ust
    {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}},  // kuzey
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},  // guney
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},  // bati
    {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}},  // dogu
};

bool drawSkyBlocks(void* screenContext, void* tess, const Vec3f& cam, bool first, int renderIdx) {
    const auto blocks = copySkyPositions();
    if (blocks.empty()) return false;

    constexpr float kEps = 0.003f;
    const float maxDist2 = g_params.maxDist * g_params.maxDist;

    if (first) armStage("draw:begin");
    g_tessBegin(tess, nullptr, 1 /*QuadList*/, 0, 0);
    if (first) {
        disarmStage();
        armStage("draw:color");
    }
    g_tessColor(tess, g_params.r, g_params.g, g_params.b, 1.0f);
    if (first) {
        disarmStage();
        armStage("draw:vertex");
    }

    std::uint32_t quads = 0;
    for (const auto& b : blocks) {
        const float dx = static_cast<float>(b.x) + 0.5f - cam.x;
        const float dy = static_cast<float>(b.y) + 0.5f - cam.y;
        const float dz = static_cast<float>(b.z) + 0.5f - cam.z;
        if (dx * dx + dy * dy + dz * dz > maxDist2) continue;

        for (int f = 0; f < 6; ++f) {
            float v[4][3];
            for (int c = 0; c < 4; ++c) {
                v[c][0] = static_cast<float>(b.x) + (kFaceCorners[f][c][0] ? 1.f + kEps : -kEps) - cam.x;
                v[c][1] = static_cast<float>(b.y) + (kFaceCorners[f][c][1] ? 1.f + kEps : -kEps) - cam.y;
                v[c][2] = static_cast<float>(b.z) + (kFaceCorners[f][c][2] ? 1.f + kEps : -kEps) - cam.z;
            }
            for (int c = 0; c < 4; ++c) g_tessVertex(tess, v[c][0], v[c][1], v[c][2]);
            for (int c = 3; c >= 0; --c) g_tessVertex(tess, v[c][0], v[c][1], v[c][2]);
            quads += 2;
        }
    }

    if (first) {
        disarmStage();
        armStage(kRenderStages[renderIdx]);
    }
    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));
    g_renderMeshes[renderIdx](screenContext, tess, &g_material, pad);
    if (first) disarmStage();
    g_lastQuadCount.store(quads, std::memory_order_relaxed);
    g_renderIdxUsed.store(renderIdx, std::memory_order_relaxed);
    return true;
}

void tryDraw(void* screenContext) {
    if (!g_params.draw) {
        setNote("draw=0 in sky_params.txt");
        return;
    }
    if (!stageAllowed("draw:begin") || !stageAllowed("draw:color") || !stageAllowed("draw:vertex")) {
        setNote("disabled: begin/color/vertex crashed before (see failed stages)");
        return;
    }
    int renderIdx = -1;
    for (int i = 0; i < 2; ++i) {
        if (g_renderMeshes[i] && stageAllowed(kRenderStages[i])) {
            renderIdx = i;
            break;
        }
    }
    if (renderIdx < 0) {
        setNote("disabled: no usable renderMesh (missing or crashed before)");
        return;
    }
    const auto tess = g_renderTess.load();
    if (!tess) return setNote("waiting: tessellator not captured yet");
    if (!g_tessBegin || !g_tessColor || !g_tessVertex)
        return setNote("missing draw functions (begin/color/vertex)");
    if (!g_material) return setNote("waiting: no material");
    if (!isReadable(tess, 0x100)) return setNote("tessellator not readable");
    if (!isReadable(reinterpret_cast<std::uintptr_t>(screenContext), 0x40))
        return setNote("screenContext not readable");

    Vec3f pos;
    if (!readPlayerPosition(pos)) return setNote("player position unreadable");
    const Vec3f cam{pos.x, pos.y + g_params.eye, pos.z};

    const bool first = g_drawState.load() == 0;
    const bool drew = drawSkyBlocks(screenContext, reinterpret_cast<void*>(tess), cam, first, renderIdx);
    if (first && drew) g_drawState.store(1);
    if (drew) g_drawFrames.fetch_add(1, std::memory_order_relaxed);
    setNote(drew ? "drawing" : "no sky blocks in range");
}

// ------------------------------------------------- RenderLevel

// Tum tamsayi/float register'lari aynen iletilir; imza farkli olsa bile arguman kaybolmaz.
using RenderLevelFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*,
                                float, float, float, float);

RenderLevelFn g_renderLevelOriginal = nullptr;
bedrocktools::hooks::Handle g_renderLevelHandle = nullptr;
std::atomic<std::uint32_t> g_renderCalls{0};
std::atomic<std::uintptr_t> g_arg0{0}, g_arg1{0};

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

    if (n % 120 == 0) loadParams();
    if (n >= 120 && g_renderTess.load() != 0) acquireMaterial();
    if (n >= 120) tryDraw(a1);
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

    out << "RenderLevel hook: " << (g_renderLevelHandle ? "yes" : "NO")
        << " calls=" << g_renderCalls.load() << "\n";
    out << "vertex fn: " << (g_tessVertex ? "ok" : "MISSING") << ", begin: " << (g_tessBegin ? "ok" : "MISSING")
        << ", color: " << (g_tessColor ? "ok" : "MISSING")
        << ", renderMesh[0]: " << (g_renderMeshes[0] ? "ok" : "MISSING")
        << ", renderMesh[1]: " << (g_renderMeshes[1] ? "ok" : "MISSING")
        << ", used: " << g_renderIdxUsed.load() << "\n";
    out << "tessellator (captured in RenderLevel): 0x" << std::hex << g_renderTess.load() << std::dec << "\n";
    out << "RenderLevel args: 0x" << std::hex << g_arg0.load() << " 0x" << g_arg1.load() << std::dec << "\n";
    out << "material: " << g_materialName << (g_material ? " (acquired)" : "") << "\n";

    Vec3f pos;
    const bool posOk = readPlayerPosition(pos);
    out << "player ptr: 0x" << std::hex << g_player.load() << std::dec << "  position: ";
    if (posOk) out << pos.x << ", " << pos.y << ", " << pos.z << "\n";
    else out << "UNREADABLE\n" << scanPlayerForPosition();

    out << "params: eye=" << g_params.eye << " draw=" << g_params.draw << " color=(" << g_params.r << ","
        << g_params.g << "," << g_params.b << ") maxdist=" << g_params.maxDist << "\n";
    {
        std::lock_guard lock(g_noteMutex);
        out << "draw: " << g_drawNote << "  frames=" << g_drawFrames.load()
            << " quads/frame=" << g_lastQuadCount.load() << "\n";
    }
    out << "failed stages:";
    for (const auto& s : g_failedStages) out << " " << s;
    out << "\n";

    const auto blocks = copySkyPositions();
    out << "sky blocks seen: " << blocks.size() << "\n";
    std::size_t shown = 0;
    for (const auto& s : blocks) {
        if (shown++ >= 12) break;
        out << "  (" << s.x << ", " << s.y << ", " << s.z << ")\n";
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

    loadStages();
    loadParams();
    resolveDrawFunctions();
    installPlayerTracking();
    installFaceHooks();
    installBeginHook();
    installRenderLevelHook();
    setMasterEnabled(true);
}

void SkyModule::onFrame() {
    static unsigned frame = 0;
    ++frame;
    if (frame % 300 == 0) writeReport();
}

void SkyModule::onEnable() { g_active.store(true, std::memory_order_relaxed); }

void SkyModule::onDisable() { g_active.store(false, std::memory_order_relaxed); }
