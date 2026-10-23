#pragma once

// Opt-in evidence bytes only. This is not a physical-volume capacity grant.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace product_probe_capacity {
inline constexpr auto legacy_contract = "b14_native_probe_evidence_capacity_v1";
inline constexpr auto contract = "b14_native_probe_evidence_capacity_v2";
inline constexpr std::uint64_t reserve = 65536;
inline constexpr std::uint64_t legacy_row_limit = 131072;
// Keep all per-track metrics and raw nodes. This is an enforced write ceiling,
// not a predicted Formal size; three complete rows still fit the 1 MiB tail.
inline constexpr std::uint64_t row_limit = 262144;
inline constexpr std::uint64_t page_limit = 4194304;

class Writer final {
public:
    Writer(const std::filesystem::path& path, std::uint64_t maximum, std::string run,
           std::string version = contract)
        : root_(path.parent_path()), maximum_(maximum), run_(std::move(run)), contract_(std::move(version)) {
        if (contract_ != contract && contract_ != legacy_contract) fail(ERROR_INVALID_PARAMETER);
        row_limit_ = contract_ == legacy_contract ? legacy_row_limit : row_limit;
        if (maximum <= 2 * reserve || maximum > 8ull * 1024 * 1024 * 1024
            || !path.is_absolute() || path.filename() != L"process-probe.jsonl"
            || run_.size() != 32 || run_.find_first_not_of("0123456789abcdef") != std::string::npos)
            fail(ERROR_INVALID_PARAMETER);
        const auto name = path.native();
        if (name.size() < 3 || name[1] != L':' || name.substr(2).find(L':') != std::wstring::npos
            || name.rfind(L"\\\\", 0) == 0) fail(ERROR_INVALID_NAME);
        for (const auto& part : path) if (part == L"..") fail(ERROR_INVALID_NAME);
        for (auto p = root_; !p.empty();) {
            const auto attr = GetFileAttributesW(p.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) fail(GetLastError());
            if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
                fail(ERROR_REPARSE_TAG_INVALID);
            const auto next = p.parent_path();
            if (next == p) break;
            p = next;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) fail(GetLastError());
        birth_ = ((static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime)
            + 504911232000000000ull; // FILETIME -> .NET UTC ticks, no UTC/QPC inference.
        for (const auto* other : {L"heap-page-journal.json", L"process-probe-capacity.json"}) {
            const auto attr = GetFileAttributesW((root_ / other).c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) fail(ERROR_FILE_EXISTS);
            if (GetLastError() != ERROR_FILE_NOT_FOUND) fail(GetLastError());
        }
        stream_ = create(path);
    }
    ~Writer() {
        for (auto file : {stream_, pages_, terminal_}) if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    std::uint64_t birth() const { return birth_; }
    std::uint64_t maximum() const { return maximum_; }
    std::uint64_t stream_bytes() const { return stream_bytes_; }
    std::uint64_t rows() const { return rows_; }
    const std::string& version() const { return contract_; }
    void append(std::string_view row) {
        if (ended_) fail(ERROR_INVALID_STATE);
        check_line(row, row_limit_);
        budget(row.size());
        write(stream_, row, stream_bytes_);
        ++rows_;
        if (!FlushFileBuffers(stream_)) fail(GetLastError());
    }
    void pages(std::string_view content) {
        if (ended_ || pages_ != INVALID_HANDLE_VALUE) fail(ERROR_INVALID_STATE);
        check_line(content, page_limit);
        budget(content.size());
        pages_ = create(root_ / L"heap-page-journal.json");
        write(pages_, content, page_bytes_);
        if (!FlushFileBuffers(pages_)) fail(GetLastError());
    }
    void finish(bool complete, DWORD error = 0) {
        if (ended_) fail(ERROR_INVALID_STATE);
        // A later COMPLETE file alone cannot hide failed data or terminal flush:
        // the product's joined Finish() also propagates failure as exit code 5.
        if (!FlushFileBuffers(stream_)) fail(GetLastError());
        if (pages_ != INVALID_HANDLE_VALUE && !FlushFileBuffers(pages_)) fail(GetLastError());
        std::uint64_t terminal_size = 0;
        std::string row;
        for (int attempt = 0; attempt < 16; ++attempt) {
            std::ostringstream out;
            out << "{\"schema\":\"" << contract_ << "\",\"status\":\"" << (complete ? "COMPLETE" : "FAILED")
                << "\",\"error_win32\":" << error << ",\"run_id\":\"" << run_ << "\",\"pid\":" << GetCurrentProcessId()
                << ",\"process_start_ticks\":" << birth_ << ",\"stream_schema\":2,\"maximum_bytes\":" << maximum_
                << ",\"terminal_reserve_bytes\":" << reserve << ",\"maximum_row_bytes\":" << row_limit_
                << ",\"maximum_page_bytes\":" << page_limit << ",\"stream_bytes\":" << stream_bytes_
                << ",\"stream_rows\":" << rows_ << ",\"heap_bytes\":" << page_bytes_
                << ",\"terminal_bytes\":" << terminal_size << ",\"final_file_bytes\":"
                << stream_bytes_ + page_bytes_ + terminal_size << "}\n";
            row = out.str();
            if (row.size() == terminal_size) break;
            terminal_size = row.size();
        }
        if (row.size() != terminal_size || row.size() > reserve
            || stream_bytes_ + page_bytes_ + row.size() > maximum_) fail(ERROR_FILE_TOO_LARGE);
        terminal_ = create(root_ / L"process-probe-capacity.json");
        std::uint64_t written = 0;
        write(terminal_, row, written);
        if (!FlushFileBuffers(terminal_)) fail(GetLastError());
        ended_ = true;
    }
private:
    [[noreturn]] static void fail(DWORD error) {
        throw std::system_error(static_cast<int>(error ? error : ERROR_WRITE_FAULT), std::system_category());
    }
    static HANDLE create(const std::filesystem::path& path) {
        const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) fail(GetLastError());
        return file;
    }
    static void check_line(std::string_view row, std::uint64_t limit) {
        if (row.empty() || row.back() != '\n' || row.find('\n') != row.size() - 1
            || row.find('\r') != std::string_view::npos) fail(ERROR_INVALID_DATA);
        if (row.size() > limit) fail(ERROR_FILE_TOO_LARGE);
    }
    void budget(std::uint64_t bytes) const {
        if (stream_bytes_ + page_bytes_ + bytes + reserve > maximum_) fail(ERROR_FILE_TOO_LARGE);
    }
    static void write(HANDLE file, std::string_view row, std::uint64_t& total) {
        LARGE_INTEGER actual{}, position{}, zero{};
        if (!GetFileSizeEx(file, &actual) || !SetFilePointerEx(file, zero, &position, FILE_CURRENT)) fail(GetLastError());
        if (static_cast<std::uint64_t>(actual.QuadPart) != total
            || static_cast<std::uint64_t>(position.QuadPart) != total) fail(ERROR_FILE_INVALID);
        DWORD written = 0;
        const bool ok = WriteFile(file, row.data(), static_cast<DWORD>(row.size()), &written, nullptr) != FALSE;
        const auto error = ok ? ERROR_SUCCESS : GetLastError();
        total += written;
        if (!ok) fail(error);
        if (written != row.size()) fail(ERROR_WRITE_FAULT);
    }
    std::filesystem::path root_;
    std::uint64_t maximum_ = 0, birth_ = 0, stream_bytes_ = 0, page_bytes_ = 0, rows_ = 0;
    std::string run_;
    std::string contract_;
    std::uint64_t row_limit_ = 0;
    HANDLE stream_ = INVALID_HANDLE_VALUE, pages_ = INVALID_HANDLE_VALUE, terminal_ = INVALID_HANDLE_VALUE;
    bool ended_ = false;
};
} // namespace product_probe_capacity
