#include "simulator.h"

#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace waste::m01sim;

unsigned bounded(std::string_view input, unsigned lo, unsigned hi) {
    unsigned value{};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (error != std::errc{} || end != input.data() + input.size() || value < lo || value > hi) {
        throw std::invalid_argument("numeric option outside its allowed range");
    }
    return value;
}

Scenario scenario(std::string_view value) {
    for (auto option : {Scenario::Normal, Scenario::Missing, Scenario::Late, Scenario::Mixed,
                        Scenario::SlowArchive, Scenario::Reconnect, Scenario::TransportDelay,
                        Scenario::ExposureDelay, Scenario::Compensated, Scenario::ExposureLimit,
                        Scenario::ClockMissing, Scenario::CountInterpolation, Scenario::InvalidWhite,
                        Scenario::InferenceOverload, Scenario::ModelSwitch, Scenario::RgbFrameLoss,
                        Scenario::PetPick, Scenario::PetDepthLoss, Scenario::PetEncoderLoss, Scenario::PetReceiptLoss}) {
        if (name(option) == value) return option;
    }
    throw std::invalid_argument("unknown scenario");
}

InferenceMode inference_mode(std::string_view value) {
    for (auto mode : {InferenceMode::Off, InferenceMode::Rgb, InferenceMode::Hsi, InferenceMode::Split,
                      InferenceMode::Joint}) {
        if (name(mode) == value) return mode;
    }
    throw std::invalid_argument("unknown inference mode");
}

Options parse(int argc, char** argv) {
    Options out;
    bool scenario_set{}, inference_set{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--export-bcaf") {
            out.export_bcaf = true;
            continue;
        }
        if (++i >= argc) throw std::invalid_argument("missing option value");
        const std::string_view value = argv[i];
        if (option == "--scenario") {
            out.scenario = scenario(value);
            scenario_set = true;
        }
        else if (option == "--output") out.output = value;
        else if (option == "--speed-mm-s") out.speed_mm_s = bounded(value, 500, 3000);
        else if (option == "--rgb-width") out.profile.rgb_width = bounded(value, 16, 8192);
        else if (option == "--hsi-width") out.profile.hsi_width = bounded(value, 16, 640);
        else if (option == "--hsi-bands") out.profile.hsi_bands = bounded(value, 2, 224);
        else if (option == "--packet-lines") out.profile.packet_lines = bounded(value, 1, 8);
        else if (option == "--mujoco" && (value == "normal" || value == "slow" || value == "offset")) out.mujoco_case = value;
        else if (option == "--depth" && value == "independent") out.depth = DepthMode::Independent;
        else if (option == "--depth" && value == "encoder") out.depth = DepthMode::Encoder;
        else if (option == "--m03") {
            out.inference = inference_mode(value);
            inference_set = true;
        }
        else if (option == "--m03-mask" && value == "allow") out.allow_masks = true;
        else if (option == "--m03-mask" && value == "strict") out.allow_masks = false;
        else if (option == "--layout" && value == "production") out.production = true;
        else if (option == "--layout" && value == "demo") out.production = false;
        else throw std::invalid_argument("unknown option or value: " + std::string(option));
    }
    if (out.production) {
        if (!scenario_set) out.scenario = Scenario::Normal;
        if (!inference_set) out.inference = InferenceMode::Rgb;
        out.profile = {8192, 640, 224, 256, production_frame_rows};
    }
    if (out.output.empty()) {
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        out.output = std::filesystem::path("build/m01-sim-runs") /
                     (std::to_string(time) + "-" + std::to_string(::getpid()));
    }
    return out;
}

void help() {
    std::cout << "m01_sim [--layout demo|production] [--scenario NAME]\n"
                 "        m01_sim --production-layout prints the production station geometry as JSON\n"
                 "        normal|missing|late|mixed|slow-archive|reconnect\n"
                 "        transport-delay|exposure-delay|compensated|exposure-limit\n"
                 "        clock-missing|count-interpolation|invalid-white|inference-overload|model-switch\n"
                 "        production uses normal|rgb-frame-loss, RGB/off and 1000..3000 mm/s\n"
                 "        pet-pick|pet-depth-loss|pet-encoder-loss|pet-receipt-loss use production, RGB/strict at 1000 mm/s\n"
                 "        [--mujoco normal|slow|offset] selects headless MuJoCo for a PET scenario\n"
                 "        [--speed-mm-s 500..3000] [--depth independent|encoder]\n"
                 "        [--m03 off|rgb|hsi|split|joint] [--m03-mask strict|allow]\n"
                 "        [--export-bcaf] export calibrated production RGB+HSI for offline inference\n"
                 "        [--rgb-width 16..8192] [--hsi-width 16..640]\n"
                 "        [--hsi-bands 2..224] [--packet-lines 1..8] [--output NEW_DIRECTORY]\n";
}

void print_production_layout() {
    const auto& layout = production_layout;
    std::cout << "{\"layout\":\"production\",\"beltWidth\":" << layout.width
              << ",\"beltLength\":" << layout.length << ",\"scanX\":" << layout.rgb_x
              << ",\"hsiScanX\":" << layout.hsi_x << ",\"depthScanX\":" << layout.depth_x
              << ",\"pickBegin\":" << layout.pick_begin << ",\"pickEnd\":" << layout.pick_end << "}\n";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        help();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--production-layout") {
        print_production_layout();
        return 0;
    }
    try {
        const auto options = parse(argc, argv);
        const auto report = run(options);
        std::cout << (report.passed ? "PASS" : "FAIL")
                  << " pairs=" << report.paired << " missing=" << report.missing << " late=" << report.late
                  << " rgb_rows=" << report.rgb_rows << " independent_hsi=" << report.independent_hsi
                  << " m02_coverage=" << report.covered << "/" << report.calibrated
                  << " m03_detected=" << report.detected_windows << " m03_empty=" << report.empty_windows
                  << " m03_invalid=" << report.invalid_windows << " m03_expired=" << report.expired_windows
                  << " m03_queue_full=" << report.full_windows
                  << " depth=" << report.depth << " verified_archive_blocks=" << report.verified << '\n'
                  << "Report: " << (options.output / "report.json").string() << '\n'
                  << "Ledger: " << (options.output / "ledger.csv").string() << '\n';
        return report.passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "m01_sim: " << error.what() << '\n';
        return 2;
    }
}
