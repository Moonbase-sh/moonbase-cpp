#pragma once

#include <chrono>
#include <cctype>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace moonbase::detail {

inline std::time_t timegm_utc(std::tm* tm)
{
#if defined(_WIN32)
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

inline std::chrono::system_clock::time_point from_epoch_seconds(long long seconds)
{
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

inline long long to_epoch_seconds(std::chrono::system_clock::time_point time)
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        time.time_since_epoch()).count();
}

inline int parse_fixed_digits(const std::string& value, std::size_t offset, std::size_t count)
{
    if (offset + count > value.size()) {
        throw std::runtime_error("Invalid ISO-8601 timestamp");
    }

    int result = 0;
    for (std::size_t index = 0; index != count; ++index) {
        const auto c = value[offset + index];
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            throw std::runtime_error("Invalid ISO-8601 timestamp");
        }
        result = (result * 10) + (c - '0');
    }
    return result;
}

inline void require_timestamp_char(const std::string& value, std::size_t offset, char expected)
{
    if (offset >= value.size() || value[offset] != expected) {
        throw std::runtime_error("Invalid ISO-8601 timestamp");
    }
}

inline bool utc_fields_match(
    std::time_t epoch,
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second)
{
    std::tm normalized{};
#if defined(_WIN32)
    if (gmtime_s(&normalized, &epoch) != 0) {
        return false;
    }
#else
    if (gmtime_r(&epoch, &normalized) == nullptr) {
        return false;
    }
#endif

    return normalized.tm_year == year - 1900 &&
        normalized.tm_mon == month - 1 &&
        normalized.tm_mday == day &&
        normalized.tm_hour == hour &&
        normalized.tm_min == minute &&
        normalized.tm_sec == second;
}

inline std::chrono::system_clock::time_point parse_iso8601_utc(const std::string& value)
{
    if (value.size() < 20) {
        throw std::runtime_error("Invalid ISO-8601 timestamp");
    }

    const auto year = parse_fixed_digits(value, 0, 4);
    require_timestamp_char(value, 4, '-');
    const auto month = parse_fixed_digits(value, 5, 2);
    require_timestamp_char(value, 7, '-');
    const auto day = parse_fixed_digits(value, 8, 2);
    require_timestamp_char(value, 10, 'T');
    const auto hour = parse_fixed_digits(value, 11, 2);
    require_timestamp_char(value, 13, ':');
    const auto minute = parse_fixed_digits(value, 14, 2);
    require_timestamp_char(value, 16, ':');
    const auto second = parse_fixed_digits(value, 17, 2);

    auto cursor = std::size_t{19};
    if (cursor < value.size() && value[cursor] == '.') {
        ++cursor;
        const auto fraction_start = cursor;
        while (cursor < value.size() && std::isdigit(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        if (cursor == fraction_start) {
            throw std::runtime_error("Invalid ISO-8601 timestamp");
        }
    }

    if (cursor >= value.size() || value[cursor] != 'Z' || cursor + 1 != value.size()) {
        throw std::runtime_error("Invalid ISO-8601 timestamp");
    }

    if (month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        throw std::runtime_error("Invalid ISO-8601 timestamp");
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0;

    const auto epoch = timegm_utc(&tm);
    if (!utc_fields_match(epoch, year, month, day, hour, minute, second)) {
        throw std::runtime_error("Invalid UTC timestamp");
    }
    return std::chrono::system_clock::from_time_t(epoch);
}

inline std::string format_iso8601_utc(std::chrono::system_clock::time_point time)
{
    const auto epoch = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &epoch);
#else
    gmtime_r(&epoch, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

} // namespace moonbase::detail
