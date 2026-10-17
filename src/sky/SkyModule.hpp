#pragma once

#include "modules/Module.hpp"

class SkyModule : public Module {
public:
    SkyModule() : Module("BlockOfSky", "Block of Sky: shows the sky through the block") {}

    void onInit() override;
    void onFrame() override;
    void onEnable() override;
    void onDisable() override;
};
