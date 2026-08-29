#pragma once

#include <QtWidgets/QWidget>
#include <QtCore/QTimer>
#include <QtGui/QColor>
#include <vector>
#include <cmath>

namespace MeetingUI {

class AudioVisualizerWidget : public QWidget {
    Q_OBJECT
public:
    explicit AudioVisualizerWidget(QWidget *parent = nullptr, int barCount = 7);
    ~AudioVisualizerWidget() override = default;

    // 设置当前实时音量能量 (0.0f ~ 1.0f)
    void setAudioLevel(float level);

    // 设置是否处于激活发言状态
    void setActive(bool active);
    bool isActive() const { return _active; }

    // 自定义柱条颜色
    void setBarColor(const QColor &color);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private slots:
    void updateAnimation();

private:
    int _barCount = 7;
    bool _active = false;
    float _inputLevel = 0.0f;
    QColor _barColor = QColor(0, 180, 42); // 现代声浪绿

    std::vector<float> _currentHeights; // 0.0 ~ 1.0
    std::vector<float> _targetHeights;  // 0.0 ~ 1.0

    QTimer _animTimer;
    int _animPhase = 0;
};

} // namespace MeetingUI
