#include "GlTrace.hpp"

#include "core/memory/Hooks.hpp"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace hooks = bedrocktools::hooks;
using Clock = std::chrono::steady_clock;

enum class Kind : std::uint8_t {
    DrawElements,
    DrawArrays,
    DrawElementsInstanced,
    DrawArraysInstanced,
    Bind,
    Clear,
    Swap,
    RlBegin,
    RlEnd,
};

struct Rec {
    Kind kind = Kind::DrawElements;
    int tid = 0;
    bool inRL = false;
    int mode = 0, count = 0, inst = 1;
    int a = 0, b = 0;  // Bind: target, fbo / Clear: mask
    int fbo = 0, program = 0, tex = 0;
    int vp[4] = {0, 0, 0, 0};
    bool depth = false, blend = false, cull = false, stencil = false, depthMask = false;
    unsigned cmask = 0;
};

constexpr std::size_t kMaxRecs = 8000;
constexpr int kMaxCaptures = 3;

std::filesystem::path g_dir;
std::string g_hookStatus;

std::atomic<bool> g_capturing{false};
std::mutex g_mutex;
std::vector<Rec> g_recs;
std::atomic<long long> g_nextTriggerMs{0};
long long g_captureStartMs = 0;
int g_swapsInCapture = 0;
std::atomic<int> g_captures{0};

std::atomic<std::uint64_t> g_draws{0}, g_swaps{0}, g_binds{0}, g_clears{0};
thread_local bool t_inRenderLevel = false;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

int currentTid() { return static_cast<int>(syscall(SYS_gettid)); }

bool captureActive() {
    if (g_capturing.load(std::memory_order_relaxed)) return true;
    const auto now = nowMs();
    if (now < g_nextTriggerMs.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_mutex);
    if (g_capturing.load()) return true;
    if (now < g_nextTriggerMs.load()) return false;
    g_recs.clear();
    g_recs.reserve(kMaxRecs);
    g_swapsInCapture = 0;
    g_captureStartMs = now;
    g_capturing.store(true);
    return true;
}

void push(const Rec& r) {
    std::lock_guard lock(g_mutex);
    if (!g_capturing.load()) return;
    if (g_recs.size() < kMaxRecs) g_recs.push_back(r);
    if (r.kind == Kind::Swap) ++g_swapsInCapture;
}

Rec snapshot(Kind kind) {
    Rec r;
    r.kind = kind;
    r.tid = currentTid();
    r.inRL = t_inRenderLevel;
    GLint v[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, v);
    r.fbo = v[0];
    glGetIntegerv(GL_CURRENT_PROGRAM, v);
    r.program = v[0];
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    for (int i = 0; i < 4; ++i) r.vp[i] = vp[i];
    glGetIntegerv(GL_TEXTURE_BINDING_2D, v);
    r.tex = v[0];
    r.depth = glIsEnabled(GL_DEPTH_TEST);
    r.blend = glIsEnabled(GL_BLEND);
    r.cull = glIsEnabled(GL_CULL_FACE);
    r.stencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean b = 0;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &b);
    r.depthMask = b;
    GLboolean cm[4] = {0, 0, 0, 0};
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    r.cmask = (cm[0] ? 1u : 0u) | (cm[1] ? 2u : 0u) | (cm[2] ? 4u : 0u) | (cm[3] ? 8u : 0u);
    return r;
}

bool sameDrawState(const Rec& x, const Rec& y) {
    return x.kind == y.kind && x.mode == y.mode && x.fbo == y.fbo && x.program == y.program &&
           x.tex == y.tex && x.vp[2] == y.vp[2] && x.vp[3] == y.vp[3] && x.depth == y.depth &&
           x.blend == y.blend && x.cull == y.cull && x.stencil == y.stencil &&
           x.depthMask == y.depthMask && x.cmask == y.cmask && x.inRL == y.inRL && x.tid == y.tid;
}

bool isDraw(Kind k) {
    return k == Kind::DrawElements || k == Kind::DrawArrays || k == Kind::DrawElementsInstanced ||
           k == Kind::DrawArraysInstanced;
}

const char* drawName(Kind k) {
    switch (k) {
        case Kind::DrawElements: return "DrawElements";
        case Kind::DrawArrays: return "DrawArrays";
        case Kind::DrawElementsInstanced: return "DrawElementsInst";
        case Kind::DrawArraysInstanced: return "DrawArraysInst";
        default: return "?";
    }
}

void writeTrace(const std::vector<Rec>& recs, int number) {
    char name[64];
    std::snprintf(name, sizeof name, "sky_gltrace_%d.txt", number);
    std::ofstream out(g_dir / name);
    if (!out) return;

    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    out << "capture " << number << ": " << recs.size() << " records\n";
    out << "GL renderer: " << (renderer ? renderer : "?") << "\nGL version: " << (version ? version : "?")
        << "\nGL vendor: " << (vendor ? vendor : "?") << "\n";

    EGLint w = 0, h = 0;
    const EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
    const EGLDisplay disp = eglGetCurrentDisplay();
    if (surf && disp) {
        eglQuerySurface(disp, surf, EGL_WIDTH, &w);
        eglQuerySurface(disp, surf, EGL_HEIGHT, &h);
    }
    out << "EGL context=" << eglGetCurrentContext() << " surface=" << surf << " size=" << w << "x" << h << "\n";
    out << "hooks: " << g_hookStatus << "\n";
    out << "totals so far: draws=" << g_draws.load() << " swaps=" << g_swaps.load()
        << " binds=" << g_binds.load() << " clears=" << g_clears.load() << "\n";

    std::map<int, int> perThread;
    for (const auto& r : recs) ++perThread[r.tid];
    out << "records per thread:";
    for (const auto& [tid, n] : perThread) out << " " << tid << ":" << n;
    out << "\n\n";

    std::size_t i = 0;
    while (i < recs.size()) {
        const Rec& r = recs[i];
        if (isDraw(r.kind)) {
            std::size_t j = i + 1;
            long long verts = static_cast<long long>(r.count) * r.inst;
            while (j < recs.size() && isDraw(recs[j].kind) && sameDrawState(r, recs[j])) {
                verts += static_cast<long long>(recs[j].count) * recs[j].inst;
                ++j;
            }
            char line[320];
            std::snprintf(line, sizeof line,
                          "#%zu D x%zu %s mode=%d verts=%lld fbo=%d prog=%d vp=%dx%d tex=%d depth=%d blend=%d "
                          "cull=%d stencil=%d dmask=%d cmask=%X RL=%d tid=%d\n",
                          i, j - i, drawName(r.kind), r.mode, verts, r.fbo, r.program, r.vp[2], r.vp[3], r.tex,
                          r.depth, r.blend, r.cull, r.stencil, r.depthMask, r.cmask, r.inRL, r.tid);
            out << line;
            i = j;
            continue;
        }
        char line[256];
        switch (r.kind) {
            case Kind::Bind:
                std::snprintf(line, sizeof line, "#%zu BIND target=0x%X fbo=%d tid=%d\n", i, r.a, r.b, r.tid);
                break;
            case Kind::Clear:
                std::snprintf(line, sizeof line,
                              "#%zu CLEAR mask=0x%X fbo=%d vp=%dx%d dmask=%d cmask=%X RL=%d tid=%d\n", i, r.a,
                              r.fbo, r.vp[2], r.vp[3], r.depthMask, r.cmask, r.inRL, r.tid);
                break;
            case Kind::Swap:
                std::snprintf(line, sizeof line, "#%zu ---- SWAP ---- tid=%d\n", i, r.tid);
                break;
            case Kind::RlBegin:
                std::snprintf(line, sizeof line, "#%zu >>>> RenderLevel begin tid=%d\n", i, r.tid);
                break;
            default:
                std::snprintf(line, sizeof line, "#%zu <<<< RenderLevel end tid=%d\n", i, r.tid);
                break;
        }
        out << line;
        ++i;
    }
}

void maybeFinish() {
    if (!g_capturing.load(std::memory_order_relaxed)) return;
    bool done;
    {
        std::lock_guard lock(g_mutex);
        done = g_recs.size() >= kMaxRecs || g_swapsInCapture >= 2 || nowMs() - g_captureStartMs > 2500;
    }
    if (!done) return;

    bool expected = true;
    if (!g_capturing.compare_exchange_strong(expected, false)) return;
    std::vector<Rec> recs;
    {
        std::lock_guard lock(g_mutex);
        recs.swap(g_recs);
    }
    const int n = ++g_captures;
    g_nextTriggerMs.store(n >= kMaxCaptures ? INT64_MAX : nowMs() + 15000);
    writeTrace(recs, n);
}

// ------------------------------------------------------------------ detour'lar

using DrawElementsFn = void (*)(GLenum, GLsizei, GLenum, const void*);
using DrawArraysFn = void (*)(GLenum, GLint, GLsizei);
using DrawElementsInstFn = void (*)(GLenum, GLsizei, GLenum, const void*, GLsizei);
using DrawArraysInstFn = void (*)(GLenum, GLint, GLsizei, GLsizei);
using BindFramebufferFn = void (*)(GLenum, GLuint);
using ClearFn = void (*)(GLbitfield);
using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);

DrawElementsFn g_oDrawElements = nullptr;
DrawArraysFn g_oDrawArrays = nullptr;
DrawElementsInstFn g_oDrawElementsInst = nullptr;
DrawArraysInstFn g_oDrawArraysInst = nullptr;
BindFramebufferFn g_oBindFramebuffer = nullptr;
ClearFn g_oClear = nullptr;
SwapFn g_oSwap = nullptr;

void hkDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    g_draws.fetch_add(1, std::memory_order_relaxed);
    if (captureActive()) {
        Rec r = snapshot(Kind::DrawElements);
        r.mode = static_cast<int>(mode);
        r.count = count;
        push(r);
    }
    if (g_oDrawElements) g_oDrawElements(mode, count, type, indices);
    maybeFinish();
}

void hkDrawArrays(GLenum mode, GLint first, GLsizei count) {
    g_draws.fetch_add(1, std::memory_order_relaxed);
    if (captureActive()) {
        Rec r = snapshot(Kind::DrawArrays);
        r.mode = static_cast<int>(mode);
        r.count = count;
        push(r);
    }
    if (g_oDrawArrays) g_oDrawArrays(mode, first, count);
    maybeFinish();
}

void hkDrawElementsInst(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances) {
    g_draws.fetch_add(1, std::memory_order_relaxed);
    if (captureActive()) {
        Rec r = snapshot(Kind::DrawElementsInstanced);
        r.mode = static_cast<int>(mode);
        r.count = count;
        r.inst = instances;
        push(r);
    }
    if (g_oDrawElementsInst) g_oDrawElementsInst(mode, count, type, indices, instances);
    maybeFinish();
}

void hkDrawArraysInst(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
    g_draws.fetch_add(1, std::memory_order_relaxed);
    if (captureActive()) {
        Rec r = snapshot(Kind::DrawArraysInstanced);
        r.mode = static_cast<int>(mode);
        r.count = count;
        r.inst = instances;
        push(r);
    }
    if (g_oDrawArraysInst) g_oDrawArraysInst(mode, first, count, instances);
    maybeFinish();
}

void hkBindFramebuffer(GLenum target, GLuint fb) {
    g_binds.fetch_add(1, std::memory_order_relaxed);
    if (captureActive()) {
        Rec r;
        r.kind = Kind::Bind;
        r.tid = currentTid();
        r.a = static_cast<int>(target);
        r.b = static_cast<int>(fb);
        push(r);
    }
    if (g_oBindFramebuffer) g_oBindFramebuffer(target, fb);
}

void hkClear(GLbitfield mask) {
    g_clears.fetch_add(1, std::memory_order_relaxed);
    if (captureActive()) {
        Rec r = snapshot(Kind::Clear);
        r.a = static_cast<int>(mask);
        push(r);
    }
    if (g_oClear) g_oClear(mask);
}

EGLBoolean hkSwap(EGLDisplay display, EGLSurface surface) {
    g_swaps.fetch_add(1, std::memory_order_relaxed);
    if (g_capturing.load(std::memory_order_relaxed)) {
        Rec r;
        r.kind = Kind::Swap;
        r.tid = currentTid();
        push(r);
    }
    const EGLBoolean result = g_oSwap ? g_oSwap(display, surface) : EGL_FALSE;
    maybeFinish();
    return result;
}

// ------------------------------------------------------------------- kurulum

void hookSymbol(void* lib1, void* lib2, const char* name, void* detour, void** original) {
    std::uintptr_t addr = hooks::symbol(lib1, name);
    if (!addr) addr = hooks::symbol(lib2, name);
    if (!addr) {
        g_hookStatus += std::string(name) + ": no symbol; ";
        return;
    }
    const auto handle = hooks::install(reinterpret_cast<void*>(addr), detour, original);
    g_hookStatus += std::string(name) + (handle ? ": ok; " : ": FAILED; ");
}

void writeStatusLoop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::ofstream out(g_dir / "sky_gltrace_status.txt");
        if (!out) continue;
        out << "hooks: " << g_hookStatus << "\n";
        out << "draws=" << g_draws.load() << " swaps=" << g_swaps.load() << " binds=" << g_binds.load()
            << " clears=" << g_clears.load() << " captures=" << g_captures.load() << "\n";
    }
}

}  // namespace

namespace skygl {

void init(const std::filesystem::path& dir) {
    static bool done = false;
    if (done) return;
    done = true;
    g_dir = dir;
    // Kapatma anahtari: mod klasorune sky_gl_off.txt olusturursan GL izleme hic kurulmaz.
    if (std::filesystem::exists(dir / "sky_gl_off.txt")) return;
    g_nextTriggerMs.store(nowMs() + 20000);

    void* gl3 = hooks::openLibrary("libGLESv3.so");
    void* gl2 = hooks::openLibrary("libGLESv2.so");
    void* egl = hooks::openLibrary("libEGL.so");

    hookSymbol(gl3, gl2, "glDrawElements", reinterpret_cast<void*>(&hkDrawElements),
               reinterpret_cast<void**>(&g_oDrawElements));
    hookSymbol(gl3, gl2, "glDrawArrays", reinterpret_cast<void*>(&hkDrawArrays),
               reinterpret_cast<void**>(&g_oDrawArrays));
    hookSymbol(gl3, gl2, "glDrawElementsInstanced", reinterpret_cast<void*>(&hkDrawElementsInst),
               reinterpret_cast<void**>(&g_oDrawElementsInst));
    hookSymbol(gl3, gl2, "glDrawArraysInstanced", reinterpret_cast<void*>(&hkDrawArraysInst),
               reinterpret_cast<void**>(&g_oDrawArraysInst));
    hookSymbol(gl3, gl2, "glBindFramebuffer", reinterpret_cast<void*>(&hkBindFramebuffer),
               reinterpret_cast<void**>(&g_oBindFramebuffer));
    hookSymbol(gl3, gl2, "glClear", reinterpret_cast<void*>(&hkClear), reinterpret_cast<void**>(&g_oClear));
    hookSymbol(egl, nullptr, "eglSwapBuffers", reinterpret_cast<void*>(&hkSwap),
               reinterpret_cast<void**>(&g_oSwap));

    __android_log_print(ANDROID_LOG_INFO, "BlockOfSky", "gl hooks: %s", g_hookStatus.c_str());
    std::thread(writeStatusLoop).detach();
}

void markRenderLevel(bool begin) {
    t_inRenderLevel = begin;
    if (!g_capturing.load(std::memory_order_relaxed)) return;
    Rec r;
    r.kind = begin ? Kind::RlBegin : Kind::RlEnd;
    r.tid = currentTid();
    push(r);
}

}  // namespace skygl
