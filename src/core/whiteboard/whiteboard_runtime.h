#pragma once

#include "whiteboard_asset.h"
#include "whiteboard_protocol.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace livekit::whiteboard {

enum class CollaborationState { Synchronizing, Ready, ReadOnly, Frozen, Retired };

struct PeerInstance {
    std::string identity;
    std::uint64_t nativeRoomGeneration = 0;
    std::uint64_t incarnation = 0;
    bool operator==(const PeerInstance &other) const = default;
};

struct Projection {
    std::string snapshot;
    std::uint64_t sequence = 0;
    CollaborationState state = CollaborationState::Synchronizing;
    std::string authorityIdentity;
    std::string localIdentity;
    bool locked = false;
    bool writersOpen = true;
    std::vector<std::string> writers;
    bool canEdit = false;
    bool canAdmin = false;
    std::string status;
    std::vector<std::shared_ptr<const Asset>> assets;
};

struct RuntimeConfig {
    std::string documentId;
    std::string localIdentity;
    std::string authorityIdentity;
    bool localIsAuthority = false;
};

class Runtime final {
public:
    using Send = std::function<bool(std::string_view topic, std::string_view payload,
                                    const std::vector<std::string> &destinations)>;
    using Project = std::function<void(Projection)>;

    static constexpr std::size_t MaxLogEntries = 512;
    static constexpr std::size_t MaxRateEvents = 120;
    static constexpr std::uint64_t RateWindowMs = 10000;
    static constexpr std::uint64_t HeartbeatIntervalMs = 2000;
    static constexpr std::uint64_t AuthorityTimeoutMs = 8000;

    Runtime(RuntimeConfig config, Send send, Project project);
    const Document &document() const { return document_; }
    std::uint64_t sequence() const { return sequence_; }
    CollaborationState state() const { return state_; }
    bool locked() const { return locked_; }
    bool writersOpen() const { return writersOpen_; }

    void start(std::uint64_t nowMs);
    Result propose(Command command, std::uint64_t nowMs);
    Result importImage(Asset asset, std::string commandId, std::string pageId,
                       bool replaceCurrent, std::uint64_t nowMs);
    bool setLocked(bool locked, std::uint64_t nowMs);
    bool setWriters(bool writersOpen, std::vector<std::string> writers, std::uint64_t nowMs);
    void receive(std::string_view topic, std::string_view payload,
                 const PeerInstance &sender, std::uint64_t nowMs);
    void observePeer(const PeerInstance &peer);
    void peerLeft(const PeerInstance &peer);
    void setTransportReady(bool ready, std::uint64_t nowMs);
    void tick(std::uint64_t nowMs);
    void retire();

private:
    struct PendingProposal {
        Command command;
        std::uint64_t lastSentMs = 0;
    };
    struct SnapshotAssembly {
        std::string id;
        std::string hash;
        std::uint64_t sequence = 0;
        std::uint32_t count = 0;
        bool locked = false;
        bool writersOpen = true;
        std::vector<std::string> writers;
        std::vector<std::string> chunks;
        std::vector<bool> received;
        std::size_t bytes = 0;
    };
    struct AssetAssembly {
        std::string id;
        std::string mime;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t totalBytes = 0;
        std::uint32_t count = 0;
        std::vector<std::string> chunks;
        std::vector<bool> received;
        std::size_t bytes = 0;
        std::uint64_t lastProgressMs = 0;
    };
    struct OutboundAssetTransfer {
        std::string key;
        std::shared_ptr<const Asset> asset;
        std::vector<std::string> destinations;
        std::uint32_t nextChunk = 0;
    };

    bool send(const Message &message, const std::vector<std::string> &destinations = {});
    bool allowedWriter(std::string_view identity) const;
    bool currentPeer(const PeerInstance &peer) const;
    bool acceptRate(std::string_view identity, std::uint64_t nowMs);
    Result commitProposal(Command command, const PeerInstance &sender, std::uint64_t nowMs,
                          const std::vector<std::string> &replyTo = {},
                          bool *responseSent = nullptr);
    void appendAndBroadcast(CommitEntry entry);
    bool applyEntry(const CommitEntry &entry);
    void requestSync();
    void sendDescriptor(const std::vector<std::string> &destination);
    void sendRecovery(std::uint64_t fromSequence, const std::vector<std::string> &destination);
    void sendSnapshot(const std::vector<std::string> &destination);
    void receiveSnapshot(const Message &message, std::uint64_t nowMs);
    bool cacheAsset(Asset asset);
    bool queueAsset(const std::shared_ptr<const Asset> &asset,
                    const std::vector<std::string> &destination);
    void pumpAssetTransfers();
    void receiveAsset(const Message &message, std::uint64_t nowMs);
    void requestMissingAssets(std::uint64_t nowMs);
    bool missingActiveAsset() const;
    void publish(std::string status = {});
    void rememberProposal(std::string key, CommitEntry entry);
    bool sendProposal(PendingProposal &proposal, std::uint64_t nowMs);
    std::vector<std::string> writersVector() const;

    RuntimeConfig config_;
    Send send_;
    Project project_;
    Document document_;
    CollaborationState state_ = CollaborationState::Synchronizing;
    bool transportReady_ = true;
    bool started_ = false;
    bool locked_ = false;
    bool writersOpen_ = true;
    std::set<std::string> writers_;
    std::uint64_t sequence_ = 0;
    std::uint64_t lastHeartbeatSent_ = 0;
    std::uint64_t lastAuthoritySeen_ = 0;
    std::uint64_t lastSyncRequest_ = 0;
    std::deque<CommitEntry> log_;
    std::map<std::string, CommitEntry> proposals_;
    std::deque<std::string> proposalOrder_;
    std::map<std::string, std::deque<std::uint64_t>> rateEvents_;
    std::map<std::string, PendingProposal> pendingProposals_;
    std::map<std::string, PeerInstance> peers_;
    SnapshotAssembly snapshot_;
    std::map<std::string, std::shared_ptr<const Asset>> assets_;
    std::size_t assetBytes_ = 0;
    std::map<std::string, AssetAssembly> assetAssemblies_;
    std::map<std::string, std::uint64_t> requestedAssets_;
    std::deque<OutboundAssetTransfer> outboundAssets_;
};

} // namespace livekit::whiteboard
