#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QString>

namespace OpenMeeting {

inline constexpr int kMinAccountLength = 5;
inline constexpr int kMaxAccountLength = 12;
inline constexpr int kMinPasswordLength = 6;
inline constexpr int kMaxPasswordLength = 16;

enum class CredentialValidationError { None, AccountLength, PasswordLength };

// Validate without truncating or normalizing credentials, especially passwords.
inline CredentialValidationError validateAccountCredentials(
        const QString &account, const QString &password) {
    if (account.size() < kMinAccountLength || account.size() > kMaxAccountLength) {
        return CredentialValidationError::AccountLength;
    }
    if (password.size() < kMinPasswordLength || password.size() > kMaxPasswordLength) {
        return CredentialValidationError::PasswordLength;
    }
    return CredentialValidationError::None;
}

inline QString credentialValidationMessage(CredentialValidationError error) {
    switch (error) {
    case CredentialValidationError::AccountLength:
        return QCoreApplication::translate("MeetingUI", "The account must contain %1-%2 characters")
            .arg(kMinAccountLength).arg(kMaxAccountLength);
    case CredentialValidationError::PasswordLength:
        return QCoreApplication::translate("MeetingUI", "The password must contain %1-%2 characters")
            .arg(kMinPasswordLength).arg(kMaxPasswordLength);
    case CredentialValidationError::None:
        return {};
    }
    return {};
}

} // namespace OpenMeeting
