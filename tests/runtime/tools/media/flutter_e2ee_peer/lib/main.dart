import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/material.dart' hide ConnectionState;
import 'package:flutter/foundation.dart' show listEquals;
import 'package:flutter_webrtc/flutter_webrtc.dart' as rtc;
import 'package:livekit_client/livekit_client.dart';
import 'package:logging/logging.dart';
import 'product_whiteboard.dart';
import 'camera_correspondence.dart';

// This fixture consumes the pinned SDK with separately recorded task-owned
// transport scheduling repairs. Never print exception
// strings, packet bytes, credentials, key material or full stats reports.
final env = Platform.environment;
late IOSink evidence;
Room? room;
FixtureKeyProvider? provider;
// Application-level send-slot selection through the SDK's public provider API.
// Crypto primitives and packet serialization remain in the pinned SDK/plugin.
class FixtureKeyProvider extends BaseKeyProvider {
  int sendIndex = 0;
  FixtureKeyProvider(BaseKeyProvider base) : super(base.keyProvider, base.options);
  @override int getLatestIndex(String participantId) => sendIndex;
}
int generation = 0, keyEpoch = 0, sequence = 0, reconnects = 0;
bool closing = false, sampling = false;
bool relayPublished = false;
bool receiveKeyReinstalled = false;
bool fullReconnectRequested = false;
bool rpcStarted = false;
bool transportRebuilding = false;
bool lifecycleMuted = false, lifecycleUnmuted = false;
int lifecycleCycles = 0;
LocalVideoTrack? relayTrack;
String? relaySourceSid;
Timer? sampleTimer, finishTimer;
final tracks = <Track>[];
final pcmCounts = <RemoteAudioTrack, int>{};
final pcmStops = <RemoteAudioTrack, Future<void> Function()>{};
final rendered = ValueNotifier<VideoTrack?>(null);
final productTransfers = <String, Map<int, String>>{};
ProductWhiteboard? productBoard;
final byteEpochsSent = <int>{};
String productTextBlock(int epoch, int index) => List.filled(1024, 'e2ee-text-$epoch-$index:中🙂;').join();
Future<void> sendProductBytes(int epoch) async {
  if (!byteEpochsSent.add(epoch)) return;
  try {
    final writers = await Future.wait(List.generate(2, (index) => room!.localParticipant!.streamBytes(
      StreamBytesOptions(name: 'bytes-$epoch-$index', topic: 'e2ee.bytes', totalSize: 65536))));
    for (var offset = 0; offset < 65536; offset += 8192) {
      for (var index = 0; index < 2; index++) {
        await writers[index].write(Uint8List.fromList(List.generate(8192, (i) => (offset + i + epoch + index) & 255)));
      }
    }
    await Future.wait(writers.map((writer) => writer.close()));
    record('product_byte_sent', {'epoch': epoch, 'streams': 2});
    final textWriters = await Future.wait(List.generate(2, (index) => room!.localParticipant!.streamText(
      StreamTextOptions(topic: 'e2ee.concurrent-text', attributes: {'epoch': '$epoch', 'index': '$index'},
        totalSize: utf8.encode(productTextBlock(epoch, index)).length * 2))));
    for (var chunk = 0; chunk < 2; chunk++) {
      for (var index = 0; index < 2; index++) { await textWriters[index].write(productTextBlock(epoch, index)); }
    }
    await Future.wait(textWriters.map((writer) => writer.close()));
    record('product_text_sent', {'epoch': epoch, 'streams': 2});
  } catch (error) { record('product_byte_failed', {'type': error.runtimeType.toString()}); }
}
Future<void> productBoardQueue = Future.value();
Future<void> productChat(DataReceivedEvent event) async {
  try {
    final value = jsonDecode(utf8.decode(event.data)) as Map<String, dynamic>;
    final type = value['om_type'];
    if (type == 'chat_text') {
      final text = value['text'] as String;
      final epoch = int.tryParse(text.split('-').last);
      final valid = (epoch == 4 || epoch == 6) && text == 'e2ee-public-product-text-$epoch';
      record('product_chat_received', {'epoch': epoch, 'valid': valid});
      if (!valid) return;
    } else if (type == 'media_start' || type == 'media_chunk') {
      final epoch = int.tryParse((value['fileName'] as String).split('-').last);
      final count = value['totalChunks'] as int;
      if ((epoch != 4 && epoch != 6) || value['totalSize'] != 65536 || count < 1 || count > 16) {
        record('product_business_failed'); return;
      }
      if (type == 'media_chunk') {
        final transfer = value['transferId'] as String;
        if (!productTransfers.containsKey(transfer) && productTransfers.length >= 4) {
          record('product_business_failed'); return;
        }
        final index = value['chunkIndex'] as int;
        if (index < 0 || index >= count) { record('product_business_failed'); return; }
        final chunks = productTransfers.putIfAbsent(transfer, () => <int, String>{});
        chunks[index] = value['chunkData'] as String;
        if (chunks.length == count) {
          final bytes = base64Decode(List.generate(count, (i) => chunks[i]!).join());
          record('product_file_received', {'epoch': epoch,
            'valid': bytes.length == 65536 && bytes.every((byte) => byte == 65 + epoch!), 'bytes': bytes.length});
          productTransfers.remove(transfer);
        }
      }
    } else { record('product_business_failed'); return; }
    // Echo only the bounded public fixture schema through the official SDK's
    // normal encrypted user-packet path. Do not implement crypto in the fixture.
    await room!.localParticipant!.publishData(event.data, reliable: true, topic: 'chat');
  } catch (error) { record('product_business_failed', {'type': error.runtimeType.toString()}); }
}
void record(String event, [Map<String, Object?> fields = const {}]) {
  evidence.writeln(jsonEncode({'event': event, 'time_utc': DateTime.now().toUtc().toIso8601String(),
    'run': env['E2EE_RUN_ID'], 'generation': generation, 'key_epoch': keyEpoch,
    'key_index': provider?.sendIndex ?? 0, 'sequence': sequence++, ...fields}));
}
Future<void> join() async {
  generation++;
  tracks.clear();
  final enabled = env['E2EE_MODE'] != 'off';
  provider = FixtureKeyProvider(await BaseKeyProvider.create(
    ratchetWindowSize: int.parse(env['E2EE_RATCHET_WINDOW'] ?? '16'),
    failureTolerance: int.parse(env['E2EE_FAILURE_TOLERANCE'] ?? '-1')));
  if (env['E2EE_KEY_STATE'] != 'missing') {
    await provider!.setSharedKey(env['E2EE_TEST_KEY'] ?? '');
  }
  if (env['E2EE_KEY_ACTION'] == 'slot') {
    await provider!.setSharedKey(env['E2EE_NEXT_TEST_KEY']!, keyIndex: 1);
    record('receive_slot_prepared', {'prepared_slot': 1});
  }
  final value = Room(roomOptions: RoomOptions(adaptiveStream: false, dynacast: false,
    encryption: enabled ? E2EEOptions(keyProvider: provider!) : null));
  room = value;
  if (env['E2EE_PRODUCT_WHITEBOARD'] == '1') {
    productBoard = ProductWhiteboard((topic, bytes) => value.localParticipant!.publishData(
      bytes, reliable: true, topic: topic), record, env['E2EE_FLUTTER_AUTHORITY'] == '1');
  }
  value.registerRpcMethod('e2ee.echo', (data) async {
    final valid = data.payload == 'e2ee-public-rpc-request';
    record('rpc_handler', {'content_valid': valid});
    if (!valid) throw StateError('unexpected_test_payload');
    return 'e2ee-public-rpc-response';
  });
  final thisGeneration = generation;
  final listener = value.createListener();
  listener
    ..on<TrackE2EEStateEvent>((e) {
      if (thisGeneration != generation || closing) return;
      record('crypto_state', {'participant': e.participant.identity, 'track': e.publication.sid,
        'state': e.state.name, 'direction': e.participant is LocalParticipant ? 'tx' : 'rx'});
    })
    ..on<TrackSubscribedEvent>((e) {
      if (thisGeneration != generation || closing) return;
      tracks.add(e.track);
      if (env['E2EE_PRODUCT_LIFECYCLE'] == '1' && e.track is RemoteAudioTrack) {
        final audio = e.track as RemoteAudioTrack;
        pcmCounts[audio] = 0;
        pcmStops[audio] = audio.addAudioRenderer(onFrame: (frame) {
          if (!closing && thisGeneration == generation && pcmCounts.containsKey(audio)) {
            pcmCounts[audio] = pcmCounts[audio]! + frame.data.length ~/ 2;
          }
        });
      }
      if (e.track is VideoTrack) {
        rendered.value = e.track as VideoTrack;
      }
      record('subscribed', {'track': e.publication.sid, 'kind': e.track.kind.name,
        'encryption': e.publication.encryptionType.name});
    })
    ..on<DataReceivedEvent>((e) {
      if (thisGeneration != generation || closing) return;
      if (env['E2EE_PRODUCT_BYTE_STREAMS'] == '1' && e.topic == 'e2ee.bytes.control') {
        final epoch = keyEpoch == 0 ? 4 : 6;
        if (listEquals(e.data, utf8.encode('native-bytes-ready-$epoch'))) unawaited(sendProductBytes(epoch));
        return;
      }
      if ((e.topic?.startsWith('whiteboard.v1.') ?? false) && productBoard != null) {
        final receiptEpoch = keyEpoch;
        productBoardQueue = productBoardQueue.then((_) async {
          if (thisGeneration != generation || closing || receiptEpoch != keyEpoch) return;
          await productBoard!.receive(e.topic!, Uint8List.fromList(e.data), receiptEpoch == 0 ? 4 : 6);
        }).catchError((Object error) { record('product_board_failed', {'type': error.runtimeType.toString()}); });
        return;
      }
      if (e.topic == 'chat' && env['E2EE_PRODUCT_BUSINESS'] == '1') { unawaited(productChat(e)); return; }
      if (e.topic != 'e2ee.probe') return;
      record('data_received', {'bytes': e.data.length,
        'content_valid': utf8.decode(e.data, allowMalformed: true) == 'e2ee-probe-public-test-payload'});
    })
    ..on<TrackUnsubscribedEvent>((e) {
      if (thisGeneration != generation || closing) return;
      tracks.remove(e.track);
      if (e.track is RemoteAudioTrack) {
        final audio = e.track as RemoteAudioTrack;
        pcmCounts.remove(audio);
        final stop = pcmStops.remove(audio);
        if (stop != null) unawaited(stop());
      }
      if (rendered.value == e.track) rendered.value = null;
    })
    ..on<RoomReconnectingEvent>((e) {
      if (thisGeneration != generation || closing) return;
      transportRebuilding = true;
      reconnects++; record('reconnecting');
    })
    ..on<RoomReconnectedEvent>((e) {
      if (thisGeneration != generation || closing) return;
      transportRebuilding = false;
      record('reconnected', {'count': reconnects});
    });
  value.registerTextStreamHandler('e2ee.stream', (reader, identity) async {
    try {
      final text = await reader.readAll();
      if (thisGeneration == generation && !closing) record('stream_received', {
        'participant': identity, 'content_valid': text == 'e2ee-probe-public-test-stream'});
    } catch (e) { record('stream_failed', {'type': e.runtimeType.toString()}); }
  });
  if (env['E2EE_PRODUCT_BYTE_STREAMS'] == '1') {
    value.registerTextStreamHandler('e2ee.concurrent-text', (reader, identity) async {
      try {
        final epoch = int.tryParse(reader.info!.attributes['epoch'] ?? '');
        final index = int.tryParse(reader.info!.attributes['index'] ?? '');
        final text = await reader.readAll();
        var valid = (epoch == 4 || epoch == 6) && (index == 0 || index == 1);
        if (valid) { final block = productTextBlock(epoch!, index!); valid = text == block + block; }
        record('product_text_received', {'epoch': epoch, 'index': index, 'valid': valid});
      } catch (error) { record('product_text_failed', {'type': error.runtimeType.toString()}); }
    });
    value.registerByteStreamHandler('e2ee.bytes', (reader, identity) async {
      try {
        final parts = reader.info!.name.split('-');
        final epoch = parts.length == 3 ? int.tryParse(parts[1]) : null;
        final index = parts.length == 3 ? int.tryParse(parts[2]) : null;
        final bytes = (await reader.readAll()).expand((chunk) => chunk).toList();
        var valid = (epoch == 4 || epoch == 6) && (index == 0 || index == 1) && bytes.length == 65536;
        for (var i = 0; valid && i < bytes.length; i++) { valid = bytes[i] == ((i + epoch! + index!) & 255); }
        record('product_byte_received', {'epoch': epoch, 'index': index, 'valid': valid});
      } catch (error) { record('product_byte_failed', {'type': error.runtimeType.toString()}); }
    });
  }
  await value.connect(env['LIVEKIT_URL']!, env['LIVEKIT_TOKEN']!);
  record('connected', {'mode': enabled ? 'required' : 'off', 'participant': value.localParticipant!.identity});
  if (env['E2EE_PUBLISH'] == '1') {
    if (env['E2EE_PRODUCT_CAMERA'] == '1') {
      try {
        final camera = await LocalVideoTrack.createCameraTrack();
        await value.localParticipant!.publishVideoTrack(camera, publishOptions: VideoPublishOptions(
          videoCodec: env['E2EE_CODEC'] ?? 'vp8', simulcast: false));
        tracks.add(camera);
        record('published', {'kind': 'video', 'source': 'camera'});
      } catch (e) { record('publish_failed', {'kind': 'camera', 'type': e.runtimeType.toString()}); }
    }
    if (env['E2EE_VIDEO_SOURCE'] != 'relay' && env['E2EE_VIDEO_SOURCE'] != 'camera') {
    try {
      final sources = await rtc.desktopCapturer.getSources(types: [rtc.SourceType.Window]);
      final source = sources.firstWhere((s) => s.name == 'E2EE Synthetic Source');
      final video = await LocalVideoTrack.createScreenShareTrack(ScreenShareCaptureOptions(
        sourceId: source.id, maxFrameRate: 10, params: VideoParametersPresets.h360_169));
      await value.localParticipant!.publishVideoTrack(video, publishOptions: VideoPublishOptions(
        videoCodec: env['E2EE_CODEC'] ?? 'vp8', simulcast: false));
      tracks.add(video);
      record('published', {'kind': 'video', 'source': 'synthetic_fixture_window'});
    } catch (e) { record('publish_failed', {'kind': 'video', 'type': e.runtimeType.toString()}); }
    }
    try {
      final audio = await LocalAudioTrack.create(AudioCaptureOptions(
        echoCancellation: false, noiseSuppression: false, autoGainControl: false));
      await value.localParticipant!.publishAudioTrack(audio);
      tracks.add(audio);
      record('published', {'kind': 'audio', 'source': 'microphone'});
    } catch (e) { record('publish_failed', {'kind': 'audio', 'type': e.runtimeType.toString()}); }
  }
}
Future<void> publishRelayVideo() async {
  if (env['E2EE_VIDEO_SOURCE'] != 'relay' || room?.connectionState != ConnectionState.connected) return;
  if (relayTrack != null && !tracks.whereType<RemoteVideoTrack>().any((t) => t.sid == relaySourceSid)) {
    final retired = relayTrack!;
    await room!.localParticipant!.removePublishedTrack(retired.sid!);
    tracks.remove(retired);
    relayTrack = null; relaySourceSid = null; relayPublished = false;
    record('relay_retired');
  }
  if (relayPublished) return;
  final incoming = tracks.whereType<RemoteVideoTrack>().firstOrNull;
  if (incoming == null) return;
  relayPublished = true;
  try {
    // A separately labelled protocol fixture: decoded remote frames are
    // re-encoded by the official SDK sender. This is NOT a camera/share test.
    final video = LocalVideoTrack(TrackSource.camera, incoming.mediaStream,
      incoming.mediaStreamTrack, const CameraCaptureOptions());
    await room!.localParticipant!.publishVideoTrack(video, publishOptions:
      VideoPublishOptions(videoCodec: env['E2EE_CODEC'] ?? 'vp8', simulcast: false));
    relayTrack = video; relaySourceSid = incoming.sid;
    tracks.add(video);
    record('published', {'kind': 'video', 'source': 'decoded_video_relay'});
  } catch (e) { record('publish_failed', {'kind': 'video', 'type': e.runtimeType.toString()}); }
}
Future<void> lifecycleSample() async {
  if (env['E2EE_PRODUCT_LIFECYCLE'] != '1') return;
  final elapsed = DateTime.now().millisecondsSinceEpoch - int.parse(env['E2EE_LIFECYCLE_START_MS']!);
  final audio = tracks.whereType<LocalAudioTrack>().first;
  if (!lifecycleMuted && elapsed >= 10000) {
    lifecycleMuted = true;
    await audio.mute();
    record('product_audio_control', {'muted': true});
    record('product_audio_state', {'muted': audio.muted});
  }
  if (lifecycleMuted && !lifecycleUnmuted && elapsed >= 15000) {
    lifecycleUnmuted = true;
    await audio.unmute();
    record('product_audio_control', {'muted': false});
    record('product_audio_state', {'muted': audio.muted});
  }
  if (lifecycleCycles >= 3 || elapsed < 25000 + lifecycleCycles * 20000) return;
  final old = tracks.whereType<LocalVideoTrack>().firstWhere((t) => t.source == TrackSource.screenShareVideo);
  final oldSid = old.sid;
  record('product_share_stop', {'cycle': lifecycleCycles + 1, 'track': oldSid});
  await room!.localParticipant!.removePublishedTrack(oldSid!);
  tracks.remove(old);
  record('product_share_retired', {'cycle': lifecycleCycles + 1, 'track': oldSid});
  final sources = await rtc.desktopCapturer.getSources(types: [rtc.SourceType.Window]);
  final source = sources.firstWhere((s) => s.name == 'E2EE Synthetic Source');
  final video = await LocalVideoTrack.createScreenShareTrack(ScreenShareCaptureOptions(
    sourceId: source.id, maxFrameRate: 10, params: VideoParametersPresets.h360_169));
  await room!.localParticipant!.publishVideoTrack(video, publishOptions: VideoPublishOptions(
    videoCodec: env['E2EE_CODEC'] ?? 'vp8', simulcast: false));
  tracks.add(video);
  lifecycleCycles++;
  record('product_share_restart', {'cycle': lifecycleCycles, 'track': video.sid});
}
Future<void> sample() async {
  if (sampling || closing) return;
  sampling = true;
  try {
    if (!fullReconnectRequested && env['E2EE_RECONNECT'] == 'flutter-full' &&
        DateTime.now().millisecondsSinceEpoch >= int.parse(env['E2EE_RECONNECT_AT_MS']!)) {
      fullReconnectRequested = true;
      transportRebuilding = true;
      // A relayed remote source cannot survive receiver replacement. Retire
      // this fixture-only publication before the SDK rebuilds its transports.
      if (relayTrack != null) {
        final retired = relayTrack!;
        await room!.localParticipant!.removePublishedTrack(retired.sid!);
        tracks.remove(retired);
        relayTrack = null; relaySourceSid = null; relayPublished = false;
        record('relay_retired', {'reason': 'before_full_reconnect'});
      }
      tracks.removeWhere((track) => track is RemoteTrack);
      record('reconnect_requested', {'kind': 'flutter_full'});
      await room!.sendSimulateScenario(fullReconnect: true);
      return;
    }
    // Sender/receiver handles can be retired while the SDK is reconnecting.
    if (transportRebuilding || room?.connectionState != ConnectionState.connected) return;
    await lifecycleSample();
    if (env['E2EE_PRODUCT_LIFECYCLE'] == '1' || env['E2EE_PRODUCT_CAMERA'] == '1') {
      final transport = room!.engine.publisher;
      if (transport != null) {
        final local = await transport.pc.getLocalDescription();
        final remote = await transport.pc.getRemoteDescription();
        int sections(String? sdp, String kind) => (sdp ?? '').split('\n')
            .where((line) => line.startsWith('m=$kind ')).length;
        record('product_transport', {
          'signaling_state': (await transport.pc.getSignalingState()).toString(),
          'renegotiate_pending': transport.renegotiate,
          'local_audio_sections': sections(local?.sdp, 'audio'),
          'remote_audio_sections': sections(remote?.sdp, 'audio'),
          'local_video_sections': sections(local?.sdp, 'video'),
          'remote_video_sections': sections(remote?.sdp, 'video')});
      }
    }
    for (final entry in pcmCounts.entries) {
      record('product_pcm', {'track': entry.key.sid, 'samples': entry.value});
    }
    final keyRounds = int.parse(env['E2EE_KEY_ROUNDS'] ?? '1');
    final rotationReady = (env['E2EE_KEY_ACTION'] ?? 'none') != 'none' && (env['E2EE_PRODUCT_BOARD_PARTIAL'] == '1'
        ? File(env['E2EE_KEY_TRIGGER']!).existsSync()
        : DateTime.now().millisecondsSinceEpoch >= int.parse(env['E2EE_KEY_ACTION_AT_MS']!) + keyEpoch * 10000);
    if (keyEpoch < keyRounds && (env['E2EE_KEY_ACTION'] ?? 'none') != 'none' &&
        rotationReady) {
      if (env['E2EE_KEY_ACTION'] == 'slot') {
        provider!.sendIndex = 1;
        await room!.e2eeManager!.setKeyIndex(1);
      } else if (env['E2EE_KEY_ACTION'] == 'ratchet') {
        await provider!.ratchetSharedKey(keyIndex: 0);
      } else {
        final next = env['E2EE_NEXT_TEST_KEY']! + (keyRounds > 1 ? '-${keyEpoch + 1}' : '');
        await provider!.setSharedKey(next, keyIndex: 0);
      }
      keyEpoch++;
      record('key_changed', {'action': env['E2EE_KEY_ACTION']});
    }
    if (keyEpoch == 1 && !receiveKeyReinstalled && env['E2EE_RECOVER_INVALID_KEY'] == '1' &&
        DateTime.now().millisecondsSinceEpoch >= int.parse(env['E2EE_KEY_ACTION_AT_MS']!) + 5000) {
      await provider!.setSharedKey(env['E2EE_NEXT_TEST_KEY']!, keyIndex: 1);
      receiveKeyReinstalled = true;
      record('receive_key_reinstalled', {'slot': 1});
    }
    if (env['E2EE_CAMERA_CONTENT'] == '1') {
      await sampleCameraCorrespondence(room!, List<Track>.from(tracks), keyEpoch == 0 ? 4 : 6, record);
    }
    await publishRelayVideo();
    if (productBoard != null) {
      productBoardQueue = productBoardQueue.then((_) => productBoard!.heartbeat(keyEpoch == 0 ? 4 : 6));
      await productBoardQueue;
    }
    if (keyEpoch > 0 && env['E2EE_KEY_ACTION'] == 'replace') {
      // Compare only in memory. Exported material and identities are never logged.
      final expected = Uint8List.fromList((env['E2EE_NEXT_TEST_KEY']! +
          (keyRounds > 1 ? '-$keyEpoch' : '')).codeUnits);
      final initial = Uint8List.fromList(env['E2EE_TEST_KEY']!.codeUnits);
      final shared = await provider!.exportSharedKey(keyIndex: 0);
      record('key_generation_check', {'scope': 'shared', 'matches_current_generation': listEquals(shared, expected)});
      final identities = <String, String>{
        if (room?.localParticipant != null) room!.localParticipant!.identity: 'local',
        for (final peer in room!.remoteParticipants.values) peer.identity: 'remote',
      };
      for (final entry in identities.entries) {
        final installed = await provider!.exportKey(entry.key, 0);
        record('key_generation_check', {'scope': entry.value, 'matches_current_generation': listEquals(installed, expected),
          'matches_initial_generation': listEquals(installed, initial)});
      }
    }
    for (final track in tracks.toList()) {
      if (env['E2EE_PRODUCT_LIFECYCLE'] == '1' && track is LocalTrack) {
        record('product_sender_identity', {'kind': track.kind.name,
          'sender_present': track.sender != null, 'sender_kind': track.sender?.track?.kind,
          'matches_source': track.sender?.track?.id == track.mediaStreamTrack.id});
      }
      final reports = track is LocalTrack ? await track.sender?.getStats() : await track.receiver?.getStats();
      final codecs = <String, String>{};
      for (final report in reports ?? <rtc.StatsReport>[]) {
        final mime = report.values['mimeType'];
        if (report.type == 'codec' && mime is String &&
            const {'audio/opus', 'video/vp8', 'video/vp9', 'video/h264', 'video/av1'}.contains(mime.toLowerCase())) {
          codecs[report.id] = mime.toLowerCase();
        }
      }
      for (final report in reports ?? <rtc.StatsReport>[]) {
        if (report.type != 'outbound-rtp' && report.type != 'inbound-rtp' && report.type != 'media-source') continue;
        final codec = codecs[report.values['codecId']];
        final reportedKind = report.values['kind'] ?? report.values['mediaType'] ?? codec?.split('/').first;
        if (reportedKind != null && reportedKind.toString().toLowerCase() != track.kind.name.toLowerCase()) {
          record('stats_kind_mismatch', {'requested_kind': track.kind.name, 'reported_kind': reportedKind});
          continue;
        }
        final safe = <String, Object?>{};
        for (final name in ['packetsSent', 'packetsReceived', 'packetsLost', 'framesEncoded',
          'framesDecoded', 'framesReceived', 'framesSent', 'framesDropped', 'bytesSent', 'bytesReceived',
          'totalSamplesReceived', 'totalAudioEnergy', 'concealedSamples', 'jitter', 'totalDecodeTime',
          'totalEncodeTime', 'keyFramesDecoded', 'keyFramesEncoded', 'frames', 'framesPerSecond', 'width', 'height', 'frameWidth', 'frameHeight']) {
          final value = report.values[name];
          if (value is num || value is bool) safe[name] = value;
        }
        record(report.type == 'media-source' ? 'media_source' : 'rtp', {'track': track.sid, 'kind': track.kind.name, 'direction': track is LocalTrack ? 'tx' : 'rx',
          'observed_codec': codecs[report.values['codecId']], 'codec_measurement': 'rtp_stats_codec_id',
          'counters': safe, 'crypto_frame_count': null, 'crypto_frame_count_reason': 'not_exposed_by_official_plugin'});
      }
    }
    if (room?.connectionState == ConnectionState.connected && room!.remoteParticipants.isNotEmpty) {
      if (env['E2EE_PRODUCT_BYTE_STREAMS'] == '1') {
        await room!.localParticipant!.publishData(Uint8List.fromList(utf8.encode('bytes-ready-${keyEpoch == 0 ? 4 : 6}')),
            reliable: true, topic: 'e2ee.bytes.control');
      }
      await room!.localParticipant!.publishData(Uint8List.fromList(utf8.encode('e2ee-probe-public-test-payload')),
        reliable: true, topic: 'e2ee.probe');
      await room!.localParticipant!.sendText('e2ee-probe-public-test-stream',
        options: SendTextOptions(topic: 'e2ee.stream'));
      record('data_sent');
      if (env['E2EE_RPC'] == '1' && !rpcStarted &&
          DateTime.now().millisecondsSinceEpoch >= int.parse(env['E2EE_RPC_AT_MS']!)) {
        rpcStarted = true;
        try {
          final result = await room!.localParticipant!.performRpc(PerformRpcParams(
            destinationIdentity: room!.remoteParticipants.values.first.identity,
            method: 'e2ee.echo', payload: 'e2ee-public-rpc-request',
            responseTimeoutMs: const Duration(seconds: 5)));
          record('rpc_response', {'content_valid': result == 'e2ee-public-rpc-response'});
        } catch (e) { record('rpc_failed', {'type': e.runtimeType.toString()}); }
      }
    }
  } catch (e) { record('sample_failed', {'type': e.runtimeType.toString()}); }
  finally { sampling = false; await evidence.flush(); }
}
Future<void> finish() async {
  if (closing) return;
  closing = true;
  sampleTimer?.cancel(); finishTimer?.cancel();
  while (sampling) { await Future<void>.delayed(const Duration(milliseconds: 10)); }
  for (final stop in pcmStops.values.toList()) { await stop(); }
  pcmStops.clear(); pcmCounts.clear();
  try { await room?.disconnect(); await room?.dispose(); record('closed'); }
  catch (e) { record('close_failed', {'type': e.runtimeType.toString()}); }
  await evidence.flush(); await evidence.close(); exit(0);
}
void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  Logger.root.level = Level.OFF;
  evidence = File('${env['E2EE_EVIDENCE_DIR']}/flutter.jsonl').openWrite(mode: FileMode.writeOnlyAppend);
  FlutterError.onError = (details) => record('ui_error', {'type': details.exception.runtimeType.toString()});
  runApp(const ProbeApp());
  WidgetsBinding.instance.addPostFrameCallback((_) async {
    record('starting', {'sdk': '2.11.0', 'flutter_webrtc': '1.6.0'});
    try { await join(); }
    catch (e) { record('join_failed', {'type': e.runtimeType.toString()}); await evidence.flush(); }
    sampleTimer = Timer.periodic(const Duration(seconds: 2), (_) { unawaited(sample()); });
    finishTimer = Timer(Duration(seconds: int.parse(env['E2EE_DURATION'] ?? '90')), () { unawaited(finish()); });
  });
}
class ProbeApp extends StatelessWidget {
  const ProbeApp({super.key});
  @override Widget build(BuildContext context) => MaterialApp(home: Scaffold(
    backgroundColor: Colors.black,
    body: Column(children: [const Text('E2EE test fixture', style: TextStyle(color: Colors.white, fontSize: 28)),
      Expanded(child: ValueListenableBuilder<VideoTrack?>(valueListenable: rendered,
        builder: (context, track, _) => track == null
          ? const Center(child: Icon(Icons.lock, color: Colors.green, size: 120))
          : VideoTrackRenderer(track)))])));
}
