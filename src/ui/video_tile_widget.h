#pragma once

#include "ui/widgets/rp_window.h"
#include "src/core/room.h"
#include "src/net/session_manager.h"
#include "src/render/video_render_frame.h"
#include "src/ui/audio_visualizer_widget.h"

#include <QtGui/QFont>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QWidget>

#include <mutex>
#include <optional>

class ParticipantWindowInspectionAccess;

namespace MeetingUI {

// ----------------------------------------------------
// VideoTileWidget: 单个视频/头像渲染画框组件
// ----------------------------------------------------
class VideoTileWidget : public Ui::RpWidget {
	Q_OBJECT
public:
	explicit VideoTileWidget(const QString &displayName, bool isLocal, QWidget *parent = nullptr,
		bool isScreenShare = false);
	~VideoTileWidget() override;

	void setDisplayName(const QString &name);
	QString displayName() const { return _displayName; }
	void setNameDisplayMode(OpenMeeting::ParticipantNameDisplayMode mode);
	void resetForReuse(const QString &displayName, float remoteVolume = 1.0f, bool locallyMuted = false);

	bool isLocal() const { return _isLocal; }
	bool isVideoActive() const { return _isVideoActive; }
	void setVideoActive(bool active);

	bool isAudioMuted() const { return _isAudioMuted; }
	void setAudioMuted(bool muted);
	void setConnectionQuality(livekit::ConnectionQuality quality);
	void setVideoStreamPaused(bool paused);
	bool isVideoStreamPaused() const { return _isVideoStreamPaused; }
	void setVideoSubscriptionError(livekit::TrackPublication::SubscriptionError error);
	livekit::TrackPublication::SubscriptionError videoSubscriptionError() const {
		return _videoSubscriptionError;
	}

	void setSpeaking(bool speaking, float level = 0.0f);
	bool isSpeaking() const { return _isSpeaking; }
	float audioLevel() const { return _audioLevel; }

	void setFrame(
		const QImage &image,
		livekit::render::VideoRenderFrame::Ptr renderFrame = {});
	QRectF remoteControlContentRect();
	void setRemoteInputEnabled(bool enabled);

	// 远端独立音量与静音管理
	float remoteVolume() const { return _remoteVolume; }
	bool isLocallyMuted() const { return _isLocallyMuted; }

	// Pin 钉住与身份
	void setPinned(bool pinned);
	bool isPinned() const { return _isPinned; }

	QString identity() const { return _identity; }
	void setIdentity(const QString &id) { _identity = id; }
	QString renderKey() const { return _renderKey.isEmpty() ? _identity : _renderKey; }
	void setRenderKey(const QString &key) { _renderKey = key; }

	// PIP 小窗交互
	void setPipMode(bool pip);
	bool isPipMode() const { return _isPip; }

	// DX11 硬件加速模式支持
	void setHardwareCanvasMode(bool enabled);
	bool isHardwareCanvasMode() const { return _useHardwareCanvas; }
	// UI-thread-only decoration cache. Video pixels never pass through this image.
	QImage hardwareDecoration(const QSize &pixels, bool hasFrame, bool hovered);
	void paintOnExternalSurface(QPainter &p, const QRect &targetRect, bool hovered);
	void setExternalCpuSurfaceState(bool active, bool visible, bool minimized, const QSize &logicalSize);
	bool isOnExternalSurface() const { return _externalSurfaceActive; }
	QRect pinButtonRect() const;
	// Tile-local visible part; the full widget size still owns video fitting.
	void setLayoutClip(const QRect &visible);
	QRect layoutClipRect() const { return _layoutClip.value_or(rect()); }

signals:
	void tileDoubleClicked();
	void tileClicked();
	void pinToggled(bool pinned);
    void moreRequested();
	void remoteVolumeChanged(float volume);
	void remoteLocalMuteToggled(bool muted);
	void presentationChanged();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void changeEvent(QEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	friend class ::ParticipantWindowInspectionAccess;
	std::optional<QRect> _layoutClip;
	struct NameTagPresentation {
		QRect rect;
		QFont font;
		QString text;
		bool compact = false;
	};
	NameTagPresentation bottomNameTag(const QRect &r) const;
	bool nameVisible(bool hovered) const;
	void drawAvatarPlaceholder(QPainter &p, const QRect &r, bool showName);
	void drawVideoFrame(QPainter &p, const QRect &r);
	void drawBottomNameTag(QPainter &p, const QRect &r);
	void drawNetworkQualityBadge(QPainter &p, const QRect &r);
	void drawVideoPlaceholder(QPainter &p, const QRect &r);
	void paintCard(QPainter &p, bool decorationOnly, bool hasFrame, bool hovered);
	void invalidatePresentation();
	void setupVolumeControls();
	void ensureVolumePopup();
	void ensureVisualizer();
	void updateRenderExpectation();

	QString _identity;
	QString _renderKey;
	bool _isScreenShare = false;
	QString _displayName;
	OpenMeeting::ParticipantNameDisplayMode _nameDisplayMode = OpenMeeting::ParticipantNameDisplayMode::Auto;
	bool _isLocal = false;
	bool _isVideoActive = false;
	bool _isAudioMuted = false;
	bool _isVideoStreamPaused = false;
	livekit::TrackPublication::SubscriptionError _videoSubscriptionError =
		livekit::TrackPublication::SubscriptionError::None;
	livekit::ConnectionQuality _connectionQuality = livekit::ConnectionQuality::Unknown;
	bool _isSpeaking = false;
	bool _isPinned = false;
	float _audioLevel = 0.0f;
	bool _isPip = false;
	bool _useHardwareCanvas = true;

	// 远端独立音量控制
	float _remoteVolume = 1.0f;
	bool _isLocallyMuted = false;
	QPushButton *_volBtn = nullptr;
	QPushButton *_pinBtn = nullptr;
	QWidget *_volPopup = nullptr;
	QSlider *_volSlider = nullptr;
	QLabel *_volLabel = nullptr;
	QPushButton *_muteRemoteBtn = nullptr;

	// 频域多柱跳动波形组件
	AudioVisualizerWidget *_visualizer = nullptr;

	QImage _currentFrame;
	livekit::render::RenderFrameMetadata::Ptr _currentRenderFrame;
	std::mutex _frameMutex;
	bool _remoteInputEnabled = false;
	bool _hasLoggedFirstPaint = false;
	QImage _hardwareDecoration;
	QSize _decorationLogicalSize;
	bool _decorationHasFrame = false;
	bool _decorationHovered = false;
	quint64 _decorationThemeRevision = 0;
	bool _externalSurfaceActive = false;
	bool _externalSurfaceVisible = false;
	bool _externalSurfaceMinimized = false;
	QSize _externalLogicalSize;
};

} // namespace MeetingUI
