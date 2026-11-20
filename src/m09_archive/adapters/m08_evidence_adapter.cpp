#include "m08_evidence_adapter.h"
#include "../evidence/reconciliation_service.h"
#include "../storage/record_codec.h"

#include <algorithm>
#include <charconv>

namespace waste::m09 {
namespace {

std::optional<std::uint64_t> number(std::string_view text) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return {};
    return value;
}

std::string describe(const Gap& gap) {
    return gap.source + "/" + gap.session + " " + gap.basis + " [" + std::to_string(gap.range.begin) +
           ", " + std::to_string(gap.range.end) + ") " + gap.reason;
}

std::vector<std::string> gaps(const Manifest& manifest, const Selection& permit) {
    std::vector<std::string> result;
    const auto add = [&](const Gap& gap) {
        if (result.size() < 100 && includes_source(permit, gap.source)) result.push_back(describe(gap));
    };
    for (const auto& gap : manifest.gaps) add(gap);
    for (const auto& row : manifest.segments) {
        for (const auto& gap : row.segment.gaps) add(gap);
        for (const auto& gap : row.recovery_gaps) add(gap);
    }
    return result;
}

}  // namespace

M08EvidenceAdapter::M08EvidenceAdapter(const ArchiveService& archive, ExportService& exports,
                                       Authorize authorize, m08::Scope scope, std::uint64_t csv_bytes)
    : archive_(archive), exports_(exports), authorize_(std::move(authorize)),
      scope_(std::move(scope)), csv_bytes_(csv_bytes) {
    if (!wire::identifier(scope_.run) || !wire::identifier(scope_.station) || !csv_bytes_) {
        throw std::invalid_argument("invalid M08 archive scope or CSV limit");
    }
}

std::optional<m08::EvidencePage> M08EvidenceAdapter::list(const m08::EvidenceQuery& query,
                                                          std::string_view reader) {
    const auto version = number(query.manifest);
    const auto cursor = query.cursor.empty() ? std::optional<std::uint64_t>(0) : number(query.cursor);
    if (query.scope != scope_ || !version || !*version || !cursor || !query.limit || query.limit > 1000 || !authorize_) {
        return {};
    }
    const Selection requested;
    const auto grant = authorize_(reader, scope_.run, Access::Read, "batch list", requested);
    if (!grant || !subset(*grant, requested)) return {};
    const auto snapshot = archive_.manifest(scope_.run, *version);
    if (!snapshot.value) return {};
    const auto allowed = record_span(*grant, snapshot.value->records.size());
    const auto rows = batch_rows(archive_, *snapshot.value, allowed, allowed.end - allowed.begin);
    if (*cursor > rows.size()) return {};
    m08::EvidencePage page{scope_, query.manifest, {}, {}, gaps(*snapshot.value, *grant)};
    const auto end = std::min<std::uint64_t>(rows.size(), *cursor + query.limit);
    for (auto index = *cursor; index < end; ++index) {
        const auto& row = rows[index];
        page.rows.push_back({row.target.empty() ? row.pass : row.target, row.planned, row.actual, row.evidence});
    }
    if (end < rows.size()) page.next = std::to_string(end);
    return page;
}

m08::ExportJob M08EvidenceAdapter::project(const ExportJob& job) const {
    const auto& request = job.request;
    auto stage = m08::ExportStage::Queued;
    if (job.stage == ExportStage::Ready) stage = m08::ExportStage::Ready;
    if (job.stage == ExportStage::Failed) stage = m08::ExportStage::Failed;
    m08::ExportJob out{request.owner, {scope_, request.id, std::to_string(request.manifest),
        request.first_label, request.last_label, request.purpose, static_cast<std::size_t>(request.row_limit)},
        stage, {}, job.digest, {}};
    if (stage == m08::ExportStage::Ready) {
        out.actual_range = request.first_label + " – " + request.last_label;
        const auto snapshot = archive_.manifest(scope_.run, request.manifest);
        if (snapshot.value) out.gaps = gaps(*snapshot.value, job.approved);
    }
    return out;
}

std::optional<m08::ExportJob> M08EvidenceAdapter::enqueue(const m08::ExportRequest& request,
                                                           std::string_view owner) {
    const auto version = number(request.manifest);
    if (request.scope != scope_ || !version || !*version || request.manifest != std::to_string(*version)) return {};
    ExportRequest query{scope_.run, request.id, std::string(owner), request.purpose, *version, {}, csv_bytes_,
                        ExportFormat::Csv, request.max_rows, request.first, request.last};
    const auto job = exports_.enqueue(query);
    return job.value ? std::optional(project(*job.value)) : std::nullopt;
}

std::optional<m08::ExportJob> M08EvidenceAdapter::find(const m08::Scope& scope, std::string_view id,
                                                        std::string_view owner) {
    if (scope != scope_) return {};
    const auto job = exports_.status(owner, scope.run, id);
    return job.value && job.value->request.format == ExportFormat::Csv
        ? std::optional(project(*job.value)) : std::nullopt;
}

std::optional<std::string> M08EvidenceAdapter::download(const m08::ExportJob& job) {
    const auto current = find(job.request.scope, job.request.id, job.owner);
    if (!current || current->request != job.request || current->stage != m08::ExportStage::Ready ||
        current->checksum != job.checksum) return {};
    std::string text;
    const auto result = exports_.stream(job.owner, scope_.run, job.request.id, [&](auto chunk) {
        if (chunk.size() > csv_bytes_ - text.size()) return false;
        text.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        return true;
    });
    return result == Status::Ok ? std::optional(std::move(text)) : std::nullopt;
}

}  // namespace waste::m09
