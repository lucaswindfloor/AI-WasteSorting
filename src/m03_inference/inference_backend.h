#pragma once

#include "model_manifest.h"

namespace waste::m03 {

struct Box {
    std::size_t x0{}, y0{}, x1{}, y1{};  // Half-open model pixels.
};

struct Candidate {
    std::size_t class_index{};
    float score{};
    Box box;
    // Empty means every pixel in box. Otherwise row-major, one byte (0/1) per pixel.
    std::vector<std::uint8_t> mask;
};

struct BackendCompletion {
    std::vector<Candidate> candidates;
    std::string error;
};

class BackendJob {
public:
    virtual ~BackendJob() = default;
    // Returns no value while execution (including DMA) still uses the input.
    // If an implementation throws, destruction must still make input reuse safe.
    virtual std::optional<BackendCompletion> poll(Tick now) = 0;
};

class LoadedModel {
public:
    virtual ~LoadedModel() = default;
    // The backend attests to the artifact and execution settings actually loaded.
    virtual ModelIdentity identity() const = 0;
    virtual BackendCompletion warmup() = 0;
    // The job must retain any device work and input until completion or safe teardown.
    virtual std::unique_ptr<BackendJob> submit(std::shared_ptr<const PreparedInput> input, Tick now) = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::shared_ptr<LoadedModel> load(const Manifest& manifest,
                                               std::span<const std::byte> artifact) = 0;
};

}  // namespace waste::m03
