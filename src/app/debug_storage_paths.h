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
	const auto ledgerAttributes = GetFileAttributesW(result.stabilityLedger.c_str());
	if (ledgerAttributes != INVALID_FILE_ATTRIBUTES) {
		if (ledgerAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return result;
	} else {
		const auto error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return result;
	}
	for (const auto &directory : {result.crashEvidence, result.diagnostics,
		result.telemetryReports, result.emojiCache}) {
		if (!debug_storage_detail::SafeAncestors(directory)) return result;
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		if (error || !debug_storage_detail::SafeAncestors(directory)) return result;
	}
	result.valid = true;
	result.enabled = true;
	return result;
}
} // namespace MeetingApp
