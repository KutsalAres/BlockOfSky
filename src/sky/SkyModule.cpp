#include "SkyModule.hpp"

#include "config/ConfigManager.hpp"
#include <bedrocktools/memory/Signatures.hpp>

#include <android/log.h>
#include <filesystem>
#include <fstream>

namespace {

struct Entry {
    bedrocktools::memory::SignatureId id;
    const char* name;
};

}

void SkyModule::onInit() {
    setMasterEnabled(true);

    using bedrocktools::memory::SignatureId;
    const Entry entries[] = {
        {SignatureId::VersionString, "VersionString"},
        {SignatureId::RenderLevel, "RenderLevel"},
        {SignatureId::SetupFogPlayer, "SetupFogPlayer"},
        {SignatureId::TessellatorBegin, "TessellatorBegin"},
        {SignatureId::TessellatorColor, "TessellatorColor"},
        {SignatureId::TessellatorVertex, "TessellatorVertex"},
        {SignatureId::MeshHelpersRenderMeshImmediately, "MeshHelpersRenderMeshImmediately"},
        {SignatureId::MeshHelpersRenderMeshImmediately2, "MeshHelpersRenderMeshImmediately2"},
        {SignatureId::RenderMaterialGroupCommon, "RenderMaterialGroupCommon"},
        {SignatureId::BlockTessellatorTessellateFaceDown, "TessellateFaceDown"},
        {SignatureId::BlockTessellatorTessellateFaceUp, "TessellateFaceUp"},
        {SignatureId::BlockTessellatorTessellateFaceNorth, "TessellateFaceNorth"},
        {SignatureId::BlockTessellatorTessellateFaceSouth, "TessellateFaceSouth"},
        {SignatureId::BlockTessellatorTessellateFaceWest, "TessellateFaceWest"},
        {SignatureId::BlockTessellatorTessellateFaceEast, "TessellateFaceEast"},
        {SignatureId::BlockGraphicsGetTexture, "BlockGraphicsGetTexture"},
        {SignatureId::BlockSourceGetBlockForTessellation, "BlockSourceGetBlockForTessellation"},
        {SignatureId::BlockOccluderUpdateRenderFace, "BlockOccluderUpdateRenderFace"},
        {SignatureId::TextureUVCoordinateSetCopyCtor, "TextureUVCopyCtor"},
        {SignatureId::TextureUVCoordinateSetDtor, "TextureUVDtor"},
        {SignatureId::RenderChunkCoordinatorSetAllDirty, "RenderChunkSetAllDirty"},
    };

    const auto configPath = std::filesystem::path(
        bedrocktools::config::ConfigManager::get().getConfigPath());
    std::ofstream out(configPath.parent_path() / "sky_signatures.txt");

    for (const auto& entry : entries) {
        const bool ok = bedrocktools::memory::resolve(entry.id) != 0;
        __android_log_print(ANDROID_LOG_INFO, "BlockOfSky", "%s: %s",
                            entry.name, ok ? "OK" : "MISSING");
        if (out) out << entry.name << ": " << (ok ? "OK" : "MISSING") << "\n";
    }
}
