#include <iostream>
#include "tests/support/test_check.h"
#include <vector>
#include <string>
#include "key_provider.h"
#include "frame_cryptor.h"
#include "room.h"

using namespace livekit;

class TestE2eeListener : public RoomListener {
public:
    void OnE2eeStateChanged(const std::string& participant_identity, const std::string& track_sid, EncryptionState state) override {
        last_identity = participant_identity;
        last_state = state;
        event_count++;
    }

    std::string last_identity;
    EncryptionState last_state{EncryptionState::NEW};
    int event_count{0};
};

void TestKeyProviderAndRatchet() {
    std::cout << "[TEST 1] Testing KeyProvider & Key Ratcheting..." << std::endl;

    KeyProviderOptions opts;
    opts.shared_key = true;
    opts.ratchet_salt = "LKFrameEncryptionKey";

    KeyProvider provider(opts);
    std::vector<uint8_t> secret_key = {'m', 'y', '_', 's', 'e', 'c', 'r', 'e', 't', '_', 'k', 'e', 'y'};
    provider.SetSharedKey(secret_key, 0);

    TEST_CHECK(provider.GetSharedKey(0) == secret_key);
    std::cout << "  [PASS] SetSharedKey and GetSharedKey verified." << std::endl;

    // Ratchet Key Index 0 -> Index 1
    std::vector<uint8_t> ratcheted_key = provider.RatchetSharedKey(0);
    TEST_CHECK(!ratcheted_key.empty());
    TEST_CHECK(ratcheted_key != secret_key);
    TEST_CHECK(provider.GetSharedKey(1) == ratcheted_key);
    std::cout << "  [PASS] RatchetSharedKey successfully derived 256-bit ratcheted key." << std::endl;

    // Per-participant key test
    std::vector<uint8_t> alice_key = {'a', 'l', 'i', 'c', 'e', '_', 'k', 'e', 'y'};
    provider.SetKey("alice", 0, alice_key);
    TEST_CHECK(provider.GetKey("alice", 0) == alice_key);

    std::vector<uint8_t> alice_ratcheted = provider.RatchetKey("alice", 0);
    TEST_CHECK(!alice_ratcheted.empty());
    TEST_CHECK(provider.GetKey("alice", 1) == alice_ratcheted);
    std::cout << "  [PASS] Per-participant key setting & ratcheting verified." << std::endl;
}

void TestFrameCryptorAesGcm() {
    std::cout << "[TEST 2] Testing FrameCryptor AES-256-GCM Payload Encryption & Decryption..." << std::endl;

    KeyProviderOptions opts;
    auto provider = std::make_shared<KeyProvider>(opts);
    std::vector<uint8_t> master_key(32, 0x42);
    provider->SetSharedKey(master_key, 0);

    FrameCryptor cryptor("user_alice", "TR_video_123", provider);

    std::string raw_string = "Hello LiveKit E2EE AES-256-GCM Encrypted Video Frame Payload!";
    std::vector<uint8_t> plaintext(raw_string.begin(), raw_string.end());

    std::vector<uint8_t> ciphertext;
    bool enc_ok = cryptor.EncryptFrame(plaintext, ciphertext);
    TEST_CHECK(enc_ok);
    TEST_CHECK(ciphertext != plaintext);
    TEST_CHECK(ciphertext.size() == plaintext.size() + 12 + 16); // IV (12) + Tag (16)
    TEST_CHECK(cryptor.state() == EncryptionState::OK);
    std::cout << "  [PASS] Frame payload AES-256-GCM encryption verified (Length: " << ciphertext.size() << " bytes)." << std::endl;

    std::vector<uint8_t> decrypted;
    bool dec_ok = cryptor.DecryptFrame(ciphertext, decrypted);
    TEST_CHECK(dec_ok);
    TEST_CHECK(decrypted == plaintext);
    std::string decrypted_str(decrypted.begin(), decrypted.end());
    TEST_CHECK(decrypted_str == raw_string);
    std::cout << "  [PASS] Frame payload AES-256-GCM decryption verified ('" << decrypted_str << "')." << std::endl;

    // Tamper ciphertext test
    std::vector<uint8_t> tampered_ciphertext = ciphertext;
    tampered_ciphertext[ tampered_ciphertext.size() - 1 ] ^= 0xFF; // flip bits in Tag
    std::vector<uint8_t> tampered_decrypted;
    bool tamper_dec_ok = cryptor.DecryptFrame(tampered_ciphertext, tampered_decrypted);
    TEST_CHECK(!tamper_dec_ok);
    TEST_CHECK(cryptor.state() == EncryptionState::DECRYPTION_FAILED);
    std::cout << "  [PASS] Decryption failed as expected when ciphertext/tag was tampered." << std::endl;
}

void TestDataPacketCryptor() {
    std::cout << "[TEST 3] Testing DataPacketCryptor DataChannel Encryption..." << std::endl;

    KeyProviderOptions opts;
    auto provider = std::make_shared<KeyProvider>(opts);
    provider->SetSharedKey(std::vector<uint8_t>(32, 0x88), 0);

    DataPacketCryptor data_cryptor(provider);
    std::string secret_chat = "Confidential RPC / Chat Data Packet Payload";
    std::vector<uint8_t> plain_bytes(secret_chat.begin(), secret_chat.end());

    std::vector<uint8_t> encrypted_bytes;
    bool enc_ok = data_cryptor.EncryptData(plain_bytes, encrypted_bytes);
    TEST_CHECK(enc_ok);

    std::vector<uint8_t> decrypted_bytes;
    bool dec_ok = data_cryptor.DecryptData(encrypted_bytes, decrypted_bytes);
    TEST_CHECK(dec_ok);
    TEST_CHECK(decrypted_bytes == plain_bytes);
    std::cout << "  [PASS] DataPacketCryptor DataChannel encryption & decryption verified." << std::endl;
}

int main() {
    std::cout << "[TEST] Starting End-to-End Encryption (E2EE) Unit Tests..." << std::endl;
    TestKeyProviderAndRatchet();
    TestFrameCryptorAesGcm();
    TestDataPacketCryptor();
    std::cout << "[SUCCESS] ALL E2EE Unit Tests Passed!" << std::endl;
    return 0;
}
