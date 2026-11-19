#include "fixtures.h"

namespace waste::m03::test {

void builder_test() {
    const auto m = manifest();
    auto window = input_window(m);
    WindowBuilder builder({"P0", 1}, m, 2147);
    check(builder.add(2149, *window.rows[2]) == Insert::Accepted &&
          builder.add(2147, *window.rows[0]) == Insert::Accepted &&
          builder.add(2150, *window.rows[3]) == Insert::Accepted && !builder.take(3ms),
          "out-of-order input closed a window early");
    check(builder.mark_gap(2148, 2148) == Insert::Accepted, "confirmed sync gap rejected");
    const auto first = builder.take(4ms);
    check(first && first->rows[0] && !first->rows[1] && first->rows[2] && first->rows[3] &&
          first->id.first == 2147 && first->id.last == 2150, "gap was compressed");
    check(builder.seal_through(2152) == Insert::Accepted, "cannot seal known upstream horizon");
    const auto second = builder.take(5ms);
    check(second && second->id.first == 2149 && second->rows[0]->owner == first->rows[2]->owner &&
          second->rows[0]->trigger_index == first->rows[2]->trigger_index,
          "overlap lost original source identity");
    check(builder.add(2148, *window.rows[1]) == Insert::Closed, "late input reopened a closed slot");

    WindowBuilder time_limit({"P0", 1}, m, 2147);
    time_limit.add(2147, *window.rows[0]);
    check(!time_limit.take(9ms), "window expired early");
    const auto timed_out = time_limit.take(10ms);
    check(timed_out && timed_out->rows[0] && !timed_out->rows[3], "window waiting was unbounded");
    check(time_limit.add(8000, *window.rows[0]) == Insert::OutOfRange, "lookahead unbounded");
}

}  // namespace waste::m03::test
