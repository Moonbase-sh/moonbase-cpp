#pragma once

#include <chrono>
#include <cstdio>
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

inline std::chrono::system_clock::time_point parse_iso8601_utc(const std::string& value)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(
            value.c_str(),
            "%d-%d-%dT%d:%d:%d",
            &year,
            &month,
            &day,
            &hour,
            &minute,
            &second) != 6) {
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
    if (epoch == static_cast<std::time_t>(-1)) {
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
