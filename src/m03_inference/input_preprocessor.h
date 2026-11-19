#pragma once

#include "model_manifest.h"

namespace waste::m03 {

struct Preparation {
    std::shared_ptr<const PreparedInput> input;
    std::string issue;
};

std::size_t input_bytes(const Manifest& manifest);
Preparation prepare(const Manifest& manifest, const Window& window);

}  // namespace waste::m03
