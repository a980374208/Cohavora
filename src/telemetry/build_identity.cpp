#include "build_identity.h"

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace livekit::telemetry {
namespace {

std::string ComputeBuildId() {
#if defined(_WIN32)
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return "unknown";
    std::ifstream input(std::filesystem::path(path.data()), std::ios::binary);
    if (!input) return "unknown";
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context digest(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        return "unknown";
    std::array<char, 64 * 1024> chunk{};
    while (input.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) ||
           input.gcount() > 0) {
        if (EVP_DigestUpdate(digest.get(), chunk.data(),
                static_cast<std::size_t>(input.gcount())) != 1)
            return "unknown";
    }
    if (input.bad()) return "unknown";
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned length_bytes = 0;
    if (EVP_DigestFinal_ex(digest.get(), hash.data(), &length_bytes) != 1 ||
        length_bytes != 32) return "unknown";
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned i = 0; i < length_bytes; ++i) {
        result.push_back(digits[hash[i] >> 4]);
        result.push_back(digits[hash[i] & 15]);
    }
    return result;
#else
    return "unknown";
#endif
}

std::string ComputePdbIdentity() {
#if defined(_WIN32)
    const auto* base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    if (!base) return "unknown";
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 1024 * 1024) return "unknown";
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE) return "unknown";
    const auto image_size = pe->OptionalHeader.SizeOfImage;
    const auto& debug = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const auto inside = [image_size](std::uint32_t rva, std::uint32_t bytes) {
        return rva != 0 && rva <= image_size && bytes <= image_size - rva;
    };
    if (!inside(debug.VirtualAddress, debug.Size) ||
        debug.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0) return "unknown";
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
        base + debug.VirtualAddress);
    struct RsdsHeader {
        char magic[4];
        GUID guid;
        std::uint32_t age;
    };
    for (std::size_t i = 0; i < debug.Size / sizeof(*entries); ++i) {
        const auto& item = entries[i];
        if (item.Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
            !inside(item.AddressOfRawData, item.SizeOfData) ||
            item.SizeOfData < sizeof(RsdsHeader)) continue;
        const auto* rsds = reinterpret_cast<const RsdsHeader*>(
            base + item.AddressOfRawData);
        if (std::string_view(rsds->magic, 4) != "RSDS") continue;
        const auto& id = rsds->guid;
        std::ostringstream output;
        output << std::hex << std::setfill('0')
               << std::setw(8) << id.Data1 << '-'
               << std::setw(4) << id.Data2 << '-'
               << std::setw(4) << id.Data3 << '-';
        for (std::size_t byte = 0; byte < 8; ++byte)
            output << std::setw(2) << static_cast<unsigned>(id.Data4[byte]);
        output << '-' << std::dec << rsds->age;
        return output.str();
    }
#endif
    return "unknown";
}

} // namespace

const std::string& CurrentExecutableBuildId() {
    static const std::string id = ComputeBuildId();
    return id;
}

const std::string& CurrentExecutablePdbIdentity() {
    static const std::string id = ComputePdbIdentity();
    return id;
}

} // namespace livekit::telemetry
