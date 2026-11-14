#pragma once

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace mb {

// A minimal streaming JSON writer. Keys and values are written in call order; commas are inserted automatically.
class Json {
    std::ostream& out_;
    std::vector<bool> first_;  // per open container: no element written yet
    bool after_key_ = false;

    void separate() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (!first_.empty()) {
            if (!first_.back()) {
                out_ << ',';
            }
            first_.back() = false;
        }
    }

    void string(std::string_view s) {
        out_ << '"';
        for (char c : s) {
            switch (c) {
                case '"':
                    out_ << "\\\"";
                    break;
                case '\\':
                    out_ << "\\\\";
                    break;
                case '\n':
                    out_ << "\\n";
                    break;
                case '\t':
                    out_ << "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        out_ << ' ';
                    } else {
                        out_ << c;
                    }
            }
        }
        out_ << '"';
    }

   public:
    explicit Json(std::ostream& out) : out_(out) {
    }

    Json& begin_object() {
        separate();
        out_ << '{';
        first_.push_back(true);
        return *this;
    }

    Json& end_object() {
        first_.pop_back();
        out_ << '}';
        return *this;
    }

    Json& begin_array() {
        separate();
        out_ << '[';
        first_.push_back(true);
        return *this;
    }

    Json& end_array() {
        first_.pop_back();
        out_ << ']';
        return *this;
    }

    Json& key(std::string_view k) {
        separate();
        string(k);
        out_ << ':';
        after_key_ = true;
        return *this;
    }

    Json& value(std::string_view v) {
        separate();
        string(v);
        return *this;
    }

    Json& value(char const* v) {
        return value(std::string_view(v));
    }

    Json& value(bool v) {
        separate();
        out_ << (v ? "true" : "false");
        return *this;
    }

    template <std::integral T>
        requires(!std::same_as<T, bool>)
    Json& value(T v) {
        separate();
        out_ << v;
        return *this;
    }

    Json& value(double v) {
        separate();
        if (!std::isfinite(v)) {
            out_ << "null";
            return *this;
        }
        char buf[32];
        auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        out_ << std::string_view(buf, static_cast<std::size_t>(end - buf));
        return *this;
    }

    template <typename T>
    Json& value(std::vector<T> const& v) {
        begin_array();
        for (auto const& x : v) {
            value(x);
        }
        return end_array();
    }

    template <typename T>
    Json& entry(std::string_view k, T const& v) {
        key(k);
        return value(v);
    }
};

}  // namespace mb
