#pragma once

#include "modules/Module.hpp"

class SkyModule : public Module {
public:
    SkyModule() : Module("BlockOfSky", "Shows the real sky through Block of Sky") {}

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
};
