#pragma once

#include "../m04_tracking/fixtures.h"
#include "m05_eligibility/eligibility_service.h"

namespace waste::m05::test {

using namespace std::chrono_literals;
using m02::test::check;
using m02::test::near;

inline ToolIdentity cup() { return {"arm-A", "cup-1", "tool-1", "tcp-1"}; }
inline BinIdentity primary() { return {"B01", "PET-bin-1"}; }
inline BinIdentity backup() { return {"B02", "PET-bin-2"}; }

inline Recipe recipe() {
    Recipe r;
    r.generation = {"P0", 1};
    r.version = "recipe-1";
    r.bin_mapping_version = "mapping-1";
    r.coverage = {"coverage-1", 1, 1, 0.05, 0.05};
    r.tools.emplace(cup(), ToolRule{cup(), 0.9, 0.35, 0.1, 1.0, 12});
    r.materials.emplace("PET", MaterialRule{"PET", {cup()}, {primary(), backup()},
                                            {BinCondition::Full}, false});
    r.limits = {256, 256, 4, 8, 8, 8, 4096};
    r.valid_until = 200ms;
    r.max_fact_age = 50ms;
    return r;
}

inline Snapshot facts() {
    return {{"P0", 1}, 18, "mapping-1",
            {{cup(), ToolCondition::Ready, {"E01", 10ms, 62ms}}},
            {{primary(), 11, BinCondition::Ready, {"PLC", 10ms, 62ms}},
             {backup(), 21, BinCondition::Ready, {"PLC", 10ms, 62ms}}}};
}

inline m04::Target target() {
    m04::Target t;
    t.id = 42;
    t.version = 1;
    t.generation = {"P0", 1};
    t.footprint.reference = m04::test::ref();
    t.state = m04::TargetState::Tracked;
    t.valid_until = 110ms;
    t.material.status = m04::MaterialStatus::Known;
    t.material.label = "PET";
    t.material.supporting = {{{m01::Source::Hsi, 13, 500, 1}, 10}};
    t.material.valid_until = 90ms;
    for (std::size_t row = 0; row < 5; ++row) {
        m04::SurfacePatch patch;
        patch.valid_until = 85ms;
        for (std::size_t col = 0; col < 5; ++col) {
            patch.points.push_back({{{{m01::Source::E05, 14, 22, static_cast<std::uint32_t>(row)}, col},
                                     31, {0, 20}}, t.footprint.reference,
                                    {-2.0 + row, 404.0 + col, 48.6}, 0.05});
        }
        patch.min = patch.points.front().position;
        patch.max = patch.points.back().position;
        t.surfaces.push_back(std::move(patch));
    }
    return t;
}

inline SurfaceGrid grid(const m04::Target& t, std::string id = "E05-proof-A") {
    SurfaceGrid g{binding(t), std::move(id), "coverage-1", -2.5, 403.5, 5, 5,
                  std::vector<Cell>(25), 82ms};
    for (const auto& patch : t.surfaces) {
        for (const auto& point : patch.points) {
            const auto index = point.source.key.observation.row * g.columns + point.source.key.column;
            g.cells[index] = {CellState::Observed, point.source.key, 3};
        }
    }
    return g;
}

inline Result evaluate(const EligibilityService& service, const m04::Target& target,
                       const Snapshot& snapshot, const SurfaceGrid& surface, Tick now = 15ms) {
    const std::array areas{Region{"A", 0, 406, 0}};
    return service.evaluate(target, snapshot, {std::span(&surface, 1), areas}, now);
}

void geometry_test();
void rules_test();
void bin_test();
void remeasurement_test();

}  // namespace waste::m05::test
