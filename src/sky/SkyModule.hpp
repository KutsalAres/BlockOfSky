#pragma once

#include "modules/Module.hpp"

class SkyModule : public Module {
public:
    SkyModule() : Module("BlockOfSky", "Renders the sky on Block of Sky blocks") {}

    void onInit() override { setMasterEnabled(true); }
};
