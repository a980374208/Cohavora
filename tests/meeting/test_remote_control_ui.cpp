#include "src/core/meeting_coordinator.h"
#include "src/ui/remote_control_ui.h"
#include "src/ui/render/video_surface_input.h"
#include "src/ui/app_theme.h"
#include "tests/support/test_check.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QPushButton>
#include <QtGui/QMouseEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QWheelEvent>
#include <QtGui/QWindow>
#include <QtPlugin>
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)

// Exercise the same native QWindow -> QWidget bridge used by both GPU surfaces.
class InputSurface final : public QWindow {
public:
    explicit InputSurface(QWidget& host) : host_(host) {}
protected:
    bool event(QEvent* event) override {
        return livekit::render::ForwardVideoSurfaceInput(&host_,event) || QWindow::event(event);
    }
private:
    QWidget& host_;
};

namespace OpenMeeting {
class SessionManagerTestAccess {
public:
    using Ptr = std::unique_ptr<SessionManager,void(*)(SessionManager*)>;
    static Ptr create(const QString& path) {
        return Ptr(new SessionManager(std::make_unique<QSettings>(path,QSettings::IniFormat)),
            [](SessionManager* s){delete s;});
    }
};
class MeetingCoordinatorTestAccess {
public:
    static std::unique_ptr<MeetingCoordinator> create(SessionManager& s) {
        return std::unique_ptr<MeetingCoordinator>(new MeetingCoordinator(s,{},nullptr));
    }
    static void bind(MeetingCoordinator& c,std::shared_ptr<MeetingSessionRuntime> runtime) {
        c._state=MeetingState::InMeeting; c._sessionRuntime=std::move(runtime);
    }
};
}
int main(int argc,char** argv) {
    QApplication app(argc,argv); app.setQuitOnLastWindowClosed(false);
    MeetingUI::AppTheme::install(app);
    QTemporaryDir directory; TEST_CHECK(directory.isValid());
    auto session = OpenMeeting::SessionManagerTestAccess::create(directory.filePath("settings.ini"));
    auto coordinator = OpenMeeting::MeetingCoordinatorTestAccess::create(*session);
    namespace rc=livekit::remote_control;
    {
    QWidget stage; stage.resize(800,600);
    bool annotationBlocked = false;
    QString hostStatus;
    MeetingUI::RemoteControlUi ui(&stage,coordinator.get(),[]{return std::vector<MeetingUI::RemoteControlTarget>{};},
        [&](bool value){annotationBlocked=value;},[&](const QString& value){hostStatus=value;});
    rc::Projection pending{rc::State::AwaitingConsent,rc::Reason::None,"alice","request-1","","track-1"};
    emit coordinator->remoteControlChanged(pending);
    auto* dialog=stage.findChild<QDialog*>("remoteControlConsent"); TEST_CHECK(dialog);
    TEST_CHECK(dialog->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    auto* allow=dialog->findChild<QPushButton*>("remoteControlAllow");
    TEST_CHECK(allow && !allow->isDefault() && !allow->autoDefault());
    TEST_CHECK(!annotationBlocked);
    auto controlled=pending; controlled.state=rc::State::Controlled; controlled.grant="grant-1";
    emit coordinator->remoteControlChanged(controlled);
    TEST_CHECK(annotationBlocked && ui.active());
    TEST_CHECK(!hostStatus.isEmpty() && !stage.findChild<QDialog*>("remoteControlStatus"));
    ui.stop(); TEST_CHECK(!ui.active() && !annotationBlocked && hostStatus.isEmpty());
    // A queued projection of the cancelled request must not reopen control.
    emit coordinator->remoteControlChanged(controlled); TEST_CHECK(!ui.active());
    emit coordinator->remoteControlChanged(rc::Projection{}); TEST_CHECK(!ui.active());
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    TEST_CHECK(stage.findChild<QDialog*>("remoteControlConsent")==nullptr);
    pending.request="request-2"; emit coordinator->remoteControlChanged(pending);
    TEST_CHECK(stage.findChild<QDialog*>("remoteControlConsent")!=nullptr);
    emit coordinator->stateChanged(OpenMeeting::MeetingState::Reconnecting,QString());
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    TEST_CHECK(!ui.active() && stage.findChild<QDialog*>("remoteControlConsent")==nullptr);
    }
    { // Real Qt event -> Coordinator -> session strand -> protocol wire.
        QWidget window; window.resize(800,600);
        QWidget video(&window); video.setGeometry(window.rect()); video.setAttribute(Qt::WA_NativeWindow);
        video.winId();
        InputSurface surface(video); surface.setParent(video.windowHandle()); surface.setGeometry(video.rect());
        auto context=std::make_shared<asio::io_context>();
        auto runtime=std::make_shared<OpenMeeting::MeetingSessionRuntime>(*context,91,QStringLiteral("local"),context);
        OpenMeeting::MeetingCoordinatorTestAccess::bind(*coordinator,runtime);
        std::vector<rc::Message> wire;
        rc::Projection latest;
        int requestIds = 0;
        auto control=std::make_shared<rc::Runtime>(rc::Runtime::Hooks{
            [&](std::string_view,std::string_view payload){wire.push_back(*rc::Decode(payload));return true;},
            {},[&]{return "qt-request-"+std::to_string(++requestIds);},[&](auto p){latest=p;},{}});
        runtime->post([runtime,control]{runtime->remoteControlOnStrand()=control;});
        context->poll(); context->restart();
        MeetingUI::RemoteControlUi inputUi(&video,coordinator.get(),[&]{
            return std::vector<MeetingUI::RemoteControlTarget>{{"bob","TR_screen","Bob",&video,QRectF(100,100,600,400)}};
        },[](bool){});
        window.show(); surface.show(); window.activateWindow();
        QCoreApplication::processEvents();
        const auto controlEpoch = runtime->remoteControlEpoch();
        control->request({"bob",1,1},[runtime,controlEpoch]{
            return runtime->remoteControlEpoch() == controlEpoch;
        },"TR_screen",rc::NowMs());
        auto grant=wire.back(); grant.kind=rc::Kind::Grant; grant.grant="qt-grant";
        grant.share="share"; grant.epoch=1; grant.width=600; grant.height=400;
        control->receive(grant,{"bob",1,1},[]{return true;},rc::NowMs());
        QWidget otherApplication; otherApplication.resize(160,100); otherApplication.show();
        otherApplication.activateWindow(); QCoreApplication::processEvents();
        emit coordinator->remoteControlChanged(latest);
        inputUi.noteFrame("TR_screen");
        QEventLoop settle; QTimer::singleShot(50,&settle,&QEventLoop::quit); settle.exec();
        TEST_CHECK(inputUi.active() && control->state()==rc::State::AwaitingActivation);
        TEST_CHECK(wire.size()==1 && wire.back().kind==rc::Kind::Request);
        auto* entry=window.findChild<QPushButton*>(QStringLiteral("remoteControlRequest"));
        TEST_CHECK(entry && entry->text()==QStringLiteral("结束远程控制"));
        window.activateWindow(); QCoreApplication::processEvents();
        // The first click activates the granted session but must not reach the
        // remote application. Its release may arrive after Ready is projected.
        QMouseEvent activateDown(QEvent::MouseButtonPress,QPointF(120,120),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&surface,&activateDown); context->poll(); context->restart();
        TEST_CHECK(control->state()==rc::State::Controlling && wire.back().kind==rc::Kind::Ready);
        emit coordinator->remoteControlChanged(latest);
        const auto afterReady=wire.size();
        QMouseEvent activateUp(QEvent::MouseButtonRelease,QPointF(120,120),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(&surface,&activateUp); context->poll(); context->restart();
        TEST_CHECK(wire.size()==afterReady);
        uint64_t hostSequence = 0;
        const auto acknowledge = [&](uint64_t epoch, bool paused, uint64_t focusRevision) {
            auto message=grant; message.kind=rc::Kind::InputState;
            message.sequence=++hostSequence; message.inputEpoch=epoch; message.paused=paused;
            message.challenge=focusRevision;
            control->receive(message,{"bob",1,1},[]{return true;},rc::NowMs());
            emit coordinator->remoteControlChanged(latest);
        };
        acknowledge(1,false,0);
        TEST_CHECK(window.isActiveWindow() && video.hasFocus());
        TEST_CHECK(inputUi.active());
        QKeyEvent shortcut(QEvent::ShortcutOverride,Qt::Key_A,Qt::NoModifier);
        shortcut.ignore(); QApplication::sendEvent(&surface,&shortcut);
        TEST_CHECK(shortcut.isAccepted());
        const auto beforeKeys=wire.size();
        // Physical scan codes survive the native bridge, including extended keys.
        for (const auto scan : {0x1eu,0x14du}) {
            const auto key = scan == 0x1e ? Qt::Key_A : Qt::Key_Right;
            QKeyEvent keyDown(QEvent::KeyPress,key,Qt::NoModifier,scan,0,0);
            QApplication::sendEvent(&surface,&keyDown); context->poll(); context->restart();
            TEST_CHECK(wire.back().kind==rc::Kind::Input && wire.back().input.kind==rc::InputKind::Key);
            TEST_CHECK(wire.back().input.code==int(scan & 0xff) && wire.back().input.extended==(scan>0xff));
            TEST_CHECK(wire.back().input.down);
            QKeyEvent keyUp(QEvent::KeyRelease,key,Qt::NoModifier,scan,0,0);
            QApplication::sendEvent(&surface,&keyUp); context->poll(); context->restart();
            TEST_CHECK(!wire.back().input.down);
        }
        TEST_CHECK(wire.size()==beforeKeys+4);
        QWheelEvent wheel(QPointF(100,100),QPointF(100,100),QPoint(),QPoint(0,120),
            Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(&surface,&wheel); context->poll(); context->restart();
        TEST_CHECK(wire.size()==beforeKeys+5 && wire.back().input.kind==rc::InputKind::Wheel);
        TEST_CHECK(wire.back().input.code==120 && wire.back().input.x==0 && wire.back().input.y==0);
        const auto count=wire.size();
        QMouseEvent down(QEvent::MouseButtonPress,QPointF(100,100),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&surface,&down); context->poll(); context->restart();
        TEST_CHECK(wire.size()==count+1 && wire.back().kind==rc::Kind::Input);
        TEST_CHECK(wire.back().input.down && wire.back().input.x==0 && wire.back().input.y==0);
        QMouseEvent up(QEvent::MouseButtonRelease,QPointF(699,499),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(&surface,&up); context->poll(); context->restart();
        TEST_CHECK(!wire.back().input.down && wire.back().input.x==65535 && wire.back().input.y==65535);
        const auto after=wire.size();
        QMouseEvent black(QEvent::MouseButtonPress,QPointF(10,10),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&surface,&black); context->poll(); context->restart();
        TEST_CHECK(wire.size()==after);
        // The input is queued before FocusOut. Only input admission is revoked;
        // the consent and live session survive focus loss and minimization.
        QApplication::sendEvent(&video,&down);
        QFocusEvent focus(QEvent::FocusOut); QApplication::sendEvent(&video,&focus);
        context->poll(); context->restart(); TEST_CHECK(control->state()==rc::State::Controlling);
        TEST_CHECK(wire.size()==after+1 && wire.back().kind==rc::Kind::Pause);
        TEST_CHECK(inputUi.active() && latest.inputPaused);
        emit coordinator->remoteControlChanged(latest);
        acknowledge(2,true,wire.back().challenge);
        window.showMinimized(); QCoreApplication::processEvents();
        QApplication::sendEvent(&surface,&down); context->poll(); context->restart();
        TEST_CHECK(inputUi.active() && wire.size()==after+1);
        window.showNormal(); window.activateWindow(); video.setFocus(); inputUi.noteFrame("TR_screen");
        QEventLoop resume; QTimer::singleShot(50,&resume,&QEventLoop::quit); resume.exec();
        context->poll(); context->restart();
        TEST_CHECK(wire.back().kind==rc::Kind::Resume && latest.inputPaused);
        const auto afterResume=wire.size();
        // Repeat/release of a key held before the pause cannot resurrect it.
        acknowledge(3,false,wire.back().challenge);
        QKeyEvent repeat(QEvent::KeyPress,Qt::Key_A,Qt::NoModifier,0x1e,0,0,QString(),true);
        QApplication::sendEvent(&surface,&repeat); context->poll(); context->restart();
        TEST_CHECK(wire.size()==afterResume);
        QApplication::sendEvent(&surface,&down); context->poll(); context->restart();
        TEST_CHECK(wire.size()==afterResume+1 && wire.back().kind==rc::Kind::Input && wire.back().inputEpoch==3);
        QApplication::sendEvent(&surface,&up); context->poll(); context->restart();
        inputUi.stop(); context->poll(); context->restart();
        TEST_CHECK(control->state()==rc::State::Idle && wire.back().kind==rc::Kind::End);
        acknowledge(4,false,2); TEST_CHECK(!inputUi.active());
        // Foreground consent starts without a second click. The active button
        // remains an exit throughout Grant -> Ready, even before projection.
        window.activateWindow(); QCoreApplication::processEvents();
        control->request({"bob",1,1},[]{return true;},"TR_screen",rc::NowMs());
        auto nextGrant=wire.back(); nextGrant.kind=rc::Kind::Grant; nextGrant.grant="qt-grant-2";
        nextGrant.share="share"; nextGrant.epoch=1; nextGrant.width=600; nextGrant.height=400;
        control->receive(nextGrant,{"bob",1,1},[]{return true;},rc::NowMs());
        inputUi.noteFrame("TR_screen");
        const auto beforeAutoReady=wire.size();
        emit coordinator->remoteControlChanged(latest);
        TEST_CHECK(entry->text()==QStringLiteral("结束远程控制"));
        QEventLoop automatic; QTimer::singleShot(50,&automatic,&QEventLoop::quit); automatic.exec();
        context->poll(); context->restart();
        TEST_CHECK(control->state()==rc::State::Controlling && wire.size()==beforeAutoReady+1);
        TEST_CHECK(wire.back().kind==rc::Kind::Ready);
        emit coordinator->remoteControlChanged(latest);
        TEST_CHECK(entry->text()==QStringLiteral("结束远程控制"));
        entry->click(); context->poll(); context->restart();
        TEST_CHECK(control->state()==rc::State::Idle && wire.back().kind==rc::Kind::End);
        OpenMeeting::MeetingCoordinatorTestAccess::bind(*coordinator,{});
    }
    return 0;
}
