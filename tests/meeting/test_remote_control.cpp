#include "src/core/remote_control/remote_control.h"
#include "src/core/remote_control/input_arbitration.h"
#include "tests/support/test_check.h"
#include <deque>
#include <iostream>
#include <limits>
#include <vector>

namespace rc = livekit::remote_control;
struct FakeInput : rc::InputBackend {
    std::shared_ptr<rc::Lease> lease;
    std::vector<rc::Input> events;
    int starts = 0, stops = 0;
    bool accept = true;
    rc::InputArbitration arbitration;
    uint64_t now = 10000;
    int releases = 0;
    bool start(const livekit::ScreenBinding&, std::shared_ptr<rc::Lease> l) override {
        lease = l; ++starts; arbitration.reset(); arbitration.poll(now,true); return accept;
    }
    bool submit(const rc::Input& i, uint64_t epoch) override {
        if (!accept) return false;
        if (arbitration.accepts(epoch)) events.push_back(i);
        return true;
    }
    void pause(bool value) override { arbitration.pause(value); ++releases; arbitration.poll(now,true); }
    rc::InputStatus status() override { arbitration.poll(now,true); return arbitration.status(); }
    void stop() override { if (lease) lease->valid = false; ++stops; }
};
struct Pair {
    uint64_t now = 10000;
    int ids = 0;
    bool membership = true, transport = true;
    rc::Peer alice{"alice",1,1}, bob{"bob",1,2};
    std::optional<rc::Surface> surface;
    rc::Projection a, b;
    std::shared_ptr<FakeInput> injector = std::make_shared<FakeInput>();
    struct Packet { bool fromA; rc::Message message; };
    std::deque<Packet> network;
    std::unique_ptr<rc::Runtime> controller, host;
    Pair() {
        livekit::ScreenBinding binding;
        binding.share_session_id = "screen-1"; binding.source_epoch = 1;
        binding.physical_x = -1920; binding.physical_width = 1920; binding.physical_height = 1080;
        surface = rc::Surface{"TR_screen",binding};
        const auto sender = [this](bool fromA) {
            return [this,fromA](std::string_view identity,std::string_view wire) {
                TEST_CHECK(identity == (fromA ? "bob" : "alice"));
                if (!transport) return false;
                const auto message = rc::Decode(wire); TEST_CHECK(message);
                network.push_back({fromA,*message}); return true;
            };
        };
        controller = std::make_unique<rc::Runtime>(rc::Runtime::Hooks{sender(true),{},
            [this]{return "id-"+std::to_string(++ids);},[this](auto p){a=p;},{}});
        host = std::make_unique<rc::Runtime>(rc::Runtime::Hooks{sender(false),[this]{return surface;},
            [this]{return "id-"+std::to_string(++ids);},[this](auto p){b=p;},injector});
    }
    void drain() {
        int count = 0;
        while (!network.empty()) {
            TEST_CHECK(++count < 1000);
            const auto p = network.front(); network.pop_front();
            (p.fromA ? host : controller)->receive(p.message,p.fromA ? alice : bob,
                [this]{return membership;},now);
        }
    }
    void request() { controller->request(bob,[this]{return membership;},"TR_screen",now); drain(); }
    void connect() {
        const auto starts = injector->starts;
        request(); TEST_CHECK(b.state == rc::State::AwaitingConsent); TEST_CHECK(injector->starts == starts);
        host->consent(b.request,true,now); drain();
        TEST_CHECK(a.state == rc::State::AwaitingActivation && injector->starts == starts);
        controller->activate(a.grant,now); drain();
        TEST_CHECK(a.state == rc::State::Controlling); TEST_CHECK(b.state == rc::State::Controlled);
        TEST_CHECK(injector->starts == starts + 1);
    }
    void heartbeat() { injector->now=now; controller->tick(now); host->tick(now); drain(); }
};
int main() {
    { // Decode bounds and exact typed wire round trip.
        rc::Message m; m.request="r"; m.track="t"; m.kind=rc::Kind::Input;
        m.input={rc::InputKind::Key,0,0,0x1d,true,true};
        auto parsed=rc::Decode(rc::Encode(m)); TEST_CHECK(parsed && parsed->input==m.input);
        TEST_CHECK(!rc::Decode(std::string(2049,'x')));
        TEST_CHECK(!rc::Decode("[]"));
        auto wire=rc::Encode(m); const auto pos=wire.find("\"v\":2"); wire.replace(pos,5,"\"v\":1");
        TEST_CHECK(!rc::Decode(wire));
        m.input.code=200; TEST_CHECK(!rc::Decode(rc::Encode(m)));
        m.kind=rc::Kind::Request; m.request.clear(); TEST_CHECK(!rc::Decode(rc::Encode(m)));
    }
    { // Negative origin, selected secondary screen, endpoints and containment.
        TEST_CHECK((rc::NormalizePointer(299,119,100,20,200,100)==rc::AbsolutePoint{65535,65535}));
        TEST_CHECK((rc::NormalizePointer(100,20,100,20,200,100)==rc::AbsolutePoint{0,0}));
        TEST_CHECK(!rc::NormalizePointer(99,20,100,20,200,100));
        TEST_CHECK(!rc::NormalizePointer(300,20,100,20,200,100));
        TEST_CHECK(!rc::NormalizePointer(std::numeric_limits<double>::quiet_NaN(),20,100,20,200,100));
        Pair p; const auto& binding=p.surface->binding;
        TEST_CHECK((rc::MapAbsolute(0,0,binding,-1920,0,3840,1080)==rc::AbsolutePoint{0,0}));
        auto point=rc::MapAbsolute(65535,65535,binding,-1920,0,3840,1080);
        TEST_CHECK(point && point->x==32759 && point->y==65535);
        TEST_CHECK(!rc::MapAbsolute(0,0,binding,0,0,1920,1080));
        TEST_CHECK(!rc::MapAbsolute(0,0,binding,-1920,0,1,1080));
    }
    { // Consent is mandatory and linked to the still-pending request.
        Pair p; p.request(); p.host->consent("old",true,p.now); TEST_CHECK(p.injector->starts==0);
        p.host->consent(p.b.request,false,p.now); p.drain();
        TEST_CHECK(p.a.state==rc::State::Idle && p.b.state==rc::State::Idle);
    }
    { // Ordered pointer/key edges, duplicate suppression, no sender spoofing.
        Pair p; p.connect();
        const rc::Input down{rc::InputKind::Button,123,456,1,true,false};
        p.controller->input(down,p.a.grant,p.now,p.a.inputEpoch);
        const auto packet=p.network.front().message;
        p.host->receive(packet,{"intruder",1,9},[]{return true;},p.now);
        p.host->receive(packet,{"alice",1,8},[]{return true;},p.now);
        TEST_CHECK(p.injector->events.empty());
        p.drain(); TEST_CHECK(p.injector->events.size()==1 && p.injector->events[0]==down);
        p.host->receive(packet,p.alice,[]{return true;},p.now); TEST_CHECK(p.injector->events.size()==1);
        p.host->stop(); p.drain(); TEST_CHECK(!p.injector->lease->valid);
        p.host->receive(packet,p.alice,[]{return true;},p.now); TEST_CHECK(p.injector->events.size()==1);
    }
    { // An old UI input cannot enter a successor grant.
        Pair p; p.connect(); const auto old=p.a.grant;
        p.controller->stop(); p.drain(); p.now+=2100; p.connect();
        p.controller->input({rc::InputKind::Key,0,0,30,true,false},old,p.now,p.a.inputEpoch);
        p.drain(); TEST_CHECK(p.injector->events.empty());
    }
    { // Missing reliable edge fails closed rather than guessing key state.
        Pair p; p.connect();
        p.controller->input({rc::InputKind::Move,1,2},p.a.grant,p.now,p.a.inputEpoch);
        p.network.front().message.sequence+=2; p.drain();
        TEST_CHECK(p.b.state==rc::State::Idle && !p.injector->lease->valid);
    }
    { // Ongoing input does not substitute for a fresh heartbeat challenge.
        Pair p; p.connect(); p.heartbeat();
        p.now+=rc::LeaseMs;
        p.controller->input({rc::InputKind::Move,1,2},p.a.grant,p.now,p.a.inputEpoch);
        p.host->tick(p.now); p.drain(); TEST_CHECK(p.b.state==rc::State::Idle);
    }
    { // Heartbeats keep an unchanged live scope active, then source change revokes.
        Pair p; p.connect();
        for(int i=0;i<15;++i){p.now+=500;p.heartbeat();TEST_CHECK(p.b.state==rc::State::Controlled);}
        ++p.surface->binding.source_epoch; p.host->tick(p.now); p.drain();
        TEST_CHECK(p.b.state==rc::State::Idle && !p.injector->lease->valid);
    }
    { // Background network heartbeats cannot renew a stalled frontend lease.
        Pair p;
        auto frontend=std::make_shared<rc::Lease>(); frontend->current=[]{return true;};
        frontend->deadline=p.now+rc::LeaseMs;
        p.controller->request(p.bob,[&]{return p.membership && frontend->permits(p.now);},"TR_screen",p.now);
        p.drain(); p.host->consent(p.b.request,true,p.now); p.drain();
        p.controller->activate(p.a.grant,p.now); p.drain();
        for(int i=0;i<5;++i){p.now+=500;p.heartbeat();TEST_CHECK(p.a.state==rc::State::Controlling);}
        p.now+=500; p.heartbeat();
        TEST_CHECK(p.a.state==rc::State::Idle && p.b.state==rc::State::Idle && !p.injector->lease->valid);
    }
    { Pair p; p.connect(); p.membership=false; p.host->tick(p.now); TEST_CHECK(!p.injector->lease->valid); }
    { Pair p; p.connect(); p.injector->lease->valid=false; p.host->tick(p.now); p.drain(); TEST_CHECK(p.a.state==rc::State::Idle); }
    { Pair p; p.connect(); p.injector->accept=false; p.controller->input({rc::InputKind::Move,1,2},p.a.grant,p.now,p.a.inputEpoch); p.drain(); TEST_CHECK(p.b.state==rc::State::Idle); }
    { Pair p; p.connect(); p.transport=false; p.controller->input({rc::InputKind::Move,1,2},p.a.grant,p.now,p.a.inputEpoch); TEST_CHECK(p.a.state==rc::State::Idle); p.now+=rc::LeaseMs; p.host->tick(p.now); TEST_CHECK(!p.injector->lease->valid); }
    { // Cancel crossing a Grant cannot revive either side.
        Pair p; p.request(); p.host->consent(p.b.request,true,p.now); p.controller->stop(); p.drain();
        TEST_CHECK(p.a.state==rc::State::Idle && p.b.state==rc::State::Idle && p.injector->starts==0);
    }
    { // A delayed but correctly sequenced Pong cannot renew an old challenge.
        Pair p; p.connect(); p.host->tick(p.now);
        auto pong=p.network.front().message; p.network.clear();
        pong.kind=rc::Kind::Pong; pong.sequence=2;
        p.now+=2000; p.host->receive(pong,p.alice,[]{return true;},p.now);
        p.now+=1000; p.host->tick(p.now); TEST_CHECK(!p.injector->lease->valid);
    }
    { Pair p; p.request(); p.now+=rc::RequestMs; p.host->consent(p.b.request,true,p.now); p.drain(); TEST_CHECK(p.injector->starts==0 && p.b.state==rc::State::Idle); }
    { // Preparation failure cannot leave an authorized peer or input lease.
        Pair p; p.request(); p.injector->accept=false;
        p.host->consent(p.b.request,true,p.now); p.drain();
        p.controller->activate(p.a.grant,p.now); p.drain();
        TEST_CHECK(p.a.state==rc::State::Idle && p.b.state==rc::State::Idle && !p.injector->lease->valid);
    }
    { // Local revocation also blocks a Ready already waiting in the session queue.
        Pair p; p.request(); p.host->consent(p.b.request,true,p.now);
        const auto grant=p.network.front(); p.network.pop_front();
        p.controller->receive(grant.message,p.bob,[]{return true;},p.now);
        p.controller->activate(p.a.grant,p.now);
        TEST_CHECK(p.network.front().message.kind==rc::Kind::Ready);
        p.membership=false; p.drain(); p.host->tick(p.now);
        TEST_CHECK(p.injector->starts==0 && p.b.state==rc::State::Idle);
    }
    { Pair p; p.connect(); for(int i=0;i<181;++i) p.controller->input({rc::InputKind::Move,1,2},p.a.grant,p.now,p.a.inputEpoch); p.drain(); TEST_CHECK(p.a.state==rc::State::Idle); }
    { // Approval is not Ready. Background consent can wait for explicit activation.
        Pair p; p.request(); p.host->consent(p.b.request,true,p.now); p.drain();
        TEST_CHECK(p.a.state==rc::State::AwaitingActivation && p.b.state==rc::State::AwaitingReady);
        p.controller->input({rc::InputKind::Button,1,2,1,true,false},p.a.grant,p.now,p.a.inputEpoch);
        p.controller->activate("stale-grant",p.now);
        p.now+=5000; p.heartbeat();
        TEST_CHECK(p.injector->starts==0 && p.injector->events.empty() && p.a.state==rc::State::AwaitingActivation);
        p.controller->activate(p.a.grant,p.now); p.drain();
        TEST_CHECK(p.injector->starts==1 && p.a.state==rc::State::Controlling && p.b.state==rc::State::Controlled);
    }
    { // Abandoned activation still expires and cannot start the input owner later.
        Pair p; p.request(); p.host->consent(p.b.request,true,p.now); p.drain();
        const auto grant=p.a.grant;
        p.now+=rc::RequestMs; p.heartbeat(); p.controller->activate(grant,p.now); p.drain();
        TEST_CHECK(p.a.state==rc::State::Idle && p.b.state==rc::State::Idle && p.injector->starts==0);
    }
    { // Local priority lasts through held keys and a full idle interval; failed
      // remote releases and the controller's pause independently block admission.
        rc::InputArbitration gate; gate.poll(10000,true);
        const auto old = gate.status().epoch; TEST_CHECK(gate.accepts(old));
        gate.localActivity(10001,0x1d,true); gate.poll(12000,true);
        TEST_CHECK(gate.status().paused && !gate.accepts(old));
        gate.localActivity(12001,0x1d,false); gate.poll(13000,true); TEST_CHECK(gate.status().paused);
        gate.poll(13001,false); TEST_CHECK(gate.status().paused);
        gate.pause(true); gate.poll(14000,true); TEST_CHECK(gate.status().paused);
        gate.pause(false); gate.poll(14000,true);
        TEST_CHECK(!gate.status().paused && !gate.accepts(old));
        TEST_CHECK(gate.accepts(gate.status().epoch));
    }
    { // Controller focus changes retain consent, maintain liveness, release
      // pressed input and wait for a fresh host acknowledgement before resuming.
        Pair p; p.connect(); const auto grant=p.a.grant; const auto oldEpoch=p.a.inputEpoch;
        p.controller->pause(true,grant,p.now); p.drain();
        TEST_CHECK(p.a.state==rc::State::Controlling && p.b.state==rc::State::Controlled);
        TEST_CHECK(p.a.inputPaused && p.b.controllerPaused && p.injector->releases==1);
        for (int i=0;i<10;++i) { p.now+=500; p.heartbeat(); }
        TEST_CHECK(p.a.grant==grant && p.injector->lease->valid);
        p.controller->pause(false,grant,p.now);
        p.controller->input({rc::InputKind::Key,0,0,30,true,false},grant,p.now,oldEpoch);
        TEST_CHECK(p.network.size()==1 && p.network.front().message.kind==rc::Kind::Resume);
        p.drain(); TEST_CHECK(!p.a.inputPaused && p.a.inputEpoch>oldEpoch);
        p.controller->input({rc::InputKind::Key,0,0,30,true,false},grant,p.now,oldEpoch); p.drain();
        TEST_CHECK(p.injector->events.empty());
        p.controller->input({rc::InputKind::Key,0,0,30,true,false},grant,p.now,p.a.inputEpoch); p.drain();
        TEST_CHECK(p.injector->events.size()==1);
        p.controller->pause(true,grant,p.now); p.drain();
        p.now+=rc::LeaseMs; p.heartbeat(); TEST_CHECK(p.a.state==rc::State::Idle && p.b.state==rc::State::Idle);
    }
    { // Host local activity invalidates input already in flight. Old events
      // remain discarded even after the native owner becomes ready again.
        Pair p; p.connect(); const auto grant=p.a.grant;
        p.controller->input({rc::InputKind::Button,1,2,1,true,false},grant,p.now,p.a.inputEpoch);
        p.injector->arbitration.localActivity(p.now,0x1e,true);
        p.injector->arbitration.localActivity(p.now,0x1e,false);
        p.now+=1000; p.injector->now=p.now; p.injector->status();
        p.drain(); TEST_CHECK(p.injector->events.empty() && p.injector->lease->valid);
        p.heartbeat(); TEST_CHECK(!p.a.inputPaused && p.a.grant==grant);
        p.controller->input({rc::InputKind::Move,30,40},grant,p.now,p.a.inputEpoch); p.drain();
        TEST_CHECK(p.injector->events.size()==1);
        p.injector->arbitration.localActivity(p.now,0x1e,true); p.heartbeat();
        TEST_CHECK(p.a.inputPaused && !p.b.controllerPaused);
        p.host->stop(); p.drain();
        p.injector->arbitration.localActivity(p.now,0x1e,false); p.now+=2000; p.heartbeat();
        TEST_CHECK(p.a.state==rc::State::Idle && p.b.state==rc::State::Idle);
    }
    std::cout<<"PASS remote control protocol, consent, lifecycle, ordering, lease, pause and coordinate contracts\n";
}
