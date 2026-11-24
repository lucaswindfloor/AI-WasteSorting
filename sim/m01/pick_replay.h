#pragma once

#include "simulator.h"
#include "m03_inference/inference_types.h"

#include <functional>

namespace waste::m09 { class ArchiveService; }

namespace waste::m01sim {

inline constexpr auto pet_depth_period = std::chrono::milliseconds(4);

// Keeps the acquisition simulator's clock and connects typed M02/M03 results to M04–M07.
class PickReplay {
public:
    using Log = std::function<void(m01::Tick, std::string_view, std::string)>;

    PickReplay(Scenario scenario, std::string mujoco_case, const Layout& layout, unsigned depth_width, m09::ArchiveService& archive,
               const m02::Snapshot& snapshot, const m03::ModelIdentity& model, Log log);
    ~PickReplay();
    void encoder(m01::Tick now, std::int64_t count);
    void surfaces(m01::Tick now, const m02::Result& result);
    void window(m01::Tick now, const m03::Result& result);
    void advance(m01::Tick now);
    void drain();
    std::string verify();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace waste::m01sim
