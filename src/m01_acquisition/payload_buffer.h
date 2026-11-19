#pragma once

#include "acquisition_types.h"

#include <atomic>

namespace waste::m01 {

class PayloadBuffer {
public:
    PayloadBuffer();
    Payload copy(std::span<const std::byte> data, std::size_t capacity);
    std::size_t used() const;

private:
    // Consumers may release on other threads or outlive the acquisition service.
    std::shared_ptr<std::atomic_size_t> used_;
};

}  // namespace waste::m01
