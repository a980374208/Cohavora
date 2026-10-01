import 'dart:convert';
import 'dart:typed_data';
import 'package:flutter/services.dart';
import 'package:livekit_client/livekit_client.dart';

// Test-only memory sampling. Never call captureFrame (it writes a PNG).
Future<void> sampleCameraCorrespondence(Room room, List<Track> tracks, int epoch,
    void Function(String, Map<String, Object?>) record) async {
  const channel = MethodChannel('FlutterWebRTC.Method');
  for (final track in tracks) {
    if (track is! VideoTrack || track.source != TrackSource.camera) continue;
    try {
      final cells = await channel.invokeListMethod<int>('e2eeMemoryFrameSample',
          {'trackId': track.mediaStreamTrack.id});
      if (cells == null || cells.length != 64) throw StateError('invalid_frame_summary');
      await room.localParticipant!.publishData(Uint8List.fromList(utf8.encode(
          jsonEncode({'epoch': epoch, 'cells': cells}))),
          reliable: true, topic: 'e2ee.camera.correspondence');
      record('product_camera_sample_sent', {'epoch': epoch});
    } catch (error) {
      record('product_camera_sample_failed', {'type': error.runtimeType.toString(), 'code': error is PlatformException && const ['frame_unavailable', 'invalid_track', 'sample_busy'].contains(error.code) ? error.code : 'other'});
    }
  }
}
