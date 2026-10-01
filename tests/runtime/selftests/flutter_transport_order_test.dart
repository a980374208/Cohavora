import 'dart:async';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart' as rtc;
import 'package:livekit_client/livekit_client.dart';
import 'package:livekit_client/src/core/transport.dart';

// No sockets, media, keys, timers or SDP from a real session. Hold the platform
// future to force answer completion into the offer's asynchronous gap.
class ControlledPeer implements rtc.RTCPeerConnection {
  final readEntered = Completer<void>();
  final releaseRead = Completer<rtc.RTCSessionDescription?>();
  var state = rtc.RTCSignalingState.RTCSignalingStateHaveLocalOffer;
  int offers = 0;
  bool failOffer = false;
  @override
  Future<rtc.RTCSignalingState?> getSignalingState() async => state;
  @override
  Future<rtc.RTCSessionDescription?> getRemoteDescription() {
    if (!readEntered.isCompleted) readEntered.complete();
    return releaseRead.future;
  }
  @override
  Future<void> setRemoteDescription(rtc.RTCSessionDescription description) async {
    state = rtc.RTCSignalingState.RTCSignalingStateStable;
  }
  @override
  Future<rtc.RTCSessionDescription> createOffer([Map<String, dynamic>? constraints]) async {
    offers++;
    if (failOffer) { failOffer = false; throw StateError('controlled offer failure'); }
    return rtc.RTCSessionDescription('v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n', 'offer');
  }
  @override
  Future<void> setLocalDescription(rtc.RTCSessionDescription description) async {
    state = rtc.RTCSignalingState.RTCSignalingStateHaveLocalOffer;
  }
  @override
  Future<List<rtc.RTCRtpSender>> getSenders() async => [];
  @override
  Future<void> close() async {}
  @override
  Future<void> dispose() async {}
  @override
  dynamic noSuchMethod(Invocation invocation) => null;
}

void main() {
  testWidgets('answer completion cannot lose pending publication negotiation', (tester) async {
    final peer = ControlledPeer();
    final transport = await Transport.create((configuration, [constraints = const <String, dynamic>{}]) async => peer,
        connectOptions: const ConnectOptions());
    transport.onOffer = (_) {};
    final pendingOffer = transport.createAndSendOffer();
    await tester.pump();
    expect(peer.readEntered.isCompleted, isTrue);
    final answer = rtc.RTCSessionDescription('v=0\r\n', 'answer');
    final pendingAnswer = transport.setRemoteDescription(answer);
    await tester.pump();
    peer.releaseRead.complete(answer);
    await tester.pump();
    await Future.wait([pendingOffer, pendingAnswer]);
    await transport.dispose();
    expect(peer.offers, 1, reason: 'the queued publication must create an offer after the answer');
    expect(transport.renegotiate, isFalse);
  });
  testWidgets('a failed platform operation does not poison subsequent negotiation', (tester) async {
    final peer = ControlledPeer()
      ..state = rtc.RTCSignalingState.RTCSignalingStateStable
      ..failOffer = true;
    final transport = await Transport.create((configuration, [constraints = const <String, dynamic>{}]) async => peer,
        connectOptions: const ConnectOptions());
    transport.onOffer = (_) {};
    final failed = expectLater(transport.createAndSendOffer(), throwsStateError);
    final next = transport.createAndSendOffer();
    await tester.pump();
    await failed;
    await next;
    expect(peer.offers, 2);
    await transport.dispose();
  });
}
