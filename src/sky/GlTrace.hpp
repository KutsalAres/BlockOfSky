#pragma once

#include <filesystem>

namespace skygl {

// OpenGL ES cagrilarini (draw, bind, clear, swap) izler ve bir kare boyunca
// hangi durumda neyin cizildigini sky_gltrace_N.txt dosyalarina yazar.
void init(const std::filesystem::path& dir);

// RenderLevel basliyor (true) / bitiyor (false). RenderLevel thread'inde cagrilir.
void markRenderLevel(bool begin);

}  // namespace skygl
