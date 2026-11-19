#include "payload_buffer.h"

namespace waste::m01 {

PayloadBuffer::PayloadBuffer() : used_(std::make_shared<std::atomic_size_t>(0)) {}

Payload PayloadBuffer::copy(std::span<const std::byte> data, std::size_t capacity) {
    const auto occupied = used();
    if (occupied > capacity || data.size() > capacity - occupied) {
        return {};
    }
    auto bytes = std::make_unique<const std::vector<std::byte>>(data.begin(), data.end());
    used_->fetch_add(data.size());
    return Payload(bytes.release(), [used = used_](const std::vector<std::byte>* value) {
        const auto size = value->size();
        delete value;
        used->fetch_sub(size);
    });
}

std::size_t PayloadBuffer::used() const {
    return used_->load();
}

}  // namespace waste::m01
