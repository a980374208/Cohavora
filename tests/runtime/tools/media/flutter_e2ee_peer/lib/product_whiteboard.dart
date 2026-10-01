import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:crypto/crypto.dart';

// Bounded public test application protocol. Encryption and transport are
// exclusively supplied by the official SDK via send(). No user data is saved.
class ProductWhiteboard {
  ProductWhiteboard(this.send, this.record, this.authority);
  final Future<void> Function(String, Uint8List) send;
  final void Function(String, Map<String, Object?>) record;
  final bool authority;
  final snapshots = <String, Map<int, String>>{};
  final assets = <String, Map<int, Uint8List>>{};
  final completedAssets = <String>{};
  final requested = <int>{};
  final verified = <int>{};
  final proposals = <int>{};
  final sentAuthority = <int>{};
  String document = '';

  Uint8List asset(int epoch) => File(Platform.environment['E2EE_WHITEBOARD_ASSET$epoch']!).readAsBytesSync();
  String hash(List<int> bytes) => sha256.convert(bytes).toString();
  // Native whiteboard v1 uses this non-security checksum for document IDs,
  // actors and snapshots; PNG assets separately use SHA-256.
  String documentHash(List<int> bytes) {
    var result = BigInt.parse('1469598103934665603');
    final mask = (BigInt.one << 64) - BigInt.one;
    for (final byte in bytes) {
      result = ((result ^ BigInt.from(byte)) * BigInt.from(1099511628211)) & mask;
    }
    return result.toRadixString(16).padLeft(16, '0');
  }
  Map<String, dynamic> envelope(String kind, int seq) => {
    'v': 1, 'kind': kind, 'documentId': document,
    'authority': authority ? 'receiver' : 'publisher', 'seq': '$seq'};
  Future<void> emit(String topic, Map<String, dynamic> value) =>
      send('whiteboard.v1.$topic', Uint8List.fromList(utf8.encode(jsonEncode(value))));
  Map<String, dynamic> object(int epoch, {bool reply = false}) => {
    'id': reply ? 'e2ee-flutter-object-$epoch' : 'e2ee-object-$epoch',
    'author': 'actor-${documentHash(utf8.encode('receiver'))}', 'kind': 6,
    'color': 0x1950be, 'width': 2, 'fontSize': 18,
    'points': [[10, 10], [60, 60]],
    'text': reply ? 'public flutter whiteboard $epoch' : 'public e2ee whiteboard $epoch'};
  Map<String, dynamic> command(int epoch, Map<String, dynamic> context) => {
    'id': 'flutter-command-$epoch', 'actor': 'actor-${documentHash(utf8.encode('receiver'))}', 'context': context,
    'kind': 0, 'target': '', 'assetId': '', 'pageWidth': 320, 'pageHeight': 180,
    'object': object(epoch, reply: true)};

  Future<void> receive(String topic, Uint8List bytes, int epoch) async {
    if (bytes.length > 12288 || !topic.startsWith('whiteboard.v1.')) throw StateError('bounds');
    final value = jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
    if (value['v'] != 1 || (epoch != 4 && epoch != 6)) throw StateError('profile');
    document = value['documentId'] as String;
    if (authority) {
      if (value['kind'] == 'discover' || value['kind'] == 'sync_request') {
        await publishAuthority(epoch);
      }
      return;
    }
    final kind = value['kind'];
    if (kind == 'commit') {
      final entry = value['entry'] as Map<String, dynamic>;
      if (entry['control'] != true) {
        final c = entry['command'] as Map<String, dynamic>;
        if (c['object']['id'] == 'e2ee-object-$epoch') {
          record('product_board_operation_received', {'epoch': epoch,
            'valid': c['object']['text'] == 'public e2ee whiteboard $epoch'});
        }
        // Request a full snapshot through the protocol's ahead-of-authority
        // recovery path after the image-page commit, once per install.
        if (c['kind'] == 7 && requested.add(epoch)) {
          await emit('sync', {...envelope('sync_request', 0),
            'baseSeq': '${int.parse(value['seq']) + 1}'});
        }
      }
    } else if (kind == 'asset_chunk') {
      final count = value['chunkCount'] as int, index = value['chunkIndex'] as int;
      if (count < 1 || count > 32 || index < 0 || index >= count || assets.length > 4) throw StateError('asset_bounds');
      final id = value['assetId'] as String;
      final chunks = assets.putIfAbsent(id, () => {});
      chunks[index] = base64Decode(value['payload'] as String);
      if (chunks.length == count) {
        final assembled = BytesBuilder();
        for (var i = 0; i < count; i++) { assembled.add(chunks[i]!); }
        final content = assembled.takeBytes();
        final valid = hash(content) == id && id == hash(asset(epoch)) && content.length == value['bytes'];
        if (completedAssets.add(id)) record('product_board_asset_received', {'epoch': epoch, 'valid': valid});
        assets.remove(id);
      }
    } else if (kind == 'snapshot_chunk') {
      final count = value['chunkCount'] as int, index = value['chunkIndex'] as int;
      if (count < 1 || count > 32 || index < 0 || index >= count || snapshots.length > 4) throw StateError('snapshot_bounds');
      final id = value['snapshotId'] as String;
      final chunks = snapshots.putIfAbsent(id, () => {});
      chunks[index] = value['payload'] as String;
      if (chunks.length != count) return;
      final text = List.generate(count, (i) => chunks[i]!).join();
      snapshots.remove(id);
      final doc = jsonDecode(text) as Map<String, dynamic>;
      final pages = doc['pages'] as List;
      final valid = documentHash(utf8.encode(text)) == value['snapshotHash'] && doc['id'] == document &&
        pages.any((p) => (p['objects'] as List).any((o) => o['id'] == 'e2ee-object-$epoch' &&
          o['text'] == 'public e2ee whiteboard $epoch')) &&
        pages.any((p) => p['backgroundAssetId'] == hash(asset(epoch)));
      if (verified.add(epoch)) record('product_board_snapshot_received', {'epoch': epoch, 'valid': valid});
      if (valid && proposals.add(epoch)) {
        final page = pages.firstWhere((p) => p['id'] == doc['active']);
        await emit('ops', {...envelope('propose', int.parse(value['seq'])),
          'requestId': 'flutter-command-$epoch', 'command': command(epoch, {
            'documentId': document, 'pageId': page['id'], 'pageEpoch': page['epoch'],
            'interactionEpoch': doc['interactionEpoch']})});
      }
    } else if (kind == 'reject') {
      record('product_board_failed', {'reason': 'proposal_rejected'});
    }
  }

  Future<void> publishAuthority(int epoch) async {
    // This fixture is the authenticated room authority in this mode.
    if (!sentAuthority.add(epoch)) return;
    final pages = [for (final e in [4, if (epoch == 6) 6]) {
      'id': 'image-page-$e', 'epoch': '1', 'width': 320, 'height': 180,
      'backgroundAssetId': hash(asset(e)), 'objects': [object(e)]}];
    final doc = {'version': 2, 'id': document, 'owner': 'actor-${documentHash(utf8.encode('receiver'))}',
      'active': 'image-page-$epoch', 'interactionEpoch': '$epoch', 'pages': pages};
    final text = jsonEncode(doc);
    await emit('sync', {...envelope('snapshot_chunk', epoch), 'baseSeq': '$epoch',
      'snapshotId': 'flutter-snapshot-$epoch', 'snapshotHash': documentHash(utf8.encode(text)),
      'chunkIndex': 0, 'chunkCount': 1, 'locked': false, 'writersOpen': true,
      'writers': <String>[], 'payload': text});
    final content = asset(epoch);
    final partial = Platform.environment['E2EE_PRODUCT_BOARD_PARTIAL'] == '1';
    await emit('asset', {...envelope('asset_chunk', epoch), 'assetId': hash(content),
      'mime': 'image/png', 'width': 320, 'height': 180, 'bytes': content.length,
      'chunkIndex': 0, 'chunkCount': partial && epoch == 4 ? 2 : 1,
      'payload': base64Encode(partial && epoch == 4 ? content.sublist(0, content.length ~/ 2) : content)});
    if (partial && epoch == 4) record('product_board_partial_sent', {'epoch': epoch});
    if (partial && epoch == 6) {
      final old = asset(4);
      await emit('asset', {...envelope('asset_chunk', epoch), 'assetId': hash(old),
        'mime': 'image/png', 'width': 320, 'height': 180, 'bytes': old.length,
        'chunkIndex': 1, 'chunkCount': 2, 'payload': base64Encode(old.sublist(old.length ~/ 2))});
      record('product_board_old_tail_sent', {'epoch': epoch});
    }
    await emit('ops', {...envelope('commit', epoch + 1), 'entry': {
      'seq': '${epoch + 1}', 'control': false, 'command': command(epoch, {
        'documentId': document, 'pageId': 'image-page-$epoch', 'pageEpoch': '1',
        'interactionEpoch': '$epoch'})}});
    record('product_board_authority_sent', {'epoch': epoch});
  }

  Future<void> heartbeat(int epoch) async {
    if (!authority || document.isEmpty) return;
    // Same 2000 ms cadence as native Runtime::HeartbeatIntervalMs. A heartbeat
    // exposes a new sequence after key transition so normal sync can recover.
    await emit('presence', {...envelope('heartbeat', epoch + 1),
      'sentAtMs': '${DateTime.now().millisecondsSinceEpoch}'});
  }
}
