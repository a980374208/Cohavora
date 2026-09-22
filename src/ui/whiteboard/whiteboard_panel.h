#pragma once

#include "whiteboard_canvas.h"
#include "whiteboard_image_loader.h"

#include <QtCore/QVariantMap>
#include <QtCore/QSet>

class QComboBox;
class QLabel;
class QPushButton;
class QThread;

namespace MeetingUI {
class WhiteboardPanel final : public QWidget {
    Q_OBJECT
public:
    explicit WhiteboardPanel(QWidget *parent = nullptr);
    ~WhiteboardPanel() override;
    WhiteboardCanvas *canvas() const { return canvas_; }
    const livekit::whiteboard::Document &document() const { return document_; }
    void enableCollaboration();
    void applyProjection(const QByteArray &snapshot,
                         quint64 sequence,
                         int collaborationState,
                         const QString &authorityIdentity,
                         const QString &localActor,
                         bool locked,
                         bool writersOpen,
                         bool canEdit,
                         bool canAdmin,
                         const QVariantMap &assets,
                         const QString &status);

signals:
    void closeRequested();
    void commandProposed(const livekit::whiteboard::Command &command);
    void lockRequested(bool locked);
    void writersOpenRequested(bool open);
    void imageProposed(const QByteArray &png,
                       const QString &assetId,
                       int width,
                       int height,
                       const QString &pageId,
                       bool replaceCurrent);

private:
    void refreshControls();
    void confirmClear();
    void exportPng();
    void importImage();
    void ensureActiveBackground();
    void updateEditingState();
    QString imageErrorText(WhiteboardImageError error) const;
    livekit::whiteboard::Document document_;
    WhiteboardCanvas *canvas_ = nullptr;
    QComboBox *pages_ = nullptr, *zoom_ = nullptr;
    QPushButton *undo_ = nullptr, *redo_ = nullptr, *addPage_ = nullptr;
    QPushButton *import_ = nullptr, *export_ = nullptr;
    QPushButton *clear_ = nullptr, *lock_ = nullptr, *writers_ = nullptr;
    QLabel *title_ = nullptr, *mode_ = nullptr, *status_ = nullptr;
    QThread *exportWorker_ = nullptr, *importWorker_ = nullptr, *decodeWorker_ = nullptr;
    QVariantMap assetBytes_;
    QSet<QString> failedAssets_;
    QString decodingAssetId_;
    bool collaborative_ = false, canEdit_ = true, projectedCanEdit_ = true, canAdmin_ = false;
    QString localActor_;
};
} // namespace MeetingUI
