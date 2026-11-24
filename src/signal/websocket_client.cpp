#include "websocket_client.h"
#include "telemetry/log_redaction.h"
#include "telemetry/sdp_negotiation_trace.h"
#include "safe_spawn.h"
#include <random>
#include <istream>
#include <sstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>
#include <openssl/sha.h>

#ifdef _WIN32
#include <wincrypt.h>
#endif

namespace livekit {

namespace {

class WebSocketHttpErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "websocket_http";
    }

    std::string message(int value) const override {
        return "WebSocket upgrade rejected with HTTP " + std::to_string(value);
    }
};

const std::error_category& websocket_http_error_category() {
    static WebSocketHttpErrorCategory category;
    return category;
}

std::string LowerAsciiCopy(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string TrimAndCollapseHeaderValue(std::string_view raw, size_t max_len = 256) {
    size_t begin = 0;
    size_t end = raw.size();
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (begin < end && is_ws(static_cast<unsigned char>(raw[begin]))) {
        ++begin;
    }
    while (end > begin && is_ws(static_cast<unsigned char>(raw[end - 1]))) {
        --end;
    }
    std::string out;
    out.reserve(std::min(end - begin, max_len));
    for (size_t i = begin; i < end && out.size() < max_len; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x20 || c == 0x7f) {
            out.push_back(' ');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

constexpr size_t kMaxHandshakeErrorBodyBytes = 4096;
constexpr auto kHandshakeErrorBodyReadTimeout = std::chrono::milliseconds(500);

void DecodeChunkedBody(
    std::string_view raw,
    size_t max_bytes,
    std::string& decoded,
    bool& complete) {
    decoded.clear();
    complete = false;
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t line_end = raw.find('\n', pos);
        if (line_end == std::string_view::npos) {
            return;
        }
        std::string_view size_line = raw.substr(pos, line_end - pos);
        if (!size_line.empty() && size_line.back() == '\r') {
            size_line.remove_suffix(1);
        }
        if (const size_t semi = size_line.find(';'); semi != std::string_view::npos) {
            size_line = size_line.substr(0, semi);
        }
        while (!size_line.empty() && (size_line.front() == ' ' || size_line.front() == '\t')) {
            size_line.remove_prefix(1);
        }
        while (!size_line.empty() && (size_line.back() == ' ' || size_line.back() == '\t')) {
            size_line.remove_suffix(1);
        }
        if (size_line.empty()) {
            return;
        }
        size_t chunk_size = 0;
        for (char c : size_line) {
            unsigned int digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<unsigned int>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<unsigned int>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<unsigned int>(c - 'A' + 10);
            } else {
                return;
            }
            if (chunk_size > (std::numeric_limits<size_t>::max() - digit) / 16) {
                chunk_size = std::numeric_limits<size_t>::max();
            } else {
                chunk_size = chunk_size * 16 + digit;
            }
        }
        if (chunk_size == 0) {
            complete = true;
            return;
        }
        const size_t data_start = line_end + 1;
        const size_t avail = raw.size() - data_start;
        const size_t remaining_cap = max_bytes - decoded.size();
        const size_t copy_len = std::min({ avail, chunk_size, remaining_cap });
        if (copy_len > 0) {
            decoded.append(raw.substr(data_start, copy_len));
        }
        if (decoded.size() >= max_bytes) {
            complete = true;
            return;
        }
        if (avail < chunk_size) {
            return;
        }
        pos = data_start + chunk_size;
        if (pos >= raw.size()) {
            return;
        }
        if (raw[pos] == '\r') {
            if (pos + 1 >= raw.size()) {
                return;
            }
            if (raw[pos + 1] != '\n') {
                return;
            }
            pos += 2;
        } else if (raw[pos] == '\n') {
            ++pos;
        } else {
            return;
        }
    }
}

struct HandshakeJsonError {
    std::string message;
    std::string code;
};

class BoundedJsonErrorReader final {
public:
    explicit BoundedJsonErrorReader(std::string_view input) : src_(input) {}

    bool ReadRoot(HandshakeJsonError& out) {
        SkipWs();
        if (pos_ >= src_.size() || src_[pos_] != '{') {
            return false;
        }
        ++pos_;
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < src_.size()) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWs();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                return false;
            }
            ++pos_;
            SkipWs();
            if (key == "error") {
                if (!ReadErrorField()) {
                    return false;
                }
            } else if (key == "message") {
                if (!ReadStringOrSkip(top_message_, 1)) {
                    return false;
                }
            } else if (key == "detail") {
                if (!ReadDetailField()) {
                    return false;
                }
            } else if (key == "code") {
                if (!ReadScalarOrSkip(top_code_, 1)) {
                    return false;
                }
            } else if (key == "errorCode") {
                if (!ReadScalarOrSkip(top_error_code_, 1)) {
                    return false;
                }
            } else {
                if (!SkipValue(1)) {
                    return false;
                }
            }
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                Finalize(out);
                return true;
            }
            return false;
        }
        return false;
    }

private:
    static constexpr int kMaxJsonDepth = 8;

    void SkipWs() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    static bool ParseHex4(std::string_view hex, uint32_t& value) {
        if (hex.size() != 4) {
            return false;
        }
        value = 0;
        for (char c : hex) {
            uint32_t d = 0;
            if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
            else return false;
            value = (value << 4) | d;
        }
        return true;
    }

    static void AppendUtf8(uint32_t cp, std::string& out) {
        if (cp <= 0x7Fu) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0x10FFFFu) {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    bool ParseString(std::string& out, bool store = true) {
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                if (store && out.size() < 1024) {
                    out.push_back(c);
                }
                continue;
            }
            if (pos_ >= src_.size()) {
                return false;
            }
            const char esc = src_[pos_++];
            char decoded = esc;
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                decoded = esc;
                if (store && out.size() < 1024) out.push_back(decoded);
                break;
            case 'b':
                if (store && out.size() < 1024) out.push_back('\b');
                break;
            case 'f':
                if (store && out.size() < 1024) out.push_back('\f');
                break;
            case 'n':
                if (store && out.size() < 1024) out.push_back('\n');
                break;
            case 'r':
                if (store && out.size() < 1024) out.push_back('\r');
                break;
            case 't':
                if (store && out.size() < 1024) out.push_back('\t');
                break;
            case 'u': {
                if (pos_ + 4 > src_.size()) {
                    return false;
                }
                uint32_t cp = 0;
                if (!ParseHex4(src_.substr(pos_, 4), cp)) {
                    return false;
                }
                pos_ += 4;
                if (cp >= 0xD800u && cp <= 0xDBFFu &&
                    pos_ + 6 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                    uint32_t low = 0;
                    if (ParseHex4(src_.substr(pos_ + 2, 4), low) && low >= 0xDC00u && low <= 0xDFFFu) {
                        pos_ += 6;
                        cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                    }
                }
                if (store && out.size() < 1024) {
                    AppendUtf8(cp, out);
                }
                break;
            }
            default:
                if (store && out.size() < 1024) {
                    out.push_back(esc);
                }
                break;
            }
        }
        return false;
    }

    bool ParseNumberLiteral(std::string& out, bool store) {
        const size_t start = pos_;
        if (pos_ < src_.size() && src_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= src_.size()) {
            return false;
        }
        if (src_[pos_] == '0') {
            ++pos_;
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        } else {
            return false;
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                return false;
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                return false;
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (store) {
            out.assign(src_.substr(start, std::min<size_t>(pos_ - start, 128)));
        }
        return true;
    }

    bool SkipValue(int depth) {
        if (depth > kMaxJsonDepth) {
            return false;
        }
        SkipWs();
        if (pos_ >= src_.size()) {
            return false;
        }
        const char c = src_[pos_];
        if (c == '"') {
            std::string dummy;
            return ParseString(dummy, false);
        }
        if (c == '{') {
            ++pos_;
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            while (pos_ < src_.size()) {
                SkipWs();
                std::string dummy_key;
                if (!ParseString(dummy_key, false)) return false;
                SkipWs();
                if (pos_ >= src_.size() || src_[pos_] != ':') return false;
                ++pos_;
                if (!SkipValue(depth + 1)) return false;
                SkipWs();
                if (pos_ < src_.size() && src_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < src_.size() && src_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                return false;
            }
            return false;
        }
        if (c == '[') {
            ++pos_;
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            while (pos_ < src_.size()) {
                if (!SkipValue(depth + 1)) return false;
                SkipWs();
                if (pos_ < src_.size() && src_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < src_.size() && src_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                return false;
            }
            return false;
        }
        if (src_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return true;
        }
        if (src_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return true;
        }
        if (src_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            std::string dummy;
            return ParseNumberLiteral(dummy, false);
        }
        return false;
    }

    bool ReadStringOrSkip(std::string& target, int depth) {
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '"') {
            return ParseString(target);
        }
        return SkipValue(depth);
    }

    bool ReadScalarOrSkip(std::string& target, int depth) {
        SkipWs();
        if (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == '"') {
                return ParseString(target);
            }
            if (c == '-' || (c >= '0' && c <= '9')) {
                return ParseNumberLiteral(target, true);
            }
        }
        return SkipValue(depth);
    }

    bool ReadErrorField() {
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '"') {
            return ParseString(top_error_);
        }
        if (pos_ >= src_.size() || src_[pos_] != '{') {
            return SkipValue(1);
        }
        ++pos_;
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < src_.size()) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos_ >= src_.size() || src_[pos_] != ':') return false;
            ++pos_;
            SkipWs();
            if (key == "message") {
                if (!ReadStringOrSkip(nested_error_msg_, 2)) return false;
            } else if (key == "detail") {
                if (!ReadStringOrSkip(nested_error_detail_, 2)) return false;
            } else if (key == "code") {
                if (!ReadScalarOrSkip(nested_error_code_, 2)) return false;
            } else if (key == "type") {
                if (!ReadStringOrSkip(nested_error_type_, 2)) return false;
            } else {
                if (!SkipValue(2)) return false;
            }
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
        return false;
    }

    bool ReadDetailField() {
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '"') {
            return ParseString(top_detail_);
        }
        if (pos_ >= src_.size() || src_[pos_] != '{') {
            return SkipValue(1);
        }
        ++pos_;
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < src_.size()) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos_ >= src_.size() || src_[pos_] != ':') return false;
            ++pos_;
            SkipWs();
            if (key == "message") {
                if (!ReadStringOrSkip(nested_detail_msg_, 2)) return false;
            } else if (key == "status") {
                if (!ReadStringOrSkip(nested_detail_status_, 2)) return false;
            } else {
                if (!SkipValue(2)) return false;
            }
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
        return false;
    }

    void Finalize(HandshakeJsonError& out) const {
        if (!nested_error_msg_.empty()) out.message = nested_error_msg_;
        else if (!nested_error_detail_.empty()) out.message = nested_error_detail_;
        else if (!top_error_.empty()) out.message = top_error_;
        else if (!top_message_.empty()) out.message = top_message_;
        else if (!top_detail_.empty()) out.message = top_detail_;
        else if (!nested_detail_msg_.empty()) out.message = nested_detail_msg_;

        if (!nested_error_code_.empty()) out.code = nested_error_code_;
        else if (!top_code_.empty()) out.code = top_code_;
        else if (!top_error_code_.empty()) out.code = top_error_code_;
        else if (!nested_error_type_.empty()) out.code = nested_error_type_;
        else if (!nested_detail_status_.empty()) out.code = nested_detail_status_;
    }

    std::string_view src_;
    size_t pos_ = 0;
    std::string nested_error_msg_;
    std::string nested_error_detail_;
    std::string top_error_;
    std::string top_message_;
    std::string top_detail_;
    std::string nested_detail_msg_;
    std::string nested_error_code_;
    std::string top_code_;
    std::string top_error_code_;
    std::string nested_error_type_;
    std::string nested_detail_status_;
};

HandshakeJsonError ExtractJsonHandshakeError(std::string_view body) {
    HandshakeJsonError out;
    BoundedJsonErrorReader reader(body);
    if (!reader.ReadRoot(out)) {
        return {};
    }
    if (!out.message.empty()) {
        out.message = livekit::secure_log::SanitizeForOutput(
            TrimAndCollapseHeaderValue(out.message, 256));
    }
    if (!out.code.empty()) {
        out.code = livekit::secure_log::SanitizeForOutput(
            TrimAndCollapseHeaderValue(out.code, 64));
    }
    return out;
}

std::string FormatHandshakeDiagnosticDetail(
    std::string_view api_status_code,
    std::string_view api_message,
    std::string_view dg_error,
    std::string_view tt_logid,
    std::string_view request_id,
    std::string_view trace_id,
    std::string_view json_code,
    std::string_view json_message) {
    std::string primary;
    if (!api_status_code.empty() && !api_message.empty()) {
        primary = std::string(api_status_code) + ": " + std::string(api_message);
    } else if (!api_message.empty()) {
        primary = std::string(api_message);
    } else if (!dg_error.empty()) {
        primary = std::string(dg_error);
    } else if (!json_message.empty()) {
        if (!json_code.empty() &&
            json_code.size() <= 32 &&
            json_code.find(' ') == std::string_view::npos &&
            json_message.find(json_code) == std::string_view::npos) {
            primary = std::string(json_code) + ": " + std::string(json_message);
        } else {
            primary = std::string(json_message);
        }
    } else if (!api_status_code.empty()) {
        primary = "status=" + std::string(api_status_code);
    } else if (!json_code.empty()) {
        primary = std::string(json_code);
    }

    std::string trace;
    if (!tt_logid.empty()) {
        trace = "logid=" + std::string(tt_logid);
    } else if (!request_id.empty()) {
        trace = "request_id=" + std::string(request_id);
    } else if (!trace_id.empty()) {
        trace = "trace_id=" + std::string(trace_id);
    }

    if (!primary.empty() && !trace.empty()) {
        return primary + " (" + trace + ")";
    }
    if (!primary.empty()) {
        return primary;
    }
    return trace;
}

bool IsValidHttpHeaderName(std::string_view name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    for (unsigned char c : name) {
        if (c <= 0x20 || c >= 0x7f || c == ':') {
            return false;
        }
    }
    const std::string lower = LowerAsciiCopy(name);
    return lower != "host" &&
           lower != "upgrade" &&
           lower != "connection" &&
           lower != "sec-websocket-key" &&
           lower != "sec-websocket-version" &&
           lower != "sec-websocket-accept" &&
           lower != "content-length" &&
           lower != "transfer-encoding";
}

bool IsValidHttpHeaderValue(std::string_view value) {
    if (value.size() > 8192) {
        return false;
    }
    for (unsigned char c : value) {
        if (c == 0 || c == '\r' || c == '\n' || c == 0x7f || (c < 0x20 && c != '\t')) {
            return false;
        }
    }
    return true;
}

bool IsAuthorizationHeaderName(std::string_view name) {
    return LowerAsciiCopy(name) == "authorization";
}

bool ValidateExtraHeaders(const WebSocketClient::HeaderList& headers) {
    for (const auto& [name, value] : headers) {
        if (!IsValidHttpHeaderName(name) || !IsValidHttpHeaderValue(value)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::error_code MakeWebSocketHttpError(unsigned int status_code) {
    return {static_cast<int>(status_code), websocket_http_error_category()};
}

std::optional<unsigned int> WebSocketHttpStatus(const std::error_code& error) {
    if (&error.category() != &websocket_http_error_category() || error.value() < 100) {
        return std::nullopt;
    }
    return static_cast<unsigned int>(error.value());
}

bool IsWebSocketHttpStatus(const std::error_code& error, unsigned int status_code) {
    const auto actual = WebSocketHttpStatus(error);
    return actual.has_value() && *actual == status_code;
}

std::error_code CheckWebSocketFrameLength(uint64_t payload_length) noexcept {
    constexpr uint64_t kMaxFramePayload = 64ULL * 1024 * 1024;
    if (payload_length > (std::numeric_limits<size_t>::max)() ||
        payload_length > kMaxFramePayload) {
        return std::make_error_code(std::errc::message_size);
    }
    return {};
}

static std::string Base64Encode(const unsigned char* buffer, size_t length) {
    static const char char_set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((length + 2) / 3) * 4);
    size_t i = 0;
    while (i < length) {
        size_t count = length - i;
        uint32_t octet_a = buffer[i++];
        uint32_t octet_b = (count > 1) ? buffer[i++] : 0;
        uint32_t octet_c = (count > 2) ? buffer[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        result.push_back(char_set[(triple >> 18) & 0x3F]);
        result.push_back(char_set[(triple >> 12) & 0x3F]);
        result.push_back((count > 1) ? char_set[(triple >> 6) & 0x3F] : '=');
        result.push_back((count > 2) ? char_set[triple & 0x3F] : '=');
    }
    return result;
}

static std::string GenerateWebSocketKey() {
    unsigned char key[16];
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<int> distribution(0, 255);
    for (int i = 0; i < 16; ++i) {
        key[i] = static_cast<unsigned char>(distribution(generator));
    }
    return Base64Encode(key, 16);
}

static std::optional<std::string> GetProxyFromEnv(bool is_secure) {
    const char* proxy = nullptr;
    if (is_secure) {
        proxy = std::getenv("HTTPS_PROXY");
        if (!proxy) proxy = std::getenv("https_proxy");
    } else {
        proxy = std::getenv("HTTP_PROXY");
        if (!proxy) proxy = std::getenv("http_proxy");
    }
    if (proxy && strlen(proxy) > 0) {
        return std::string(proxy);
    }
    return std::nullopt;
}

struct ProxyEndpoint {
    std::string host;
    std::string port;
    std::string credentials;
};

static std::optional<ProxyEndpoint> ParseProxyEndpoint(std::string value) {
    if (value.find("://") == std::string::npos) value = "http://" + value;
    const size_t scheme_end = value.find("://");
    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = value.find_first_of("/?#", authority_start);
    std::string authority = value.substr(
        authority_start,
        authority_end == std::string::npos ? std::string::npos : authority_end - authority_start);

    std::string credentials;
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        credentials = authority.substr(0, at);
        authority = authority.substr(at + 1);
    }

    std::error_code error;
    auto parsed = AdmitCredentialUrl(
        value.substr(0, authority_start) + authority,
        CredentialUrlKind::Http,
        CredentialUrlPolicy{true, false},
        error);
    if (!parsed) return std::nullopt;
    return ProxyEndpoint{parsed->host, parsed->port, std::move(credentials)};
}

static void LoadSystemCertificates(asio::ssl::context& ssl_ctx) {
#ifdef _WIN32
    HCERTSTORE hStore = CertOpenSystemStoreA(0, "ROOT");
    if (hStore) {
        X509_STORE* store = SSL_CTX_get_cert_store(ssl_ctx.native_handle());
        PCCERT_CONTEXT pContext = NULL;
        while ((pContext = CertFindCertificateInStore(hStore, X509_ASN_ENCODING, 0, CERT_FIND_ANY, NULL, pContext)) != NULL) {
            const unsigned char* d2i_ptr = pContext->pbCertEncoded;
            X509* x509 = d2i_X509(NULL, &d2i_ptr, pContext->cbCertEncoded);
            if (x509) {
                X509_STORE_add_cert(store, x509);
                X509_free(x509);
            }
        }
        CertCloseStore(hStore, 0);
    }
#else
    ssl_ctx.set_default_verify_paths();
#endif
}

WebSocketClient::WebSocketClient(asio::io_context& io_ctx, asio::ssl::context& ssl_ctx)
    : io_ctx_(io_ctx), ssl_ctx_(ssl_ctx), strand_(asio::make_strand(io_ctx)) {
    LoadSystemCertificates(ssl_ctx_);
}

WebSocketClient::~WebSocketClient() {
}

asio::awaitable<std::error_code> WebSocketClient::Connect(std::string url_str,
                                                         std::string token,
                                                         std::chrono::milliseconds timeout,
                                                         CredentialUrlPolicy policy) {
    co_return co_await Connect(
        std::move(url_str), std::move(token), timeout, policy, HeaderList{});
}

asio::awaitable<std::error_code> WebSocketClient::Connect(std::string url_str,
                                                         std::string token,
                                                         std::chrono::milliseconds timeout,
                                                         CredentialUrlPolicy policy,
                                                         HeaderList extra_headers) {
    auto self = shared_from_this();
    co_return co_await asio::co_spawn(
        strand_,
        [self, url_str = std::move(url_str), token = std::move(token), timeout, policy,
         extra_headers = std::move(extra_headers)]() mutable
            -> asio::awaitable<std::error_code> {
            co_return co_await self->ConnectOnOwner(
                std::move(url_str), std::move(token), timeout, policy, std::move(extra_headers));
        },
        asio::use_awaitable);
}

asio::awaitable<std::error_code> WebSocketClient::ConnectOnOwner(
    std::string url_str,
    std::string token,
    std::chrono::milliseconds timeout,
    CredentialUrlPolicy policy,
    HeaderList extra_headers) {
    auto self = shared_from_this();
    if (abort_started_) {
        co_return asio::error::operation_aborted;
    }
    if (!ValidateExtraHeaders(extra_headers)) {
        co_return std::make_error_code(std::errc::invalid_argument);
    }
    {
        std::lock_guard<std::mutex> lock(handshake_diag_mutex_);
        last_handshake_error_detail_.clear();
    }
    fragment_opcode_ = 0;
    fragment_buffer_.clear();
    std::error_code admission_error;
    auto admitted = AdmitCredentialUrl(
        url_str, CredentialUrlKind::WebSocket, policy, admission_error);
    if (!admitted) co_return admission_error;
    const Url url = std::move(*admitted);
    is_ssl_ = url.secure;
    closed_by_us_ = false;

    auto executor = co_await asio::this_coro::executor;
    auto timer = std::make_shared<asio::steady_timer>(executor);
    timer->expires_after(timeout);
    
    auto connect_done = std::make_shared<std::atomic<bool>>(false);
    
    timer->async_wait([this, self, connect_done](const std::error_code& ec) {
        if (!ec && !connect_done->load()) {
            shutdown_stream();
        }
    });

    try {
        auto proxy_env = GetProxyFromEnv(is_ssl_);
        if (proxy_env) {
            auto proxy = ParseProxyEndpoint(*proxy_env);
            if (!proxy) {
                throw std::system_error(std::make_error_code(std::errc::invalid_argument));
            }
            std::string auth_hdr;
            if (!proxy->credentials.empty()) {
                auth_hdr = "Proxy-Authorization: Basic " +
                    Base64Encode(
                        reinterpret_cast<const unsigned char*>(proxy->credentials.data()),
                        proxy->credentials.size()) +
                    "\r\n";
            }
            
            co_await AsyncConnectSocket(proxy->host, proxy->port);
            std::optional<std::string> auth_opt;
            if (!auth_hdr.empty()) auth_opt = auth_hdr;
            co_await AsyncHttpProxyConnect(url.host, url.port, url.host, url.port, auth_opt);
        } else {
            co_await AsyncConnectSocket(url.host, url.port);
        }

        if (is_ssl_) {
            co_await AsyncSslHandshake(url.host);
        }

        co_await AsyncWsHandshake(url, std::move(token), std::move(extra_headers));
        
        connect_done->store(true);
        timer->cancel();
        connected_.store(true, std::memory_order_release);
        
        co_return std::error_code{};
    } catch (const std::system_error& e) {
        connect_done->store(true);
        timer->cancel();
        shutdown_stream();
        co_return e.code();
    } catch (...) {
        connect_done->store(true);
        timer->cancel();
        shutdown_stream();
        co_return std::make_error_code(std::errc::connection_aborted);
    }
}

void WebSocketClient::StartRead() {
    asio::post(strand_, [self = this->shared_from_this()]() {
        if (self->abort_started_ ||
            !self->connected_.load(std::memory_order_acquire)) {
            return;
        }
        livekit::safe_co_spawn(self->strand_, [self]() -> asio::awaitable<void> {
            try {
                co_await self->ReadLoop();
            } catch (...) {}
        });
    });
}

asio::awaitable<void> WebSocketClient::SendBinary(std::vector<uint8_t> data,
    std::shared_ptr<diagnostic::SdpSendTrace> trace) {
    auto self = shared_from_this();
    co_await asio::co_spawn(
        strand_,
        [self, data = std::move(data), trace = std::move(trace)]() -> asio::awaitable<void> {
            co_await self->SendRawFrame(0x2, data, trace);
        },
        asio::use_awaitable);
}

asio::awaitable<void> WebSocketClient::SendText(std::string text) {
    auto self = shared_from_this();
    co_await asio::co_spawn(
        strand_,
        [self, text = std::move(text)]() -> asio::awaitable<void> {
            std::vector<uint8_t> data(text.begin(), text.end());
            co_await self->SendRawFrame(0x1, data);
        },
        asio::use_awaitable);
}

asio::awaitable<void> WebSocketClient::SendPing(std::vector<uint8_t> payload) {
    auto self = shared_from_this();
    co_await asio::co_spawn(
        strand_,
        [self, payload = std::move(payload)]() -> asio::awaitable<void> {
            co_await self->SendRawFrame(0x9, payload);
        },
        asio::use_awaitable);
}

asio::awaitable<void> WebSocketClient::Close(uint16_t code, const std::string& reason) {
    auto self = shared_from_this();
    co_await asio::co_spawn(
        strand_,
        [self, code, reason]() -> asio::awaitable<void> {
            co_await self->CloseOnOwner(code, reason);
        },
        asio::use_awaitable);
}

asio::awaitable<void> WebSocketClient::CloseOnOwner(uint16_t code, std::string reason) {
    if (!connected_.load(std::memory_order_acquire)) co_return;
    closed_by_us_ = true;
    
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code & 0xFF));
    payload.insert(payload.end(), reason.begin(), reason.end());
    
    try {
        co_await SendRawFrame(0x8, payload);
    } catch (...) {
        // Ignore errors during close frame send
    }
    shutdown_stream();
}

void WebSocketClient::Abort() {
    asio::post(strand_, [self = shared_from_this()]() {
        self->AbortOnOwner();
    });
}

void WebSocketClient::AbortOnOwner() {
    if (abort_started_) return;
    abort_started_ = true;
    closed_by_us_ = true;
    message_cb_ = nullptr;
    text_message_cb_ = nullptr;
    close_cb_ = nullptr;
    error_cb_ = nullptr;
    shutdown_stream();
}

void WebSocketClient::SetOnMessage(MessageCallback cb) {
    asio::post(strand_, [self = shared_from_this(), cb = std::move(cb)]() mutable {
        if (!self->abort_started_) self->message_cb_ = std::move(cb);
    });
}

void WebSocketClient::SetOnTextMessage(TextMessageCallback cb) {
    asio::post(strand_, [self = shared_from_this(), cb = std::move(cb)]() mutable {
        if (!self->abort_started_) self->text_message_cb_ = std::move(cb);
    });
}

void WebSocketClient::SetOnClose(CloseCallback cb) {
    asio::post(strand_, [self = shared_from_this(), cb = std::move(cb)]() mutable {
        if (!self->abort_started_) self->close_cb_ = std::move(cb);
    });
}

void WebSocketClient::SetOnError(ErrorCallback cb) {
    asio::post(strand_, [self = shared_from_this(), cb = std::move(cb)]() mutable {
        if (!self->abort_started_) self->error_cb_ = std::move(cb);
    });
}

asio::awaitable<void> WebSocketClient::ReadLoop() {
    try {
        while (connected_.load(std::memory_order_acquire)) {
            co_await ReadFrame();
        }
    } catch (const std::system_error& e) {
        handle_error(e.code());
    } catch (...) {
        std::error_code ec = asio::error::operation_aborted;
        handle_error(ec);
    }
}

asio::awaitable<void> WebSocketClient::ReadFrame() {
    uint8_t header[2];
    co_await async_read_stream(asio::buffer(header, 2));

    bool fin = (header[0] & 0x80) != 0;
    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint8_t base_len = header[1] & 0x7F;

    uint64_t payload_len = 0;
    if (base_len <= 125) {
        payload_len = base_len;
    } else if (base_len == 126) {
        uint8_t ext_len[2];
        co_await async_read_stream(asio::buffer(ext_len, 2));
        payload_len = (static_cast<uint64_t>(ext_len[0]) << 8) | ext_len[1];
    } else if (base_len == 127) {
        uint8_t ext_len[8];
        co_await async_read_stream(asio::buffer(ext_len, 8));
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | ext_len[i];
        }
    }

    if (const auto error = CheckWebSocketFrameLength(payload_len)) {
        throw std::system_error(error);
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        co_await async_read_stream(asio::buffer(mask_key, 4));
    }

    std::vector<uint8_t> payload(static_cast<size_t>(payload_len));
    if (payload_len > 0) {
        co_await async_read_stream(asio::buffer(payload));
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= mask_key[i % 4];
            }
        }
    }

    co_await HandleFrame(opcode, fin, std::move(payload));
}

asio::awaitable<void> WebSocketClient::HandleFrame(uint8_t opcode, bool fin, std::vector<uint8_t> payload) {
    if (opcode == 0x8) {
        uint16_t code = 1000;
        std::string reason;
        if (payload.size() >= 2) {
            code = (static_cast<uint16_t>(payload[0]) << 8) | payload[1];
            reason = std::string(payload.begin() + 2, payload.end());
        }
        if (close_cb_) {
            close_cb_(code, reason);
        }
        shutdown_stream();
    } else if (opcode == 0x9) {
        co_await SendRawFrame(0xA, payload);
    } else if (opcode == 0xA) {
        // Pong
    } else if (opcode == 0x1 || opcode == 0x2) {
        if (fragment_opcode_ != 0) {
            fragment_opcode_ = 0;
            fragment_buffer_.clear();
            handle_error(std::make_error_code(std::errc::protocol_error));
            co_return;
        }
        if (fin) {
            if (opcode == 0x2) {
                if (message_cb_) {
                    message_cb_(payload);
                }
            } else {
                if (text_message_cb_) {
                    text_message_cb_(std::string(payload.begin(), payload.end()));
                }
            }
        } else {
            if (payload.size() > kMaxAssembledMessageBytes) {
                fragment_opcode_ = 0;
                fragment_buffer_.clear();
                handle_error(std::make_error_code(std::errc::message_size));
                co_return;
            }
            fragment_opcode_ = opcode;
            fragment_buffer_ = std::move(payload);
        }
    } else if (opcode == 0x0) {
        if (fragment_opcode_ == 0) {
            handle_error(std::make_error_code(std::errc::protocol_error));
            co_return;
        }
        if (payload.size() > kMaxAssembledMessageBytes ||
            fragment_buffer_.size() > kMaxAssembledMessageBytes - payload.size()) {
            fragment_opcode_ = 0;
            fragment_buffer_.clear();
            handle_error(std::make_error_code(std::errc::message_size));
            co_return;
        }
        fragment_buffer_.insert(fragment_buffer_.end(), payload.begin(), payload.end());
        if (fin) {
            const uint8_t completed_opcode = fragment_opcode_;
            std::vector<uint8_t> assembled = std::move(fragment_buffer_);
            fragment_opcode_ = 0;
            fragment_buffer_.clear();
            if (completed_opcode == 0x2) {
                if (message_cb_) {
                    message_cb_(assembled);
                }
            } else if (completed_opcode == 0x1) {
                if (text_message_cb_) {
                    text_message_cb_(std::string(assembled.begin(), assembled.end()));
                }
            }
        }
    }
}

asio::awaitable<void> WebSocketClient::SendRawFrame(uint8_t opcode, const std::vector<uint8_t>& payload,
    std::shared_ptr<diagnostic::SdpSendTrace> trace) {
    std::vector<uint8_t> frame;
    frame.reserve(10 + payload.size());
    
    frame.push_back(0x80 | (opcode & 0x0F));
    uint8_t mask_bit = 0x80;
    
    size_t payload_len = payload.size();
    if (payload_len <= 125) {
        frame.push_back(mask_bit | static_cast<uint8_t>(payload_len));
    } else if (payload_len <= 65535) {
        frame.push_back(mask_bit | 126);
        frame.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    } else {
        frame.push_back(mask_bit | 127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF));
        }
    }
    
    uint8_t mask_key[4];
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<int> distribution(0, 255);
    for (int i = 0; i < 4; ++i) {
        mask_key[i] = static_cast<uint8_t>(distribution(generator));
        frame.push_back(mask_key[i]);
    }
    
    size_t header_size = frame.size();
    frame.resize(header_size + payload_len);
    for (size_t i = 0; i < payload_len; ++i) {
        frame[header_size + i] = payload[i] ^ mask_key[i % 4];
    }
 
    std::shared_ptr<QueuedMessage> msg;
    bool should_spawn = false;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        const size_t frame_bytes = frame.size();
        msg = std::make_shared<QueuedMessage>(QueuedMessage{std::move(trace), std::move(frame)});
        write_queue_.push(msg);
        queued_bytes_.fetch_add(frame_bytes, std::memory_order_acq_rel);
        
        if (!writing_) {
            writing_ = true;
            should_spawn = true;
        }
    }
    
    if (should_spawn) {
        livekit::safe_co_spawn(strand_, [self = shared_from_this()]() -> asio::awaitable<void> {
            co_await self->WriteLoop();
        });
    }
    co_return;
}

asio::awaitable<void> WebSocketClient::WriteLoop() {
    auto self = shared_from_this();
    std::shared_ptr<QueuedMessage> msg;
    try {
        while (true) {
            msg = PopWriteQueue();
            if (!msg) {
                break;
            }
            
            co_await async_write_stream(asio::buffer(msg->data));
            if (msg->trace) msg->trace->Complete();
        }
    } catch (const std::system_error& e) {
        if (msg && msg->trace) msg->trace->Fail();
        ResetWritingState();
        handle_error(e.code());
    } catch (...) {
        if (msg && msg->trace) msg->trace->Fail();
        ResetWritingState();
        std::error_code ec = asio::error::operation_aborted;
        handle_error(ec);
    }
}

std::shared_ptr<WebSocketClient::QueuedMessage> WebSocketClient::PopWriteQueue() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (write_queue_.empty() || !connected_.load(std::memory_order_acquire)) {
        writing_ = false;
        return nullptr;
    }
    auto msg = write_queue_.front();
    write_queue_.pop();
    const size_t frame_bytes = msg ? msg->data.size() : 0;
    const size_t prev = queued_bytes_.load(std::memory_order_acquire);
    queued_bytes_.store(prev >= frame_bytes ? (prev - frame_bytes) : 0, std::memory_order_release);
    return msg;
}

void WebSocketClient::ResetWritingState() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    writing_ = false;
}

asio::awaitable<void> WebSocketClient::AsyncConnectSocket(std::string host, std::string port) {
    auto executor = co_await asio::this_coro::executor;
    
    auto socket = std::make_unique<asio::ip::tcp::socket>(executor);
    auto* socket_raw = socket.get();
    stream_ = std::move(socket);
    
    std::error_code ec;
    auto addr = asio::ip::make_address(host, ec);
    if (!ec) {
        unsigned short port_num = static_cast<unsigned short>(std::stoi(port));
        asio::ip::tcp::endpoint ep(addr, port_num);
        co_await socket_raw->async_connect(ep, asio::use_awaitable);
    } else {
        asio::ip::tcp::resolver resolver(executor);
        auto results = co_await resolver.async_resolve(host, port, asio::use_awaitable);
        co_await asio::async_connect(*socket_raw, results, asio::use_awaitable);
    }
    
    if (is_ssl_) {
        auto socket_ptr = std::move(std::get<SocketPtr>(stream_));
        stream_ = std::monostate{};
        auto ssl_stream = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket_ptr), ssl_ctx_);
        stream_ = std::move(ssl_stream);
    }
}

asio::awaitable<void> WebSocketClient::AsyncHttpProxyConnect(std::string proxy_host, std::string proxy_port, std::string target_host, std::string target_port, std::optional<std::string> auth_header) {
    std::string req = "CONNECT " + target_host + ":" + target_port + " HTTP/1.1\r\n"
                      "Host: " + target_host + ":" + target_port + "\r\n";
    if (auth_header) {
        req += *auth_header;
    }
    req += "\r\n";

    auto& socket = std::get<SocketPtr>(stream_);
    co_await asio::async_write(*socket, asio::buffer(req), asio::use_awaitable);

    asio::streambuf response_buf;
    co_await asio::async_read_until(*socket, response_buf, "\r\n\r\n", asio::use_awaitable);

    std::istream response_stream(&response_buf);
    std::string http_version;
    response_stream >> http_version;
    unsigned int status_code;
    response_stream >> status_code;
    if (status_code != 200) {
        throw std::system_error(std::make_error_code(std::errc::connection_refused));
    }
}

asio::awaitable<void> WebSocketClient::AsyncSslHandshake(std::string host) {
    auto& ssl_stream = std::get<SslStreamPtr>(stream_);
    SSL_set_tlsext_host_name(ssl_stream->native_handle(), host.c_str());
    
    // Authenticate the endpoint before sending the upgrade request and token.
    // SNI selects a certificate; it does not verify the chain or host identity.
    ssl_stream->set_verify_mode(asio::ssl::verify_peer);
    ssl_stream->set_verify_callback(asio::ssl::host_name_verification(host));

    co_await ssl_stream->async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
}

asio::awaitable<void> WebSocketClient::AsyncWsHandshake(Url url, std::string token, HeaderList extra_headers) {
    std::string path_query = url.path;
    if (!url.query.empty()) {
        path_query += "?" + url.query;
    }
    std::string ws_key = GenerateWebSocketKey();
    const std::string host_hdr = FormatUrlAuthority(url);

    bool has_auth_header = false;
    for (const auto& [name, value] : extra_headers) {
        (void)value;
        if (IsAuthorizationHeaderName(name)) {
            has_auth_header = true;
            break;
        }
    }

    std::string req = "GET " + path_query + " HTTP/1.1\r\n"
                      "Host: " + host_hdr + "\r\n"
                      "User-Agent: livekit-sdk-cpp/0.1.0\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + ws_key + "\r\n"
                      "Sec-WebSocket-Version: 13\r\n";
    if (!token.empty() && !has_auth_header && url.query.find("access_token=") == std::string::npos) {
        req += "Authorization: Bearer " + token + "\r\n";
    }
    for (const auto& [name, value] : extra_headers) {
        req += name + ": " + value + "\r\n";
    }
    req += "\r\n";

    co_await async_write_stream(asio::buffer(req));
    if (std::holds_alternative<SocketPtr>(stream_)) {
        co_await asio::async_read_until(*std::get<SocketPtr>(stream_), response_buf_, "\r\n\r\n", asio::use_awaitable);
    } else if (std::holds_alternative<SslStreamPtr>(stream_)) {
        co_await asio::async_read_until(*std::get<SslStreamPtr>(stream_), response_buf_, "\r\n\r\n", asio::use_awaitable);
    } else {
        throw std::system_error(asio::error::not_connected);
    }

    std::istream response_stream(&response_buf_);
    std::string http_version;
    response_stream >> http_version;
    unsigned int status_code;
    response_stream >> status_code;
    if (status_code != 101) {
        std::string status_msg;
        std::getline(response_stream, status_msg);
        std::string api_status_code;
        std::string api_message;
        std::string dg_error;
        std::string tt_logid;
        std::string request_id;
        std::string trace_id;
        bool has_content_type = false;
        bool is_json_content_type = false;
        bool has_content_length = false;
        size_t content_length = 0;
        bool is_chunked = false;
        bool connection_close = false;
        std::string line;
        while (std::getline(response_stream, line) && line != "\r" && !line.empty()) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0) {
                continue;
            }
            const std::string name = LowerAsciiCopy(TrimAndCollapseHeaderValue(
                std::string_view(line).substr(0, colon), 128));
            const std::string_view raw_val = std::string_view(line).substr(colon + 1);
            if (name == "x-api-status-code") {
                api_status_code = TrimAndCollapseHeaderValue(raw_val);
            } else if (name == "x-api-message") {
                api_message = TrimAndCollapseHeaderValue(raw_val);
            } else if (name == "dg-error") {
                dg_error = TrimAndCollapseHeaderValue(raw_val);
            } else if (name == "x-tt-logid") {
                tt_logid = TrimAndCollapseHeaderValue(raw_val);
            } else if (name == "x-request-id") {
                request_id = TrimAndCollapseHeaderValue(raw_val);
            } else if (name == "x-trace-id") {
                trace_id = livekit::secure_log::SanitizeForOutput(
                    TrimAndCollapseHeaderValue(raw_val));
            } else if (name == "content-type") {
                const std::string ct = LowerAsciiCopy(TrimAndCollapseHeaderValue(raw_val));
                if (!ct.empty()) {
                    has_content_type = true;
                    is_json_content_type = (ct.find("application/json") != std::string::npos ||
                                            ct.find("+json") != std::string::npos);
                }
            } else if (name == "content-length") {
                const std::string len_str = TrimAndCollapseHeaderValue(raw_val, 32);
                size_t parsed = 0;
                const char* begin = len_str.data();
                const char* end = begin + len_str.size();
                const auto [ptr, ec] = std::from_chars(begin, end, parsed);
                if (ec == std::errc{} && ptr == end) {
                    has_content_length = true;
                    content_length = parsed;
                }
            } else if (name == "transfer-encoding") {
                if (LowerAsciiCopy(TrimAndCollapseHeaderValue(raw_val)).find("chunked") != std::string::npos) {
                    is_chunked = true;
                }
            } else if (name == "connection") {
                if (LowerAsciiCopy(TrimAndCollapseHeaderValue(raw_val)).find("close") != std::string::npos) {
                    connection_close = true;
                }
            }
        }

        const size_t max_raw_chunked = kMaxHandshakeErrorBodyBytes * 2;
        std::string raw_tail;
        if (response_buf_.size() > 0) {
            const size_t avail = response_buf_.size();
            const size_t to_copy = std::min(avail, max_raw_chunked);
            raw_tail.resize(to_copy);
            asio::buffer_copy(asio::buffer(raw_tail), response_buf_.data());
            response_buf_.consume(avail);
        }

        std::string decoded_body;
        const bool should_read_body = is_json_content_type || !has_content_type;
        if (should_read_body) {
            bool chunked_complete = false;
            auto ends_with_brace = [](std::string_view sv) -> bool {
                while (!sv.empty() &&
                       (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
                    sv.remove_suffix(1);
                }
                return !sv.empty() && sv.back() == '}';
            };
            auto is_body_done = [&]() -> bool {
                if (is_chunked) {
                    DecodeChunkedBody(raw_tail, kMaxHandshakeErrorBodyBytes, decoded_body, chunked_complete);
                    return chunked_complete || raw_tail.size() >= max_raw_chunked;
                }
                if (has_content_length) {
                    const size_t target_len = std::min(content_length, kMaxHandshakeErrorBodyBytes);
                    return raw_tail.size() >= target_len;
                }
                if (is_json_content_type && connection_close) {
                    return raw_tail.size() >= kMaxHandshakeErrorBodyBytes || ends_with_brace(raw_tail);
                }
                return true;
            };

            if (!is_body_done()) {
                auto executor = co_await asio::this_coro::executor;
                auto body_timer = std::make_shared<asio::steady_timer>(executor);
                auto body_done = std::make_shared<std::atomic<bool>>(false);
                std::weak_ptr<WebSocketClient> weak_self = weak_from_this();
                body_timer->expires_after(kHandshakeErrorBodyReadTimeout);
                body_timer->async_wait([weak_self, body_done](const std::error_code& ec) {
                    if (!ec && !body_done->load(std::memory_order_acquire)) {
                        if (auto locked = weak_self.lock()) {
                            locked->shutdown_stream();
                        }
                    }
                });
                try {
                    const size_t max_raw_read = is_chunked
                        ? max_raw_chunked
                        : (has_content_length
                            ? std::min(content_length, kMaxHandshakeErrorBodyBytes)
                            : kMaxHandshakeErrorBodyBytes);
                    std::array<char, 1024> tmp{};
                    while (!is_body_done() && raw_tail.size() < max_raw_read) {
                        const size_t room = max_raw_read - raw_tail.size();
                        size_t n = 0;
                        if (std::holds_alternative<SocketPtr>(stream_)) {
                            n = co_await std::get<SocketPtr>(stream_)->async_read_some(
                                asio::buffer(tmp.data(), std::min(tmp.size(), room)),
                                asio::use_awaitable);
                        } else if (std::holds_alternative<SslStreamPtr>(stream_)) {
                            n = co_await std::get<SslStreamPtr>(stream_)->async_read_some(
                                asio::buffer(tmp.data(), std::min(tmp.size(), room)),
                                asio::use_awaitable);
                        } else {
                            break;
                        }
                        if (n == 0) {
                            break;
                        }
                        raw_tail.append(tmp.data(), n);
                    }
                } catch (...) {
                    // Keep any body bytes already captured; never override HTTP status error.
                }
                body_done->store(true, std::memory_order_release);
                body_timer->cancel();
                (void)is_body_done();
            }

            if (!is_chunked) {
                const size_t limit = has_content_length
                    ? std::min(content_length, kMaxHandshakeErrorBodyBytes)
                    : kMaxHandshakeErrorBodyBytes;
                decoded_body = raw_tail.substr(0, std::min(raw_tail.size(), limit));
            }
        }

        HandshakeJsonError json_err;
        bool try_parse_json = is_json_content_type;
        if (!try_parse_json && !has_content_type && !decoded_body.empty()) {
            size_t first_non_ws = 0;
            while (first_non_ws < decoded_body.size() &&
                   (decoded_body[first_non_ws] == ' ' ||
                    decoded_body[first_non_ws] == '\t' ||
                    decoded_body[first_non_ws] == '\r' ||
                    decoded_body[first_non_ws] == '\n')) {
                ++first_non_ws;
            }
            try_parse_json = (first_non_ws < decoded_body.size() && decoded_body[first_non_ws] == '{');
        }
        if (try_parse_json && !decoded_body.empty()) {
            json_err = ExtractJsonHandshakeError(decoded_body);
        }

        {
            std::lock_guard<std::mutex> lock(handshake_diag_mutex_);
            last_handshake_error_detail_ = FormatHandshakeDiagnosticDetail(
                api_status_code,
                api_message,
                dg_error,
                tt_logid,
                request_id,
                trace_id,
                json_err.code,
                json_err.message);
        }

        if (response_buf_.size() > 0) {
            response_buf_.consume(response_buf_.size());
        }

        throw std::system_error(MakeWebSocketHttpError(status_code));
    }
    
    std::string header;
    bool accept_verified = false;
    
    std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string key_magic = ws_key + magic;
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(key_magic.data()), key_magic.size(), hash);
    std::string expected_accept = Base64Encode(hash, SHA_DIGEST_LENGTH);
    
    while (std::getline(response_stream, header) && header != "\r") {
        if (!header.empty() && header.back() == '\r') {
            header.pop_back();
        }
        std::string lower_header = header;
        std::transform(lower_header.begin(), lower_header.end(), lower_header.begin(), ::tolower);
        if (lower_header.rfind("sec-websocket-accept:", 0) == 0) {
            size_t colon = header.find(':');
            if (colon != std::string::npos) {
                std::string val = header.substr(colon + 1);
                val.erase(0, val.find_first_not_of(" \t"));
                val.erase(val.find_last_not_of(" \t") + 1);
                if (val == expected_accept) {
                    accept_verified = true;
                }
            }
        }
    }
    
    if (!accept_verified) {
        throw std::system_error(std::make_error_code(std::errc::connection_refused));
    }
}

asio::awaitable<size_t> WebSocketClient::async_read_stream(asio::mutable_buffer buf) {
    if (response_buf_.size() > 0) {
        size_t bytes_to_copy = std::min(response_buf_.size(), buf.size());
        std::memcpy(buf.data(), asio::buffer_cast<const void*>(response_buf_.data()), bytes_to_copy);
        response_buf_.consume(bytes_to_copy);
        
        if (bytes_to_copy < buf.size()) {
            asio::mutable_buffer remaining_buf = buf + bytes_to_copy;
            size_t extra_bytes = co_await async_read_stream(remaining_buf);
            co_return bytes_to_copy + extra_bytes;
        }
        co_return bytes_to_copy;
    }

    size_t read_bytes = 0;
    if (std::holds_alternative<SocketPtr>(stream_)) {
        read_bytes = co_await asio::async_read(*std::get<SocketPtr>(stream_), buf, asio::use_awaitable);
    } else if (std::holds_alternative<SslStreamPtr>(stream_)) {
        read_bytes = co_await asio::async_read(*std::get<SslStreamPtr>(stream_), buf, asio::use_awaitable);
    } else {
        throw std::system_error(asio::error::not_connected);
    }
    co_return read_bytes;
}

asio::awaitable<size_t> WebSocketClient::async_write_stream(asio::const_buffer buf) {
    if (std::holds_alternative<SocketPtr>(stream_)) {
        co_return co_await asio::async_write(*std::get<SocketPtr>(stream_), buf, asio::use_awaitable);
    } else if (std::holds_alternative<SslStreamPtr>(stream_)) {
        co_return co_await asio::async_write(*std::get<SslStreamPtr>(stream_), buf, asio::use_awaitable);
    } else {
        throw std::system_error(asio::error::not_connected);
    }
}

void WebSocketClient::shutdown_stream() {
    if (before_shutdown_observer_) {
        before_shutdown_observer_(strand_.running_in_this_thread());
    }
    connected_.store(false, std::memory_order_release);
    if (strand_.running_in_this_thread()) {
        fragment_opcode_ = 0;
        fragment_buffer_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        std::queue<std::shared_ptr<QueuedMessage>> empty;
        write_queue_.swap(empty);
        queued_bytes_.store(0, std::memory_order_release);
    }
    std::visit([](auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, SocketPtr>) {
            std::error_code ec;
            s->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            s->close(ec);
        } else if constexpr (std::is_same_v<T, SslStreamPtr>) {
            std::error_code ec;
            s->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            s->lowest_layer().close(ec);
        }
    }, stream_);
}

void WebSocketClient::handle_error(const std::error_code& ec) {
    if (ec == asio::error::operation_aborted) return;
    if (closed_by_us_) return;
    shutdown_stream();
    if (error_cb_) {
        error_cb_(ec);
    }
}

std::string WebSocketClient::LastHandshakeErrorDetail() const {
    std::lock_guard<std::mutex> lock(handshake_diag_mutex_);
    return last_handshake_error_detail_;
}

} // namespace livekit
