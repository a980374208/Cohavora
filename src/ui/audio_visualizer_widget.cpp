#include "audio_visualizer_widget.h"
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <algorithm>
#include <random>

namespace MeetingUI {

AudioVisualizerWidget::AudioVisualizerWidget(QWidget *parent, int barCount)
    : QWidget(parent), _barCount(std::max(3, barCount)) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);

    _currentHeights.resize(_barCount, 0.08f);
    _targetHeights.resize(_barCount, 0.08f);

    connect(&_animTimer, &QTimer::timeout, this, &AudioVisualizerWidget::updateAnimation);
    _animTimer.setInterval(30); // ~33fps
}

void AudioVisualizerWidget::setAudioLevel(float level) {
    _inputLevel = std::clamp(level, 0.0f, 1.0f);
    if (_inputLevel > 0.02f) {
        if (!_active) {
            setActive(true);
        }
    }
}

void AudioVisualizerWidget::setActive(bool active) {
    if (_active != active) {
        _active = active;
        if (_active) {
            if (!_animTimer.isActive()) {
                _animTimer.start();
            }
        }
        update();
    }
}

void AudioVisualizerWidget::setBarColor(const QColor &color) {
    _barColor = color;
    update();
}

void AudioVisualizerWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (_active && !_animTimer.isActive()) {
        _animTimer.start();
    }
}

void AudioVisualizerWidget::hideEvent(QHideEvent *event) {
    _animTimer.stop();
    QWidget::hideEvent(event);
}

void AudioVisualizerWidget::updateAnimation() {
    _animPhase = (_animPhase + 1) % 360;

    // 对称多频段权重分布模板 (7 柱居中对称: 中间高，两侧递减)
    static const std::vector<float> kSymmetricWeights = {
        0.45f, 0.75f, 0.95f, 1.00f, 0.95f, 0.75f, 0.45f
    };

    // 当处于发言状态时，无论外界推送频率如何，都保持饱满的声浪活力
    const float baseAmp = _active ? (0.40f + 0.60f * std::max(_inputLevel, 0.35f)) : 0.0f;

    for (int i = 0; i < _barCount; ++i) {
        float weight = 1.0f;
        if (i < static_cast<int>(kSymmetricWeights.size())) {
            weight = kSymmetricWeights[i];
        } else {
            float center = (_barCount - 1) / 2.0f;
            float dist = std::abs(i - center) / (center > 0 ? center : 1.0f);
            weight = 1.0f - dist * 0.55f;
        }

        if (_active) {
            // 各柱条采用不同角频率的多频段正弦调制，形成错落有致的律动声浪
            float freq1 = 0.16f + 0.04f * static_cast<float>(i);
            float freq2 = 0.09f + 0.03f * static_cast<float>((i * 3) % 7);
            float wave1 = std::sin(static_cast<float>(_animPhase) * freq1 + static_cast<float>(i) * 0.9f);
            float wave2 = std::cos(static_cast<float>(_animPhase) * freq2 + static_cast<float>(i) * 1.4f);
            
            float dynamicLevel = baseAmp * weight * (0.50f + 0.35f * wave1 + 0.15f * wave2);
            _targetHeights[i] = std::clamp(dynamicLevel, 0.15f, 1.0f);
        } else {
            _targetHeights[i] = 0.08f; // 静止时微小低柱
        }

        // 阻尼平滑插值: 上升快速 (0.65), 回落平缓 (0.18)
        if (_targetHeights[i] > _currentHeights[i]) {
            _currentHeights[i] += (_targetHeights[i] - _currentHeights[i]) * 0.65f;
        } else {
            _currentHeights[i] += (_targetHeights[i] - _currentHeights[i]) * 0.18f;
        }
    }

    // 缓慢衰减输入音量
    _inputLevel *= 0.92f;

    update();
}

void AudioVisualizerWidget::paintEvent(QPaintEvent *event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0) return;

    const int gap = 4;
    const int totalGaps = (_barCount - 1) * gap;
    int barW = (w - totalGaps) / _barCount;
    if (barW < 3) barW = 3;

    const int totalBarsW = _barCount * barW + totalGaps;
    const int startX = (w - totalBarsW) / 2;

    for (int i = 0; i < _barCount; ++i) {
        float normalizedH = _currentHeights[i];
        int barH = std::max(4, static_cast<int>(normalizedH * static_cast<float>(h - 2)));
        int x = startX + i * (barW + gap);
        int y = (h - barH) / 2; // 垂直居中排列

        // 现代渐变与圆角绘制
        QRect barRect(x, y, barW, barH);
        QColor col = _barColor;
        col.setAlphaF(_active ? 0.95f : 0.35f);

        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawRoundedRect(barRect, barW / 2, barW / 2);
    }
}

} // namespace MeetingUI
