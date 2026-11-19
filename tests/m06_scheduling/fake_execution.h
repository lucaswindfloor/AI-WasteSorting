#pragma once

#include "fixtures.h"

#include <algorithm>
#include <map>

namespace waste::m06::test {

class FakeExecution final : public ExecutionPort {
public:
    ExecutionSnapshot data = test::snapshot();
    std::map<AttemptId, AttemptStatus> states;
    std::vector<ReservationRequest> requests;
    std::vector<Task> delivered;
    bool conflict_once{}, bin_changes_on_reserve{}, uncertain_handoff{}, unavailable_close{};
    std::size_t closes{};
    std::function<void()> after_reserve;

    std::optional<ExecutionSnapshot> snapshot() override { return data; }

    ReserveReply reserve(const ReservationRequest& request) override {
        requests.push_back(request);
        if (conflict_once) {
            conflict_once = false;
            ++data.eligibility.revision;
            return {Reply::Conflict, {}};
        }
        if (request.expected_revision != data.eligibility.revision ||
            target_in_use(data, request.attempt.target)) {
            return {Reply::Conflict, {}};
        }
        Reservation reservation{request.attempt, "hold-" + std::to_string(request.attempt.sequence),
                                request.uses, request.handoff_deadline + 1ns};
        states.emplace(request.attempt, AttemptStatus::Held);
        data.reservations.push_back(reservation);
        ++data.eligibility.revision;
        if (bin_changes_on_reserve) {
            bin_changes_on_reserve = false;
            data.eligibility.bins[0].condition = m05::BinCondition::Full;
        }
        if (after_reserve) {
            after_reserve();
        }
        return {Reply::Accepted, reservation};
    }

    Reply handoff(const Task& task) override {
        if (task.expected_revision != data.eligibility.revision ||
            std::ranges::find(data.reservations, task.reservation) == data.reservations.end()) {
            return Reply::Conflict;
        }
        delivered.push_back(task);
        states[task.attempt] = uncertain_handoff ? AttemptStatus::Unknown : AttemptStatus::Delivered;
        return uncertain_handoff ? Reply::Uncertain : Reply::Accepted;
    }

    Reply close_unused(const AttemptId& id, std::string_view token) override {
        ++closes;
        const auto found = std::ranges::find_if(data.reservations, [&](const auto& held) {
            return held.attempt == id && held.token == token;
        });
        if (unavailable_close || status(id) != AttemptStatus::Held || found == data.reservations.end()) {
            return Reply::Uncertain;
        }
        finish(id, AttemptStatus::ClosedUnused);
        return Reply::Accepted;
    }

    AttemptStatus status(const AttemptId& id) override {
        const auto it = states.find(id);
        return it == states.end() ? AttemptStatus::Unknown : it->second;
    }

    void finish(const AttemptId& id, AttemptStatus status) {
        std::erase_if(data.reservations, [&](const auto& found) { return found.attempt == id; });
        states[id] = status;
        ++data.eligibility.revision;
    }
};

class FakeTargets final : public TargetSource {
public:
    std::map<TargetKey, std::shared_ptr<CurrentTarget>> entries;

    void add(CurrentTarget target) {
        auto key = TargetKey{target.target.generation.run_id, target.target.id};
        entries[key] = std::make_shared<CurrentTarget>(std::move(target));
    }

    std::shared_ptr<const CurrentTarget> read(const TargetKey& key, Tick) override {
        const auto it = entries.find(key);
        return it == entries.end() ? nullptr : it->second;
    }
};

}  // namespace waste::m06::test
