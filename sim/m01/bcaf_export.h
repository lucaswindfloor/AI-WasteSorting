#pragma once

#include "scene.h"
#include "m02_calibration/calibration_types.h"

#include <filesystem>
#include <fstream>

namespace waste::m01sim {

class BcafExport {
public:
    explicit BcafExport(const std::filesystem::path& output);
    void append(const m02::Result& result, const Layout& layout);
    void finish();

private:
    std::ofstream rgb_, hsi_;
    std::vector<float> spectrum_;
};

}  // namespace waste::m01sim
