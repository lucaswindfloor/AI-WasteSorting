#pragma once

#include "simulator.h"
#include "m03_inference/inference_service.h"

namespace waste::m01sim {

inline constexpr std::size_t inference_rows = 32;
inline constexpr std::size_t inference_step = 16;
inline constexpr m03::Tick inference_budget = std::chrono::milliseconds(120);

std::vector<m03::Route> inference_routes(InferenceMode mode);
std::string_view name(m03::Route route);
std::string_view name(m03::State state);
std::span<const std::byte> inference_artifact(std::string_view version);
m03::Manifest inference_manifest(m03::Route route, const m01::Config& config, const m02::Snapshot& snapshot,
                                 std::string version, bool allow_masks, std::size_t rows, std::size_t step, m03::Tick budget);

class SimulationBackend final : public m03::Backend {
public:
    explicit SimulationBackend(m03::Tick delay) : delay_(delay) {}
    std::shared_ptr<m03::LoadedModel> load(const m03::Manifest& manifest,
                                           std::span<const std::byte> artifact) override;

private:
    m03::Tick delay_;
};

}  // namespace waste::m01sim
