#pragma once

#include "scene.h"
#include <filesystem>

namespace waste::m01sim {

enum class Scenario {
    Normal, Missing, Late, Mixed, SlowArchive, Reconnect, TransportDelay, ExposureDelay,
    Compensated, ExposureLimit, ClockMissing, CountInterpolation, InvalidWhite, InferenceOverload, ModelSwitch,
    RgbFrameLoss, PetPick, PetDepthLoss, PetEncoderLoss, PetReceiptLoss
};
enum class DepthMode { Independent, Encoder };
enum class InferenceMode { Off, Rgb, Hsi, Split, Joint };

struct Options {
    Scenario scenario{Scenario::Mixed};
    DepthMode depth{DepthMode::Independent};
    unsigned speed_mm_s{1000};
    Profile profile;
    std::filesystem::path output;
    InferenceMode inference{InferenceMode::Joint};
    bool allow_masks{};
    bool production{};
    bool export_bcaf{};
    std::string mujoco_case;
};

struct Report {
    bool passed{};
    unsigned paired{}, missing{}, late{}, depth{}, verified{}, calibrated{}, covered{};
    unsigned detected_windows{}, empty_windows{}, invalid_windows{}, expired_windows{}, full_windows{};
    unsigned rgb_rows{}, independent_hsi{};
};

std::string_view name(Scenario scenario);
bool pet_pick(Scenario scenario);
std::string_view name(InferenceMode mode);
Report run(const Options& options);

}  // namespace waste::m01sim
