#include "inference.h"
#include "m09_archive/storage/record_codec.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace waste::m01sim {
namespace {

using namespace m03;

constexpr std::string_view backend_id = "sim-rules";
constexpr std::array artifact_v1{std::byte{100}, std::byte{150}, std::byte{10}, std::byte{3}};
constexpr std::array artifact_v2{std::byte{108}, std::byte{150}, std::byte{10}, std::byte{4}};
constexpr std::string_view processing_version = "rules-1";

struct Rules {
    float green_min, brightness_min, reflectance_min;
    std::size_t minimum_area;
};

bool foreground(const Tensor& rgb, std::size_t pixel, const Rules& rules) {
    const auto base = pixel * rgb.channels;
    const auto values = std::span(rgb.values).subspan(base, rgb.channels);
    const auto valid = std::span(rgb.valid).subspan(base, rgb.channels);
    return std::ranges::all_of(valid, [](auto value) { return value != 0; }) &&
           values[1] > rules.green_min && *std::ranges::max_element(values) > rules.brightness_min;
}

// Only the endpoints of the synthetic spectra distinguish an increasing PET slope from a decreasing Al slope.
int material(const Tensor& hsi, std::size_t pixel, const Rules& rules) {
    const auto first = pixel * hsi.channels;
    const auto last = first + hsi.channels - 1;
    if (!hsi.valid[first] || !hsi.valid[last]) return -1;
    const auto short_wave = hsi.values[first];
    const auto long_wave = hsi.values[last];
    if (std::max(short_wave, long_wave) < rules.reflectance_min) return -1;
    return long_wave > short_wave ? 0 : 1;
}

struct Component {
    std::size_t label;
    Box box;
    std::vector<std::size_t> pixels;
};

std::vector<Component> components(std::vector<int> classes, std::size_t width, std::size_t height) {
    std::vector<Component> result;
    for (std::size_t first = 0; first < classes.size(); ++first) {
        if (classes[first] < 0) continue;
        const auto label = classes[first];
        Component component{static_cast<std::size_t>(label), {width, height, 0, 0}, {first}};
        classes[first] = -1;
        const auto visit = [&](std::size_t pixel) {
            if (classes[pixel] == label) {
                classes[pixel] = -1;
                component.pixels.push_back(pixel);
            }
        };
        for (std::size_t i = 0; i < component.pixels.size(); ++i) {
            const auto pixel = component.pixels[i];
            const auto x = pixel % width;
            const auto y = pixel / width;
            auto& box = component.box;
            box = {std::min(x, box.x0), std::min(y, box.y0), std::max(x + 1, box.x1), std::max(y + 1, box.y1)};
            if (x) visit(pixel - 1);
            if (x + 1 < width) visit(pixel + 1);
            if (y) visit(pixel - width);
            if (y + 1 < height) visit(pixel + width);
        }
        result.push_back(std::move(component));
    }
    return result;
}

BackendCompletion infer(const PreparedInput& input, const Rules& rules, std::size_t limit) {
    const Tensor* rgb = nullptr;
    const Tensor* hsi = nullptr;
    for (const auto& tensor : input.tensors) {
        if (tensor.source == m01::Source::Rgb) rgb = &tensor;
        if (tensor.source == m01::Source::Hsi) hsi = &tensor;
    }
    std::vector<int> classes(input.width * input.height, -1);
    for (std::size_t pixel = 0; pixel < classes.size(); ++pixel) {
        if (rgb && !foreground(*rgb, pixel, rules)) continue;
        classes[pixel] = hsi ? material(*hsi, pixel, rules) : 0;
    }
    auto regions = components(std::move(classes), input.width, input.height);
    std::ranges::stable_sort(regions, [](const auto& a, const auto& b) { return a.pixels.size() > b.pixels.size(); });
    BackendCompletion output;
    for (const auto& region : regions) {
        if (region.pixels.size() < rules.minimum_area || output.candidates.size() == limit) break;
        const auto& box = region.box;
        const auto width = box.x1 - box.x0;
        Candidate candidate{region.label, 1.0f, box, std::vector<std::uint8_t>(width * (box.y1 - box.y0))};
        for (const auto pixel : region.pixels) {
            candidate.mask[(pixel / input.width - box.y0) * width + pixel % input.width - box.x0] = 1;
        }
        output.candidates.push_back(std::move(candidate));
    }
    return output;
}

class SimulationJob final : public BackendJob {
public:
    SimulationJob(Tick ready, std::shared_ptr<const PreparedInput> input, Rules rules, std::size_t limit)
        : ready_(ready), input_(std::move(input)), rules_(rules), limit_(limit) {}

    std::optional<BackendCompletion> poll(Tick now) override {
        if (now < ready_) return std::nullopt;
        return infer(*input_, rules_, limit_);
    }
private:
    Tick ready_;
    std::shared_ptr<const PreparedInput> input_;
    Rules rules_;
    std::size_t limit_;
};

class SimulationModel final : public LoadedModel {
public:
    SimulationModel(Manifest manifest, Rules rules, Tick delay)
        : manifest_(std::move(manifest)), rules_(rules), delay_(delay) {}

    ModelIdentity identity() const override { return manifest_.identity; }

    BackendCompletion warmup() override {
        PreparedInput input;
        input.width = manifest_.raster.width();
        input.height = manifest_.raster.height();
        const auto add = [&](m01::Source source, std::size_t channels) {
            const auto values = input.width * input.height * channels;
            input.tensors.push_back({source, input.height, input.width, channels,
                                     std::vector<float>(values), std::vector<std::uint8_t>(values, 1)});
        };
        if (manifest_.rgb) add(m01::Source::Rgb, 3);
        if (manifest_.hsi) add(m01::Source::Hsi, manifest_.hsi->bands.size());
        return infer(input, rules_, manifest_.max_candidates);
    }

    std::unique_ptr<BackendJob> submit(std::shared_ptr<const PreparedInput> input, Tick now) override {
        return std::make_unique<SimulationJob>(now + delay_, std::move(input), rules_, manifest_.max_candidates);
    }
private:
    Manifest manifest_;
    Rules rules_;
    Tick delay_;
};

}  // namespace

std::vector<m03::Route> inference_routes(InferenceMode mode) {
    using m03::Route;
    switch (mode) {
    case InferenceMode::Off: return {};
    case InferenceMode::Rgb: return {Route::Rgb};
    case InferenceMode::Hsi: return {Route::Hsi};
    case InferenceMode::Split: return {Route::Rgb, Route::Hsi};
    case InferenceMode::Joint: return {Route::Joint};
    }
    return {};
}

std::string_view name(InferenceMode mode) {
    switch (mode) {
    case InferenceMode::Off: return "off";
    case InferenceMode::Rgb: return "rgb";
    case InferenceMode::Hsi: return "hsi";
    case InferenceMode::Split: return "split";
    case InferenceMode::Joint: return "joint";
    }
    return {};
}

std::string_view name(m03::Route route) {
    switch (route) {
    case m03::Route::Rgb: return "rgb";
    case m03::Route::Hsi: return "hsi";
    case m03::Route::Joint: return "joint";
    }
    return {};
}

std::string_view name(m03::State state) {
    switch (state) {
    case m03::State::Completed: return "completed";
    case m03::State::NoDetection: return "no_detection";
    case m03::State::InputInvalid: return "input_invalid";
    case m03::State::Expired: return "expired";
    case m03::State::QueueFull: return "queue_full";
    case m03::State::Failed: return "failed";
    case m03::State::Retired: return "retired";
    case m03::State::Rejected: return "rejected";
    }
    return {};
}

std::span<const std::byte> inference_artifact(std::string_view version) {
    return version == "1" ? std::span<const std::byte>(artifact_v1) : std::span<const std::byte>(artifact_v2);
}

m03::Manifest inference_manifest(m03::Route route, const m01::Config& config, const m02::Snapshot& snapshot,
                                 std::string version, bool allow_masks, std::size_t rows, std::size_t step, m03::Tick budget) {
    using namespace std::chrono_literals;
    m03::Manifest manifest;
    manifest.route = route;
    const auto digest = m09::wire::digest(inference_artifact(version));
    manifest.identity = {{"sim-" + std::string(name(route)), std::move(version)}, digest, std::string(backend_id), "fp32",
                         std::string(processing_version)};
    if (route != m03::Route::Hsi) {
        manifest.rgb = m03::Channels{"RGB8", {}, m02::SpectralLevel::Raw, {}, {0, 0, 0},
                                     {1.f / 255, 1.f / 255, 1.f / 255}};
    }
    if (route != m03::Route::Rgb) {
        const auto& bands = config.hsi.format.bands;
        manifest.hsi = m03::Channels{"U16LE", bands, m02::SpectralLevel::DarkWhiteCorrected,
                                     snapshot.versions.spectral_reference, std::vector<float>(bands.size()),
                                     std::vector<float>(bands.size(), 1.f)};
    }
    const auto width = manifest.rgb ? config.rgb.format.width : config.hsi.format.width;
    const auto production = config.routing == m01::Routing::RgbTriggered;
    manifest.window = {rows, step, allow_masks ? 2u : 0u, production ? rows + 2 * step : 128,
                       production ? budget : 110ms, allow_masks, allow_masks ? 0.85f : 1.f};
    const auto resized_rows = production ? rows / 4 : rows;
    const auto resized_cols = std::min<std::size_t>(width, production ? 250 : 64);
    manifest.raster = {0, 0, rows, width, resized_rows, resized_cols, 0, 0, 0, 0, 0};
    if (route == m03::Route::Rgb) {
        manifest.labels = {{"object", m03::EvidenceKind::Contour}};
    } else {
        const auto kind = route == m03::Route::Hsi ? m03::EvidenceKind::Material : m03::EvidenceKind::JointInstance;
        manifest.labels = {{"PET", kind}, {"AL", kind}};
    }
    manifest.score_kind = m03::ScoreKind::Score;
    manifest.threshold = 0.5f;
    manifest.max_candidates = 8;
    return manifest;
}

std::shared_ptr<m03::LoadedModel> SimulationBackend::load(const m03::Manifest& manifest,
                                                         std::span<const std::byte> artifact) {
    if (artifact.size() != artifact_v1.size() || manifest.identity.backend != backend_id ||
        manifest.identity.precision != "fp32" || manifest.identity.processing_version != processing_version ||
        manifest.identity.artifact_digest != m09::wire::digest(artifact)) {
        throw std::invalid_argument("invalid simulated model artifact");
    }
    const auto value = [&](std::size_t i) { return std::to_integer<unsigned>(artifact[i]); };
    const Rules rules{value(0) / 255.f, value(1) / 255.f, value(2) / 100.f, value(3)};
    return std::make_shared<SimulationModel>(manifest, rules, delay_);
}

}  // namespace waste::m01sim
