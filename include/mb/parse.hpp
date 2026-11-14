#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mb {

// Splits a string at commas, dropping empty parts.
inline std::vector<std::string> split_list(std::string_view list) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos <= list.size()) {
        auto end = list.find(',', pos);
        if (end == std::string_view::npos) {
            end = list.size();
        }
        if (end > pos) {
            parts.emplace_back(list.substr(pos, end - pos));
        }
        pos = end + 1;
    }
    return parts;
}

// Parses "4096", "4K", "4KiB", "1.5M", "2G", ... (binary multiples).
inline std::size_t parse_size(std::string_view s) {
    std::string str(s);
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(str.c_str(), &end);
    if (str.empty() || errno != 0 || end == str.c_str() || !(v >= 0)) {
        throw std::runtime_error("Invalid size '" + str + "'");
    }
    std::string_view suffix(end);
    double factor = 1;
    if (!suffix.empty()) {
        switch (suffix[0]) {
            case 'K':
            case 'k':
                factor = 1024.0;
                break;
            case 'M':
                factor = 1024.0 * 1024;
                break;
            case 'G':
                factor = 1024.0 * 1024 * 1024;
                break;
            case 'T':
                factor = 1024.0 * 1024 * 1024 * 1024;
                break;
            case 'B':
                break;
            default:
                throw std::runtime_error("Invalid size suffix in '" + str + "'");
        }
        auto rest = suffix.substr(1);
        bool valid = suffix[0] == 'B' ? rest.empty() : rest.empty() || rest == "B" || rest == "i" || rest == "iB";
        if (!valid) {
            throw std::runtime_error("Invalid size suffix in '" + str + "'");
        }
    }
    return static_cast<std::size_t>(std::llround(v * factor));
}

inline std::size_t round_down(std::size_t value, std::size_t multiple) {
    return value / multiple * multiple;
}

inline std::size_t round_up(std::size_t value, std::size_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

// Parses a comma separated list of sizes and ranges "min:max:steps", where a range contains steps sizes per doubling
// from min to max (inclusive). All sizes are rounded down to a multiple of granularity (at least granularity).
inline std::vector<std::size_t> parse_sizes(std::string_view list, std::size_t granularity) {
    std::vector<std::size_t> sizes;
    auto add = [&](std::size_t size) {
        size = std::max(granularity, round_down(size, granularity));
        if (sizes.empty() || sizes.back() != size) {
            sizes.push_back(size);
        }
    };
    for (auto const& part : split_list(list)) {
        auto first = part.find(':');
        if (first == std::string::npos) {
            add(parse_size(part));
            continue;
        }
        auto second = part.find(':', first + 1);
        auto min = parse_size(std::string_view(part).substr(0, first));
        auto max = parse_size(std::string_view(part).substr(
            first + 1, second == std::string::npos ? std::string::npos : second - first - 1));
        auto steps = second == std::string::npos ? 1 : std::strtol(part.c_str() + second + 1, nullptr, 10);
        if (min == 0 || max < min || steps <= 0) {
            throw std::runtime_error("Invalid size range '" + part + "'");
        }
        for (long k = 0;; ++k) {
            auto size = static_cast<double>(min) * std::exp2(static_cast<double>(k) / static_cast<double>(steps));
            if (size > static_cast<double>(max) * (1 + 1e-9)) {
                break;
            }
            add(static_cast<std::size_t>(std::llround(size)));
        }
    }
    if (sizes.empty()) {
        throw std::runtime_error("No sizes given");
    }
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    return sizes;
}

// Parses a CPU list such as "0-3,8,10-11".
inline std::vector<std::size_t> parse_cpu_list(std::string_view list) {
    std::vector<std::size_t> cpus;
    for (auto const& part : split_list(list)) {
        auto dash = part.find('-');
        char* end = nullptr;
        auto first = std::strtoul(part.c_str(), &end, 10);
        auto last = dash == std::string::npos ? first : std::strtoul(part.c_str() + dash + 1, &end, 10);
        if (*end != '\0' || last < first) {
            throw std::runtime_error("Invalid CPU list '" + std::string(list) + "'");
        }
        for (auto c = first; c <= last; ++c) {
            cpus.push_back(c);
        }
    }
    return cpus;
}

}  // namespace mb
