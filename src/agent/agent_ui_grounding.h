#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace OpenMeeting {

// Builds the grounded client UI navigation and in-meeting operations map
// dynamically using the application's active Qt translations ("MeetingUI",
// "MeetingRoomWindow", etc.) so the assistant quotes exact UI labels.
[[nodiscard]] QString buildClientUiNavigationGuidePrompt();

[[nodiscard]] QString localizedBooleanSettingLabel(bool enabled);
[[nodiscard]] QString localizedThemeLabel(const QString &themeId);
[[nodiscard]] QString localizedLanguageLabel(const QString &langId);
[[nodiscard]] QString localizedParticipantNameDisplayLabel(const QString &mode);
[[nodiscard]] QString localizedMirrorModeLabel(const QString &mode);
[[nodiscard]] QString localizedReceiveResolutionMatchLabel(const QString &mode);
[[nodiscard]] QString localizedVideoEncoderLabel(const QString &encoderPref);
[[nodiscard]] QString localizedVideoCodecLabel(const QString &codec);
[[nodiscard]] QString localizedScreenShareResolutionLabel(const QString &resPreset);

// Formats a user-facing UI label alongside its canonical tool parameter value
// (e.g. "Dark (dark)" or "深色 (dark)") when the two differ.
[[nodiscard]] QString formatPreferenceDisplayWithParam(
	const QString &displayLabel,
	const QString &rawParam);

// Builds a localized UI metadata dictionary ("uiDetails") from raw preference
// values produced by serializeClientPreferences(), mapping each setting to its
// exact Settings path, localized current value, and localized valid options.
[[nodiscard]] QJsonObject buildClientPreferencesUiDetails(
	const QJsonObject &rawPrefsPayload);

} // namespace OpenMeeting
