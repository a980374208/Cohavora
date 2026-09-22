#include "whiteboard_runtime.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace livekit::whiteboard {
namespace {
constexpr std::size_t MaxCollaborativeStrokePoints = 192;
constexpr std::size_t MaxAssetAssemblies = 2;
constexpr std::size_t MaxOutboundAssetTransfers = 16;
constexpr std::size_t AssetChunksPerPump = 8;
constexpr std::uint64_t AssetAssemblyTimeoutMs = 10000;

std::string proposalKey(std::string_view actor, std::string_view id) {
    return std::string(actor) + "/" + std::string(id);
}
std::string actorId(std::string_view identity) { return "actor-" + contentHash(identity); }

double distanceSquared(const Point &point, const Point &start, const Point &end) {
    const auto dx = end.x - start.x;
    const auto dy = end.y - start.y;
    const auto lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 0.0) {
        const auto x = point.x - start.x;
        const auto y = point.y - start.y;
        return x * x + y * y;
    }
    const auto projection = std::clamp(
        ((point.x - start.x) * dx + (point.y - start.y) * dy) / lengthSquared,
        0.0, 1.0);
    const auto x = point.x - (start.x + projection * dx);
    const auto y = point.y - (start.y + projection * dy);
    return x * x + y * y;
}

std::vector<Point> simplifyStroke(const std::vector<Point> &points, double tolerance) {
    if (points.size() <= 2) return points;
    std::vector<bool> retained(points.size(), false);
    retained.front() = retained.back() = true;
    std::vector<std::pair<std::size_t, std::size_t>> ranges{{0, points.size() - 1}};
    const auto toleranceSquared = tolerance * tolerance;
    while (!ranges.empty()) {
        const auto [first, last] = ranges.back();
        ranges.pop_back();
        double furthestDistance = 0.0;
        std::size_t furthest = first;
        for (auto index = first + 1; index < last; ++index) {
            const auto distance = distanceSquared(points[index], points[first], points[last]);
            if (distance > furthestDistance) {
                furthestDistance = distance;
                furthest = index;
            }
        }
        if (furthestDistance <= toleranceSquared) continue;
        retained[furthest] = true;
        ranges.push_back({first, furthest});
        ranges.push_back({furthest, last});
    }
    std::vector<Point> result;
    result.reserve(points.size());
    for (std::size_t index = 0; index < points.size(); ++index)
        if (retained[index]) result.push_back(points[index]);
    return result;
}

std::vector<Point> evenlySample(const std::vector<Point> &points) {
    std::vector<Point> result;
    result.reserve(MaxCollaborativeStrokePoints);
    for (std::size_t index = 0; index < MaxCollaborativeStrokePoints; ++index) {
        const auto source = index * (points.size() - 1) /
            (MaxCollaborativeStrokePoints - 1);
        result.push_back(points[source]);
    }
    return result;
}

void normalizeCollaborativeStroke(Command &command) {
    if (command.kind != CommandKind::Add ||
        (command.object.kind != ObjectKind::Pen &&
         command.object.kind != ObjectKind::Highlighter) ||
        command.object.points.size() <= MaxCollaborativeStrokePoints) return;

    const auto &points = command.object.points;
    const auto widthTolerance = std::isfinite(command.object.width)
        ? command.object.width * 0.125 : 0.5;
    const auto minimumTolerance = std::max(0.5, widthTolerance);
    auto simplified = simplifyStroke(points, minimumTolerance);
    if (simplified.size() <= MaxCollaborativeStrokePoints) {
        command.object.points = std::move(simplified);
        return;
    }

    double minX = points.front().x, maxX = points.front().x;
    double minY = points.front().y, maxY = points.front().y;
    for (const auto &point : points) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    double low = minimumTolerance;
    double high = std::hypot(maxX - minX, maxY - minY);
    if (!std::isfinite(high) || high <= low) {
        command.object.points = evenlySample(simplified);
        return;
    }
    auto best = simplifyStroke(points, high);
    for (int iteration = 0; iteration < 24; ++iteration) {
        const auto middle = (low + high) / 2.0;
        auto candidate = simplifyStroke(points, middle);
        if (candidate.size() > MaxCollaborativeStrokePoints) {
            low = middle;
        } else {
            high = middle;
            best = std::move(candidate);
        }
    }
    command.object.points = best.size() <= MaxCollaborativeStrokePoints
        ? std::move(best) : evenlySample(simplified);
}
}

Runtime::Runtime(RuntimeConfig config, Send send, Project project)
    : config_(std::move(config)), send_(std::move(send)), project_(std::move(project)),
      document_(config_.documentId, actorId(config_.authorityIdentity)) {
    state_ = config_.localIsAuthority ? CollaborationState::Ready : CollaborationState::Synchronizing;
}

void Runtime::start(std::uint64_t nowMs) {
    if (started_ || state_ == CollaborationState::Retired) return;
    started_ = true;
    lastAuthoritySeen_ = nowMs;
    if (!transportReady_) state_ = CollaborationState::ReadOnly;
    if (config_.localIsAuthority) {
        sendDescriptor({});
        lastHeartbeatSent_ = nowMs;
    } else if (transportReady_) {
        Message discover;
        discover.kind = MessageKind::Discover;
        discover.documentId = config_.documentId;
        discover.authorityIdentity = config_.authorityIdentity;
        discover.sequence = sequence_;
        discover.baseSequence = sequence_;
        send(discover, {config_.authorityIdentity});
        lastSyncRequest_ = nowMs;
    }
    publish(config_.localIsAuthority ? "ready" : "synchronizing");
}

bool Runtime::send(const Message &message, const std::vector<std::string> &destinations) {
    if (!transportReady_ || state_ == CollaborationState::Retired || !send_) return false;
    const auto bytes = encodeMessage(message);
    return bytes && send_(topicFor(message.kind), *bytes, destinations);
}

bool Runtime::allowedWriter(std::string_view identity) const {
    return identity == config_.authorityIdentity ||
        (!locked_ && (writersOpen_ || writers_.count(std::string(identity)) != 0));
}

bool Runtime::currentPeer(const PeerInstance &peer) const {
    const auto found = peers_.find(peer.identity);
    return found == peers_.end() || found->second == peer;
}

bool Runtime::acceptRate(std::string_view identity, std::uint64_t nowMs) {
    auto &events = rateEvents_[std::string(identity)];
    while (!events.empty() && nowMs - events.front() > RateWindowMs) events.pop_front();
    if (events.size() >= MaxRateEvents) return false;
    events.push_back(nowMs);
    return true;
}

Result Runtime::propose(Command command, std::uint64_t nowMs) {
    if (state_ != CollaborationState::Ready || !transportReady_) return {Status::Forbidden, {}};
    if (!allowedWriter(config_.localIdentity) || !acceptRate(config_.localIdentity, nowMs))
        return {Status::Forbidden, {}};
    command.actor = actorId(config_.localIdentity);
    if (command.kind == CommandKind::Add) command.object.author = command.actor;
    normalizeCollaborativeStroke(command);
    Message probe;
    probe.kind = MessageKind::Propose;
    probe.documentId = config_.documentId;
    probe.authorityIdentity = config_.authorityIdentity;
    probe.sequence = sequence_;
    probe.requestId = command.id;
    probe.entry.command = command;
    if (!encodeMessage(probe)) return {Status::LimitReached, {}};
    if (config_.localIsAuthority) {
        PeerInstance local{config_.localIdentity, 0, 0};
        return commitProposal(std::move(command), local, nowMs);
    }
    if (pendingProposals_.size() >= 256) return {Status::LimitReached, {}};
    PendingProposal pending{std::move(command), 0};
    const auto id = pending.command.id;
    auto [entry, inserted] = pendingProposals_.emplace(id, std::move(pending));
    if (!inserted) return {Status::Duplicate, {}};
    if (!sendProposal(entry->second, nowMs)) {
        state_ = CollaborationState::ReadOnly;
        publish("transport unavailable");
        return {Status::NoChange, {}};
    }
    return {Status::NoChange, {}};
}

Result Runtime::importImage(Asset asset, std::string commandId, std::string pageId,
                            bool replaceCurrent, std::uint64_t nowMs) {
    if (!config_.localIsAuthority || state_ != CollaborationState::Ready ||
        !transportReady_ || !validAsset(asset)) return {Status::Forbidden, {}};
    const bool alreadyCached = assets_.count(asset.id) != 0;
    if (!alreadyCached && (assets_.size() >= MaxAssets ||
        asset.bytes.size() > MaxAssetTotalBytes - assetBytes_)) return {Status::LimitReached, {}};
    const auto assetId = asset.id;
    if (!alreadyCached && !cacheAsset(std::move(asset))) return {Status::LimitReached, {}};
    if (!queueAsset(assets_.at(assetId), {})) {
        if (!alreadyCached) {
            assetBytes_ -= assets_.at(assetId)->bytes.size();
            assets_.erase(assetId);
        }
        return {Status::LimitReached, {}};
    }

    Command command;
    command.id = std::move(commandId);
    command.context = document_.context();
    command.kind = replaceCurrent ? CommandKind::SetBackground : CommandKind::AddImagePage;
    command.target = std::move(pageId);
    command.assetId = assetId;
    const auto &cached = *assets_.at(assetId);
    command.pageWidth = cached.width;
    command.pageHeight = cached.height;
    PeerInstance local{config_.localIdentity, 0, 0};
    const auto result = commitProposal(std::move(command), local, nowMs);
    if (!result.changed() && !alreadyCached) {
        assetBytes_ -= cached.bytes.size();
        assets_.erase(assetId);
    }
    return result;
}

bool Runtime::sendProposal(PendingProposal &proposal, std::uint64_t nowMs) {
    Message message;
    message.kind = MessageKind::Propose;
    message.documentId = config_.documentId;
    message.authorityIdentity = config_.authorityIdentity;
    message.sequence = sequence_;
    message.requestId = proposal.command.id;
    message.entry.command = proposal.command;
    proposal.lastSentMs = nowMs;
    return send(message, {config_.authorityIdentity});
}

Result Runtime::commitProposal(Command command, const PeerInstance &sender, std::uint64_t,
                               const std::vector<std::string> &replyTo, bool *responseSent) {
    if (responseSent) *responseSent = false;
    command.actor = actorId(sender.identity);
    if (command.kind == CommandKind::Add) command.object.author = command.actor;
    normalizeCollaborativeStroke(command);
    const auto key = proposalKey(sender.identity, command.id);
    if (const auto duplicate = proposals_.find(key); duplicate != proposals_.end()) {
        Message response;
        response.kind = MessageKind::Commit;
        response.documentId = config_.documentId;
        response.authorityIdentity = config_.authorityIdentity;
        response.sequence = duplicate->second.sequence;
        response.entry = duplicate->second;
        send(response, replyTo);
        if (responseSent) *responseSent = true;
        return {Status::Duplicate, {}};
    }
    if (!allowedWriter(sender.identity)) return {Status::Forbidden, {}};
    const auto result = document_.apply(command);
    if (!result.changed()) return result;
    CommitEntry entry;
    entry.sequence = ++sequence_;
    entry.command = std::move(command);
    rememberProposal(key, entry);
    appendAndBroadcast(std::move(entry));
    publish("ready");
    return result;
}

void Runtime::rememberProposal(std::string key, CommitEntry entry) {
    constexpr std::size_t MaxProposals = 2048;
    if (proposalOrder_.size() == MaxProposals) {
        proposals_.erase(proposalOrder_.front());
        proposalOrder_.pop_front();
    }
    proposalOrder_.push_back(key);
    proposals_.emplace(std::move(key), std::move(entry));
}

void Runtime::appendAndBroadcast(CommitEntry entry) {
    if (log_.size() == MaxLogEntries) log_.pop_front();
    log_.push_back(entry);
    Message commit;
    commit.kind = MessageKind::Commit;
    commit.documentId = config_.documentId;
    commit.authorityIdentity = config_.authorityIdentity;
    commit.sequence = entry.sequence;
    commit.entry = std::move(entry);
    send(commit);
}

bool Runtime::setLocked(bool locked, std::uint64_t) {
    if (!config_.localIsAuthority || state_ != CollaborationState::Ready || locked_ == locked) return false;
    locked_ = locked;
    document_.advanceInteractionBarrier();
    CommitEntry entry;
    entry.sequence = ++sequence_;
    entry.control = true;
    entry.locked = locked_;
    entry.writersOpen = writersOpen_;
    entry.writers = writersVector();
    appendAndBroadcast(std::move(entry));
    publish("ready");
    return true;
}

bool Runtime::setWriters(bool writersOpen, std::vector<std::string> writers, std::uint64_t) {
    if (!config_.localIsAuthority || state_ != CollaborationState::Ready || writers.size() > 256) return false;
    std::set<std::string> normalized;
    for (auto &writer : writers) if (!writer.empty() && writer.size() <= 128) normalized.insert(std::move(writer));
    if (writersOpen_ == writersOpen && writers_ == normalized) return false;
    CommitEntry candidate;
    candidate.sequence = sequence_ + 1;
    candidate.control = true;
    candidate.locked = locked_;
    candidate.writersOpen = writersOpen;
    candidate.writers = {normalized.begin(), normalized.end()};
    Message validation;
    validation.kind = MessageKind::Commit;
    validation.documentId = config_.documentId;
    validation.authorityIdentity = config_.authorityIdentity;
    validation.sequence = candidate.sequence;
    validation.entry = candidate;
    const auto encoded = encodeMessage(validation);
    if (!encoded || encoded->size() > 4 * 1024) return false;
    writersOpen_ = writersOpen;
    writers_ = std::move(normalized);
    document_.advanceInteractionBarrier();
    CommitEntry entry;
    entry.sequence = ++sequence_;
    entry.control = true;
    entry.locked = locked_;
    entry.writersOpen = writersOpen_;
    entry.writers = writersVector();
    appendAndBroadcast(std::move(entry));
    publish("ready");
    return true;
}

bool Runtime::applyEntry(const CommitEntry &entry) {
    if (entry.sequence != sequence_ + 1) return false;
    if (entry.control) {
        document_.advanceInteractionBarrier();
        locked_ = entry.locked;
        writersOpen_ = entry.writersOpen;
        writers_ = std::set<std::string>(entry.writers.begin(), entry.writers.end());
    } else {
        const auto result = document_.apply(entry.command);
        if (!result.changed() && result.status != Status::Duplicate) return false;
        if (entry.command.actor == actorId(config_.localIdentity))
            pendingProposals_.erase(entry.command.id);
    }
    sequence_ = entry.sequence;
    return true;
}

void Runtime::observePeer(const PeerInstance &peer) {
    if (state_ == CollaborationState::Retired || peer.identity.empty()) return;
    peers_[peer.identity] = peer;
}

void Runtime::peerLeft(const PeerInstance &peer) {
    const auto found = peers_.find(peer.identity);
    if (found != peers_.end() && !(found->second == peer)) return;
    peers_.erase(peer.identity);
    rateEvents_.erase(peer.identity);
    if (!config_.localIsAuthority && peer.identity == config_.authorityIdentity) {
        state_ = CollaborationState::Frozen;
        publish("authority left; board frozen");
    }
}

void Runtime::receive(std::string_view topic, std::string_view payload,
                      const PeerInstance &sender, std::uint64_t nowMs) {
    if (state_ == CollaborationState::Retired || state_ == CollaborationState::Frozen ||
        !transportReady_ || !currentPeer(sender)) return;
    const auto parsed = decodeMessage(topic, payload);
    if (!parsed || parsed->documentId != config_.documentId ||
        parsed->authorityIdentity != config_.authorityIdentity) return;
    const auto &message = *parsed;
    if (sender.identity == config_.authorityIdentity) lastAuthoritySeen_ = nowMs;

    if (config_.localIsAuthority) {
        if (sender.identity == config_.authorityIdentity) return;
        if (message.kind == MessageKind::Discover || message.kind == MessageKind::SyncRequest) {
            sendDescriptor({sender.identity});
            sendRecovery(message.baseSequence, {sender.identity});
            return;
        }
        if (message.kind == MessageKind::AssetRequest) {
            if (!acceptRate(sender.identity, nowMs)) return;
            const auto asset = assets_.find(message.assetId);
            if (asset != assets_.end()) queueAsset(asset->second, {sender.identity});
            return;
        }
        if (message.kind != MessageKind::Propose || !acceptRate(sender.identity, nowMs)) return;
        auto command = message.entry.command;
        command.actor = actorId(sender.identity);
        if (command.kind == CommandKind::Add) command.object.author = command.actor;
        bool responseSent = false;
        const auto result = commitProposal(
            std::move(command), sender, nowMs, {sender.identity}, &responseSent);
        if (!result.changed() && !responseSent) {
            Message reject;
            reject.kind = MessageKind::Reject;
            reject.documentId = config_.documentId;
            reject.authorityIdentity = config_.authorityIdentity;
            reject.sequence = sequence_;
            reject.requestId = message.requestId;
            reject.reason = result.status == Status::Duplicate ? "duplicate" :
                result.status == Status::Stale ? "stale" :
                result.status == Status::Forbidden ? "forbidden" : "invalid";
            send(reject, {sender.identity});
        }
        return;
    }

    if (sender.identity != config_.authorityIdentity) return;
    switch (message.kind) {
    case MessageKind::Descriptor:
        if (message.sequence == sequence_ && contentHash(document_.toJson()) == message.snapshotHash) {
            locked_ = message.locked;
            writersOpen_ = message.writersOpen;
            writers_ = std::set<std::string>(message.writers.begin(), message.writers.end());
            state_ = CollaborationState::Ready;
            requestMissingAssets(nowMs);
            publish("ready");
        } else requestSync();
        break;
    case MessageKind::Commit:
        if (message.entry.sequence <= sequence_) {
            if (!message.entry.control &&
                message.entry.command.actor == actorId(config_.localIdentity))
                pendingProposals_.erase(message.entry.command.id);
            return;
        }
        if (!applyEntry(message.entry)) {
            state_ = CollaborationState::Synchronizing;
            requestSync();
        } else {
            state_ = CollaborationState::Ready;
            requestMissingAssets(nowMs);
            publish("ready");
        }
        break;
    case MessageKind::Delta:
        if (message.baseSequence != sequence_) { requestSync(); return; }
        for (const auto &entry : message.entries) {
            if (!applyEntry(entry)) { requestSync(); return; }
        }
        state_ = CollaborationState::Ready;
        requestMissingAssets(nowMs);
        publish("ready");
        break;
    case MessageKind::SnapshotChunk:
        receiveSnapshot(message, nowMs);
        break;
    case MessageKind::AssetChunk:
        receiveAsset(message, nowMs);
        break;
    case MessageKind::Heartbeat:
        if (message.sequence > sequence_ || state_ == CollaborationState::ReadOnly ||
            state_ == CollaborationState::Synchronizing) requestSync();
        break;
    case MessageKind::Reject:
        pendingProposals_.erase(message.requestId);
        publish(message.reason.empty() ? "operation rejected" : message.reason);
        break;
    default: break;
    }
}

void Runtime::requestSync() {
    if (config_.localIsAuthority || !transportReady_ || state_ == CollaborationState::Retired ||
        state_ == CollaborationState::Frozen) return;
    state_ = CollaborationState::Synchronizing;
    Message request;
    request.kind = started_ ? MessageKind::SyncRequest : MessageKind::Discover;
    request.documentId = config_.documentId;
    request.authorityIdentity = config_.authorityIdentity;
    request.sequence = sequence_;
    request.baseSequence = sequence_;
    send(request, {config_.authorityIdentity});
    publish("synchronizing");
}

void Runtime::sendDescriptor(const std::vector<std::string> &destination) {
    Message descriptor;
    descriptor.kind = MessageKind::Descriptor;
    descriptor.documentId = config_.documentId;
    descriptor.authorityIdentity = config_.authorityIdentity;
    descriptor.sequence = sequence_;
    descriptor.locked = locked_;
    descriptor.writersOpen = writersOpen_;
    descriptor.writers = writersVector();
    descriptor.snapshotHash = contentHash(document_.toJson());
    send(descriptor, destination);
}

void Runtime::sendRecovery(std::uint64_t fromSequence, const std::vector<std::string> &destination) {
    if (fromSequence == sequence_) return;
    const bool canDelta = !log_.empty() && fromSequence + 1 >= log_.front().sequence &&
        fromSequence < sequence_;
    if (!canDelta) { sendSnapshot(destination); return; }
    std::size_t offset = 0;
    std::vector<CommitEntry> pending;
    for (const auto &entry : log_) if (entry.sequence > fromSequence) pending.push_back(entry);
    std::uint64_t base = fromSequence;
    while (offset < pending.size()) {
        Message delta;
        delta.kind = MessageKind::Delta;
        delta.documentId = config_.documentId;
        delta.authorityIdentity = config_.authorityIdentity;
        delta.sequence = sequence_;
        delta.baseSequence = base;
        while (offset < pending.size()) {
            delta.entries.push_back(pending[offset]);
            if (!encodeMessage(delta)) {
                delta.entries.pop_back();
                break;
            }
            base = pending[offset].sequence;
            ++offset;
        }
        if (delta.entries.empty() || !send(delta, destination)) { sendSnapshot(destination); return; }
    }
}

void Runtime::sendSnapshot(const std::vector<std::string> &destination) {
    const auto snapshot = document_.toJson();
    constexpr std::size_t ChunkBytes = 6000;
    const auto count = static_cast<std::uint32_t>((snapshot.size() + ChunkBytes - 1) / ChunkBytes);
    const auto hash = contentHash(snapshot);
    const auto id = "snapshot-" + std::to_string(sequence_) + "-" + hash;
    for (std::uint32_t index = 0; index < count; ++index) {
        Message chunk;
        chunk.kind = MessageKind::SnapshotChunk;
        chunk.documentId = config_.documentId;
        chunk.authorityIdentity = config_.authorityIdentity;
        chunk.sequence = sequence_;
        chunk.baseSequence = sequence_;
        chunk.snapshotId = id;
        chunk.snapshotHash = hash;
        chunk.chunkIndex = index;
        chunk.chunkCount = count;
        chunk.locked = locked_;
        chunk.writersOpen = writersOpen_;
        chunk.writers = writersVector();
        const auto begin = static_cast<std::size_t>(index) * ChunkBytes;
        chunk.payload = snapshot.substr(begin, std::min(ChunkBytes, snapshot.size() - begin));
        if (!send(chunk, destination)) return;
    }
}

void Runtime::receiveSnapshot(const Message &message, std::uint64_t nowMs) {
    if (message.baseSequence < sequence_) return;
    if (snapshot_.id != message.snapshotId) {
        snapshot_ = {};
        snapshot_.id = message.snapshotId;
        snapshot_.hash = message.snapshotHash;
        snapshot_.sequence = message.baseSequence;
        snapshot_.count = message.chunkCount;
        snapshot_.locked = message.locked;
        snapshot_.writersOpen = message.writersOpen;
        snapshot_.writers = message.writers;
        snapshot_.chunks.resize(message.chunkCount);
        snapshot_.received.assign(message.chunkCount, false);
    }
    if (snapshot_.hash != message.snapshotHash || snapshot_.sequence != message.baseSequence ||
        snapshot_.count != message.chunkCount || snapshot_.received[message.chunkIndex]) return;
    if (snapshot_.bytes + message.payload.size() > Document::MaxSnapshotBytes) { snapshot_ = {}; return; }
    snapshot_.chunks[message.chunkIndex] = message.payload;
    snapshot_.received[message.chunkIndex] = true;
    snapshot_.bytes += message.payload.size();
    const auto received = static_cast<std::size_t>(std::count(snapshot_.received.begin(), snapshot_.received.end(), true));
    publish("recovering " + std::to_string(received) + "/" + std::to_string(snapshot_.count));
    if (received != snapshot_.count) return;
    std::string bytes;
    bytes.reserve(snapshot_.bytes);
    for (const auto &chunk : snapshot_.chunks) bytes += chunk;
    if (contentHash(bytes) != snapshot_.hash) { snapshot_ = {}; requestSync(); return; }
    auto restored = Document::fromJson(bytes);
    if (!restored || restored->id() != config_.documentId ||
        restored->owner() != actorId(config_.authorityIdentity)) {
        snapshot_ = {};
        requestSync();
        return;
    }
    document_ = std::move(*restored);
    sequence_ = snapshot_.sequence;
    locked_ = snapshot_.locked;
    writersOpen_ = snapshot_.writersOpen;
    writers_ = std::set<std::string>(snapshot_.writers.begin(), snapshot_.writers.end());
    snapshot_ = {};
    state_ = CollaborationState::Ready;
    requestMissingAssets(nowMs);
    publish("ready");
}

bool Runtime::cacheAsset(Asset asset) {
    if (!validAsset(asset)) return false;
    if (assets_.count(asset.id)) return true;
    if (assets_.size() >= MaxAssets || asset.bytes.size() > MaxAssetTotalBytes - assetBytes_)
        return false;
    assetBytes_ += asset.bytes.size();
    const auto id = asset.id;
    assets_.emplace(id, std::make_shared<const Asset>(std::move(asset)));
    return true;
}

bool Runtime::queueAsset(const std::shared_ptr<const Asset> &asset,
                         const std::vector<std::string> &destination) {
    if (!asset || !validAsset(*asset)) return false;
    std::string key = asset->id + "|";
    for (const auto &identity : destination) key += identity + ",";
    if (std::any_of(outboundAssets_.begin(), outboundAssets_.end(), [&](const auto &transfer) {
        return transfer.key == key;
    })) return true;
    if (outboundAssets_.size() >= MaxOutboundAssetTransfers) return false;
    outboundAssets_.push_back({std::move(key), asset, destination, 0});
    pumpAssetTransfers();
    return true;
}

void Runtime::pumpAssetTransfers() {
    std::size_t budget = AssetChunksPerPump;
    while (budget > 0 && !outboundAssets_.empty()) {
        auto &transfer = outboundAssets_.front();
        const auto &asset = *transfer.asset;
        const auto count = static_cast<std::uint32_t>(
            (asset.bytes.size() + AssetChunkBytes - 1) / AssetChunkBytes);
        const auto index = transfer.nextChunk;
        Message chunk;
        chunk.kind = MessageKind::AssetChunk;
        chunk.documentId = config_.documentId;
        chunk.authorityIdentity = config_.authorityIdentity;
        chunk.sequence = sequence_;
        chunk.assetId = asset.id;
        chunk.assetMime = asset.mime;
        chunk.assetWidth = asset.width;
        chunk.assetHeight = asset.height;
        chunk.assetBytes = static_cast<std::uint32_t>(asset.bytes.size());
        chunk.chunkIndex = index;
        chunk.chunkCount = count;
        const auto begin = static_cast<std::size_t>(index) * AssetChunkBytes;
        chunk.payload = asset.bytes.substr(begin,
            std::min(AssetChunkBytes, asset.bytes.size() - begin));
        if (!send(chunk, transfer.destinations)) return;
        ++transfer.nextChunk;
        --budget;
        if (transfer.nextChunk == count) outboundAssets_.pop_front();
    }
}

void Runtime::receiveAsset(const Message &message, std::uint64_t nowMs) {
    if (assets_.count(message.assetId)) {
        requestedAssets_.erase(message.assetId);
        return;
    }
    auto found = assetAssemblies_.find(message.assetId);
    if (found == assetAssemblies_.end()) {
        if (assetAssemblies_.size() >= MaxAssetAssemblies) return;
        AssetAssembly assembly;
        assembly.id = message.assetId;
        assembly.mime = message.assetMime;
        assembly.width = message.assetWidth;
        assembly.height = message.assetHeight;
        assembly.totalBytes = message.assetBytes;
        assembly.count = message.chunkCount;
        assembly.chunks.resize(message.chunkCount);
        assembly.received.assign(message.chunkCount, false);
        assembly.lastProgressMs = nowMs;
        found = assetAssemblies_.emplace(message.assetId, std::move(assembly)).first;
    }
    auto &assembly = found->second;
    if (assembly.mime != message.assetMime || assembly.width != message.assetWidth ||
        assembly.height != message.assetHeight || assembly.totalBytes != message.assetBytes ||
        assembly.count != message.chunkCount || assembly.received[message.chunkIndex]) return;
    if (message.payload.size() > assembly.totalBytes - assembly.bytes) {
        assetAssemblies_.erase(found);
        requestedAssets_.erase(message.assetId);
        return;
    }
    assembly.chunks[message.chunkIndex] = message.payload;
    assembly.received[message.chunkIndex] = true;
    assembly.bytes += message.payload.size();
    assembly.lastProgressMs = nowMs;
    if (std::count(assembly.received.begin(), assembly.received.end(), true) != assembly.count) return;

    Asset asset;
    asset.id = assembly.id;
    asset.mime = assembly.mime;
    asset.width = assembly.width;
    asset.height = assembly.height;
    asset.bytes.reserve(assembly.bytes);
    for (const auto &chunk : assembly.chunks) asset.bytes += chunk;
    const bool complete = asset.bytes.size() == assembly.totalBytes && cacheAsset(std::move(asset));
    assetAssemblies_.erase(found);
    requestedAssets_.erase(message.assetId);
    if (complete) publish("ready");
}

void Runtime::requestMissingAssets(std::uint64_t nowMs) {
    if (config_.localIsAuthority || state_ == CollaborationState::Retired ||
        state_ == CollaborationState::Frozen || !transportReady_) return;
    for (const auto &page : document_.pages()) {
        if (page.backgroundAssetId.empty() || assets_.count(page.backgroundAssetId)) continue;
        const auto requested = requestedAssets_.find(page.backgroundAssetId);
        if (requested != requestedAssets_.end() && nowMs - requested->second < HeartbeatIntervalMs)
            continue;
        Message request;
        request.kind = MessageKind::AssetRequest;
        request.documentId = config_.documentId;
        request.authorityIdentity = config_.authorityIdentity;
        request.sequence = sequence_;
        request.assetId = page.backgroundAssetId;
        if (send(request, {config_.authorityIdentity})) requestedAssets_[page.backgroundAssetId] = nowMs;
    }
}

bool Runtime::missingActiveAsset() const {
    const auto &id = document_.page().backgroundAssetId;
    return !id.empty() && assets_.count(id) == 0;
}

void Runtime::setTransportReady(bool ready, std::uint64_t nowMs) {
    if (state_ == CollaborationState::Retired || transportReady_ == ready) return;
    transportReady_ = ready;
    if (state_ == CollaborationState::Frozen) {
        publish("authority left; board frozen");
        return;
    }
    if (!ready) {
        state_ = CollaborationState::ReadOnly;
        publish("connection interrupted; read-only");
        return;
    }
    lastAuthoritySeen_ = nowMs;
    if (config_.localIsAuthority) {
        state_ = CollaborationState::Ready;
        sendDescriptor({});
        publish("ready");
    } else requestSync();
}

void Runtime::tick(std::uint64_t nowMs) {
    if (!started_ || !transportReady_ || state_ == CollaborationState::Retired) return;
    for (auto it = assetAssemblies_.begin(); it != assetAssemblies_.end();) {
        if (nowMs - it->second.lastProgressMs < AssetAssemblyTimeoutMs) { ++it; continue; }
        requestedAssets_.erase(it->first);
        it = assetAssemblies_.erase(it);
    }
    if (config_.localIsAuthority && nowMs - lastHeartbeatSent_ >= HeartbeatIntervalMs) {
        Message heartbeat;
        heartbeat.kind = MessageKind::Heartbeat;
        heartbeat.documentId = config_.documentId;
        heartbeat.authorityIdentity = config_.authorityIdentity;
        heartbeat.sequence = sequence_;
        heartbeat.sentAtMs = nowMs;
        send(heartbeat);
        lastHeartbeatSent_ = nowMs;
    } else if (!config_.localIsAuthority) {
        requestMissingAssets(nowMs);
        if (state_ == CollaborationState::Ready) {
            for (auto &[_, proposal] : pendingProposals_) {
                if (nowMs - proposal.lastSentMs >= HeartbeatIntervalMs &&
                    !sendProposal(proposal, nowMs)) {
                    state_ = CollaborationState::ReadOnly;
                    publish("transport unavailable");
                    break;
                }
            }
        } else if (state_ == CollaborationState::Synchronizing &&
                   nowMs - lastSyncRequest_ >= HeartbeatIntervalMs) {
            lastSyncRequest_ = nowMs;
            requestSync();
        }
        if (state_ != CollaborationState::Frozen &&
            nowMs - lastAuthoritySeen_ >= AuthorityTimeoutMs) {
            state_ = CollaborationState::ReadOnly;
            publish("authority unavailable; read-only");
        }
    }
    pumpAssetTransfers();
}

void Runtime::retire() {
    if (state_ == CollaborationState::Retired) return;
    state_ = CollaborationState::Retired;
    transportReady_ = false;
    send_ = {};
    snapshot_ = {};
    log_.clear();
    proposals_.clear();
    proposalOrder_.clear();
    rateEvents_.clear();
    pendingProposals_.clear();
    peers_.clear();
    assets_.clear();
    assetBytes_ = 0;
    assetAssemblies_.clear();
    requestedAssets_.clear();
    outboundAssets_.clear();
}

std::vector<std::string> Runtime::writersVector() const {
    return {writers_.begin(), writers_.end()};
}

void Runtime::publish(std::string status) {
    if (!project_ || state_ == CollaborationState::Retired) return;
    Projection value;
    value.snapshot = document_.toJson();
    value.sequence = sequence_;
    value.state = state_;
    value.authorityIdentity = config_.authorityIdentity;
    value.localIdentity = actorId(config_.localIdentity);
    value.locked = locked_;
    value.writersOpen = writersOpen_;
    value.writers = writersVector();
    value.canAdmin = config_.localIsAuthority && state_ == CollaborationState::Ready;
    const bool waitingForAsset = missingActiveAsset();
    value.canEdit = state_ == CollaborationState::Ready &&
        allowedWriter(config_.localIdentity) && !waitingForAsset;
    value.status = waitingForAsset && state_ == CollaborationState::Ready
        ? "background loading" : std::move(status);
    value.assets.reserve(assets_.size());
    for (const auto &[_, asset] : assets_) value.assets.push_back(asset);
    project_(std::move(value));
}

} // namespace livekit::whiteboard
