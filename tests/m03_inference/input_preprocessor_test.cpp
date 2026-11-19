#include "fixtures.h"
#include "m03_inference/input_preprocessor.h"

namespace waste::m03::test {

void preprocessor_test() {
    auto m = manifest(Route::Joint);
    auto window = input_window(m, true);
    auto prepared = prepare(m, window);
    check(prepared.input && prepared.input->tensors.size() == 2, "masked joint input rejected");
    const auto& input = *prepared.input;
    const auto& rgb = input.tensors[0];
    const auto& hsi = input.tensors[1];
    check(rgb.width == 4 && rgb.height == 4 && rgb.values[3] == 2 && hsi.values[1] == 0.5f &&
          !hsi.valid[0] && hsi.values[0] == -3, "shape, normalization, or letterbox is wrong");
    check(!hsi.valid[5] && !input.source_map[5].rgb && input.source_map[9].rgb &&
          input.source_map[9].rgb->source.trigger.id == 2149, "joint masked row moved later rows");

    m.window.accepts_mask = false;
    m.window.max_missing_rows = 0;
    m.window.min_valid_fraction = 1;
    check(!prepare(m, window).input, "required missing modality accepted without a mask");
    m.hsi->spectral_reference = "not-white1";
    check(!prepare(m, input_window(m)).input, "corrected spectrum used with wrong reference");

    auto single = manifest();
    single.raster.left = 1;
    single.raster.columns = 1;
    const auto cropped = prepare(single, input_window(single));
    check(cropped.input && cropped.input->source_map[1].rgb->source.column == 1,
          "cropping changed provenance");
}

}  // namespace waste::m03::test
