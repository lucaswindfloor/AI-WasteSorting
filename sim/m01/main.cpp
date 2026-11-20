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
                        Scenario::SlowArchive, Scenario::Reconnect}) {
        if (name(option) == value) return option;
    }
    throw std::invalid_argument("unknown scenario");
}

Options parse(int argc, char** argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (++i >= argc) throw std::invalid_argument("missing option value");
        const std::string_view value = argv[i];
        if (option == "--scenario") out.scenario = scenario(value);
        else if (option == "--output") out.output = value;
        else if (option == "--speed-mm-s") out.speed_mm_s = bounded(value, 500, 3000);
        else if (option == "--rgb-width") out.profile.rgb_width = bounded(value, 16, 8192);
        else if (option == "--hsi-width") out.profile.hsi_width = bounded(value, 16, 640);
        else if (option == "--hsi-bands") out.profile.hsi_bands = bounded(value, 2, 224);
        else if (option == "--packet-lines") out.profile.packet_lines = bounded(value, 1, 8);
        else if (option == "--depth" && value == "independent") out.depth = DepthMode::Independent;
        else if (option == "--depth" && value == "encoder") out.depth = DepthMode::Encoder;
        else throw std::invalid_argument("unknown option or value: " + std::string(option));
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
    std::cout << "m01_sim [--scenario normal|missing|late|mixed|slow-archive|reconnect]\n"
                 "        [--speed-mm-s 500..3000] [--depth independent|encoder]\n"
                 "        [--rgb-width 16..8192] [--hsi-width 16..640]\n"
                 "        [--hsi-bands 2..224] [--packet-lines 1..8] [--output NEW_DIRECTORY]\n";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        help();
        return 0;
    }
    try {
        const auto options = parse(argc, argv);
        const auto report = run(options);
        std::cout << (report.passed ? "PASS" : "FAIL")
                  << " pairs=" << report.paired << " missing=" << report.missing << " late=" << report.late
                  << " depth=" << report.depth << " verified_archive_blocks=" << report.verified << '\n'
                  << "Report: " << (options.output / "report.json").string() << '\n'
                  << "Ledger: " << (options.output / "ledger.csv").string() << '\n';
        return report.passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "m01_sim: " << error.what() << '\n';
        return 2;
    }
}
