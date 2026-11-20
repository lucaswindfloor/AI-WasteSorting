#pragma once

#include "../archive_types.h"

#include <bit>
#include <charconv>
#include <concepts>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace waste::m09::wire {

std::string digest(std::span<const std::byte> bytes);
bool identifier(std::string_view value);
std::string key(std::initializer_list<std::string_view> parts);

// Field order is part of the on-disk format. Increment the record version
// before changing an existing layout or enum representation.
template<class T> struct Layout;

#define M09_LAYOUT(Type, ...) \
    template<> struct Layout<Type> { \
        static void visit(auto& x, auto&& fields) { fields(__VA_ARGS__); } \
    }

template<class T> struct IsOptional : std::false_type {};
template<class T> struct IsOptional<std::optional<T>> : std::true_type {};
template<class T> struct IsVector : std::false_type {};
template<class T> struct IsVector<std::vector<T>> : std::true_type {};
template<class T> struct IsMap : std::false_type {};
template<class K, class V, class C> struct IsMap<std::map<K, V, C>> : std::true_type {};

class Encoder {
public:
    Bytes data;

    template<class... T>
    void fields(const T&... value) { (put(value), ...); }

    template<class T>
    void put(const T& value) {
        if constexpr (std::same_as<T, bool>) {
            data.push_back(value ? std::byte{1} : std::byte{0});
        } else if constexpr (std::same_as<T, std::byte>) {
            data.push_back(value);
        } else if constexpr (std::is_enum_v<T>) {
            put(static_cast<std::int64_t>(value));
        } else if constexpr (std::integral<T>) {
            std::uint64_t bits;
            if constexpr (std::is_signed_v<T>) bits = std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(value));
            else bits = value;
            for (unsigned shift = 0; shift < 64; shift += 8) data.push_back(std::byte((bits >> shift) & 255));
        } else if constexpr (std::same_as<T, double>) {
            put(std::bit_cast<std::uint64_t>(value));
        } else if constexpr (std::same_as<T, long double>) {
            char text[128];
            const auto [end, error] = std::to_chars(text, text + sizeof(text), value, std::chars_format::hex);
            if (error != std::errc{}) throw std::runtime_error("unrepresentable extended archive number");
            put(std::string(text, end));
        } else if constexpr (std::same_as<T, Tick>) {
            put(value.count());
        } else if constexpr (std::same_as<T, std::string> || std::same_as<T, Bytes>) {
            put(value.size());
            const auto bytes = std::as_bytes(std::span(value));
            data.insert(data.end(), bytes.begin(), bytes.end());
        } else if constexpr (IsOptional<T>::value) {
            put(value.has_value());
            if (value) put(*value);
        } else if constexpr (IsVector<T>::value) {
            put(value.size());
            for (const auto& item : value) put(item);
        } else if constexpr (IsMap<T>::value) {
            put(value.size());
            for (const auto& [key, mapped] : value) fields(key, mapped);
        } else {
            Layout<T>::visit(value, [this](const auto&... members) { fields(members...); });
        }
    }
};

class Decoder {
public:
    explicit Decoder(std::span<const std::byte> input) : input_(input) {}
    bool complete() const { return input_.empty(); }

    template<class... T>
    void fields(T&... value) { (get(value), ...); }

    template<class T>
    void get(T& value) {
        if constexpr (std::same_as<T, bool>) {
            const auto byte = octet();
            if (byte > 1) invalid();
            value = byte != 0;
        } else if constexpr (std::same_as<T, std::byte>) {
            value = std::byte(octet());
        } else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{};
            get(raw);
            value = static_cast<T>(raw);
        } else if constexpr (std::integral<T>) {
            std::uint64_t bits = 0;
            for (unsigned shift = 0; shift < 64; shift += 8) bits |= std::uint64_t(octet()) << shift;
            if constexpr (std::is_signed_v<T>) {
                const auto signed_value = std::bit_cast<std::int64_t>(bits);
                if (!std::in_range<T>(signed_value)) invalid();
                value = static_cast<T>(signed_value);
            } else {
                if (!std::in_range<T>(bits)) invalid();
                value = static_cast<T>(bits);
            }
        } else if constexpr (std::same_as<T, double>) {
            std::uint64_t bits{};
            get(bits);
            value = std::bit_cast<double>(bits);
        } else if constexpr (std::same_as<T, long double>) {
            std::string text;
            get(text);
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::hex);
            if (error != std::errc{} || end != text.data() + text.size()) invalid();
        } else if constexpr (std::same_as<T, Tick>) {
            Tick::rep count{};
            get(count);
            value = Tick(count);
        } else if constexpr (std::same_as<T, std::string> || std::same_as<T, Bytes>) {
            const auto count = length();
            const auto bytes = input_.first(count);
            if constexpr (std::same_as<T, std::string>) {
                value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            } else {
                value.assign(bytes.begin(), bytes.end());
            }
            input_ = input_.subspan(count);
        } else if constexpr (IsOptional<T>::value) {
            bool exists{};
            get(exists);
            if (exists) get(value.emplace());
            else value.reset();
        } else if constexpr (IsVector<T>::value) {
            const auto count = items();
            value.clear();
            for (std::size_t i = 0; i < count; ++i) {
                typename T::value_type item{};
                get(item);
                value.push_back(std::move(item));
            }
        } else if constexpr (IsMap<T>::value) {
            const auto count = items();
            value.clear();
            for (std::size_t i = 0; i < count; ++i) {
                typename T::key_type key{};
                typename T::mapped_type mapped{};
                fields(key, mapped);
                if (!value.emplace(std::move(key), std::move(mapped)).second) invalid();
            }
        } else {
            Layout<T>::visit(value, [this](auto&... members) { fields(members...); });
        }
    }

private:
    std::span<const std::byte> input_;
    [[noreturn]] static void invalid() { throw std::runtime_error("invalid archive record"); }

    unsigned octet() {
        if (input_.empty()) invalid();
        const auto value = std::to_integer<unsigned>(input_.front());
        input_ = input_.subspan(1);
        return value;
    }

    std::size_t length() {
        std::size_t count{};
        get(count);
        if (count > input_.size()) invalid();
        return count;
    }

    std::size_t items() {
        const auto count = length();
        if (count > 1000000) invalid();
        return count;
    }
};

Bytes envelope(std::string_view schema, std::span<const std::byte> body);
std::optional<Bytes> open(std::string_view schema, std::span<const std::byte> bytes);

template<class T>
Bytes encode(std::string_view schema, const T& value) {
    Encoder out;
    out.put(value);
    return envelope(schema, out.data);
}

template<class T>
std::optional<T> decode(std::string_view schema, std::span<const std::byte> value) {
    const auto body = open(schema, value);
    if (!body) return {};
    try {
        Decoder in(*body);
        T result{};
        in.get(result);
        if (in.complete()) return result;
    } catch (const std::exception&) {
    }
    return {};
}

M09_LAYOUT(Time, x.domain, x.ticks);
M09_LAYOUT(Range, x.begin, x.end);
M09_LAYOUT(SegmentKey, x.run, x.source, x.session, x.id);
M09_LAYOUT(Gap, x.source, x.session, x.basis, x.range, x.reason);
M09_LAYOUT(Block, x.frame, x.row, x.bytes, x.trigger, x.occurred, x.received, x.format, x.digest, x.metadata);
M09_LAYOUT(Segment, x.key, x.epoch, x.format, x.digest, x.length, x.blocks, x.gaps);
M09_LAYOUT(SegmentEntry, x.segment, x.readable, x.recovery_gaps);
M09_LAYOUT(RecordKey, x.kind, x.id, x.version);
M09_LAYOUT(BlockRef, x.segment, x.index);
M09_LAYOUT(Record, x.run, x.key, x.epoch, x.occurred, x.received, x.related, x.raw,
           x.target, x.planned_bin, x.actual_bin, x.description, x.detail);
M09_LAYOUT(Manifest, x.run, x.version, x.previous, x.sealed, x.reconciliation_pending, x.reason,
           x.segments, x.records, x.gaps);
M09_LAYOUT(MaterialPass, x.run, x.scope, x.id, x.sample, x.received, x.evidence);
M09_LAYOUT(Reconciliation, x.run, x.scope, x.pass, x.version, x.previous, x.match, x.destination,
           x.actual_bin, x.reviewer, x.reason, x.reviewed, x.coverage_verified, x.targets, x.attempts,
           x.measurements, x.evidence);
M09_LAYOUT(Position, x.frame, x.reference, x.x_mm, x.y_mm, x.z_mm);
M09_LAYOUT(Measurement, x.run, x.scope, x.pass, x.id, x.kind, x.occurred, x.received, x.attempt, x.valid,
           x.contact, x.reference, x.instructed, x.actual, x.destination, x.bin, x.evidence, x.reason);
M09_LAYOUT(Selection, x.sources, x.record_range, x.include_raw);
M09_LAYOUT(ExportRequest, x.run, x.id, x.owner, x.purpose, x.manifest, x.selection, x.byte_limit,
           x.format, x.row_limit, x.first_label, x.last_label);
M09_LAYOUT(ExportJob, x.request, x.approved, x.stage, x.result, x.bytes, x.digest);

#undef M09_LAYOUT

}  // namespace waste::m09::wire
