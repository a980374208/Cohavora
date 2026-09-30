#include "src/ui/render/gl_video_canvas.h"
#include "tests/support/test_check.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtGui/QWindow>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QScreen>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QDateTime>
#include <iostream>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <d3d11.h>
#include <wrl/client.h>

void RunOpenGlContract();
class ParticipantWindowTestAccess {
public:
    static QWindow* surface(livekit::render::GlVideoCanvas* canvas) { return canvas->windowSurface(); }
    static void presentHook(livekit::render::GlVideoCanvas* canvas, std::function<void()> hook) { canvas->setBeforePresentForTest(std::move(hook)); }
    static bool idle() { return livekit::render::GlVideoCanvas::workersIdleForTest(); }
    static void submitScene(livekit::render::GlVideoCanvas* canvas) { canvas->requestRender(); }
    static bool presented(livekit::render::GlVideoCanvas* canvas) { return canvas->scenePresentedForTest(); }
};
namespace {
using namespace livekit::render;
bool Wait(const std::function<bool()>& done, int timeout=5000) {
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed()<timeout) QApplication::processEvents(QEventLoop::AllEvents,20);
    return done();
}
void Settle() { QElapsedTimer t; t.start(); TEST_CHECK(Wait([&] { return t.elapsed()>180; },1000)); }
VideoRenderFrame::Ptr Frame(livekit::VideoBufferType type, bool blue=false) {
    auto f=livekit::VideoFrame::create(9,7,type);
    if(type==livekit::VideoBufferType::RGBA) {
        for(size_t i=0;i<f.dataSize();i+=4) {f.data()[i]=blue?20:240;f.data()[i+1]=20;f.data()[i+2]=blue?240:20;f.data()[i+3]=255;}
    } else {
        std::fill(f.data(),f.data()+63,uint8_t(81));
        for(size_t i=63;i<f.dataSize();++i) f.data()[i]=type==livekit::VideoBufferType::I420?(i<83?90:240):((i-63)%2?240:90);
    }
    return VideoRenderFrame::CopyFrom(f);
}
struct Window : QWidget {
    QLabel title{this},cpu{this},banner{this};
    GlVideoCanvas* canvas=nullptr;
    int ready=0,failed=0,swaps=0;
    bool cpuActive=false;
    Window() {
        setWindowTitle("LiveKit desktop OpenGL validation");
        title.setText("Production OpenGL module + Qt desktop surface");
        cpu.setStyleSheet("background: rgb(20, 80, 220); color: white;");
        cpu.setText("Qt CPU fallback"); cpu.setAlignment(Qt::AlignCenter);cpu.hide();
        banner.setStyleSheet("background: rgb(20, 220, 60); color: black;");
        banner.setText("Recovery overlay / desktop OpenGL");
        canvas=dynamic_cast<GlVideoCanvas*>(CreateVideoCanvas(this)); TEST_CHECK(canvas);
        canvas->setStageOverlay(&banner);
        connect(canvas,&VideoCanvas::rendererInitialized,this,[&]{++ready;});
        connect(canvas,&VideoCanvas::rendererUnavailable,this,[&]{
            ++failed;canvas->shutdownRenderer();canvas->hide();cpu.show();cpu.raise();banner.raise();cpuActive=true;
        },Qt::QueuedConnection);
        connect(canvas,&GlVideoCanvas::framePresented,this,[&]{++swaps;});
        resize(860,620); arrange();
    }
    void arrange() {
        title.setGeometry(12,0,width()-24,32);
        const QRect stage(0,32,width(),height()-32);
        cpu.setGeometry(stage);canvas->setGeometry(stage);
        banner.setGeometry(30,52,350,36);
        canvas->setTilesLayout({{"main",0,0,stage.width(),stage.height(),false,0,true},
            {"pip",stage.width()-210,stage.height()-165,190,145,false,0,true}});
        canvas->setTileDecoration("pip",[](const QSize& size,bool,bool){
            QImage image(size,QImage::Format_RGBA8888_Premultiplied);image.fill(Qt::transparent);
            QPainter painter(&image);painter.setPen(QPen(Qt::yellow,4));painter.drawRect(image.rect().adjusted(2,2,-2,-2));return image;
        },QRect(4,4,30,25));
    }
    void resizeEvent(QResizeEvent*) override { if(canvas) arrange(); }
    QImage capture(const QString& directory,const QString& name) {
        Settle();
        const auto image=screen()->grabWindow(winId()).toImage();TEST_CHECK(!image.isNull());
        TEST_CHECK(image.save(QDir(directory).filePath(name+".png")));return image;
    }
    QColor pixel(const QImage& image,int x,int y) const {
        return image.pixelColor(x*image.width()/width(),y*image.height()/height());
    }
};
void SurfaceProbe(const QString& directory,bool waitLoss) {
    TEST_CHECK(QDir().mkpath(directory));
    Window window;window.show();
    TEST_CHECK(Wait([&]{return window.ready==1&&window.swaps>0;}));
    TEST_CHECK(window.canvas->rendererReady()&&!window.failed);
    auto* surface=ParticipantWindowTestAccess::surface(window.canvas);
    TEST_CHECK(!window.canvas->driverDescription().contains("OpenGL ES"));
    std::cout<<"DESKTOP_GL_DRIVER "<<window.canvas->driverDescription().toStdString()<<"\n";
    const auto native=window.winId();
    window.canvas->updateFrame("pip",Frame(livekit::VideoBufferType::RGBA,true));
    for(auto format:{livekit::VideoBufferType::RGBA,livekit::VideoBufferType::I420,livekit::VideoBufferType::NV12}) {
        const int swaps=window.swaps;window.canvas->updateFrame("main",Frame(format));
        TEST_CHECK(Wait([&]{return window.swaps>swaps;}));
        auto image=window.capture(directory,QString("native-format-%1").arg(int(format)));
        auto color=window.pixel(image,window.width()/2,window.height()/2);
        TEST_CHECK(color.red()>220&&color.green()<40&&color.blue()<40);
        color=window.pixel(image,window.width()-115,window.height()-90);
        TEST_CHECK(color.blue()>220&&color.red()<40);
        color=window.pixel(image,40,60);TEST_CHECK(color.green()>200&&color.red()<40);
    }
    int pins=0,doubles=0;
    QObject::connect(window.canvas,&VideoCanvas::tilePinRequested,&window,[&](const QString& key){TEST_CHECK(key=="pip");++pins;});
    QObject::connect(window.canvas,&VideoCanvas::tileDoubleClicked,&window,[&](const QString& key){TEST_CHECK(key=="pip");++doubles;});
    const QPoint pin(window.width()-200,window.canvas->height()-155);
    QMouseEvent press(QEvent::MouseButtonPress,pin,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease,pin,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(surface,&press);QApplication::sendEvent(surface,&release);TEST_CHECK(pins==1);
    QMouseEvent dbl(QEvent::MouseButtonDblClick,QPoint(window.width()-110,window.canvas->height()-90),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(surface,&dbl);TEST_CHECK(doubles==1);
    window.resize(920,680);auto image=window.capture(directory,"native-resized");
    TEST_CHECK(window.pixel(image,460,340).red()>220&&native==window.winId());
    window.banner.hide();image=window.capture(directory,"native-overlay-hidden");
    TEST_CHECK(window.pixel(image,40,60).green()<200);
    window.hide();QApplication::processEvents();window.show();image=window.capture(directory,"native-reshown");
    TEST_CHECK(window.pixel(image,460,340).red()>220);
    if(waitLoss) {
        // Independent OS/runtime witness: a host failure signal alone is not
        // evidence of a real driver reset. This device is never removed by us.
        Microsoft::WRL::ComPtr<ID3D11Device> witness;
        TEST_CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
            nullptr,0,D3D11_SDK_VERSION,witness.GetAddressOf(),nullptr,nullptr)));
        TEST_CHECK(witness->GetDeviceRemovedReason()==S_OK);
        QFile ready(QDir(directory).filePath("ready-for-driver-loss.txt"));TEST_CHECK(ready.open(QIODevice::WriteOnly));
        ready.write("Native desktop GL is presenting. Waiting for EXTERNAL authorized driver reset; this process never triggers a reset.\n");ready.close();
        std::cout<<"WAITING_FOR_REAL_DRIVER_LOSS pid="<<QCoreApplication::applicationPid()<<std::endl;
        QTimer producer;QObject::connect(&producer,&QTimer::timeout,&window,[&]{window.canvas->updateFrame("main",Frame(livekit::VideoBufferType::RGBA));});producer.start(33);
        HRESULT reason=S_OK;
        int uiTicks=0;
        qint64 maxUiGap=0;
        QElapsedTimer uiClock;uiClock.start();qint64 lastTick=0;
        const auto statePath=QDir(directory).filePath("driver-observer.json");
        const auto writeState=[&](const QString& phase) {
            QSaveFile state(statePath);TEST_CHECK(state.open(QIODevice::WriteOnly));
            state.write(QJsonDocument(QJsonObject{
                {"pid",double(QCoreApplication::applicationPid())},
                {"timestampMs",double(QDateTime::currentMSecsSinceEpoch())},
                {"phase",phase},{"reason",QString("0x%1").arg(quint32(witness->GetDeviceRemovedReason()),0,16)},
                {"gpuReady",window.canvas->rendererReady()},{"cpuActive",window.cpuActive},
                {"swaps",window.swaps},{"failures",window.failed},
                {"hostFailure",window.canvas->failureReason()},{"uiTicks",uiTicks},{"maxUiGapMs",double(maxUiGap)}
            }).toJson());TEST_CHECK(state.commit());
        };
        writeState("waiting");
        QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,&window,[&]{++uiTicks;const auto now=uiClock.elapsed();maxUiGap=std::max(maxUiGap,now-lastTick);lastTick=now;writeState("waiting");});heartbeat.start(250);
        const bool fallback=Wait([&]{reason=witness->GetDeviceRemovedReason();return FAILED(reason)&&window.cpuActive;},180000);producer.stop();
        heartbeat.stop();writeState(fallback?"observed":"timeout");
        std::cout<<"REAL_DRIVER_LOSS_OBSERVED reason=0x"<<std::hex<<static_cast<unsigned long>(reason)
            <<std::dec<<" fallback="<<fallback<<" failures="<<window.failed<<std::endl;
        TEST_CHECK(FAILED(reason)&&fallback&&window.failed==1&&native==window.winId());
        image=window.capture(directory,"native-cpu-after-driver-loss");
        TEST_CHECK(window.pixel(image,460,340).blue()>200);
        TEST_CHECK(uiTicks>0);
        QElapsedTimer responsive;responsive.start();const int ticksBefore=uiTicks;
        heartbeat.start(25);
        TEST_CHECK(Wait([&]{return uiTicks>=ticksBefore+10;},1500));heartbeat.stop();
        const int failures=window.failed,readyCount=window.ready;
        window.resize(940,700);window.hide();QApplication::processEvents();window.show();
        image=window.capture(directory,"native-cpu-responsive");
        TEST_CHECK(window.pixel(image,470,350).blue()>200&&window.failed==failures&&window.ready==readyCount);
        writeState("verified");
        std::cout<<"DESKTOP_GL_DRIVER_LOSS_CPU PASS uiTicks="<<uiTicks<<" maxUiGapMs="<<maxUiGap
            <<" hostFailure="<<window.canvas->failureReason().toStdString()<<"\n";
    }
    window.canvas->shutdownRenderer();window.canvas->hide();
    TEST_CHECK(!window.canvas->rendererReady());
    std::cout<<"DESKTOP_GL_SURFACE PASS: native RGBA/I420/NV12 pixels, overlay/PiP, Pin input, swap, resize, hide/show, teardown; DPR="<<window.devicePixelRatioF()<<std::endl;
}
void BlockedPresentProbe(const QString& directory) {
    TEST_CHECK(QDir().mkpath(directory));
    auto window=std::make_unique<Window>();window->show();
    TEST_CHECK(Wait([&]{return window->ready==1;}));
    window->canvas->updateFrame("main",Frame(livekit::VideoBufferType::RGBA));
    struct Gate {std::mutex mutex;std::condition_variable wake;bool released=false;std::atomic<bool> entered{false};};
    auto gate=std::make_shared<Gate>();
    ParticipantWindowTestAccess::presentHook(window->canvas,[gate]{
        gate->entered.store(true);std::unique_lock<std::mutex> lock(gate->mutex);
        gate->wake.wait(lock,[&]{return gate->released;});
    });
    TEST_CHECK(Wait([&]{return gate->entered.load();}));
    std::vector<std::weak_ptr<const VideoRenderFrame>> frames;
    for(int i=0;i<200;++i) {
        // Replacing a queued snapshot must preserve removals and the final frame
        // while dropping intermediate owners, even when presentation is stalled.
        if(i%10==0) window->canvas->removeUser("main");
        auto frame=Frame(livekit::VideoBufferType::RGBA,true);frames.push_back(frame);
        window->canvas->updateFrame("main",std::move(frame));
        ParticipantWindowTestAccess::submitScene(window->canvas);
    }
    TEST_CHECK(std::count_if(frames.begin(),frames.end(),[](const auto& f){return !f.expired();})==1);
    ParticipantWindowTestAccess::presentHook(window->canvas,{});
    {std::lock_guard<std::mutex> lock(gate->mutex);gate->released=true;}gate->wake.notify_one();
    TEST_CHECK(Wait([&]{return ParticipantWindowTestAccess::presented(window->canvas);}));
    auto latest=window->capture(directory,"latest-scene-after-block");
    const auto latestColor=window->pixel(latest,430,310);
    TEST_CHECK(latestColor.blue()>220&&latestColor.green()<40&&window->failed==0);
    gate=std::make_shared<Gate>();
    ParticipantWindowTestAccess::presentHook(window->canvas,[gate]{
        gate->entered.store(true);std::unique_lock<std::mutex> lock(gate->mutex);
        gate->wake.wait(lock,[&]{return gate->released;});
    });
    TEST_CHECK(Wait([&]{return gate->entered.load();}));
    int ticks=0;QTimer timer;QObject::connect(&timer,&QTimer::timeout,window.get(),[&]{++ticks;});timer.start(20);
    const auto native=window->winId();
    TEST_CHECK(Wait([&]{return window->cpuActive;},4000));
    TEST_CHECK(window->canvas->failureReason()=="presentation-timeout"&&window->failed==1&&window->ready==1&&ticks>30);
    const auto image=window->capture(directory,"blocked-present-cpu");
    TEST_CHECK(window->pixel(image,430,310).blue()>200&&window->pixel(image,430,310).green()>60&&window->winId()==native);
    QElapsedTimer close;close.start();window.reset();
    TEST_CHECK(close.elapsed()<500); // Gate is STILL held; teardown must not join.
    // A stuck driver cannot accumulate fresh GL owners on subsequent meetings.
    Window retry;retry.show();TEST_CHECK(Wait([&]{return retry.cpuActive;}));
    TEST_CHECK(retry.failed==1&&retry.ready==0);
    {std::lock_guard<std::mutex> lock(gate->mutex);gate->released=true;}gate->wake.notify_one();
    TEST_CHECK(Wait([]{return ParticipantWindowTestAccess::idle();}));
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    TEST_CHECK(retry.failed==1&&retry.ready==0);
    std::cout<<"BLOCKED_PRESENT PASS: UI ticks="<<ticks<<", CPU pixels, nonblocking teardown, no late activation, bounded quarantine\n";
}

}
int main(int argc,char** argv) {
    QApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    qputenv("LIVEKIT_RENDER_BACKEND","opengl");
    QApplication app(argc,argv);
    TEST_CHECK(QOpenGLContext::openGLModuleType()==QOpenGLContext::LibGL);
    if(app.arguments().contains("--contract")) RunOpenGlContract();
    else {
        const auto index=app.arguments().indexOf("--output");TEST_CHECK(index>=0&&index+1<app.arguments().size());
        if(app.arguments().contains("--block-present")) BlockedPresentProbe(app.arguments()[index+1]);
        else SurfaceProbe(app.arguments()[index+1],app.arguments().contains("--wait-driver-loss"));
        if(!app.arguments().contains("--wait-driver-loss")) TEST_CHECK(Wait([]{return ParticipantWindowTestAccess::idle();}));
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
}
