#pragma once

#if defined(_WIN32)
#include <Windows.h>
#include <DbgHelp.h>

#include <cstdint>
#include <limits>

namespace livekit::telemetry::detail {

inline constexpr std::uint64_t kMaximumCrashDumpBytes = 64ull * 1024 * 1024;

// DbgHelp writes at arbitrary offsets, including header rewrites. Bound the
// resulting extent before each write; summing callback sizes is insufficient.
// This object owns no handle and allocates no memory in the crash path.
class CrashDumpWriter final {
public:
    explicit CrashDumpWriter(HANDLE file, std::uint64_t maximum = kMaximumCrashDumpBytes) noexcept
        : file_(file), maximum_(maximum) {
        if (!file || file == INVALID_HANDLE_VALUE || !maximum ||
            maximum > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()))
            error_ = ERROR_INVALID_PARAMETER;
    }
    CrashDumpWriter(const CrashDumpWriter&) = delete;
    CrashDumpWriter& operator=(const CrashDumpWriter&) = delete;

    MINIDUMP_CALLBACK_INFORMATION callbacks() noexcept { return {&Callback, this}; }
    bool complete() const noexcept { return started_ && finished_ && error_ == ERROR_SUCCESS; }
    DWORD error() const noexcept { return error_; }

private:
    static BOOL CALLBACK Callback(PVOID parameter, const PMINIDUMP_CALLBACK_INPUT input,
                                  PMINIDUMP_CALLBACK_OUTPUT output) noexcept {
        if (!parameter || !input || !output) return FALSE;
        auto& self = *static_cast<CrashDumpWriter*>(parameter);
        // Serialize offset/seek/write if DbgHelp invokes I/O from another thread.
        struct Guard {
            SRWLOCK& lock;
            explicit Guard(SRWLOCK& value) noexcept : lock(value) { AcquireSRWLockExclusive(&lock); }
            ~Guard() { ReleaseSRWLockExclusive(&lock); }
        } guard(self.lock_);
        switch (input->CallbackType) {
        case IoStartCallback:
            if (self.started_ || self.finished_) self.fail(ERROR_INVALID_DATA);
            self.started_ = true;
            // S_FALSE explicitly selects callback I/O instead of direct file I/O.
            output->Status = self.error_ ? HRESULT_FROM_WIN32(self.error_) : S_FALSE;
            return TRUE;
        case IoWriteAllCallback:
            self.write(input->Io.Offset, input->Io.Buffer, input->Io.BufferBytes);
            output->Status = self.error_ ? HRESULT_FROM_WIN32(self.error_) : S_OK;
            return TRUE;
        case IoFinishCallback:
            if (!self.started_ || self.finished_) self.fail(ERROR_INVALID_DATA);
            if (!self.error_ && !FlushFileBuffers(self.file_)) self.fail(GetLastError());
            self.finished_ = true;
            output->Status = self.error_ ? HRESULT_FROM_WIN32(self.error_) : S_OK;
            return TRUE;
        default:
            return TRUE;
        }
    }

    void fail(DWORD error) noexcept {
        if (!error_) error_ = error ? error : ERROR_WRITE_FAULT;
    }

    void write(ULONG64 offset, const void* buffer, ULONG bytes) noexcept {
        if (error_) return;
        if (!started_ || finished_ || (!buffer && bytes)) { fail(ERROR_INVALID_DATA); return; }
        if (offset > maximum_ || bytes > maximum_ - offset) { fail(ERROR_FILE_TOO_LARGE); return; }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file_, position, nullptr, FILE_BEGIN)) { fail(GetLastError()); return; }
        auto* data = static_cast<const unsigned char*>(buffer);
        while (bytes) {
            DWORD written = 0;
            if (!WriteFile(file_, data, bytes, &written, nullptr)) { fail(GetLastError()); return; }
            if (!written || written > bytes) { fail(ERROR_WRITE_FAULT); return; }
            data += written;
            bytes -= written;
        }
    }

    HANDLE file_;
    std::uint64_t maximum_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    DWORD error_ = ERROR_SUCCESS;
    bool started_ = false, finished_ = false;
};

} // namespace livekit::telemetry::detail
#endif
