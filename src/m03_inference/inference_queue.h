#pragma once

#include "model_registry.h"

#include <deque>

namespace waste::m03 {

struct QueueLimits {
    std::size_t max_waiting{}, max_running{}, max_retained_bytes{};
};

struct QueueHealth {
    std::size_t waiting{}, running{}, retained_bytes{};
    Tick oldest_waiting_age{};
};

struct Work {
    Result result;
    std::shared_ptr<const InstalledModel> model;
    std::shared_ptr<const PreparedInput> input;
};

class InferenceQueue {
public:
    explicit InferenceQueue(QueueLimits limits, Clock clock = {});
    void enqueue(Work work, Tick now, Report& report);
    void advance(Tick now, Report& report);
    void discard(const Generation& generation, Tick now, Report& report);
    QueueHealth health(Tick now) const;
    std::size_t byte_limit() const;

private:
    struct Running {
        Work work;
        bool reported{};
        std::unique_ptr<BackendJob> job;
    };

    void poll(Tick now, Report& report);
    void launch(Tick now, Report& report);

    QueueLimits limits_;
    Clock clock_;
    std::deque<Work> waiting_;
    std::deque<Running> running_;
};

}  // namespace waste::m03
