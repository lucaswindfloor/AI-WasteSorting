#pragma once

#include "m09_archive/archive_service.h"
#include "m09_archive/evidence/reconciliation_service.h"

#include <cstdlib>
#include <stdexcept>

namespace waste::m09::test {

template<class T>
void check(T&& value, const char* problem) {
    if (!value) throw std::runtime_error(problem);
}

struct TemporaryArchive {
    disk::Path path;
    explicit TemporaryArchive() {
        auto name = (std::filesystem::temp_directory_path() / "waste-m09-XXXXXX").string();
        if (!::mkdtemp(name.data())) throw std::runtime_error("could not create test archive");
        path = name;
    }
    ~TemporaryArchive() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    TemporaryArchive(const TemporaryArchive&) = delete;
    TemporaryArchive& operator=(const TemporaryArchive&) = delete;
};

inline Authorize authorize() {
    return [](std::string_view name, std::string_view, Access access, std::string_view,
              const Selection& selection) -> std::optional<Selection> {
        if (name == "auditor" || (name == "reader" && access == Access::Read)) return selection;
        return {};
    };
}

inline Record record(std::string run, Kind kind, std::string id, std::uint64_t version = 1) {
    Record row;
    row.run = std::move(run);
    row.key = {kind, std::move(id), version};
    row.epoch = 1;
    row.occurred = {"test-clock", 100};
    row.received = {"test-clock", 105};
    return row;
}

inline SegmentInput segment(std::string run, std::string id, const Bytes& first, const Bytes& second) {
    Block a;
    a.frame = 21;
    a.row = 466;
    a.trigger = "2147";
    a.occurred = {"test-clock", 100};
    a.received = {"test-clock", 105};
    a.format = "uint16";
    auto b = a;
    b.row = 468;
    b.occurred.ticks = 110;
    b.received.ticks = 115;
    return {{std::move(run), "HSI", "s1", std::move(id)}, 1, "uint16", {{a, first}, {b, second}},
            {{"HSI", "s1", "row", {467, 468}, "Not captured"}}};
}

void segment_recovery();
void command_journal();
void reconciliation();
void export_replay();
void storage_pressure();
void archive_integration();

}  // namespace waste::m09::test
