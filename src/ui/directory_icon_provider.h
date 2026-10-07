#pragma once

#include <QtCore/QFileInfo>
#include <QtWidgets/QFileIconProvider>
#include <QtWidgets/QStyle>

namespace MeetingUI {

// Directory selection does not need path-specific Shell icons or overlays.
// Cache on the UI thread; the model's worker only copies these shared icons.
class DirectoryIconProvider final : public QFileIconProvider {
public:
    explicit DirectoryIconProvider(QStyle &style)
        : folder_(style.standardIcon(QStyle::SP_DirIcon)),
          file_(style.standardIcon(QStyle::SP_FileIcon)) {}

    QIcon icon(const QFileInfo &info) const override {
        return info.isDir() ? folder_ : file_;
    }
    QIcon icon(IconType type) const override {
        return type == File ? file_ : folder_;
    }

private:
    QIcon folder_;
    QIcon file_;
};

} // namespace MeetingUI
