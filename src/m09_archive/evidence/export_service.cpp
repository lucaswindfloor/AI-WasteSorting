#include "export_service.h"
#include "reconciliation_service.h"
#include "../storage/record_codec.h"

#include <algorithm>
#include <set>

namespace waste::m09 {
namespace {

constexpr std::string_view job_schema = "evidence-export-job-v1";

std::string csv_cell(std::string_view text) {
    std::string escaped{"\""};
    if (!text.empty() && std::string_view("=+-@\t\r\n").find(text.front()) != std::string_view::npos) escaped += '\'';
    for (const char c : text) {
        if (c == '"') escaped += '"';
        escaped += c;
    }
    return escaped + '"';
}

bool write_text(const Sink& sink, std::string_view text) {
    return sink(std::as_bytes(std::span(text)));
}

bool frame(const Sink& sink, const Bytes& bytes) {
    wire::Encoder length;
    length.put(bytes.size());
    return sink(length.data) && sink(bytes);
}

std::optional<std::size_t> label(const std::vector<BatchRow>& rows, std::string_view name) {
    std::optional<std::size_t> match;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].pass != name && rows[i].target != name) continue;
        if (match) return {};
        match = i;
    }
    return match;
}

Manifest projection(const Manifest& manifest, const Selection& selection) {
    auto selected = manifest;
    const auto span = record_span(selection, selected.records.size());
    selected.records = std::vector<RecordKey>(manifest.records.begin() + static_cast<std::ptrdiff_t>(span.begin),
                                              manifest.records.begin() + static_cast<std::ptrdiff_t>(span.end));
    std::erase_if(selected.segments, [&](const auto& segment) {
        return !selection.include_raw || !includes_source(selection, segment.segment.key.source);
    });
    std::erase_if(selected.gaps, [&](const auto& gap) { return !includes_source(selection, gap.source); });
    return selected;
}

}  // namespace

disk::Path ExportService::directory(std::string_view owner, std::string_view run, std::string_view id) const {
    return archive_.root() / "exports" / wire::key({owner, run, id});
}

Result<ExportJob> ExportService::read_job(const disk::Path& path) const {
    const auto state = disk::read(path / "job");
    if (!state.value) return {state.status, {}};
    if (!disk::sync(path)) return {};
    auto job = wire::decode<ExportJob>(job_schema, *state.value);
    if (!job || path != directory(job->request.owner, job->request.run, job->request.id)) {
        return {Status::Corrupt, {}};
    }
    return {Status::Ok, std::move(job)};
}

std::optional<Selection> ExportService::approval(const ExportRequest& request) const {
    const auto grant = authorize_ ? authorize_(request.owner, request.run, Access::Export,
                                                  request.purpose, request.selection) : std::nullopt;
    return grant && subset(*grant, request.selection) ? grant : std::nullopt;
}

Result<ExportJob> ExportService::enqueue(const ExportRequest& request) {
    if (!wire::identifier(request.run) || !wire::identifier(request.id) || !wire::identifier(request.owner) ||
        !wire::identifier(request.purpose) || !request.manifest || !valid_selection(request.selection) ||
        !request.byte_limit || request.byte_limit > max_bytes_ ||
        (request.format != ExportFormat::Package && request.format != ExportFormat::Csv) ||
        (request.format == ExportFormat::Csv && (request.selection.include_raw || !request.row_limit ||
         !wire::identifier(request.first_label) || !wire::identifier(request.last_label)))) {
        return {Status::Invalid, {}};
    }
    const auto approved = approval(request);
    if (!approved) return {Status::Denied, {}};
    const auto snapshot = archive_.manifest(request.run, request.manifest);
    if (!snapshot.value) return {snapshot.status, {}};
    const auto path = directory(request.owner, request.run, request.id);
    if (!disk::ensure(path)) return {};
    const disk::Lock lock(path);
    if (!lock) return {};
    const auto earlier = read_job(path);
    if (earlier.value) {
        if (earlier.value->request != request) return {Status::Conflict, {}};
        if (!subset(earlier.value->approved, *approved)) return {Status::Denied, {}};
        return {Status::Duplicate, earlier.value};
    }
    if (earlier.status != Status::Missing) return earlier;
    const ExportJob job{request, *approved, ExportStage::Queued, Status::Pending, 0, {}};
    if (!disk::publish(path / "job", wire::encode(job_schema, job))) return {};
    return {Status::Ok, job};
}

Result<ExportJob> ExportService::status(std::string_view owner, std::string_view run, std::string_view id) const {
    if (!wire::identifier(owner) || !wire::identifier(run) || !wire::identifier(id)) return {Status::Invalid, {}};
    const auto path = directory(owner, run, id);
    auto job = read_job(path);
    if (!job.value) return job;
    const auto approved = approval(job.value->request);
    if (!approved || !subset(job.value->approved, *approved)) return {Status::Denied, {}};
    return job;
}

Result<ExportJob> ExportService::advance(std::string_view owner, std::string_view run, std::string_view id) {
    auto result = status(owner, run, id);
    if (!result.value || result.value->stage != ExportStage::Queued) return result;
    const auto path = directory(owner, run, id);
    const disk::Lock lock(path);
    if (!lock) return {};
    result = status(owner, run, id);
    if (!result.value || result.value->stage != ExportStage::Queued) return result;
    auto& job = *result.value;
    const auto manifest = archive_.manifest(run, job.request.manifest);
    const auto output = path / "evidence";
    job.result = manifest.value ? generate(*manifest.value, job, output) : manifest.status;
    if (accepted(job.result)) {
        const auto length = disk::size(output);
        const auto checksum = disk::digest(output);
        if (!length || !checksum || *length > job.request.byte_limit) return {};
        job.bytes = *length;
        job.digest = *checksum;
        job.stage = ExportStage::Ready;
    } else {
        job.stage = ExportStage::Failed;
        std::error_code ignored;
        std::filesystem::remove(path / "evidence.pending", ignored);
    }
    if (!disk::publish(path / "job", wire::encode(job_schema, job))) return {};
    return result;
}

Status ExportService::generate(const Manifest& manifest, const ExportJob& job, const disk::Path& output) const {
    if (archive_.pressure(0, 0, 0, UINT64_MAX).usable_disk < job.request.byte_limit) return Status::Capacity;
    Status result = Status::Unavailable;
    std::uint64_t written = 0;
    const auto completed = disk::publish(output, [&](const Sink& write) {
        const Sink bounded = [&](std::span<const std::byte> data) {
            if (data.size() > job.request.byte_limit - written ||
                data.size() > archive_.pressure(0, 0, 0, UINT64_MAX).usable_disk) {
                result = Status::Capacity;
                return false;
            }
            if (!write(data)) return false;
            written += data.size();
            return true;
        };
        if (job.request.format == ExportFormat::Csv) {
            const auto span = record_span(job.approved, manifest.records.size());
            const auto rows = batch_rows(archive_, manifest, span, span.end - span.begin);
            const auto first = label(rows, job.request.first_label);
            const auto last = label(rows, job.request.last_label);
            if (!first || !last || *first > *last) {
                result = Status::Missing;
                return false;
            }
            if (*last - *first + 1 > job.request.row_limit) {
                result = Status::Capacity;
                return false;
            }
            if (!write_text(bounded, "pass_id,target,planned_bin,actual_bin,evidence\n")) return false;
            for (auto i = *first; i <= *last; ++i) {
                const auto& row = rows[i];
                if (!write_text(bounded, csv_cell(row.pass) + ',' + csv_cell(row.target) + ',' +
                    csv_cell(row.planned) + ',' + csv_cell(row.actual) + ',' + csv_cell(row.evidence) + '\n')) {
                    return false;
                }
            }
            return true;
        }

        const auto visible = projection(manifest, job.approved);
        const std::set<RecordKey> allowed(visible.records.begin(), visible.records.end());
        if (!write_text(bounded, "M09EVIDENCE1\n") ||
            !frame(bounded, wire::encode("export-manifest-v1", visible)) ||
            !frame(bounded, wire::encode("export-approved-selection-v1", job.approved))) return false;
        for (const auto& key : visible.records) {
            auto record = archive_.record(visible, key);
            if (!record.value) {
                result = record.status;
                return false;
            }
            std::erase_if(record.value->related, [&](const auto& ref) { return !allowed.contains(ref); });
            std::erase_if(record.value->raw, [&](const auto& ref) {
                return !job.approved.include_raw || !includes_source(job.approved, ref.segment.source);
            });
            if (!job.approved.sources.empty() || job.approved.record_range || !job.approved.include_raw) {
                record.value->detail.clear();
            }
            if (!frame(bounded, wire::encode("export-record-v1", *record.value))) return false;
        }
        for (const auto& row : visible.segments) {
            for (std::size_t i = 0; i < row.readable.size(); ++i) {
                if (!row.readable[i]) continue;
                const auto block = archive_.raw(visible, {row.segment.key, i});
                if (!block.value) {
                    result = block.status;
                    return false;
                }
                if (!frame(bounded, *block.value)) return false;
            }
        }
        return true;
    });
    return completed ? Status::Ok : result;
}

Status ExportService::stream(std::string_view owner, std::string_view run, std::string_view id,
                              const Sink& sink) const {
    const auto job = status(owner, run, id);
    if (!job.value) return job.status;
    if (job.value->stage != ExportStage::Ready) return job.value->result;
    const auto path = directory(owner, run, id) / "evidence";
    if (disk::size(path) != job.value->bytes || disk::digest(path) != job.value->digest) return Status::Corrupt;
    return disk::stream(path, sink) ? Status::Ok : Status::Unavailable;
}

}  // namespace waste::m09
