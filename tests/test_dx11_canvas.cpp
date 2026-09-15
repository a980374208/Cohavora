#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLabel>
#include <iostream>
#include "src/ui/dx11/dx11_video_canvas.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("DX11 Video Canvas Test");
    window.resize(1280, 720);

    auto* canvas = new livekit::dx11::Dx11VideoCanvas(&window);
    window.setCentralWidget(canvas);

    // 在 canvas 内部放一个浮层按钮与标签测试
    auto* overlay = new QWidget(canvas);
    overlay->setStyleSheet("background-color: rgba(0, 0, 0, 180); border-radius: 8px;");
    overlay->setGeometry(50, 50, 260, 100);

    auto* label = new QLabel("Hello DX11 Canvas Overlay", overlay);
    label->setStyleSheet("color: white; font-weight: bold; background: transparent;");
    label->move(20, 20);

    auto* btn = new QPushButton("Click Me", overlay);
    btn->setStyleSheet("background-color: #00b42a; color: white; border-radius: 4px; padding: 4px 10px;");
    btn->move(20, 50);
    QObject::connect(btn, &QPushButton::clicked, []() {
        std::cout << "[Test] Overlay Button Clicked Successfully!" << std::endl;
    });

    // 构造测试 RGBA 帧 (渐变彩条)
    const int w = 640;
    const int h = 360;
    std::vector<uint8_t> rgba(w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = (y * w + x) * 4;
            rgba[idx + 0] = static_cast<uint8_t>((x * 255) / w);       // R
            rgba[idx + 1] = static_cast<uint8_t>((y * 255) / h);       // G
            rgba[idx + 2] = 128;                                       // B
            rgba[idx + 3] = 255;                                       // A
        }
    }

    livekit::VideoFrame testFrame(w, h, livekit::VideoBufferType::RGBA, std::move(rgba));
    canvas->updateFrame("user_1", testFrame);

    std::vector<livekit::dx11::TileRect> tiles = {
        { "user_1", 20, 20, 640, 360, false, 0.0f, true }
    };
    canvas->setTilesLayout(tiles);

    window.show();
    std::cout << "[Test] Window shown, starting event loop..." << std::endl;

    // 自动退出定时器 (用于 CI/无头自动化测试) 或保留窗口
    QTimer::singleShot(2000, [&]() {
        std::cout << "[Test] Test passed 2s running check, exiting..." << std::endl;
        app.quit();
    });

    return app.exec();
}
