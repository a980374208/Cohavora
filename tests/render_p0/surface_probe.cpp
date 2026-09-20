// P0 surface experiment only. No Room/Track, production renderer or module loader.
// The two RGBA textures stand in for video and the host-rasterized stage overlay.
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtGui/QOpenGLWindow>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtGui/QMouseEvent>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtPlugin>
#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

#ifdef LIVEKIT_P0_STATIC_WINDOWS_QT
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
#endif

namespace {
QRectF relativeRect(const QSize& s, double x, double y, double w, double h) {
    return {x * s.width(), y * s.height(), w * s.width(), h * s.height()};
}
struct SceneImages { QImage video; QImage overlay; };
SceneImages sceneImages(const QSize& size, qreal dpr) {
    const QSize physical(qRound(size.width()*dpr), qRound(size.height()*dpr));
    SceneImages images{QImage(physical, QImage::Format_RGBA8888_Premultiplied),
                       QImage(physical, QImage::Format_RGBA8888_Premultiplied)};
    // Rasterize fonts and geometry in the same logical coordinates for CPU and
    // GL. Otherwise GL overlays become physically smaller on high-DPI screens.
    images.video.setDevicePixelRatio(dpr); images.overlay.setDevicePixelRatio(dpr);
    images.video.fill(QColor(20, 170, 80));
    QPainter video(&images.video);
    video.fillRect(relativeRect(size, 0, 0, .5, 1), QColor(220, 40, 30));
    video.fillRect(relativeRect(size, .65, .62, .30, .30), QColor(30, 70, 220));
    video.end();
    images.overlay.fill(Qt::transparent);
    QPainter overlay(&images.overlay);
    overlay.fillRect(relativeRect(size, .1, .1, .8, .2), QColor(255, 255, 255, 128));
    overlay.fillRect(relativeRect(size, .85, .12, .04, .07), QColor(250, 220, 30));
    overlay.setPen(Qt::black);
    overlay.drawText(relativeRect(size, .25, .12, .55, .15), Qt::AlignCenter,
                     QStringLiteral("Recovery overlay / Pin"));
    return images;
}
QString hit(const QSize& size, const QPointF& point, bool doubleClick) {
    if (relativeRect(size, .85, .12, .04, .07).contains(point)) return QStringLiteral("pin");
    if (doubleClick && relativeRect(size, .65, .62, .30, .30).contains(point)) return QStringLiteral("pip");
    return doubleClick ? QStringLiteral("main") : QString();
}
struct Metrics {
    int initialized = 0;
    int painted = 0;
    int swapped = 0;
    int destroyed = 0;
    int releasedCurrent = 0;
    int abandoned = 0;
    bool destroyedInPaint = false;
    bool isEs = false;
    QString glVersion;
    QString renderer;
    QJsonArray channelBits;
    QSize lastPixelSize;
};

class CpuCanvas final : public QWidget {
public:
    using QWidget::QWidget;
    std::function<void(QString)> input;
protected:
    void paintEvent(QPaintEvent*) override {
        const auto images = sceneImages(size(), devicePixelRatioF());
        QPainter painter(this);
        painter.drawImage(rect(), images.video);
        painter.drawImage(rect(), images.overlay);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && input) input(hit(size(), e->localPos(), false));
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && input) input(hit(size(), e->localPos(), true));
    }
};

class GlCanvas final : public QOpenGLWindow {
public:
    explicit GlCanvas(std::shared_ptr<Metrics> metrics, bool failInitialization)
        : QOpenGLWindow(NoPartialUpdate), metrics_(std::move(metrics)), failInitialization_(failInitialization) {
        // Request channel precision on this GL child only: a global alpha
        // request also changes the native format of ordinary raster windows.
        auto format = QSurfaceFormat::defaultFormat();
        format.setRedBufferSize(8); format.setGreenBufferSize(8);
        format.setBlueBufferSize(8); format.setAlphaBufferSize(8);
        setFormat(format);
        connect(this, &QOpenGLWindow::frameSwapped, this, [this] { ++metrics_->swapped; });
    }
    ~GlCanvas() override {
        metrics_->destroyedInPaint |= inPaint_;
        disconnect(contextConnection_);
        cleanup();
        ++metrics_->destroyed;
    }
    std::function<void()> ready;
    std::function<void(QString)> failed;
    std::function<void(QString)> input;
    void failNextPaint() { failPaint_ = true; update(); }
protected:
    void resizeGL(int, int) override {
        // Let the native child/EGL surface finish its resize before requesting
        // a frame. With an on-demand renderer there may be no next video tick.
        QTimer::singleShot(0, this, [this] { if (!failed_) update(); });
    }
    void initializeGL() override {
        ++metrics_->initialized;
        if (!context() || !context()->isValid()) { fail(QStringLiteral("context-unavailable")); return; }
        gl_ = context()->extraFunctions();
        gl_->initializeOpenGLFunctions();
        metrics_->isEs = context()->isOpenGLES();
        metrics_->glVersion = QString::fromLatin1(reinterpret_cast<const char*>(gl_->glGetString(GL_VERSION)));
        metrics_->renderer = QString::fromLatin1(reinterpret_cast<const char*>(gl_->glGetString(GL_RENDERER)));
        contextConnection_ = connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
            cleanup();
            fail(QStringLiteral("context-destroyed"));
        }, Qt::DirectConnection);
        if (failInitialization_) { fail(QStringLiteral("injected-init-failure")); return; }
        const auto format = context()->format();
        metrics_->channelBits = QJsonArray{format.redBufferSize(), format.greenBufferSize(),
                                          format.blueBufferSize(), format.alphaBufferSize()};
        if (format.majorVersion() < 3 || (!metrics_->isEs && format.majorVersion() == 3 && format.minorVersion() < 3)) {
            fail(QStringLiteral("unsupported-gl-profile")); return;
        }
        const QByteArray prefix = metrics_->isEs ? "#version 300 es\nprecision highp float;\n" : "#version 330 core\n";
        const QByteArray vertex = prefix +
            "out vec2 uv; void main() { vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
            "uv=vec2(p.x,1.0-p.y); gl_Position=vec4(p*2.0-1.0,0.0,1.0); }";
        const QByteArray fragment = prefix +
            "in vec2 uv; uniform sampler2D pixels; out vec4 color; void main(){ color=texture(pixels,uv); }";
        const GLuint vs = compile(GL_VERTEX_SHADER, vertex);
        const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
        if (!vs || !fs) {
            if (vs) gl_->glDeleteShader(vs);
            if (fs) gl_->glDeleteShader(fs);
            fail(QStringLiteral("shader-compile")); return;
        }
        program_ = gl_->glCreateProgram();
        gl_->glAttachShader(program_, vs); gl_->glAttachShader(program_, fs);
        gl_->glLinkProgram(program_);
        gl_->glDeleteShader(vs); gl_->glDeleteShader(fs);
        GLint linked = 0;
        gl_->glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (!linked) { fail(QStringLiteral("shader-link")); return; }
        gl_->glGenVertexArrays(1, &vao_);
        gl_->glGenTextures(2, textures_.data());
        for (auto texture : textures_) {
            gl_->glBindTexture(GL_TEXTURE_2D, texture);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    void paintGL() override {
        struct PaintGuard { bool& value; explicit PaintGuard(bool& v) : value(v) { value = true; }
                            ~PaintGuard() { value = false; } } guard(inPaint_);
        if (failed_ || !program_) return;
        if (failPaint_) { fail(QStringLiteral("injected-paint-failure")); return; }
        const QSize pixels(qRound(width() * devicePixelRatio()), qRound(height() * devicePixelRatio()));
        if (pixels.isEmpty()) return;
        gl_->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        gl_->glViewport(0, 0, pixels.width(), pixels.height());
        gl_->glDisable(GL_DEPTH_TEST); gl_->glDisable(GL_SCISSOR_TEST);
        gl_->glClearColor(0, 0, 0, 1); gl_->glClear(GL_COLOR_BUFFER_BIT);
        gl_->glUseProgram(program_); gl_->glBindVertexArray(vao_);
        gl_->glActiveTexture(GL_TEXTURE0);
        gl_->glUniform1i(gl_->glGetUniformLocation(program_, "pixels"), 0);
        if (uploadedSize_ != pixels) {
            const auto images = sceneImages(size(), devicePixelRatio());
            const std::array<const QImage*, 2> sources{{&images.video, &images.overlay}};
            gl_->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl_->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            for (size_t i = 0; i != sources.size(); ++i) {
                gl_->glBindTexture(GL_TEXTURE_2D, textures_[i]);
                gl_->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pixels.width(), pixels.height(), 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, sources[i]->constBits());
            }
            uploadedSize_ = pixels;
        }
        gl_->glDisable(GL_BLEND);
        gl_->glBindTexture(GL_TEXTURE_2D, textures_[0]); gl_->glDrawArrays(GL_TRIANGLES, 0, 3);
        gl_->glEnable(GL_BLEND);
        gl_->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        gl_->glBindTexture(GL_TEXTURE_2D, textures_[1]); gl_->glDrawArrays(GL_TRIANGLES, 0, 3);
        gl_->glDisable(GL_BLEND);
        gl_->glBindVertexArray(0); gl_->glUseProgram(0);
        if (gl_->glGetError() != GL_NO_ERROR) { fail(QStringLiteral("gl-draw-error")); return; }
        ++metrics_->painted;
        metrics_->lastPixelSize = pixels;
        if (!announced_) {
            announced_ = true;
            QTimer::singleShot(0, this, [this] { if (!failed_ && ready) ready(); });
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && input) input(hit(size(), e->localPos(), false));
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && input) input(hit(size(), e->localPos(), true));
    }
private:
    GLuint compile(GLenum type, const QByteArray& source) {
        const GLuint shader = gl_->glCreateShader(type);
        const char* data = source.constData();
        gl_->glShaderSource(shader, 1, &data, nullptr); gl_->glCompileShader(shader);
        GLint compiled = 0; gl_->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) { gl_->glDeleteShader(shader); return 0; }
        return shader;
    }
    void fail(const QString& reason) {
        if (failed_) return;
        failed_ = true;
        if (failed) failed(reason); // Owner schedules teardown after this paint returns.
    }
    void cleanup() {
        if (!program_ && !vao_ && !textures_[0] && !textures_[1]) return;
        if (context() && context()->isValid()) {
            makeCurrent();
            if (QOpenGLContext::currentContext() == context()) {
                gl_->glDeleteTextures(2, textures_.data());
                if (vao_) gl_->glDeleteVertexArrays(1, &vao_);
                if (program_) gl_->glDeleteProgram(program_);
                ++metrics_->releasedCurrent;
                doneCurrent();
            } else { ++metrics_->abandoned; }
        } else { ++metrics_->abandoned; }
        program_ = vao_ = 0; textures_ = {}; uploadedSize_ = {}; gl_ = nullptr;
    }
    std::shared_ptr<Metrics> metrics_;
    QOpenGLExtraFunctions* gl_ = nullptr;
    QMetaObject::Connection contextConnection_;
    GLuint program_ = 0, vao_ = 0;
    std::array<GLuint, 2> textures_{};
    QSize uploadedSize_;
    bool failInitialization_ = false, failPaint_ = false, failed_ = false, announced_ = false, inPaint_ = false;
};

class Stage final : public QWidget {
public:
    enum class State { Cpu, Waiting, Ready, Failed };
    explicit Stage(QWidget* parent = nullptr) : QWidget(parent), cpu(new CpuCanvas(this)) {
        cpu->input = [this](const QString& action) { recordInput(action); };
    }
    ~Stage() override { retireSurface(); }
    void start(bool failInitialization = false) {
        retireSurface();
        lastFailure.clear(); state = State::Waiting;
        const auto current = ++generation;
        gl = new GlCanvas(metrics, failInitialization);
        gl->ready = [this, current] {
            if (current != generation || state != State::Waiting) return;
            state = State::Ready; cpu->hide();
        };
        gl->failed = [this, current](const QString& reason) {
            if (current != generation || state == State::Failed) return;
            state = State::Failed; lastFailure = reason;
            QTimer::singleShot(0, this, [this, current] {
                if (current != generation) return;
                retireSurface(); state = State::Cpu;
            });
        };
        gl->input = [this, current](const QString& action) {
            if (current == generation && state == State::Ready) recordInput(action);
        };
        container = QWidget::createWindowContainer(gl, this);
        container->setFocusPolicy(Qt::StrongFocus);
        container->setGeometry(rect()); container->show();
    }
    void retireSurface() {
        ++generation; // Invalidates queued Ready/Failed/input work before destruction.
        if (gl) { gl->ready = {}; gl->failed = {}; gl->input = {}; }
        delete container; container = nullptr; gl = nullptr; // Container owns QWindow.
        cpu->setGeometry(rect()); cpu->show(); cpu->update();
    }
    void recordInput(const QString& action) {
        if (action == QStringLiteral("pin")) ++pins;
        if (action == QStringLiteral("pip")) ++pipClicks;
    }
    CpuCanvas* cpu;
    QWidget* container = nullptr;
    QPointer<GlCanvas> gl;
    std::shared_ptr<Metrics> metrics = std::make_shared<Metrics>();
    State state = State::Cpu;
    QString lastFailure;
    uint64_t generation = 0;
    int pins = 0, pipClicks = 0;
protected:
    void resizeEvent(QResizeEvent*) override {
        cpu->setGeometry(rect());
        if (container) container->setGeometry(rect());
    }
};

class ProbeWindow final : public QWidget {
public:
    ProbeWindow() {
        setWindowTitle(QStringLiteral("LiveKit P0 - isolated GL surface"));
        auto* layout = new QVBoxLayout(this);
        auto* controls = new QHBoxLayout;
        auto* fail = new QPushButton(QStringLiteral("Fail GPU -> CPU"), this);
        auto* restart = new QPushButton(QStringLiteral("Create GL surface"), this);
        controls->addWidget(fail); controls->addWidget(restart);
        layout->addLayout(controls);
        stage = new Stage(this); layout->addWidget(stage, 1);
        auto* note = new QLabel(QStringLiteral("P0 probe: RGBA video + premultiplied recovery overlay; no meeting connection."), this);
        layout->addWidget(note);
        connect(fail, &QPushButton::clicked, this, [this] { if (stage->gl) stage->gl->failNextPaint(); });
        connect(restart, &QPushButton::clicked, this, [this] { stage->start(); });
        resize(720, 500);
    }
    Stage* stage;
};

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000) {
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}
void settle() {
    QElapsedTimer elapsed; elapsed.start();
    waitUntil([&] { return elapsed.elapsed() >= 150; }, 300);
}
QImage captureStage(ProbeWindow& window) {
    // Capture the actual native child composition, not QWidget::grab(), which
    // cannot establish whether an independently presented GL child was visible.
    auto image = window.screen()->grabWindow(window.winId()).toImage();
    const auto offset = window.stage->mapTo(&window, QPoint());
    const double sx = double(image.width()) / window.width();
    const double sy = double(image.height()) / window.height();
    return image.copy(qRound(offset.x() * sx), qRound(offset.y() * sy),
                      qRound(window.stage->width() * sx), qRound(window.stage->height() * sy));
}
void sendInput(QObject* receiver, const QSize& size, QEvent::Type type, double x, double y) {
    const QPointF position(x * size.width(), y * size.height());
    QMouseEvent event(type, position, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(receiver, &event); // Deterministic synthetic input, not physical mouse evidence.
}
int selfTest(ProbeWindow& window, const QString& outputDir) {
    QJsonArray checks;
    QJsonObject report;
    QDir().mkpath(outputDir);
    auto require = [&](bool value, const QString& name) {
        checks.append(QJsonObject{{"case", name}, {"result", value ? "PASS" : "FAIL"}});
        std::cout << (value ? "PASS " : "FAIL ") << name.toStdString() << std::endl;
        if (!value) throw std::runtime_error(name.toStdString());
    };
    auto pixels = [&](const QImage& image, const QString& name) {
        require(!image.isNull(), name + " capture");
        const std::array<QPointF, 5> positions{{{.05, .5}, {.55, .5}, {.7, .75}, {.2, .15}, {.87, .15}}};
        const std::array<QColor, 5> expected{{QColor(220,40,30), QColor(20,170,80), QColor(30,70,220),
                                            QColor(238,148,143), QColor(250,220,30)}};
        for (size_t i = 0; i != positions.size(); ++i) {
            const auto actual = image.pixelColor(qRound(positions[i].x()*image.width()), qRound(positions[i].y()*image.height()));
            const bool match = qAbs(actual.red()-expected[i].red()) <= 3 &&
                qAbs(actual.green()-expected[i].green()) <= 3 && qAbs(actual.blue()-expected[i].blue()) <= 3;
            if (!match) std::cerr << "pixel " << i << " actual " << actual.name().toStdString() << std::endl;
            require(match, name + " pixel " + QString::number(i));
        }
    };
    int result = 0;
    auto* stage = window.stage;
    try {
        require(waitUntil([&] { return window.windowHandle() && window.windowHandle()->isExposed(); }), "raster window exposed");
        const auto initialSurfaceType = window.windowHandle()->surfaceType();
        report["initial_surface_type"] = int(initialSurfaceType);
        // Qt Windows may mark ordinary QWidget windows RasterGLSurface even
        // before any GL child exists. That flag permits GL; it does not prove
        // the backing store is using GL composition. Verify actual CPU pixels
        // before GL creation and after GL destruction on the same top-level.
        require(initialSurfaceType == QSurface::RasterSurface || initialSurfaceType == QSurface::RasterGLSurface,
                "top-level supports raster painting");
        settle(); const auto initialCpu = captureStage(window);
        initialCpu.save(outputDir + "/cpu-before-gl.png"); pixels(initialCpu, "CPU before GL creation");
        stage->start();
        require(waitUntil([&] { return stage->state == Stage::State::Ready || stage->state == Stage::State::Cpu; }), "asynchronous initialization completes");
        require(stage->state == Stage::State::Ready, "actual GL context ready: " + stage->lastFailure);
        require(waitUntil([&] { return stage->metrics->swapped > 0; }), "GL frame swapped");
        settle();
        auto screen = captureStage(window);
        screen.save(outputDir + "/gpu-native.png"); pixels(screen, "native GL + overlay");
        auto framebuffer = stage->gl->grabFramebuffer(); // Test-only readback, never a display path.
        framebuffer.save(outputDir + "/gpu-framebuffer.png"); pixels(framebuffer, "GL framebuffer");
        sendInput(stage->gl, stage->gl->size(), QEvent::MouseButtonRelease, .87, .15);
        sendInput(stage->gl, stage->gl->size(), QEvent::MouseButtonDblClick, .7, .75);
        require(stage->pins == 1 && stage->pipClicks == 1, "GL Pin and frontmost PiP synthetic input");

        // Native child stacking: record whether a raised sibling widget is
        // actually visible; the supported overlay above was drawn in the GL target.
        QLabel sibling(stage);
        sibling.setStyleSheet("background: #ff00ff;"); sibling.setGeometry(8, 8, 35, 25);
        sibling.show(); sibling.raise(); settle();
        const auto siblingShot = captureStage(window);
        const double dpr = double(siblingShot.width()) / stage->width();
        report["raised_sibling_widget_visible"] = siblingShot.pixelColor(qRound(20*dpr), qRound(18*dpr)) == QColor(255,0,255);
        sibling.hide();

        QMenu menu(&window); menu.addAction(QStringLiteral("Native popup above GL"));
        menu.popup(stage->mapToGlobal(QPoint(50, 100)));
        require(waitUntil([&] { return menu.isVisible(); }), "popup above native child opens");
        menu.hide();
        stage->container->setFocus(); stage->gl->requestActivate(); settle();
        require(stage->container->hasFocus() || QGuiApplication::focusWindow() == stage->gl, "native child focus");

        const int beforeResize = stage->metrics->painted;
        window.resize(903, 587);
        require(waitUntil([&] {
            const QSize expected(qRound(stage->width()*stage->gl->devicePixelRatio()),
                                 qRound(stage->height()*stage->gl->devicePixelRatio()));
            return stage->metrics->painted > beforeResize && stage->metrics->lastPixelSize == expected;
        }), "resize redraw at current physical dimensions");
        settle(); auto resized = captureStage(window); resized.save(outputDir + "/gpu-resized.png"); pixels(resized, "resized GL");
        window.hide(); settle();
        require(stage->state == Stage::State::Ready, "hidden surface is not failure");
        window.show(); stage->gl->update();
        require(waitUntil([&] { return stage->gl && stage->gl->isExposed(); }), "hidden surface re-exposed");
        settle(); pixels(captureStage(window), "shown GL");

        const auto rootId = window.winId();
        const auto oldGeneration = stage->generation;
        QPointer<GlCanvas> oldSurface = stage->gl;
        stage->gl->failNextPaint();
        require(waitUntil([&] { return stage->state == Stage::State::Cpu && oldSurface.isNull(); }), "paint failure destroys GL and restores CPU");
        require(!stage->metrics->destroyedInPaint, "teardown occurs after paint returns");
        require(stage->generation > oldGeneration, "retired surface generation invalidated");
        require(stage->metrics->releasedCurrent > 0, "GPU resources released with owning context current");
        require(window.winId() == rootId && window.windowHandle()->surfaceType() == initialSurfaceType,
                "CPU fallback preserves top-level and its surface type");
        settle(); auto cpu = captureStage(window); cpu.save(outputDir + "/cpu-after-failure.png"); pixels(cpu, "CPU after GPU failure");
        sendInput(stage->cpu, stage->cpu->size(), QEvent::MouseButtonRelease, .87, .15);
        sendInput(stage->cpu, stage->cpu->size(), QEvent::MouseButtonDblClick, .7, .75);
        require(stage->pins == 2 && stage->pipClicks == 2, "CPU retains Pin/PiP input");

        stage->start(true);
        require(waitUntil([&] { return stage->state == Stage::State::Cpu && !stage->gl; }), "injected init failure restores CPU");
        require(stage->lastFailure == QStringLiteral("injected-init-failure"), "init failure reason retained");
        settle(); pixels(captureStage(window), "CPU after init failure");
        stage->start(); // Queue initialization and immediately retire it.
        stage->retireSurface(); stage->state = Stage::State::Cpu;
        settle(); require(!stage->gl && stage->state == Stage::State::Cpu, "queued surface work cannot revive retired owner");
        require(window.winId() == rootId, "no top-level reconstruction");
    } catch (const std::exception& e) {
        report["failure"] = QString::fromUtf8(e.what()); result = 1;
    }
    const auto& m = *stage->metrics;
    report["result"] = result == 0 ? "PASS" : "FAIL";
    report["platform"] = QGuiApplication::platformName();
    report["qt_version"] = QT_VERSION_STR;
    report["is_gles"] = m.isEs;
    report["gl_version"] = m.glVersion;
    report["renderer"] = m.renderer;
    report["rgba_channel_bits"] = m.channelBits;
    report["device_pixel_ratio"] = window.devicePixelRatioF();
    report["initialized"] = m.initialized; report["painted"] = m.painted; report["swapped"] = m.swapped;
    report["destroyed"] = m.destroyed; report["released_current"] = m.releasedCurrent; report["abandoned"] = m.abandoned;
    report["checks"] = checks;
    report["limits"] = QJsonArray{"NOT_RUN: real device/context loss", "NOT_RUN: cross-monitor DPI",
        "NOT_RUN: physical mouse input", "NOT_RUN: Linux/macOS", "NOT_RUN: real meeting/video decoding"};
    QFile file(outputDir + "/surface-result.json");
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0) return 1;
    std::cout << "Evidence: " << outputDir.toStdString() << std::endl;
    return result;
}
} // namespace

int main(int argc, char** argv) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QSurfaceFormat format;
#ifdef QT_OPENGL_ES
    format.setRenderableType(QSurfaceFormat::OpenGLES); format.setVersion(3, 0);
#else
    format.setRenderableType(QSurfaceFormat::OpenGL);
#ifdef Q_OS_MACOS
    format.setVersion(4, 1);
#else
    format.setVersion(3, 3);
#endif
    format.setProfile(QSurfaceFormat::CoreProfile);
#endif
    format.setDepthBufferSize(0); format.setStencilBufferSize(0); format.setSamples(0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    ProbeWindow window; window.show();
    if (app.arguments().contains(QStringLiteral("--self-test"))) {
        QString output = QDir::currentPath() + "/render-p0-evidence";
        const int index = app.arguments().indexOf(QStringLiteral("--output"));
        if (index >= 0 && index + 1 < app.arguments().size()) output = app.arguments()[index + 1];
        return selfTest(window, output);
    }
    window.stage->start();
    return app.exec();
}
