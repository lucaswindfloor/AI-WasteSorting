#include "fixtures.h"
#include "m03_inference/input_preprocessor.h"
#include "m03_inference/output_decoder.h"
#include "m03_inference/output_projection.h"

namespace waste::m03::test {

void projection_test() {
    auto m = manifest();
    m.raster.resized_columns = 4;
    const auto prepared = prepare(m, input_window(m));
    check(static_cast<bool>(prepared.input), "projection fixture failed");
    // Include padding, both repeated pixels from column zero, and a masked cell.
    BackendCompletion output{{{0, 0.8f, {0, 0, 4, 1}, {1, 1, 1, 0}},
                              {0, 0.9f, {0, 1, 1, 2}, {}}}, {}};
    check(validate_output(m, output).empty(), "valid model output rejected");
    const auto evidence = project_output(m, *prepared.input, output);
    check(evidence.size() == 1 && evidence.front().samples.size() == 1 &&
          evidence.front().samples.front().source.trigger.id == 2147 &&
          evidence.front().samples.front().source.column == 0 &&
          evidence.front().samples.front().belt.position.y == 406,
          "projection forgot mask, padding, deduplication or B coordinates");
    output.candidates[0].mask.pop_back();
    check(validate_output(m, output) == "invalid_candidate_mask", "malformed output mask accepted");
}

}  // namespace waste::m03::test
