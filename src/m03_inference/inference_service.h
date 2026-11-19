#pragma once

#include "inference_queue.h"
#include "window_builder.h"

namespace waste::m03 {

struct Activation {
    std::uint64_t first_trigger{};
    std::vector<ModelKey> models;
};

struct DeploymentStatus {
    std::vector<ModelIdentity> loaded;
    std::map<Generation, std::vector<ModelIdentity>> active;
};

class InferenceService {
public:
    InferenceService(std::size_t model_capacity, std::size_t active_capacity, QueueLimits queue,
                     Clock clock = {});
    void install(Manifest manifest, std::span<const std::byte> artifact, Backend& backend);
    bool unload(const ModelKey& key);
    void activate(const m01::Config& acquisition, const m02::Snapshot& calibration,
                  const Activation& activation);
    Report ingest(std::shared_ptr<const m02::Result> input, Tick now, Tick valid_until);
    Report advance(Tick now);
    Report seal_through(const Generation& generation, std::uint64_t trigger, Tick now);
    Report retire(const Generation& generation, Tick now, bool drain_approved_results);
    QueueHealth health() const;
    DeploymentStatus deployment_status() const;

private:
    struct Branch {
        std::shared_ptr<const InstalledModel> model;
        WindowBuilder builder;
    };
    struct Active {
        m01::Config acquisition;
        m02::Versions versions;
        std::vector<Branch> branches;
    };

    void check_time(Tick now);
    Report deliver(Report report) const;
    void build(Active& active, Report& report);
    void build_branch(Branch& branch, const m02::Versions& versions, Report& report);

    ModelRegistry registry_;
    std::size_t active_capacity_;
    std::map<Generation, Active> active_;
    std::map<std::string, std::uint64_t> last_generation_;
    Clock clock_;
    InferenceQueue queue_;
    Tick now_{}, input_time_{};
};

}  // namespace waste::m03
