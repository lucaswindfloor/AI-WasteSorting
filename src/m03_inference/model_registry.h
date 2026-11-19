#pragma once

#include "inference_backend.h"

#include <map>

namespace waste::m03 {

struct InstalledModel {
    Manifest manifest;
    std::shared_ptr<LoadedModel> engine;
};

class ModelRegistry {
public:
    explicit ModelRegistry(std::size_t capacity);
    void install(Manifest manifest, std::span<const std::byte> artifact, Backend& backend);
    std::shared_ptr<const InstalledModel> find(const ModelKey& key) const;
    bool unload(const ModelKey& key);
    std::vector<ModelIdentity> loaded() const;

private:
    std::size_t capacity_;
    std::map<ModelKey, std::shared_ptr<const InstalledModel>> models_;
    std::map<ModelKey, ModelIdentity> history_;
};

}  // namespace waste::m03
