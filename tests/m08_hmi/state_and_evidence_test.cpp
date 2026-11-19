#include "fixtures.h"
#include "test_cases.h"

#include <iostream>

using namespace waste::m08;
using namespace waste::m08::test;

void projection_and_evidence_tests() {
    Fixture f;
    auto& state = f.cell.runtime;
    state.bins.front().stamp.valid_until = f.now;
    state.safety.stamp.valid_until = f.now;
    auto view = f.hmi.operations().workbench("operator");
    check(view && !view->safety_fresh && view->stale_bins == std::vector<std::string>{"B01"},
          "old source facts hidden by valid snapshot revision");
    check(f.hmi.operations().submit("operator", f.request("stale-p02", Action::Start, std::string(baseline)))
          .issue == Issue::Conditions && f.cell.submissions.empty(), "stale facts allowed start");
    state.tasks.front().destination = "B02";
    view = f.hmi.operations().workbench("operator");
    check(view->runtime.tasks.front().planned_bin == "B01" &&
          view->runtime.tasks.front().destination == "B02", "actual destination overwrote the plan");

    EvidenceQuery page{scope, std::string(manifest), {}, 1};
    auto first = f.hmi.evidence().list("auditor", page);
    check(first.data && first.data->rows.size() == 1 && first.data->next == "T041", "page cursor missing");
    page.cursor = first.data->next;
    const auto second = f.hmi.evidence().list("auditor", page);
    check(second.data && second.data->rows.front().actual_bin == "B02" && second.data->gaps.size() == 1,
          "evidence gap or actual bin missing");
    check(f.hmi.evidence().list("viewer", page).issue == Issue::Unauthorized, "evidence scope bypassed");
    page.scope.run = "foreign";
    check(f.hmi.evidence().list("auditor", page).issue == Issue::Unauthorized, "foreign evidence returned");

    ExportRequest job{scope, "export", std::string(manifest), "T041", "T042", "sample", 2};
    check(f.hmi.evidence().enqueue("operator", job).issue == Issue::Unauthorized, "export privileges bypassed");
    check(f.hmi.evidence().enqueue("auditor", job).data->stage == ExportStage::Queued, "export did not queue");
    check(f.hmi.evidence().download("auditor", scope, job.id).issue == Issue::NotReady, "pending export leaked");
    f.archive.finish = true;
    check(f.hmi.evidence().download("auditor", scope, job.id).data == Archive::csv, "ready export unavailable");
    check(f.hmi.evidence().download("viewer", scope, job.id).issue == Issue::Unauthorized, "export recheck missing");
    job.max_rows = 101;
    check(f.hmi.evidence().enqueue("auditor", job).issue == Issue::Invalid, "row limit bypassed");
}

int main() {
    try {
        access_policy_tests();
        operation_service_tests();
        bin_change_tests();
        configuration_tests();
        projection_and_evidence_tests();
        std::cout << "M08 HMI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "M08 test failed: " << error.what() << '\n';
        return 1;
    }
}
