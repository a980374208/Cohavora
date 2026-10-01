import 'dart:convert';
import 'package:flutter_test/flutter_test.dart';
import '../tools/media/flutter_e2ee_peer/lib/product_whiteboard.dart';

void main() {
  test('native whiteboard v1 actor uses documented 64-bit checksum', () {
    final board = ProductWhiteboard((_, __) async {}, (_, __) {}, false);
    expect(board.documentHash(utf8.encode('receiver')), 'a3bd48ebc86810e6');
    expect(board.documentHash([]), '14650fb0739d0383');
    expect(board.object(4)['author'], 'actor-a3bd48ebc86810e6');
    expect(board.command(4, {})['actor'], board.object(4)['author']);
    expect(board.hash(utf8.encode('receiver')).length, 64);
  });
}
