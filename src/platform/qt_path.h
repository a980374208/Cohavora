#pragma once
#include <QtCore/QString>
#include <filesystem>

namespace cohavora::platform {
inline std::filesystem::path PathFromQString(const QString &value) {
#ifdef Q_OS_WIN
    return std::filesystem::path(value.toStdWString());
#else
    const auto bytes = value.toUtf8();
    return std::filesystem::u8path(bytes.constData(), bytes.constData() + bytes.size());
#endif
}
inline QString QStringFromPath(const std::filesystem::path &value) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(value.wstring());
#else
    const auto bytes = value.u8string();
    return QString::fromUtf8(reinterpret_cast<const char *>(bytes.data()), int(bytes.size()));
#endif
}
}
