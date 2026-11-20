#pragma once

#include "../storage/record_codec.h"
#include "m07_execution/execution_types.h"
#include "m08_hmi/hmi_types.h"

namespace waste::m09::wire {

#define M09_FIELDS(Type, ...) \
    template<> struct Layout<Type> { \
        static void visit(auto& x, auto&& fields) { fields(__VA_ARGS__); } \
    }

M09_FIELDS(m01::ByteRange, x.offset, x.size);
M09_FIELDS(m01::ObservationId, x.source, x.session, x.frame, x.row);
M09_FIELDS(m03::Generation, x.run_id, x.config_epoch);
M09_FIELDS(m04::Point, x.x, x.y, x.z);
M09_FIELDS(m04::Reference, x.time, x.time_error, x.encoder_epoch, x.count, x.count_error);
M09_FIELDS(m04::SampleKey, x.observation, x.column);
M09_FIELDS(m04::SourceRef, x.key, x.segment, x.bytes);
M09_FIELDS(m05::Check, x.state, x.reason, x.valid_until);
M09_FIELDS(m05::ToolIdentity, x.station, x.tool, x.configuration, x.tcp);
M09_FIELDS(m05::BinIdentity, x.slot, x.physical_bin);
M09_FIELDS(m05::TargetBinding, x.generation, x.id, x.version, x.reference);
M09_FIELDS(m05::FactTime, x.source, x.observed, x.valid_until);
M09_FIELDS(m05::ToolFact, x.identity, x.condition, x.time);
M09_FIELDS(m05::BinFact, x.identity, x.placement_revision, x.condition, x.time);
M09_FIELDS(m05::Snapshot, x.generation, x.revision, x.bin_mapping_version, x.tools, x.bins);
M09_FIELDS(m05::Contact, x.check, x.reasons, x.missing, x.center, x.required_radius_mm, x.height_delta_mm, x.sources);
M09_FIELDS(m05::RemeasurementReference, x.requested, x.appointment_id, x.measured, x.valid_until);
M09_FIELDS(m05::Candidate, x.target, x.recipe_version, x.profile_version, x.bin_mapping_version, x.region_id, x.proof_id,
           x.snapshot_revision, x.tool, x.bin, x.rule_label, x.contact, x.assessed, x.valid_until,
           x.route_dependencies, x.remeasurement);

M09_FIELDS(m06::Window, x.open, x.close);
M09_FIELDS(m06::Interval, x.begin, x.end);
M09_FIELDS(m06::ResourceKey, x.kind, x.id);
M09_FIELDS(m06::ResourceUse, x.resource, x.time);
M09_FIELDS(m06::TargetKey, x.run_id, x.id);
M09_FIELDS(m06::AttemptId, x.target, x.scheduler_session, x.sequence);
M09_FIELDS(m06::Reservation, x.attempt, x.token, x.uses, x.valid_until);
M09_FIELDS(m06::ReservationRequest, x.attempt, x.target, x.expected_revision, x.uses, x.handoff_at, x.handoff_deadline);
M09_FIELDS(m06::ResourceCalendar, x.resource, x.valid_until, x.usable, x.occupied);
M09_FIELDS(m06::ContactWindow, x.instants, x.current_pick, x.current_reference, x.position_error_mm, x.region_id);
M09_FIELDS(m06::Deadlines, x.receipt, x.receive, x.handoff, x.receive_to_contact_min, x.receive_to_contact_max,
           x.early_margin, x.late_margin, x.handoff_to_receipt_min, x.handoff_to_receipt_max);
M09_FIELDS(m06::Task, x.attempt, x.reservation, x.candidate, x.schedule_version, x.forecast_version, x.tracking,
           x.contract, x.pick, x.reference, x.applied_mm, x.contact, x.deadlines, x.handoff_at, x.expected_revision);
M09_FIELDS(m06::PlanEnvelope, x.attempt, x.target, x.plan_id, x.reservation_token, x.valid_for_receipt_until,
           x.expected_contact);

M09_FIELDS(m07::ControllerIdentity, x.device, x.session);
M09_FIELDS(m07::DeviceLink, x.device, x.session, x.observed, x.valid_until, x.available);
M09_FIELDS(m07::Gate, x.source, x.observed, x.valid_until, x.permits);
M09_FIELDS(m07::FieldState, x.eligibility, x.calendars, x.controllers, x.terminals, x.process, x.safety_mirror,
           x.sequence, x.observed);
M09_FIELDS(m07::RequestId, x.attempt, x.operation);
M09_FIELDS(m07::Payload, x.target, x.reservation_token, x.schedule_version, x.forecast_version, x.recipe_version,
           x.proof_id, x.profile_version, x.region_id, x.bin_mapping_version, x.tool, x.bin, x.placement_revision,
           x.contract, x.tracking, x.pick, x.reference, x.applied_mm, x.contact, x.receive, x.handoff, x.early_margin,
           x.late_margin, x.receive_to_contact_min, x.receive_to_contact_max, x.handoff_at, x.valid_until,
           x.expected_revision, x.appointment_id);
M09_FIELDS(m07::Command, x.request, x.controller, x.payload, x.plan_id, x.related_request);
M09_FIELDS(m07::WireCommand, x.command, x.bytes);
M09_FIELDS(m07::Intent, x.wire, x.progress, x.armed_at, x.sent_at);
M09_FIELDS(m07::Stamp, x.source, x.device, x.session, x.sequence, x.occurred, x.valid_until, x.evidence);
M09_FIELDS(m07::Feedback, x.attempt, x.request, x.stamp, x.kind, x.plan);
M09_FIELDS(m07::AttemptRecord, x.original, x.reservation, x.task, x.controller, x.stage, x.carrier, x.issue,
           x.intents, x.facts, x.recent_events, x.plan, x.last_query, x.cancellation_requested, x.motion_seen,
           x.needs_query);
M09_FIELDS(m07::DurableState, x.generation, x.configuration, x.transaction, x.revision, x.updated, x.field,
           x.field_sequence, x.historical, x.attempts);

M09_FIELDS(m08::Scope, x.station, x.run);
M09_FIELDS(m08::Stamp, x.source, x.observed, x.valid_until, x.valid);
M09_FIELDS(m08::Package, x.id, x.digest, x.versions, x.e11);
M09_FIELDS(m08::Deployment, x.operation, x.package, x.digest, x.epoch, x.actual, x.stamp, x.problems);
M09_FIELDS(m08::OperationKey, x.scope, x.id);
M09_FIELDS(m08::Request, x.key, x.action, x.object, x.value, x.revision, x.epoch);
M09_FIELDS(m08::CommandKey, x.operation, x.step);
M09_FIELDS(m08::Command, x.key, x.object, x.value, x.fence, x.runtime_revision, x.epoch, x.placement, x.mapping,
           x.package, x.deployment_epoch);
M09_FIELDS(m08::Operation, x.request, x.actor, x.phase, x.issue, x.created, x.updated, x.old_physical,
           x.prior_fence, x.old_placement, x.old_mapping, x.deployment_epoch, x.last_applied, x.pending,
           x.issued, x.evidence);
M09_FIELDS(m08::AuditEvent, x.request, x.actor, x.issue, x.at);
M09_FIELDS(m08::Journal, x.scope, x.revision, x.highest_epoch, x.transaction, x.operations, x.approvals,
           x.activated, x.acknowledgements, x.audit);

#undef M09_FIELDS

}  // namespace waste::m09::wire
