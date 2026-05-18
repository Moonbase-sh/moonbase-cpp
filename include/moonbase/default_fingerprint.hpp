#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <openssl/sha.h>

#include "moonbase/fingerprint.hpp"

namespace moonbase {

class default_fingerprint_provider : public fingerprint_provider {
public:
    using identity_parameter = std::pair<std::string, std::string>;

    [[nodiscard]] static std::string platform_tag()
    {
#if defined(__APPLE__)
        return "mac";
#elif defined(_WIN32)
        return "windows";
#elif defined(__ANDROID__)
        return "android";
#elif defined(__linux__)
        return "linux";
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
        return "bsd";
#else
        return "unknown";
#endif
    }

    [[nodiscard]] static std::string hash_identity_parameters(
        const std::vector<identity_parameter>& parameters,
        std::string_view platform = platform_tag())
    {
        std::string material;
        material += "moonbase-cpp:fingerprint:v1\n";
        material += "platform=";
        material.append(platform.data(), platform.size());
        material += "\n";

        for (const auto& parameter : parameters) {
            auto name = trim_ascii(parameter.first);
            auto value = trim_ascii(parameter.second);
            if (!name.empty() && !value.empty()) {
                material += name;
                material += "=";
                material += value;
                material += "\n";
            }
        }

        return sha256_hex(material);
    }

    [[nodiscard]] static std::vector<identity_parameter> identity_parameters()
    {
        std::vector<identity_parameter> parameters;

#if defined(_WIN32)
        append_windows_identity_parameters(parameters);
#elif defined(__APPLE__)
        auto uuid = trim_ascii(command_output(
            "ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | "
            "awk -F\\\" '/IOPlatformUUID/{print $4; exit}'"));
        uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
        append_parameter(parameters, "ioPlatformUuid", uuid);
#elif defined(__linux__) && !defined(__ANDROID__)
        const auto board_serial = trim_ascii(read_file("/sys/class/dmi/id/board_serial"));
        if (!board_serial.empty()) {
            append_parameter(parameters, "boardSerial", board_serial);
        } else {
            append_parameter(parameters, "biosDate", read_file("/sys/class/dmi/id/bios_date"));
            append_parameter(parameters, "biosRelease", read_file("/sys/class/dmi/id/bios_release"));
            append_parameter(parameters, "biosVendor", read_file("/sys/class/dmi/id/bios_vendor"));
            append_parameter(parameters, "biosVersion", read_file("/sys/class/dmi/id/bios_version"));
        }

        const auto cpu_data = command_output("lscpu 2>/dev/null");
        if (!cpu_data.empty()) {
            append_parameter(parameters, "cpuFamily", linux_cpu_field(cpu_data, "CPU family:"));
            append_parameter(parameters, "cpuModel", linux_cpu_field(cpu_data, "Model:"));
            append_parameter(parameters, "cpuModelName", linux_cpu_field(cpu_data, "Model name:"));
            append_parameter(parameters, "cpuVendor", linux_cpu_field(cpu_data, "Vendor ID:"));
        }
#endif

        return parameters;
    }

    [[nodiscard]] std::string device_name() const override
    {
#if defined(_WIN32)
        char buffer[128]{};
        DWORD size = static_cast<DWORD>(sizeof(buffer)) - 1;
        if (GetComputerNameExA(ComputerNamePhysicalDnsHostname, buffer, &size)) {
            return std::string(buffer, size);
        }
        return {};
#else
        std::array<char, 256> buffer{};
        if (gethostname(buffer.data(), buffer.size() - 1) == 0) {
            auto name = std::string(buffer.data());
#if defined(__APPLE__)
            const auto suffix = std::string(".local");
            if (name.size() >= suffix.size()) {
                auto tail = name.substr(name.size() - suffix.size());
                std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (tail == suffix) {
                    name.erase(name.size() - suffix.size());
                }
            }
#endif
            return name;
        }
        return {};
#endif
    }

    [[nodiscard]] std::string device_id() const override
    {
        auto parameters = identity_parameters();
        if (parameters.empty()) {
            append_parameter(parameters, "deviceName", device_name());
        }
        return hash_identity_parameters(parameters);
    }

private:
    [[nodiscard]] static std::string sha256_hex(std::string_view material)
    {
        unsigned char digest[SHA256_DIGEST_LENGTH]{};
        SHA256(
            reinterpret_cast<const unsigned char*>(material.data()),
            material.size(),
            digest);

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto byte : digest) {
            output << std::setw(2) << static_cast<int>(byte);
        }
        return output.str();
    }

    [[nodiscard]] static std::string read_file(const std::string& path)
    {
        std::ifstream file(path);
        if (!file) {
            return {};
        }
        std::ostringstream out;
        out << file.rdbuf();
        return out.str();
    }

    [[nodiscard]] static std::string command_output(const std::string& command)
    {
#if defined(_WIN32)
        (void)command;
        return {};
#else
        std::array<char, 256> buffer{};
        std::string result;
        std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
        if (!pipe) {
            return {};
        }
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        return result;
#endif
    }

    [[nodiscard]] static std::string trim_ascii(std::string value)
    {
        while (!value.empty() &&
               (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
            value.pop_back();
        }
        while (!value.empty() &&
               (value.front() == '\n' || value.front() == '\r' || value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        return value;
    }

    [[nodiscard]] static std::string linux_cpu_field(const std::string& lscpu_output, const std::string& key)
    {
        const auto key_index = lscpu_output.find(key);
        if (key_index == std::string::npos) {
            return {};
        }

        const auto colon = lscpu_output.find(':', key_index);
        if (colon == std::string::npos) {
            return {};
        }

        const auto end = lscpu_output.find('\n', colon);
        return trim_ascii(lscpu_output.substr(
            colon + 1,
            end == std::string::npos ? std::string::npos : end - colon - 1));
    }

    static void append_parameter(
        std::vector<identity_parameter>& parameters,
        std::string name,
        std::string value)
    {
        name = trim_ascii(std::move(name));
        value = trim_ascii(std::move(value));
        if (!name.empty() && !value.empty()) {
            parameters.emplace_back(std::move(name), std::move(value));
        }
    }

#if defined(_WIN32)
    [[nodiscard]] static std::string windows_string_from_offset(
        const std::vector<unsigned char>& content,
        const std::vector<std::string_view>& strings,
        std::size_t byte_offset)
    {
        if (byte_offset >= content.size()) {
            return {};
        }

        const auto index = static_cast<std::size_t>(content[byte_offset]);
        if (index == 0 || index > strings.size()) {
            return {};
        }

        return std::string(strings[index - 1]);
    }

    [[nodiscard]] static std::size_t windows_bounded_string_length(const char* value, std::size_t max_length)
    {
        std::size_t length = 0;
        while (length < max_length && value[length] != '\0') {
            ++length;
        }
        return length;
    }

    static void append_windows_identity_parameters(std::vector<identity_parameter>& parameters)
    {
        constexpr DWORD signature =
            static_cast<DWORD>('R') |
            (static_cast<DWORD>('S') << 8U) |
            (static_cast<DWORD>('M') << 16U) |
            (static_cast<DWORD>('B') << 24U);

        const auto table_size = GetSystemFirmwareTable(signature, 0, nullptr, 0);
        if (table_size == 0) {
            return;
        }

        std::vector<unsigned char> smbios(table_size);
        if (GetSystemFirmwareTable(signature, 0, smbios.data(), table_size) != table_size) {
            return;
        }

        struct raw_smbios_data {
            std::uint8_t unused[4];
            std::uint32_t length;
        };

        struct smbios_header {
            std::uint8_t id;
            std::uint8_t length;
            std::uint16_t handle;
        };

        if (smbios.size() < sizeof(raw_smbios_data)) {
            return;
        }

        raw_smbios_data raw{};
        std::memcpy(&raw, smbios.data(), sizeof(raw));
        if (smbios.size() < sizeof(raw_smbios_data) + raw.length) {
            return;
        }

        std::vector<unsigned char> content(
            smbios.begin() + static_cast<std::ptrdiff_t>(sizeof(raw_smbios_data)),
            smbios.begin() + static_cast<std::ptrdiff_t>(sizeof(raw_smbios_data) + raw.length));

        std::size_t offset = 0;
        while (offset < content.size()) {
            if (content.size() - offset < sizeof(smbios_header)) {
                break;
            }

            smbios_header header{};
            std::memcpy(&header, content.data() + offset, sizeof(header));
            if (header.length == 0 || content.size() - offset < header.length) {
                break;
            }

            std::vector<std::string_view> strings;
            auto string_offset = offset + header.length;
            while (string_offset < content.size()) {
                const auto* str = reinterpret_cast<const char*>(content.data() + string_offset);
                const auto max_length = content.size() - string_offset;
                const auto length = windows_bounded_string_length(str, max_length);
                if (length == 0) {
                    break;
                }
                strings.emplace_back(str, length);
                string_offset += std::min(length + 1, max_length);
            }

            const auto end_of_table = std::min(
                content.size(),
                std::max(offset + static_cast<std::size_t>(header.length) + 2, string_offset + 1));

            const auto from_offset = [&](std::size_t byte_offset) {
                return windows_string_from_offset(content, strings, offset + byte_offset);
            };

            switch (header.id) {
                case 1: {
                    append_parameter(parameters, "systemManufacturer", from_offset(0x04));
                    append_parameter(parameters, "systemProductName", from_offset(0x05));

                    if (offset + 0x08 + 16 <= content.size()) {
                        std::ostringstream hex;
                        hex << std::uppercase << std::hex << std::setfill('0');
                        for (std::size_t index = 0; index != 16; ++index) {
                            hex << std::setw(2) << static_cast<int>(content[offset + 0x08 + index]);
                        }
                        append_parameter(parameters, "systemUuid", hex.str());
                    }
                    break;
                }

                case 2:
                    append_parameter(parameters, "baseboardManufacturer", from_offset(0x04));
                    append_parameter(parameters, "baseboardProduct", from_offset(0x05));
                    append_parameter(parameters, "baseboardVersion", from_offset(0x06));
                    append_parameter(parameters, "baseboardSerialNumber", from_offset(0x07));
                    append_parameter(parameters, "baseboardAssetTag", from_offset(0x08));
                    break;

                case 4:
                    append_parameter(parameters, "processorManufacturer", from_offset(0x07));
                    append_parameter(parameters, "processorVersion", from_offset(0x10));
                    append_parameter(parameters, "processorAssetTag", from_offset(0x21));
                    append_parameter(parameters, "processorPartNumber", from_offset(0x22));
                    break;

                default:
                    break;
            }

            offset = end_of_table;
        }
    }
#endif
};

} // namespace moonbase
