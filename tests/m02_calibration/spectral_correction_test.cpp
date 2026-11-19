#include "fixtures.h"
#include "m02_calibration/spectral_correction.h"

namespace waste::m02::test {

void test_spectrum() {
    const auto c = acquisition_config();
    auto s = calibration(c);
    auto state = environment(c);
    const TimeEstimate exposure{12ms, 1us, TimeBasis::SourceClock};
    const auto spectrum = row(m01::Source::Hsi, 13, "hsi", 12ms,
                              {std::byte{0x1e}, std::byte{0x05}, std::byte{0xff}, std::byte{0x0f}});
    auto result = correct_spectrum(s, c.hsi.format, spectrum, exposure, &state);
    check(result.samples.size() == 2 && result.reference_version == "white1" &&
          near(result.samples[0], 0.5) && result.quality[0] == Quality::Valid &&
          result.quality[1] == Quality::Invalid && result.sample_issues[1] == SpectralIssue::Saturated, "dark/white result or saturation handling is wrong");
    state.measured = 12ms;
    check(correct_spectrum(s, c.hsi.format, spectrum, exposure, &state).samples.empty(),
          "environment sampled after part of the exposure interval was accepted");
    state = environment(c);
    auto wrong = c.hsi.format;
    wrong.bands = {"other"};
    check(correct_spectrum(s, wrong, spectrum, exposure, &state).samples.empty(), "wrong band order accepted");
    s.spectral->white[0] = 109;
    result = correct_spectrum(s, c.hsi.format, spectrum, exposure, &state);
    check(result.quality[0] == Quality::Invalid && result.sample_issues[0] == SpectralIssue::InvalidReference,
          "negative denominator accepted");
    s.spectral->input_level = SpectralLevel::DarkWhiteCorrected;
    auto floats = c.hsi.format;
    floats.encoding = "F32LE";
    const auto calibrated = row(m01::Source::Hsi, 13, "hsi", 12ms,
                                {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x3f},
                                 std::byte{0}, std::byte{0}, std::byte{0x80}, std::byte{0x3f}});
    result = correct_spectrum(s, floats, calibrated, exposure, &state);
    check(result.samples.size() == 2 && near(result.samples[0], 0.5) &&
          near(result.samples[1], 1), "device-calibrated spectrum corrected a second time");
}

}  // namespace waste::m02::test
