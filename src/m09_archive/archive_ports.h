#pragma once

#include "archive_types.h"

#include <functional>
#include <span>
#include <string_view>

namespace waste::m09 {

// Bound by the host to an authenticated subject. The policy returns an approved
// subset for exports, or no selection when the operation is denied.
using Authorize = std::function<std::optional<Selection>(std::string_view subject,
    std::string_view run, Access, std::string_view purpose, const Selection&)>;
using Sink = std::function<bool(std::span<const std::byte>)>;

}  // namespace waste::m09
