#include "inference_queue.h"
#include "output_decoder.h"
#include "output_projection.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace waste::m03 {
namespace {

void finish(const Result& original, State state, std::string reason, Tick now, Report& report,
            std::vector<Evidence> evidence = {}) {
    auto result = original;
    result.state = state;
    result.reason = std::move(reason);
    result.finished = now;
    result.evidence = std::move(evidence);
    report.windows.push_back(std::move(result));
}

}  // namespace

InferenceQueue::InferenceQueue(QueueLimits limits, Clock clock)
    : limits_(limits), clock_(std::move(clock)) {
    if (!limits.max_waiting || !limits.max_running || !limits.max_retained_bytes) {
        throw std::invalid_argument("inference queue limits must be positive");
    }
}

void InferenceQueue::enqueue(Work work, Tick now, Report& report) {
    now = current_tick(clock_, now);
    if (now >= work.result.deadline) {
        finish(work.result, State::Expired, "expired_before_enqueue", now, report);
        return;
    }
    const auto occupied = health(now).retained_bytes;
    if (waiting_.size() >= limits_.max_waiting ||
        work.input->retained_bytes > limits_.max_retained_bytes - occupied) {
        finish(work.result, State::QueueFull, "inference_queue_capacity", now, report);
        return;
    }
    // Earlier deadlines start first. Equal deadlines keep their arrival order.
    const auto position = std::ranges::upper_bound(waiting_, work.result.deadline, {},
                                                   [](const Work& item) { return item.result.deadline; });
    waiting_.insert(position, std::move(work));
    launch(now, report);
}

void InferenceQueue::launch(Tick now, Report& report) {
    while (!waiting_.empty()) {
        now = current_tick(clock_, now);
        if (now >= waiting_.front().result.deadline) {
            finish(waiting_.front().result, State::Expired, "expired_in_queue", now, report);
            waiting_.pop_front();
            continue;
        }
        if (running_.size() >= limits_.max_running) {
            return;
        }
        auto work = std::move(waiting_.front());
        waiting_.pop_front();
        work.result.started = now;
        try {
            auto job = work.model->engine->submit(work.input, now);
            if (!job) {
                finish(work.result, State::Failed, "backend_returned_no_job", current_tick(clock_, now), report);
                continue;
            }
            running_.push_back({std::move(work), false, std::move(job)});
        } catch (const std::exception& e) {
            finish(work.result, State::Failed, e.what(), current_tick(clock_, now), report);
        }
    }
}

void InferenceQueue::poll(Tick now, Report& report) {
    for (auto it = running_.begin(); it != running_.end();) {
        now = current_tick(clock_, now);
        auto& work = it->work;
        if (!it->reported && now >= work.result.deadline) {
            finish(work.result, State::Expired, "expired_during_execution", now, report);
            it->reported = true;
        }
        std::optional<BackendCompletion> output;
        try {
            output = it->job->poll(now);
        } catch (const std::exception& e) {
            output = BackendCompletion{{}, std::string("backend_poll_error: ") + e.what()};
        }
        if (!output) {
            ++it;
            continue;
        }
        if (!it->reported) {
            now = current_tick(clock_, now);
            work.result.backend_completed = now;
            const auto& manifest = work.model->manifest;
            if (now >= work.result.deadline) {
                finish(work.result, State::Expired, "expired_during_execution", now, report);
            } else if (auto issue = validate_output(manifest, *output); !issue.empty()) {
                finish(work.result, State::Failed, std::move(issue), current_tick(clock_, now), report);
            } else {
                auto evidence = project_output(manifest, *work.input, *output);
                now = current_tick(clock_, now);
                if (now >= work.result.deadline) {
                    finish(work.result, State::Expired, "expired_before_delivery", now, report);
                } else {
                    const auto state = evidence.empty() ? State::NoDetection : State::Completed;
                    finish(work.result, state, state == State::NoDetection ? "no_valid_candidates" : "", now, report,
                           std::move(evidence));
                }
            }
        }
        it = running_.erase(it);
    }
}

void InferenceQueue::advance(Tick now, Report& report) {
    poll(now, report);
    launch(now, report);
    poll(now, report);
}

std::size_t InferenceQueue::byte_limit() const { return limits_.max_retained_bytes; }

void InferenceQueue::discard(const Generation& generation, Tick now, Report& report) {
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        if (it->result.window.generation == generation) {
            finish(it->result, State::Retired, "configuration_retired", now, report);
            it = waiting_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& item : running_) {
        if (item.work.result.window.generation == generation && !item.reported) {
            finish(item.work.result, State::Retired, "configuration_retired", now, report);
            item.reported = true;
        }
    }
}

QueueHealth InferenceQueue::health(Tick now) const {
    QueueHealth result{waiting_.size(), running_.size(), 0, {}};
    for (const auto& item : waiting_) {
        result.retained_bytes += item.input->retained_bytes;
        result.oldest_waiting_age = std::max(result.oldest_waiting_age, now - item.result.ready);
    }
    for (const auto& item : running_) {
        result.retained_bytes += item.work.input->retained_bytes;
    }
    return result;
}

}  // namespace waste::m03
