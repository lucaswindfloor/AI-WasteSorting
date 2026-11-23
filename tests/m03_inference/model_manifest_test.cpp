#include "fixtures.h"
#include "fake_backend.h"

namespace waste::m03::test {

void manifest_test() {
    auto c = m02::test::acquisition_config();
    auto s = m02::test::calibration(c);
    auto m = manifest(Route::Hsi);
    validate(m);
    verify_compatibility(m, c, s);
    c.hsi.format.bands = {"500nm", "550nm"};
    m.hsi->bands = {"550nm", "500nm"};
    m.hsi->offset = {0, 0};
    m.hsi->scale = {1, 1};
    try {
        verify_compatibility(m, c, s);
        throw std::logic_error("same-shape swapped bands accepted");
    } catch (const std::invalid_argument&) {
    }

    auto wide_config = m02::test::acquisition_config();
    wide_config.rgb.format.width = 8192;
    const auto wide_snapshot = m02::test::calibration(wide_config);
    auto wide = manifest();
    wide.raster.columns = 8192;
    validate(wide);
    verify_compatibility(wide, wide_config, wide_snapshot);
    ++wide.raster.columns;
    try {
        validate(wide);
        throw std::logic_error("native crop exceeded the source width limit");
    } catch (const std::invalid_argument&) {
    }

    ModelRegistry registry(2);
    FakeBackend backend;
    const std::array artifact{std::byte{1}};
    const auto rgb = manifest();
    backend.controls->wrong_identity = true;
    try {
        registry.install(rgb, artifact, backend);
        throw std::logic_error("failed attestation accepted");
    } catch (const std::runtime_error&) {
    }
    backend.controls->wrong_identity = false;
    backend.controls->warmup_output = {{{55, 0.9f, {0, 0, 1, 1}, {}}}, {}};
    try {
        registry.install(rgb, artifact, backend);
        throw std::logic_error("invalid warmup accepted");
    } catch (const std::runtime_error&) {
    }
    check(!registry.find(rgb.identity.key), "failed warmup published a model");
    backend.controls->warmup_output = {};
    registry.install(rgb, artifact, backend);
    check(registry.unload(rgb.identity.key), "unused model cannot be unloaded");
    auto mutated = rgb;
    mutated.identity.artifact_digest = "replaced";
    try {
        registry.install(mutated, artifact, backend);
        throw std::logic_error("same version now points to a different artifact");
    } catch (const std::invalid_argument&) {
    }
}

}  // namespace waste::m03::test
