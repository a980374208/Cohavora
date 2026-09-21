#include <QtCore/QCoreApplication>
#include "src/ui/camera_preview_widget.h"

#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtWidgets/QApplication>

#include <algorithm>

namespace MeetingUI {

CameraPreviewWidget::CameraPreviewWidget(QWidget *parent)
	: QWidget(parent)
	, _source(std::make_shared<livekit::VideoSource>(1280, 720))
	, _camera(livekit::CameraSourceManager::Create(_source)) {
	setMinimumHeight(220);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	_input.Attach(_source);
	_renderTimer.setInterval(33);
	connect(&_renderTimer, &QTimer::timeout, this, &CameraPreviewWidget::renderLatestFrame);
	setStatus(QCoreApplication::translate("MeetingUI", "Select a camera to preview video"), false);
}

CameraPreviewWidget::~CameraPreviewWidget() {
	stopPreview();
}

QSize CameraPreviewWidget::sizeHint() const {
	return {640, 360};
}

void CameraPreviewWidget::startPreview(
		const QString &deviceId,
		int width,
		int height,
		int fps) {
	if (!_camera || deviceId.isEmpty() || width <= 0 || height <= 0 || fps <= 0) {
		stopPreview();
		setStatus(QCoreApplication::translate("MeetingUI", "No camera or supported resolution available"), true);
		return;
	}

	livekit::DShowCaptureConfig config;
	config.device_path = deviceId.toStdString();
	config.width = width;
	config.height = height;
	config.fps = fps;
	config.output_format = livekit::VideoBufferType::NV12;
	setStatus(QCoreApplication::translate("MeetingUI", "Starting camera preview..."), false);
	_renderTimer.start();

	if (!_camera->IsRunning()) {
		if (_camera->Start(config)) {
			setStatus(QString(), false);
		} else {
			_renderTimer.stop();
			setStatus(QCoreApplication::translate("MeetingUI", "Unable to open the camera. Another application may be using it."), true);
		}
		return;
	}

	QPointer<CameraPreviewWidget> guard(this);
	_camera->ReconfigureAsync(config, 3000,
		[guard](bool success, const std::string &error) {
			QMetaObject::invokeMethod(qApp, [guard, success, error] {
				if (!guard) return;
				if (success) {
					guard->setStatus(QString(), false);
				} else {
					guard->setStatus(
						QCoreApplication::translate("MeetingUI", "Failed to switch camera preview: %1")
							.arg(QString::fromStdString(error)),
						true);
				}
			}, Qt::QueuedConnection);
		});
}

void CameraPreviewWidget::stopPreview() {
	_renderTimer.stop();
	if (_camera) _camera->Stop();
	_frame = {};
	update();
}

void CameraPreviewWidget::setMirrored(bool mirrored) {
	if (_mirrored == mirrored) return;
	_mirrored = mirrored;
	update();
}

void CameraPreviewWidget::renderLatestFrame() {
	auto frame = _input.TakeLatest();
	if (!frame) return;
	auto image = _renderer.Convert(*frame);
	if (image.isNull()) return;
	_frame = std::move(image);
	if (!_status.isEmpty()) setStatus(QString(), false);
	update();
}

void CameraPreviewWidget::setStatus(const QString &message, bool error) {
	_status = message;
	_statusIsError = error;
	emit previewStateChanged(message, error);
	update();
}

void CameraPreviewWidget::paintEvent(QPaintEvent *event) {
	Q_UNUSED(event);
	QPainter painter(this);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	painter.fillRect(rect(), QColor(22, 27, 43));
	if (!_frame.isNull()) {
		const auto target = _frame.size().scaled(size(), Qt::KeepAspectRatio);
		const QRect area((width() - target.width()) / 2,
			(height() - target.height()) / 2, target.width(), target.height());
		if (_mirrored) {
			painter.save();
			painter.translate(area.left() + area.right(), 0);
			painter.scale(-1.0, 1.0);
			painter.drawImage(QRect(area.left(), area.top(), area.width(), area.height()), _frame);
			painter.restore();
		} else {
			painter.drawImage(area, _frame);
		}
	}
	if (!_status.isEmpty()) {
		painter.setPen(_statusIsError ? QColor(255, 184, 184) : QColor(226, 230, 241));
		painter.drawText(rect().adjusted(24, 24, -24, -24),
			Qt::AlignCenter | Qt::TextWordWrap, _status);
	}
}

} // namespace MeetingUI
