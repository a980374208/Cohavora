#pragma once

#include <QtCore/QString>
#include <filesystem>

namespace MeetingApp {

struct DebugStoragePaths final {
	bool valid = true;
	bool enabled = false;
	std::filesystem::path root;
	std::filesystem::path crashEvidence;
	std::filesystem::path diagnostics;
	std::filesystem::path stabilityLedger;
	std::filesystem::path telemetryReports;
	std::filesystem::path emojiCache;
};

} // namespace MeetingApp

#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <cstring>
#include <QtCore/qt_windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <array>
#include <memory>
#include <vector>

namespace MeetingApp {
inline bool NativeDebugOptionEnabled(int argc, char *const argv[]) noexcept {
	bool enabled = false;
	for (int i = 1; i < argc; ++i) {
		const auto argument = argv[i];
		if (!argument) continue;
		if (!std::strcmp(argument, "-u") || !std::strcmp(argument, "--username") ||
			!std::strcmp(argument, "-p") || !std::strcmp(argument, "--password")) {
			if (i + 1 < argc) ++i;
		} else if (!std::strcmp(argument, "--debug")) {
			enabled = true;
		}
	}
	return enabled;
}

namespace debug_storage_detail {
inline bool IsPrivateDirectory(const std::filesystem::path &path) {
	PSID owner = nullptr;
	PACL acl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (GetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
		OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
		&owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
	const std::unique_ptr<void, decltype(&LocalFree)> descriptorGuard(descriptor, &LocalFree);
	if (!owner || !acl) return false;
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
	const std::unique_ptr<void, decltype(&CloseHandle)> tokenGuard(token, &CloseHandle);
	DWORD length = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &length);
	if (!length) return false;
	std::vector<BYTE> buffer(length);
	if (!GetTokenInformation(token, TokenUser, buffer.data(), length, &length) ||
		!EqualSid(owner, reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid)) return false;
	// Match the diagnostic sink's privacy contract; never weaken it for a debug root.
	for (const auto kind : {WinWorldSid, WinAuthenticatedUserSid, WinBuiltinUsersSid}) {
		std::array<BYTE, SECURITY_MAX_SID_SIZE> sid{};
		DWORD size = static_cast<DWORD>(sid.size());
		if (!CreateWellKnownSid(kind, nullptr, sid.data(), &size)) return false;
		TRUSTEE_W trustee{};
		BuildTrusteeWithSidW(&trustee, sid.data());
		ACCESS_MASK rights = 0;
		if (GetEffectiveRightsFromAclW(acl, &trustee, &rights) != ERROR_SUCCESS) return false;
		constexpr ACCESS_MASK unsafe = GENERIC_ALL | GENERIC_READ | GENERIC_WRITE |
			FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_DELETE_CHILD;
		if (rights & unsafe) return false;
	}
	return true;
}

inline bool CreatePrivateRoot(const std::filesystem::path &path) {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
	const std::unique_ptr<void, decltype(&CloseHandle)> tokenGuard(token, &CloseHandle);
	DWORD length = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &length);
	if (!length) return false;
	std::vector<BYTE> buffer(length);
	if (!GetTokenInformation(token, TokenUser, buffer.data(), length, &length)) return false;
	LPWSTR sid = nullptr;
	if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid,
		&sid)) return false;
	const std::unique_ptr<void, decltype(&LocalFree)> sidGuard(sid, &LocalFree);
	const auto user = std::wstring(sid);
	const auto sddl = L"O:" + user + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + user + L")";
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
		SDDL_REVISION_1, &descriptor, nullptr)) return false;
	const std::unique_ptr<void, decltype(&LocalFree)> descriptorGuard(descriptor, &LocalFree);
	SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
	// The private ACL is present at creation. Never rewrite an existing directory's ACL.
	if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
		return false;
	return IsPrivateDirectory(path);
}

inline bool SafeAncestors(const std::filesystem::path &path) {
	for (auto current = path; !current.empty(); current = current.parent_path()) {
		const auto attributes = GetFileAttributesW(current.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) {
			const auto error = GetLastError();
			if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
		} else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
			!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
			return false;
		}
		if (current == current.parent_path()) break;
	}
	return true;
}
} // namespace

inline DebugStoragePaths ResolveDebugStoragePaths(bool debugEnabled, const QString &root) {
	DebugStoragePaths result;
	if (!debugEnabled || root.isEmpty()) return result;
	result.valid = false;
	const auto normalized = QDir::fromNativeSeparators(root);
	// Local fully qualified drive paths only: no UNC/device namespace or drive-relative paths.
	if (normalized.size() < 4 || !normalized.at(0).isLetter() ||
		normalized.at(0).unicode() > 127 || normalized.at(1) != ':' ||
		normalized.at(2) != '/') return result;
	const auto components = normalized.mid(3).split('/');
	const QRegularExpression reserved(QStringLiteral(
		"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])($|\\.)"),
		QRegularExpression::CaseInsensitiveOption);
	for (const auto &component : components) {
		if (component.isEmpty() || component == "." || component == ".." ||
			component.endsWith(' ') || component.endsWith('.') || reserved.match(component).hasMatch()) return result;
		for (const auto ch : component) {
			if (ch.unicode() < 32 || QStringLiteral(":*?\"<>|").contains(ch)) return result;
		}
	}
	result.root = std::filesystem::path(normalized.toStdWString());
	result.crashEvidence = result.root / L"crash-evidence";
	result.diagnostics = result.root / L"diagnostics";
	result.stabilityLedger = result.root / L"telemetry" / L"stability-ledger-v1.json";
	result.telemetryReports = result.root / L"telemetry" / L"reports";
	result.emojiCache = result.root / L"cache" / L"emoji";
	if (!debug_storage_detail::SafeAncestors(result.root)) return result;
	std::error_code parentError;
	std::filesystem::create_directories(result.root.parent_path(), parentError);
	if (parentError || !debug_storage_detail::SafeAncestors(result.root) ||
		!debug_storage_detail::CreatePrivateRoot(result.root)) return result;
	const auto ledgerAttributes = GetFileAttributesW(result.stabilityLedger.c_str());
	if (ledgerAttributes != INVALID_FILE_ATTRIBUTES) {
		if (ledgerAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return result;
	} else {
		const auto error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return result;
	}
	for (const auto &directory : {result.crashEvidence, result.diagnostics,
		result.stabilityLedger.parent_path(), result.telemetryReports,
		result.emojiCache.parent_path(), result.emojiCache}) {
		if (!debug_storage_detail::SafeAncestors(directory)) return result;
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		if (error || !debug_storage_detail::SafeAncestors(directory) ||
			!debug_storage_detail::IsPrivateDirectory(directory)) return result;
	}
	result.valid = true;
	result.enabled = true;
	return result;
}
} // namespace MeetingApp
