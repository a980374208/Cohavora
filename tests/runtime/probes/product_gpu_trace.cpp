#define NOMINMAX
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr GUID kProvider{0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}};
constexpr std::array<USHORT, 17> kIds{27,28,29,30,31,32,175,176,177,178,179,180,244,245,361,450,451};
constexpr ULONGLONG kKeywords = 0x4000000000000841ull;
struct Properties {
    EVENT_TRACE_PROPERTIES value{};
    wchar_t name[512]{};
    wchar_t file[4096]{};
    Properties() {
        value.Wnode.BufferSize = sizeof(*this);
        value.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        value.Wnode.ClientContext = 1;
        value.BufferSize = 64;
        value.MinimumBuffers = 16;
        value.MaximumBuffers = 64;
        value.MaximumFileSize = 128;
        value.FlushTimer = 1;
        value.LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL;
        value.LoggerNameOffset = offsetof(Properties, name);
        value.LogFileNameOffset = offsetof(Properties, file);
    }
};
bool Save(const wchar_t* path, const std::string& content) {
    if (std::filesystem::exists(path)) return false;
    std::ofstream file(std::filesystem::path(path), std::ios::binary);
    file << content << '\n';
    file.flush();
    return file.good();
}
bool OwnSession(std::wstring_view name) {
    return name.starts_with(L"B14-Gpu-Release-") && name.size() < 512;
}
int Start(const wchar_t* name, const wchar_t* file, const wchar_t* result) {
    if (!OwnSession(name) || std::wcslen(file) >= 4096 || std::filesystem::exists(file) || std::filesystem::exists(result)) return 2;
    Properties props;
    wcscpy_s(props.name, name);
    wcscpy_s(props.file, file);
    TRACEHANDLE handle = 0;
    const auto started = StartTraceW(&handle, name, &props.value);
    ULONG enabled = ERROR_NOT_READY, cleanup = ERROR_SUCCESS;
    if (started == ERROR_SUCCESS) {
        std::vector<BYTE> bytes(offsetof(EVENT_FILTER_EVENT_ID, Events) + sizeof(USHORT) * kIds.size());
        auto* ids = reinterpret_cast<EVENT_FILTER_EVENT_ID*>(bytes.data());
        ids->FilterIn = TRUE;
        ids->Count = static_cast<USHORT>(kIds.size());
        std::copy(kIds.begin(), kIds.end(), ids->Events);
        EVENT_FILTER_DESCRIPTOR filter{};
        filter.Ptr = reinterpret_cast<ULONGLONG>(ids);
        filter.Size = static_cast<ULONG>(bytes.size());
        filter.Type = EVENT_FILTER_TYPE_EVENT_ID;
        ENABLE_TRACE_PARAMETERS parameters{};
        parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
        parameters.EnableFilterDesc = &filter;
        parameters.FilterDescCount = 1;
        enabled = EnableTraceEx2(handle, &kProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
            TRACE_LEVEL_VERBOSE, kKeywords, 0, 1000, &parameters);
        if (enabled != ERROR_SUCCESS) cleanup = ControlTraceW(handle, name, &props.value, EVENT_TRACE_CONTROL_STOP);
    }
    std::ostringstream out;
    out << "{\"start_error\":" << started << ",\"enable_error\":" << enabled << ",\"cleanup_error\":" << cleanup
        << ",\"event_id_filter\":[";
    for (std::size_t i = 0; i < kIds.size(); ++i) out << (i ? "," : "") << kIds[i];
    out << "],\"maximum_file_mib\":128,\"buffer_kib\":64,\"minimum_buffers\":16,\"maximum_buffers\":64}";
    if (!Save(result, out.str())) {
        if (started == ERROR_SUCCESS && enabled == ERROR_SUCCESS) ControlTraceW(handle, name, &props.value, EVENT_TRACE_CONTROL_STOP);
        return 1;
    }
    return started == ERROR_SUCCESS && enabled == ERROR_SUCCESS ? 0 : 1;
}
int Stop(const wchar_t* name, const wchar_t* result) {
    if (!OwnSession(name) || std::filesystem::exists(result)) return 2;
    Properties props;
    const auto queried = ControlTraceW(0, name, &props.value, EVENT_TRACE_CONTROL_QUERY);
    const auto stopped = queried == ERROR_SUCCESS ? ControlTraceW(0, name, &props.value, EVENT_TRACE_CONTROL_STOP) : ERROR_NOT_READY;
    std::ostringstream out;
    out << "{\"query_error\":" << queried << ",\"stop_error\":" << stopped
        << ",\"events_lost\":" << props.value.EventsLost << ",\"log_buffers_lost\":" << props.value.LogBuffersLost
        << ",\"realtime_buffers_lost\":" << props.value.RealTimeBuffersLost << ",\"buffers_written\":" << props.value.BuffersWritten << '}';
    return Save(result, out.str()) && queried == ERROR_SUCCESS && stopped == ERROR_SUCCESS ? 0 : 1;
}
constexpr const wchar_t* kFields[]{L"hProcessId",L"pDxgAdapter",L"hDevice",L"hContext",L"NodeOrdinal",L"EngineAffinity",
    L"SubmitSequence",L"ulQueueSubmitSequence",L"PacketType",L"bPreempted",L"bTimeouted",L"pQueuePacket",
    L"uliCompletionId",L"InterruptType",L"FaultedVirtualAddress",L"PageFaultFlags",L"FaultedProcessHandle",L"Status",L"Reason"};
struct Reader {
    std::ofstream output;
    std::uint64_t events = 0, filtered = 0, metadata_errors = 0, property_errors = 0;
    bool write_failed = false;
    bool live = false;
    std::atomic<DWORD> target_pid{0};
    std::atomic<std::uint64_t> seen{0}, kept{0}, written_bytes{0}, foreign{0}, prebind{0};
    std::atomic<bool> failed{false};
    std::uint64_t maximum_bytes = 0;
    std::map<std::uint64_t,std::uint64_t> device_owners, context_owners;
    std::map<std::uint64_t,std::uint64_t> retired_contexts;
    std::map<std::pair<USHORT,UCHAR>, std::vector<BYTE>> schemas;
};
bool Retain(Reader& reader, USHORT id, const std::map<std::wstring,std::uint64_t>& fields, DWORD target) {
    auto get = [&](const wchar_t* key) {auto found=fields.find(key); return found == fields.end() ? 0 : found->second;};
    // Keep every device/context lifecycle, including foreign pointer reuse.
    // Before PID binding every packet is persisted, so early product births
    // and work are never discarded by a late identity-file publication.
    if (id >= 27 && id <= 29) {
        if (!get(L"hDevice") || !get(L"hProcessId")) return true;
        if (id != 28) reader.device_owners[get(L"hDevice")] = get(L"hProcessId");
        else reader.device_owners.erase(get(L"hDevice"));
        return true;
    }
    if (id >= 30 && id <= 32) {
        const auto context=get(L"hContext");
        if (!context || !get(L"hDevice")) return true;
        if (id != 31) {
            reader.retired_contexts.erase(context);
            const auto owner=reader.device_owners.find(get(L"hDevice"));
            reader.context_owners[context]=owner == reader.device_owners.end() ? 0 : owner->second;
        } else {
            const auto owner=reader.context_owners.find(context);
            if (owner != reader.context_owners.end()) reader.retired_contexts[context]=owner->second;
            reader.context_owners.erase(context);
        }
        return true;
    }
    if (!target || id == 450 || id == 451 || !get(L"hContext")) return true;
    const auto owner=reader.context_owners.find(get(L"hContext"));
    const auto retired=reader.retired_contexts.find(get(L"hContext"));
    return (owner != reader.context_owners.end() && owner->second == target)
        || (retired != reader.retired_contexts.end() && retired->second == target);
}
bool IntegerType(USHORT type) {
    return (type >= TDH_INTYPE_INT8 && type <= TDH_INTYPE_UINT64) || type == TDH_INTYPE_BOOLEAN
        || type == TDH_INTYPE_POINTER || type == TDH_INTYPE_HEXINT32 || type == TDH_INTYPE_HEXINT64;
}
void WINAPI Event(EVENT_RECORD* event) noexcept {
    auto& reader = *static_cast<Reader*>(event->UserContext);
    if (!IsEqualGUID(event->EventHeader.ProviderId, kProvider)) return;
    ++reader.events;
    reader.seen.store(reader.events);
    if (std::find(kIds.begin(), kIds.end(), event->EventHeader.EventDescriptor.Id) == kIds.end()) {++reader.filtered; return;}
    try {
        const auto& descriptor = event->EventHeader.EventDescriptor;
        auto& bytes = reader.schemas[{descriptor.Id,descriptor.Version}];
        ULONG metadata_status = ERROR_SUCCESS;
        if (bytes.empty()) {
            ULONG size = 0;
            metadata_status = TdhGetEventInformation(event, 0, nullptr, nullptr, &size);
            if (metadata_status == ERROR_INSUFFICIENT_BUFFER) {
                bytes.resize(size);
                metadata_status = TdhGetEventInformation(event, 0, nullptr, reinterpret_cast<TRACE_EVENT_INFO*>(bytes.data()), &size);
            }
            if (metadata_status != ERROR_SUCCESS) {bytes.clear(); ++reader.metadata_errors;}
        }
        const auto filetime = event->EventHeader.TimeStamp.QuadPart;
        std::ostringstream line;
        line << "{\"sequence\":" << reader.kept.load()+1 << ",\"filetime_100ns\":" << filetime
             << ",\"utc_ms\":" << filetime/10000-11644473600000ll << ",\"header_pid\":" << event->EventHeader.ProcessId
             << ",\"event_id\":" << descriptor.Id << ",\"version\":" << unsigned(descriptor.Version)
             << ",\"task\":" << descriptor.Task << ",\"opcode\":" << unsigned(descriptor.Opcode)
             << ",\"metadata_error\":" << metadata_status << ",\"fields\":{";
        bool first = true;
        std::map<std::wstring,std::uint64_t> fields;
        const auto errors_before = reader.property_errors;
        if (!bytes.empty()) {
            const auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(bytes.data());
            for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
                const auto& property = info->EventPropertyInfoArray[i];
                const auto* name = reinterpret_cast<const wchar_t*>(bytes.data()+property.NameOffset);
                const auto found = std::find_if(std::begin(kFields),std::end(kFields),[&](const wchar_t* wanted){return std::wcscmp(name,wanted)==0;});
                if (found == std::end(kFields)) continue;
                if (!first) line << ',';
                first = false;
                line << '"';
                for (const auto character : std::wstring_view(*found)) line << static_cast<char>(character);
                line << "\":";
                PROPERTY_DATA_DESCRIPTOR data{};
                data.PropertyName = reinterpret_cast<ULONGLONG>(name);
                data.ArrayIndex = ULONG_MAX;
                ULONG size = 0;
                auto status = TdhGetPropertySize(event,0,nullptr,1,&data,&size);
                std::uint64_t value = 0;
                if (status == ERROR_SUCCESS && (size == 1 || size == 2 || size == 4 || size == 8)
                    && property.count == 1 && !(property.Flags & PropertyStruct) && IntegerType(property.nonStructType.InType))
                    status = TdhGetProperty(event,0,nullptr,1,&data,size,reinterpret_cast<BYTE*>(&value));
                else status = ERROR_INVALID_DATA;
                if (status == ERROR_SUCCESS) {line << value; fields.emplace(*found,value);}
                else {line << "null"; ++reader.property_errors;}
            }
        }
        line << "}}\n";
        const auto target=reader.target_pid.load();
        bool retain=true;
        if (reader.live) {
            retain=Retain(reader,descriptor.Id,fields,target);
            if (!target) ++reader.prebind;
            if (reader.device_owners.size()+reader.context_owners.size()+reader.retired_contexts.size()>65536) reader.failed=true;
        }
        if (metadata_status || reader.property_errors != errors_before) {reader.failed=true; retain=true;}
        if (!retain) {++reader.foreign; return;}
        const auto text=line.str();
        if (reader.live && reader.written_bytes.load()+text.size()>reader.maximum_bytes) {reader.failed=true; reader.write_failed=true; return;}
        reader.output << text;
        ++reader.kept;
        reader.written_bytes.fetch_add(text.size());
        if (!reader.output) {reader.write_failed = true; reader.failed=true;}
    } catch (...) {reader.write_failed=true; reader.failed=true;}
}
int Read(const wchar_t* input, const wchar_t* output, const wchar_t* summary) {
    if (std::filesystem::exists(output) || std::filesystem::exists(summary)) return 2;
    Reader reader;
    reader.output.open(std::filesystem::path(output),std::ios::binary);
    if (!reader.output) return 1;
    EVENT_TRACE_LOGFILEW log{};
    log.LogFileName = const_cast<wchar_t*>(input);
    // ProcessTrace converts timestamps to FILETIME when RAW_TIMESTAMP is absent.
    log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = Event;
    log.Context = &reader;
    TRACEHANDLE handle = OpenTraceW(&log);
    const auto opened = handle == INVALID_PROCESSTRACE_HANDLE ? GetLastError() : ERROR_SUCCESS;
    ULONG processed = ERROR_NOT_READY, closed = ERROR_NOT_READY;
    if (opened == ERROR_SUCCESS) {
        processed = ProcessTrace(&handle,1,nullptr,nullptr);
        closed = CloseTrace(handle);
    }
    reader.output.flush();
    reader.write_failed |= !reader.output.good();
    std::ostringstream result;
    result << "{\"open_error\":" << opened << ",\"process_error\":" << processed << ",\"close_error\":" << closed
        << ",\"events_seen\":" << reader.events << ",\"events_kept\":" << reader.events-reader.filtered
        << ",\"events_outside_filter\":" << reader.filtered << ",\"metadata_errors\":" << reader.metadata_errors
        << ",\"property_errors\":" << reader.property_errors << ",\"write_failed\":" << (reader.write_failed ? "true" : "false")
        << ",\"events_lost\":" << log.LogfileHeader.EventsLost << ",\"buffers_lost\":" << log.LogfileHeader.BuffersLost
        << ",\"trace_start_filetime_100ns\":" << log.LogfileHeader.StartTime.QuadPart
        << ",\"trace_end_filetime_100ns\":" << log.LogfileHeader.EndTime.QuadPart
        << ",\"timestamp_mode\":\"ProcessTrace normalized FILETIME; RAW_TIMESTAMP disabled\"}";
    return Save(summary,result.str()) && opened == ERROR_SUCCESS && processed == ERROR_SUCCESS && closed == ERROR_SUCCESS
        && !reader.write_failed && reader.metadata_errors == 0 && reader.property_errors == 0 ? 0 : 1;
}
std::uint64_t FiletimeNow() {
    FILETIME now{}; GetSystemTimePreciseAsFileTime(&now);
    return (static_cast<std::uint64_t>(now.dwHighDateTime)<<32)|now.dwLowDateTime;
}
struct PublicationDiagnostics {
    DWORD move_error = ERROR_SUCCESS;
    DWORD source_attributes = 0, target_attributes = 0;
    DWORD source_attributes_error = ERROR_SUCCESS, target_attributes_error = ERROR_SUCCESS;
    DWORD source_delete_error = ERROR_SUCCESS, target_delete_error = ERROR_SUCCESS;
    DWORD parent_access_error = ERROR_SUCCESS;
    std::uint64_t resolved_access_denied_retries = 0;
};
enum class PublicationConflict { Fatal, Sharing, ResolvedAccessDenied };
bool SharingError(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}
bool HealthyPublicationFile(const std::filesystem::path& path, DWORD& attributes, DWORD& error) {
    attributes = GetFileAttributesW(path.c_str());
    error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_REPARSE_POINT));
}
DWORD PublicationAccessError(const std::filesystem::path& path, DWORD access, DWORD flags) {
    const auto probe = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, flags, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(probe);
    return ERROR_SUCCESS;
}
PublicationConflict PublicationSharingConflict(const std::filesystem::path& source,
        const std::filesystem::path& target, DWORD error, PublicationDiagnostics& diagnostic) {
    diagnostic.move_error = error;
    diagnostic.source_delete_error = diagnostic.target_delete_error = diagnostic.parent_access_error = ERROR_NOT_READY;
    if (error != ERROR_ACCESS_DENIED && !SharingError(error)) return PublicationConflict::Fatal;
    const bool source_healthy = HealthyPublicationFile(source, diagnostic.source_attributes, diagnostic.source_attributes_error);
    const bool target_healthy = HealthyPublicationFile(target, diagnostic.target_attributes, diagnostic.target_attributes_error);
    if (!source_healthy || !target_healthy) return PublicationConflict::Fatal;
    // Both the temporary source and the replaced target require DELETE sharing.
    // A probe of only the target cannot distinguish a locked source from an ACL denial.
    diagnostic.source_delete_error = PublicationAccessError(source, DELETE, FILE_ATTRIBUTE_NORMAL);
    diagnostic.target_delete_error = PublicationAccessError(target, DELETE, FILE_ATTRIBUTE_NORMAL);
    for (const auto status : {diagnostic.source_delete_error, diagnostic.target_delete_error})
        if (status != ERROR_SUCCESS && !SharingError(status)) return PublicationConflict::Fatal;
    if (SharingError(diagnostic.source_delete_error) || SharingError(diagnostic.target_delete_error))
        return PublicationConflict::Sharing;
    // The denied move and DELETE probes are separate snapshots. The reader may
    // have closed between them. Retry only with healthy files, DELETE access to
    // both, and permission to add the renamed file in their common parent.
    if (source.parent_path() != target.parent_path()) return PublicationConflict::Fatal;
    diagnostic.parent_access_error = PublicationAccessError(target.parent_path(), FILE_ADD_FILE | FILE_TRAVERSE,
                                                           FILE_FLAG_BACKUP_SEMANTICS);
    if (diagnostic.parent_access_error != ERROR_SUCCESS) return PublicationConflict::Fatal;
    return error == ERROR_ACCESS_DENIED ? PublicationConflict::ResolvedAccessDenied : PublicationConflict::Sharing;
}
bool Replace(const std::filesystem::path& path, const std::string& content,
             DWORD* failure = nullptr, std::uint64_t* sharing_retries = nullptr,
             PublicationDiagnostics* diagnostic = nullptr) {
    if (failure) *failure = ERROR_SUCCESS;
    const std::filesystem::path temporary(path.wstring()+L".tmp");
    {std::ofstream output(temporary,std::ios::binary); output<<content<<'\n'; output.flush();
        if(!output) {if(failure) *failure=ERROR_WRITE_FAULT; return false;}}
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
    PublicationDiagnostics local_diagnostic;
    auto& details = diagnostic ? *diagnostic : local_diagnostic;
    for (;;) {
        if (MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return true;
        const auto error=GetLastError();
        const auto conflict = PublicationSharingConflict(temporary,path,error,details);
        if (conflict == PublicationConflict::Fatal || std::chrono::steady_clock::now()>=deadline) {
            if(failure) *failure=error;
            SetLastError(error);
            return false;
        }
        if (conflict == PublicationConflict::ResolvedAccessDenied) ++details.resolved_access_denied_retries;
        if(sharing_retries) ++*sharing_retries;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
int Live(const wchar_t* name, const wchar_t* directory, std::uint64_t seconds, std::uint64_t maximum_bytes) {
    const std::filesystem::path root(directory);
    if (!OwnSession(name) || seconds<5 || seconds>86400 || maximum_bytes<1048576
        || !std::filesystem::is_directory(root)) return 2;
    for (const auto* file:{L"events.jsonl",L"trace-ready.json",L"trace-summary.json",L"target-pid.txt",L"stop"})
        if (std::filesystem::exists(root/file)) return 2;
    Properties props;
    wcscpy_s(props.name,name);
    props.value.LogFileMode=EVENT_TRACE_REAL_TIME_MODE;
    props.value.MaximumFileSize=0;
    props.value.LogFileNameOffset=0;
    Reader reader; reader.live=true; reader.maximum_bytes=maximum_bytes;
    reader.output.open(root/L"events.jsonl",std::ios::binary);
    if (!reader.output) return 1;
    TRACEHANDLE session=0;
    const auto began=FiletimeNow();
    const auto started=StartTraceW(&session,name,&props.value);
    ULONG opened=ERROR_NOT_READY,enabled=ERROR_NOT_READY,processed=ERROR_NOT_READY,closed=ERROR_NOT_READY;
    ULONG queried=ERROR_NOT_READY,stopped=ERROR_NOT_READY;
    std::uint64_t ready_time=0;
    TRACEHANDLE handle=INVALID_PROCESSTRACE_HANDLE;
    std::thread consumer;
    EVENT_TRACE_LOGFILEW log{};
    if (started==ERROR_SUCCESS) {
        log.LoggerName=const_cast<wchar_t*>(name);
        log.ProcessTraceMode=PROCESS_TRACE_MODE_REAL_TIME|PROCESS_TRACE_MODE_EVENT_RECORD;
        log.EventRecordCallback=Event; log.Context=&reader;
        handle=OpenTraceW(&log);
        opened=handle==INVALID_PROCESSTRACE_HANDLE ? GetLastError() : ERROR_SUCCESS;
        if (opened==ERROR_SUCCESS) {
            consumer=std::thread([&]{processed=ProcessTrace(&handle,1,nullptr,nullptr);});
            std::vector<BYTE> bytes(offsetof(EVENT_FILTER_EVENT_ID,Events)+sizeof(USHORT)*kIds.size());
            auto* ids=reinterpret_cast<EVENT_FILTER_EVENT_ID*>(bytes.data());
            ids->FilterIn=TRUE; ids->Count=static_cast<USHORT>(kIds.size());
            std::copy(kIds.begin(),kIds.end(),ids->Events);
            EVENT_FILTER_DESCRIPTOR filter{reinterpret_cast<ULONGLONG>(ids),static_cast<ULONG>(bytes.size()),EVENT_FILTER_TYPE_EVENT_ID};
            ENABLE_TRACE_PARAMETERS parameters{}; parameters.Version=ENABLE_TRACE_PARAMETERS_VERSION_2;
            parameters.EnableFilterDesc=&filter; parameters.FilterDescCount=1;
            enabled=EnableTraceEx2(session,&kProvider,EVENT_CONTROL_CODE_ENABLE_PROVIDER,TRACE_LEVEL_VERBOSE,kKeywords,0,1000,&parameters);
        }
    }
    std::string reason;
    DWORD heartbeat_error=ERROR_SUCCESS;
    std::uint64_t heartbeat_sharing_retries=0;
    PublicationDiagnostics heartbeat_publication;
    bool stop_requested=false;
    if (enabled==ERROR_SUCCESS) {
        ready_time=FiletimeNow();
        std::ostringstream ready;
        ready<<"{\"start_error\":"<<started<<",\"open_error\":"<<opened<<",\"enable_error\":"<<enabled
             <<",\"capture_ready_filetime_100ns\":"<<ready_time<<",\"trace_start_filetime_100ns\":"<<began
             <<",\"event_id_filter\":[";
        for(std::size_t i=0;i<kIds.size();++i) ready<<(i?",":"")<<kIds[i];
        ready<<"],\"mode\":\"lossless realtime owner-filtered JSONL\",\"maximum_bytes\":"<<maximum_bytes<<'}';
        if (!Save((root/L"trace-ready.json").c_str(),ready.str())) {reader.failed=true; reason="ready_write_failed";}
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
        auto next_query=std::chrono::steady_clock::now();
        while (!reader.failed.load()) {
            if (std::filesystem::exists(root/L"stop")) {stop_requested=true; break;}
            if (std::chrono::steady_clock::now()>deadline) {reason="collector_watchdog_timeout"; reader.failed=true; break;}
            if (std::filesystem::exists(root/L"target-pid.txt")) {
                std::ifstream binding(root/L"target-pid.txt"); std::uint64_t pid=0; binding>>pid;
                if (!binding || !pid || pid>MAXDWORD || (reader.target_pid.load() && reader.target_pid.load()!=pid)) {
                    reason="pid_binding_invalid_or_changed"; reader.failed=true; break;
                }
                reader.target_pid=static_cast<DWORD>(pid);
            }
            if (std::chrono::steady_clock::now()>=next_query) {
                Properties health;
                queried=ControlTraceW(session,name,&health.value,EVENT_TRACE_CONTROL_QUERY);
                std::ostringstream row;
                row<<"{\"utc_ms\":"<<FiletimeNow()/10000-11644473600000ull<<",\"target_pid\":"<<reader.target_pid.load()
                   <<",\"events_seen\":"<<reader.seen.load()<<",\"events_kept\":"<<reader.kept.load()
                   <<",\"foreign_packets_filtered\":"<<reader.foreign.load()<<",\"prebind_events_kept\":"<<reader.prebind.load()
                   <<",\"written_bytes\":"<<reader.written_bytes.load()<<",\"maximum_bytes\":"<<maximum_bytes
                   <<",\"query_error\":"<<queried<<",\"events_lost\":"<<health.value.EventsLost
                   <<",\"realtime_buffers_lost\":"<<health.value.RealTimeBuffersLost<<'}';
                if (queried || health.value.EventsLost || health.value.RealTimeBuffersLost) {
                    reason="trace_health_or_loss"; reader.failed=true; break;
                }
                if (!Replace(root/L"heartbeat.json",row.str(),&heartbeat_error,&heartbeat_sharing_retries,&heartbeat_publication)) {
                    reason="heartbeat_publish_failed"; reader.failed=true; break;
                }
                next_query=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } else reason="trace_start_open_or_enable_failed";
    if (started==ERROR_SUCCESS) stopped=ControlTraceW(session,name,&props.value,EVENT_TRACE_CONTROL_STOP);
    const auto ended=FiletimeNow();
    // Stop first, then drain ProcessTrace. Closing the consumer before stop
    // would discard the last buffer and could fabricate a release result.
    if (consumer.joinable()) consumer.join();
    if (opened==ERROR_SUCCESS) closed=CloseTrace(handle);
    reader.output.flush(); reader.write_failed|=!reader.output.good();
    const bool ok=started==0 && opened==0 && enabled==0 && queried==0 && stopped==0 && processed==0 && closed==0
        && stop_requested && reader.target_pid.load() && !reader.failed.load() && !reader.write_failed
        && !reader.metadata_errors && !reader.property_errors && !reader.filtered
        && !props.value.EventsLost && !props.value.LogBuffersLost && !props.value.RealTimeBuffersLost;
    std::ostringstream summary;
    summary<<"{\"start_error\":"<<started<<",\"open_error\":"<<opened<<",\"enable_error\":"<<enabled
           <<",\"query_error\":"<<queried<<",\"stop_error\":"<<stopped<<",\"process_error\":"<<processed<<",\"close_error\":"<<closed
           <<",\"events_seen\":"<<reader.events<<",\"events_kept\":"<<reader.kept.load()<<",\"events_outside_filter\":"<<reader.filtered
           <<",\"foreign_packets_filtered\":"<<reader.foreign.load()<<",\"prebind_events_kept\":"<<reader.prebind.load()
           <<",\"metadata_errors\":"<<reader.metadata_errors<<",\"property_errors\":"<<reader.property_errors
           <<",\"events_lost\":"<<props.value.EventsLost<<",\"log_buffers_lost\":"<<props.value.LogBuffersLost
           <<",\"buffers_lost\":"<<props.value.RealTimeBuffersLost<<",\"realtime_buffers_lost\":"<<props.value.RealTimeBuffersLost
           <<",\"write_failed\":"<<(reader.write_failed?"true":"false")<<",\"collector_failed\":"<<(reader.failed.load()?"true":"false")
           <<",\"heartbeat_publish_error\":"<<heartbeat_error<<",\"heartbeat_sharing_retries\":"<<heartbeat_sharing_retries
           <<",\"heartbeat_move_error\":"<<heartbeat_publication.move_error
           <<",\"heartbeat_source_attributes\":"<<heartbeat_publication.source_attributes
           <<",\"heartbeat_target_attributes\":"<<heartbeat_publication.target_attributes
           <<",\"heartbeat_source_attributes_error\":"<<heartbeat_publication.source_attributes_error
           <<",\"heartbeat_target_attributes_error\":"<<heartbeat_publication.target_attributes_error
           <<",\"heartbeat_source_delete_error\":"<<heartbeat_publication.source_delete_error
           <<",\"heartbeat_target_delete_error\":"<<heartbeat_publication.target_delete_error
           <<",\"heartbeat_parent_access_error\":"<<heartbeat_publication.parent_access_error
           <<",\"heartbeat_resolved_access_denied_retries\":"<<heartbeat_publication.resolved_access_denied_retries
           <<",\"stop_requested\":"<<(stop_requested?"true":"false")<<",\"target_pid\":"<<reader.target_pid.load()
           <<",\"written_bytes\":"<<reader.written_bytes.load()<<",\"maximum_bytes\":"<<maximum_bytes
           <<",\"trace_start_filetime_100ns\":"<<began<<",\"capture_ready_filetime_100ns\":"<<ready_time
           <<",\"trace_end_filetime_100ns\":"<<ended<<",\"mode\":\"lossless realtime owner-filtered JSONL\""
           <<",\"timestamp_mode\":\"ProcessTrace normalized FILETIME; RAW_TIMESTAMP disabled\",\"reason\":\""<<reason<<"\",\"complete\":"<<(ok?"true":"false")<<'}';
    return Save((root/L"trace-summary.json").c_str(),summary.str()) && ok ? 0 : 1;
}
} // namespace
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc == 5 && std::wstring_view(argv[1]) == L"--start") return Start(argv[2],argv[3],argv[4]);
        if (argc == 4 && std::wstring_view(argv[1]) == L"--stop") return Stop(argv[2],argv[3]);
        if (argc == 5 && std::wstring_view(argv[1]) == L"--read") return Read(argv[2],argv[3],argv[4]);
        if (argc == 6 && std::wstring_view(argv[1]) == L"--live") return Live(argv[2],argv[3],std::stoull(argv[4]),std::stoull(argv[5]));
    } catch (...) {return 1;}
    return 2;
}
