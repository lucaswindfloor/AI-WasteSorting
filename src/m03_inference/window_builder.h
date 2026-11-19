#pragma once

#include "model_manifest.h"

#include <map>
#include <set>

namespace waste::m03 {

enum class Insert { Accepted, Closed, OutOfRange };

class WindowBuilder {
public:
    WindowBuilder(Generation generation, const Manifest& model, std::uint64_t first_trigger);
    Insert add(std::uint64_t trigger, InputRow row);
    Insert mark_gap(std::uint64_t first, std::uint64_t last);
    // The caller confirms that all sources through this trigger have settled.
    Insert seal_through(std::uint64_t trigger);
    std::optional<Window> take(Tick now);
    std::optional<WindowId> unfinished() const;

private:
    WindowId id(std::uint64_t last) const;
    bool in_range(std::uint64_t trigger) const;

    Generation generation_;
    ModelKey model_;
    WindowSpec spec_;
    std::uint64_t next_;
    bool ended_{};
    std::optional<std::uint64_t> horizon_, sealed_;
    std::map<std::uint64_t, InputRow> rows_;
    std::set<std::uint64_t> gaps_;
};

}  // namespace waste::m03
