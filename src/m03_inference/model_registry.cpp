#include "model_registry.h"
#include "output_decoder.h"

#include <stdexcept>

namespace waste::m03 {

ModelRegistry::ModelRegistry(std::size_t capacity) : capacity_(capacity) {
    if (!capacity) {
        throw std::invalid_argument("model capacity must be positive");
    }
}

void ModelRegistry::install(Manifest manifest, std::span<const std::byte> artifact, Backend& backend) {
    validate(manifest);
    const auto key = manifest.identity.key;
    if (artifact.empty() || models_.contains(key) || models_.size() >= capacity_) {
        throw std::invalid_argument("empty artifact, duplicate model key, or model registry full");
    }
    if (auto prior = history_.find(key); prior != history_.end() && prior->second != manifest.identity) {
        throw std::invalid_argument("model version identity changed");
    }
    auto engine = backend.load(manifest, artifact);
    if (!engine || engine->identity() != manifest.identity) {
        throw std::runtime_error("backend model identity mismatch");
    }
    if (const auto issue = validate_output(manifest, engine->warmup()); !issue.empty()) {
        throw std::runtime_error("warmup rejected: " + issue);
    }
    history_.try_emplace(key, manifest.identity);
    models_.emplace(key, std::make_shared<const InstalledModel>(
                             InstalledModel{std::move(manifest), std::move(engine)}));
}

std::shared_ptr<const InstalledModel> ModelRegistry::find(const ModelKey& key) const {
    const auto found = models_.find(key);
    return found == models_.end() ? nullptr : found->second;
}

std::vector<ModelIdentity> ModelRegistry::loaded() const {
    std::vector<ModelIdentity> result;
    result.reserve(models_.size());
    for (const auto& [key, model] : models_) {
        result.push_back(model->manifest.identity);
    }
    return result;
}

bool ModelRegistry::unload(const ModelKey& key) {
    const auto found = models_.find(key);
    if (found == models_.end() || found->second.use_count() != 1) {
        return false;
    }
    models_.erase(found);
    return true;
}

}  // namespace waste::m03
