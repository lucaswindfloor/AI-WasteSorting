#pragma once

#include "m03_inference/inference_backend.h"

namespace waste::m03::test {

struct FakeControls {
    Tick delay{};
    BackendCompletion output;
    BackendCompletion warmup_output;
    bool wrong_identity{};
    std::weak_ptr<const PreparedInput> latest;
    std::size_t submitted{};
};

class FakeJob final : public BackendJob {
public:
    FakeJob(Tick ready, BackendCompletion output, std::shared_ptr<const PreparedInput> input)
        : ready_(ready), output_(std::move(output)), input_(std::move(input)) {}

    std::optional<BackendCompletion> poll(Tick now) override {
        return now < ready_ ? std::nullopt : std::optional(output_);
    }

private:
    Tick ready_;
    BackendCompletion output_;
    std::shared_ptr<const PreparedInput> input_;
};

class FakeModel final : public LoadedModel {
public:
    FakeModel(ModelIdentity id, std::shared_ptr<FakeControls> controls)
        : id_(std::move(id)), controls_(std::move(controls)) {}

    ModelIdentity identity() const override {
        auto identity = id_;
        if (controls_->wrong_identity) {
            identity.artifact_digest = "wrong-digest";
        }
        return identity;
    }

    BackendCompletion warmup() override { return controls_->warmup_output; }

    std::unique_ptr<BackendJob> submit(std::shared_ptr<const PreparedInput> input, Tick now) override {
        ++controls_->submitted;
        controls_->latest = input;
        return std::make_unique<FakeJob>(now + controls_->delay, controls_->output, std::move(input));
    }

private:
    ModelIdentity id_;
    std::shared_ptr<FakeControls> controls_;
};

class FakeBackend final : public Backend {
public:
    std::shared_ptr<FakeControls> controls = std::make_shared<FakeControls>();

    std::shared_ptr<LoadedModel> load(const Manifest& m, std::span<const std::byte>) override {
        return std::make_shared<FakeModel>(m.identity, controls);
    }
};

}  // namespace waste::m03::test
