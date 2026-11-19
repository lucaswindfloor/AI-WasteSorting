#pragma once

#include "acquisition_types.h"

#include <atomic>
#include <set>

namespace waste::m01 {

class SegmentBuilder {
public:
    SegmentBuilder(std::shared_ptr<const Config> config,
                   std::shared_ptr<std::atomic_size_t> inflight);
    void add(SegmentItem item, Tick now, Output& out);
    void expire(Tick now, Output& out);
    void flush(Output& out);

private:
    std::shared_ptr<const Config> config_;
    std::shared_ptr<std::atomic_size_t> inflight_;
    std::vector<SegmentItem> items_;
    std::set<const void*> payloads_;
    std::size_t bytes_{};
    std::optional<Tick> opened_;
    std::uint64_t sequence_{};
};

}  // namespace waste::m01
