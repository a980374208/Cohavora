#pragma once

#include "src/media/camera_source_manager.h"
#include "src/render/local_video_render_input.h"
#include "src/render/qt_cpu_video_renderer.h"

#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <memory>

namespace MeetingUI {

class CameraPreviewWidget final : public QWidget {
	Q_OBJECT

public:
	explicit CameraPreviewWidget(QWidget *parent = nullptr);
	~CameraPreviewWidget() override;

	QSize sizeHint() const override;

public slots:
	void startPreview(const QString &deviceId, int width, int height, int fps);
	void stopPreview();
	void setMirrored(bool mirrored);

signals:
	void previewStateChanged(const QString &message, bool error);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	void renderLatestFrame();
	void setStatus(const QString &message, bool error);

	std::shared_ptr<livekit::VideoSource> _source;
	std::shared_ptr<livekit::CameraSourceManager> _camera;
	livekit::render::LocalVideoRenderInput _input;
	livekit::render::QtCpuVideoRenderer _renderer;
	QTimer _renderTimer;
	QImage _frame;
	QString _status;
	bool _statusIsError = false;
	bool _mirrored = false;
};

} // namespace MeetingUI
