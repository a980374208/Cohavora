#include "src/core/whiteboard/whiteboard_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wb = livekit::whiteboard;

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct Packet {
    std::string from;
    std::string topic;
    std::string payload;
    std::vector<std::string> destinations;
};

struct Network {
    std::vector<Packet> packets;
    std::map<std::string, wb::Runtime *> peers;
    std::map<std::string, wb::PeerInstance> instances;
    std::uint64_t now = 1000;
    int assetBudget = -1;

    wb::Runtime::Send sender(std::string from) {
        return [this, from = std::move(from)](std::string_view topic, std::string_view payload,
                                              const std::vector<std::string> &destinations) {
            if (topic == wb::AssetTopic) {
                if (assetBudget == 0) return false;
                if (assetBudget > 0) --assetBudget;
            }
            packets.push_back({from, std::string(topic), std::string(payload), destinations});
            return true;
        };
    }

    bool addressed(const Packet &packet, const std::string &identity) const {
        if (identity == packet.from) return false;
        return packet.destinations.empty() ||
            std::find(packet.destinations.begin(), packet.destinations.end(), identity) != packet.destinations.end();
    }

    void drain() {
        std::size_t guard = 0;
        while (!packets.empty()) {
            require(++guard < 20000, "network did not quiesce");
            auto packet = std::move(packets.front());
            packets.erase(packets.begin());
            for (const auto &[identity, runtime] : peers) {
                if (addressed(packet, identity))
                    runtime->receive(packet.topic, packet.payload, instances.at(packet.from), ++now);
            }
        }
    }
};

wb::Command addCommand(const wb::Runtime &runtime, std::string id, double x = 10) {
    wb::Command command;
    command.id = std::move(id);
    command.context = runtime.document().context();
    command.kind = wb::CommandKind::Add;
    command.object.id = "object-" + command.id;
    command.object.kind = wb::ObjectKind::Rectangle;
    command.object.color = 0x1677ff;
    command.object.width = 4;
    command.object.points = {{x, 10}, {x + 20, 30}};
    return command;
}

std::unique_ptr<wb::Runtime> makeRuntime(Network &network, std::string identity,
                                         bool authority, wb::Projection *latest = nullptr) {
    auto project = [latest](wb::Projection value) {
        if (latest) *latest = std::move(value);
    };
    auto runtime = std::make_unique<wb::Runtime>(
        wb::RuntimeConfig{"board-test", identity, "host", authority},
        network.sender(identity), std::move(project));
    network.peers[identity] = runtime.get();
    network.instances[identity] = {identity, 7, authority ? 1ULL : 2ULL};
    return runtime;
}

wb::Asset asset(std::size_t bytes = wb::AssetChunkBytes + 17) {
    wb::Asset value;
    value.mime = "image/png";
    value.width = 800;
    value.height = 600;
    value.bytes.assign(bytes, 'p');
    value.id = wb::assetContentId(value.bytes);
    return value;
}

void collaborationAndDeltaRecovery() {
    Network network;
    wb::Projection clientProjection;
    auto authority = makeRuntime(network, "host", true);
    auto client = makeRuntime(network, "writer", false, &clientProjection);
    authority->observePeer(network.instances["writer"]);
    client->observePeer(network.instances["host"]);
    authority->start(network.now);
    client->start(network.now);
    network.drain();
    require(client->state() == wb::CollaborationState::Ready, "client did not discover authority");

    auto lostProposal = addCommand(*client, "proposal-lost", 5);
    client->propose(lostProposal, ++network.now);
    network.packets.clear();
    network.now += wb::Runtime::HeartbeatIntervalMs;
    client->tick(network.now);
    network.drain();
    require(client->document().toJson() == authority->document().toJson(),
            "lost proposal was not retried with the same operation id");

    auto first = addCommand(*client, "proposal-1");
    require(client->propose(first, ++network.now).status == wb::Status::NoChange,
            "client proposal should wait for commit");
    require(network.packets.size() == 1, "proposal packet missing");
    const auto duplicate = network.packets.front();
    authority->receive(duplicate.topic, duplicate.payload, network.instances["writer"], ++network.now);
    authority->receive(duplicate.topic, duplicate.payload, network.instances["writer"], ++network.now);
    network.packets.erase(network.packets.begin());
    network.drain();
    require(authority->document().page().objects.size() == 2, "duplicate proposal created two objects");
    require(client->document().toJson() == authority->document().toJson(), "documents diverged after duplicate");

    auto lost = addCommand(*client, "proposal-2", 40);
    client->propose(lost, ++network.now);
    auto proposal = std::move(network.packets.front());
    network.packets.clear();
    authority->receive(proposal.topic, proposal.payload, network.instances["writer"], ++network.now);
    network.packets.clear(); // Drop commit #2.

    auto afterGap = addCommand(*client, "proposal-3", 70);
    client->propose(afterGap, ++network.now);
    proposal = std::move(network.packets.front());
    network.packets.clear();
    authority->receive(proposal.topic, proposal.payload, network.instances["writer"], ++network.now);
    network.drain();
    require(client->sequence() == authority->sequence(), "delta did not fill sequence gap");
    require(client->document().toJson() == authority->document().toJson(), "delta recovery hash mismatch");

    auto snapshotAcknowledged = addCommand(*client, "snapshot-ack", 90);
    client->propose(snapshotAcknowledged, ++network.now);
    network.packets.clear();
    snapshotAcknowledged.actor = clientProjection.localIdentity;
    snapshotAcknowledged.object.author = clientProjection.localIdentity;
    wb::Message oldAcknowledgement;
    oldAcknowledgement.kind = wb::MessageKind::Commit;
    oldAcknowledgement.documentId = "board-test";
    oldAcknowledgement.authorityIdentity = "host";
    oldAcknowledgement.sequence = client->sequence();
    oldAcknowledgement.entry.sequence = client->sequence();
    oldAcknowledgement.entry.command = snapshotAcknowledged;
    const auto acknowledgementBytes = wb::encodeMessage(oldAcknowledgement);
    require(acknowledgementBytes.has_value(), "old acknowledgement encoding failed");
    client->receive(wb::OperationsTopic, *acknowledgementBytes, network.instances["host"], ++network.now);
    network.now += wb::Runtime::HeartbeatIntervalMs;
    client->tick(network.now);
    require(network.packets.empty(), "snapshot-acknowledged proposal kept retrying");

    auto staleAcrossPermissionBarrier = addCommand(*client, "stale-after-lock", 100);
    authority->setLocked(true, ++network.now);
    network.drain();
    require(clientProjection.locked && !clientProjection.canEdit, "lock was not projected");
    require(client->propose(addCommand(*client, "locked-op"), ++network.now).status == wb::Status::Forbidden,
            "locked client could still propose");
    authority->setLocked(false, ++network.now);
    network.drain();
    const auto beforeStale = authority->sequence();
    client->propose(staleAcrossPermissionBarrier, ++network.now);
    network.drain();
    require(authority->sequence() == beforeStale, "pre-permission command crossed interaction barrier");

    client->setTransportReady(false, ++network.now);
    require(client->state() == wb::CollaborationState::ReadOnly, "reconnect did not force read-only");
    client->setTransportReady(true, ++network.now);
    network.drain();
    require(client->state() == wb::CollaborationState::Ready, "reconnect did not resynchronize");

    client->peerLeft(network.instances["host"]);
    require(client->state() == wb::CollaborationState::Frozen, "authority departure did not freeze board");
    client->setTransportReady(false, ++network.now);
    client->setTransportReady(true, ++network.now);
    require(client->state() == wb::CollaborationState::Frozen,
            "transport recovery revived a board whose authority instance left");
}

void snapshotAndIncarnationRecovery() {
    Network network;
    auto authority = makeRuntime(network, "host", true);
    authority->start(network.now);
    network.packets.clear();
    for (std::size_t i = 0; i < wb::Runtime::MaxLogEntries + 1; ++i) {
        auto command = addCommand(*authority, "bulk-" + std::to_string(i), 10 + static_cast<double>(i % 100));
        require(authority->propose(std::move(command), network.now += 100).changed(), "authority bulk commit failed");
        network.packets.clear();
    }

    wb::Projection lateProjection;
    auto late = makeRuntime(network, "late", false, &lateProjection);
    authority->observePeer(network.instances["late"]);
    late->observePeer(network.instances["host"]);
    late->start(++network.now);
    network.drain();
    require(late->state() == wb::CollaborationState::Ready, "late joiner did not finish snapshot");
    require(late->sequence() == authority->sequence(), "snapshot sequence mismatch");
    require(wb::contentHash(late->document().toJson()) == wb::contentHash(authority->document().toJson()),
            "snapshot content hash mismatch");

    const auto before = authority->sequence();
    authority->observePeer({"late", 7, 99});
    wb::Message stale;
    stale.kind = wb::MessageKind::Propose;
    stale.documentId = "board-test";
    stale.authorityIdentity = "host";
    stale.sequence = before;
    stale.requestId = "stale-instance";
    stale.entry.command = addCommand(*late, stale.requestId);
    const auto bytes = wb::encodeMessage(stale);
    require(bytes.has_value(), "stale proposal encoding failed");
    authority->receive(wb::OperationsTopic, *bytes, network.instances["late"], ++network.now);
    require(authority->sequence() == before, "retired participant instance was accepted");

    network.packets.clear();
    authority->retire();
    require(authority->state() == wb::CollaborationState::Retired, "runtime did not retire");
    require(authority->propose(addCommand(*authority, "after-retire"), ++network.now).status == wb::Status::Forbidden,
            "retired session accepted a proposal");
    authority->tick(network.now + wb::Runtime::HeartbeatIntervalMs);
    require(network.packets.empty(), "retired session emitted network traffic");
}

void protocolBounds() {
    require(!wb::decodeMessage("chat", "{}"), "chat topic entered whiteboard parser");
    require(wb::isWhiteboardTopic("whiteboard.v2.ops") &&
            !wb::decodeMessage("whiteboard.v2.ops", "{}"),
            "unknown whiteboard version was not isolated from chat");
    std::string oversized(wb::MaxWireBytes + 1, 'x');
    require(!wb::decodeMessage(wb::ControlTopic, oversized), "oversized packet was accepted");

    Network network;
    auto authority = makeRuntime(network, "host", true);
    auto client = makeRuntime(network, "writer", false);
    authority->observePeer(network.instances["writer"]);
    client->observePeer(network.instances["host"]);
    authority->start(network.now);
    client->start(network.now);
    network.drain();

    const auto submitLongStroke = [&](wb::Runtime &sender, std::string id,
                                      wb::ObjectKind kind, wb::Status expected) {
        auto stroke = addCommand(sender, std::move(id));
        stroke.object.kind = kind;
        stroke.object.points.clear();
        for (int i = 0; i < 2000; ++i)
            stroke.object.points.push_back({static_cast<double>(i % 1900),
                                            static_cast<double>((i * 37) % 1000)});
        const auto first = stroke.object.points.front();
        const auto last = stroke.object.points.back();
        require(sender.propose(std::move(stroke), ++network.now).status == expected,
                "long collaborative stroke was rejected");
        require(!network.packets.empty() &&
                network.packets.front().payload.size() <= wb::MaxWireBytes,
                "normalized stroke exceeded the operation wire-size bound");
        network.drain();
        const auto &stored = sender.document().page().objects.back();
        require(stored.points.size() <= 192, "collaborative stroke point cap was not enforced");
        require(stored.points.front().x == first.x && stored.points.front().y == first.y &&
                stored.points.back().x == last.x && stored.points.back().y == last.y,
                "stroke simplification did not preserve endpoints");
    };

    submitLongStroke(*authority, "long-pen", wb::ObjectKind::Pen, wb::Status::Applied);
    submitLongStroke(*client, "long-highlighter", wb::ObjectKind::Highlighter,
                     wb::Status::NoChange);
    require(authority->document().toJson() == client->document().toJson(),
            "long collaborative strokes diverged after commit");
}

void imageAssetRecovery() {
    Network network;
    wb::Projection authorityProjection, clientProjection;
    auto authority = makeRuntime(network, "host", true, &authorityProjection);
    auto client = makeRuntime(network, "writer", false, &clientProjection);
    authority->observePeer(network.instances["writer"]);
    client->observePeer(network.instances["host"]);
    authority->start(network.now);
    client->start(network.now);
    network.drain();

    const auto source = asset();
    require(authority->importImage(source, "image-op", "image-page", false, ++network.now).changed(),
            "authority image import failed");
    require(network.packets.size() == 3, "asset chunks and image commit were not emitted");
    auto packets = std::move(network.packets);
    network.packets.clear();
    const auto commit = std::find_if(packets.begin(), packets.end(), [](const Packet &packet) {
        return packet.topic == wb::OperationsTopic;
    });
    require(commit != packets.end(), "image page commit missing");
    client->receive(commit->topic, commit->payload, network.instances["host"], ++network.now);
    require(!clientProjection.canEdit && clientProjection.status == "background loading",
            "missing image background did not force read-only projection");

    std::vector<Packet> chunks;
    for (const auto &packet : packets) if (packet.topic == wb::AssetTopic) chunks.push_back(packet);
    require(chunks.size() == 2, "asset was not split into bounded chunks");
    client->receive(chunks[1].topic, chunks[1].payload, network.instances["host"], ++network.now);
    client->receive(chunks[1].topic, chunks[1].payload, network.instances["host"], ++network.now);
    require(!clientProjection.canEdit, "partial asset unexpectedly enabled editing");
    client->receive(chunks[0].topic, chunks[0].payload, network.instances["host"], ++network.now);
    require(clientProjection.canEdit && clientProjection.assets.size() == 1,
            "out-of-order asset assembly did not restore editing");
    require(client->document().toJson() == authority->document().toJson(),
            "image page document diverged");

    wb::Projection lateProjection;
    auto late = makeRuntime(network, "late-image", false, &lateProjection);
    authority->observePeer(network.instances["late-image"]);
    late->observePeer(network.instances["host"]);
    late->start(++network.now);
    network.drain();
    require(late->state() == wb::CollaborationState::Ready && lateProjection.canEdit,
            "late joiner did not recover referenced image asset");
    require(lateProjection.assets.size() == 1 && lateProjection.assets.front()->id == source.id,
            "late joiner asset projection mismatch");

    wb::Message invalid;
    invalid.kind = wb::MessageKind::AssetChunk;
    invalid.documentId = "board-test";
    invalid.authorityIdentity = "host";
    invalid.assetId = std::string(64, '0');
    invalid.assetMime = "image/png";
    invalid.assetWidth = 10;
    invalid.assetHeight = 10;
    invalid.assetBytes = 1;
    invalid.chunkCount = 1;
    invalid.payload = "x";
    const auto encoded = wb::encodeMessage(invalid);
    require(encoded && wb::decodeMessage(wb::AssetTopic, *encoded),
            "bounded asset packet did not round-trip");
    require(!wb::decodeMessage(wb::SyncTopic, *encoded), "asset packet crossed topic boundary");
}

void assetBackpressureResume() {
    Network network;
    wb::Projection clientProjection;
    auto authority = makeRuntime(network, "host", true);
    auto client = makeRuntime(network, "writer", false, &clientProjection);
    authority->observePeer(network.instances["writer"]);
    client->observePeer(network.instances["host"]);
    authority->start(network.now);
    client->start(network.now);
    network.drain();

    const auto source = asset(wb::AssetChunkBytes * 10 + 1);
    network.assetBudget = 3;
    require(authority->importImage(source, "large-image-op", "large-image-page", false,
                                   ++network.now).changed(),
            "backpressured image import was not committed");
    network.drain();
    require(!clientProjection.canEdit, "partial backpressured image enabled editing");
    network.assetBudget = -1;
    authority->tick(network.now += 50);
    network.drain();
    authority->tick(network.now += 50);
    network.drain();
    require(clientProjection.canEdit && clientProjection.assets.size() == 1,
            "asset transfer did not resume from its retained chunk offset");
}
} // namespace

int main() {
    collaborationAndDeltaRecovery();
    snapshotAndIncarnationRecovery();
    protocolBounds();
    imageAssetRecovery();
    assetBackpressureResume();
    std::cout << "WHITEBOARD_COLLAB PASS: ordering, idempotence, delta, snapshot, reconnect, freeze, incarnation\n";
    return 0;
}
