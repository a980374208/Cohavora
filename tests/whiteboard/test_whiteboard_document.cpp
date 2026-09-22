#include "src/core/whiteboard/whiteboard_document.h"
#include "src/core/whiteboard/whiteboard_asset.h"
#include "tests/support/test_check.h"
#include <nlohmann/json.hpp>
#include <limits>
#include <iostream>

using namespace livekit::whiteboard;
namespace {
int serial = 0;
Command command(const Document &doc, CommandKind kind, const std::string &actor = "local", const std::string &target = {}) {
    Command c;
    c.id = "op-" + std::to_string(++serial);
    c.actor = actor;
    c.context = doc.context();
    c.kind = kind;
    c.target = target;
    c.object.id = "object-" + std::to_string(serial);
    c.object.author = actor;
    c.object.points = {{10, 20}, {100, 200}};
    return c;
}
void historyAndBarriers() {
    Document doc("board");
    auto alice = command(doc, CommandKind::Add, "alice");
    auto bob = command(doc, CommandKind::Add, "bob");
    TEST_CHECK(doc.apply(alice).changed());
    TEST_CHECK(doc.apply(bob).changed());
    const auto revision = doc.revision();
    TEST_CHECK(doc.apply(alice).status == Status::Duplicate);
    TEST_CHECK(doc.revision() == revision && doc.page().objects.size() == 2);
    TEST_CHECK(doc.apply(command(doc, CommandKind::Erase, "alice", bob.object.id)).status == Status::Forbidden);
    TEST_CHECK(doc.apply(command(doc, CommandKind::Clear, "alice")).status == Status::Forbidden);
    TEST_CHECK(doc.apply(command(doc, CommandKind::AddPage, "alice", "page2")).status == Status::Forbidden);
    TEST_CHECK(doc.apply(command(doc, CommandKind::Undo, "alice")).objectId == alice.object.id);
    TEST_CHECK(doc.page().objects.size() == 1 && doc.page().objects.front().id == bob.object.id);
    TEST_CHECK(doc.canRedo("alice") && !doc.canUndo("alice"));
    TEST_CHECK(doc.apply(command(doc, CommandKind::Redo, "alice", alice.object.id)).status == Status::Invalid);
    TEST_CHECK(doc.apply(command(doc, CommandKind::Redo, "alice", "alice-redo")).changed());
    TEST_CHECK(doc.page().objects.back().author == "alice");
    TEST_CHECK(doc.apply(command(doc, CommandKind::Erase, "local", "alice-redo")).changed());
    TEST_CHECK(!doc.canUndo("alice"));
    auto pending = command(doc, CommandKind::Add);
    const auto firstPage = doc.page().id;
    TEST_CHECK(doc.apply(command(doc, CommandKind::AddPage, "local", "page2")).changed());
    TEST_CHECK(doc.page().objects.empty());
    TEST_CHECK(doc.apply(command(doc, CommandKind::SelectPage, "local", firstPage)).changed());
    TEST_CHECK(doc.apply(pending).status == Status::Stale);
    TEST_CHECK(doc.canUndo("bob"));
    pending = command(doc, CommandKind::Add);
    const auto epoch = doc.page().epoch;
    TEST_CHECK(doc.apply(command(doc, CommandKind::Clear)).changed());
    TEST_CHECK(doc.page().epoch == epoch + 1 && doc.page().objects.empty());
    TEST_CHECK(doc.apply(pending).status == Status::Stale);
    TEST_CHECK(!doc.canUndo("bob") && !doc.canRedo("alice"));
}
void snapshotsAndValidation() {
    Document doc("roundtrip");
    auto text = command(doc, CommandKind::Add);
    text.object.kind = ObjectKind::Text;
    text.object.text = "中文批注\nHello 世界";
    TEST_CHECK(doc.apply(text).changed());
    TEST_CHECK(doc.apply(command(doc, CommandKind::AddPage, "local", "second")).changed());
    auto dot = command(doc, CommandKind::Add);
    dot.object.points.resize(1);
    TEST_CHECK(doc.apply(dot).changed());
    const auto bytes = doc.toJson();
    const auto loaded = Document::fromJson(bytes);
    TEST_CHECK(loaded && loaded->toJson() == bytes && loaded->activePageIndex() == 1);
    TEST_CHECK(!loaded->canUndo("local"));
    TEST_CHECK(!Document::fromJson("{}"));
    TEST_CHECK(!Document::fromJson(std::string(Document::MaxSnapshotBytes + 1, ' ')));
    const auto source = nlohmann::json::parse(bytes);
    for (const auto badKind : {nlohmann::json(0.5), nlohmann::json(0x100000000ULL), nlohmann::json(-1)}) {
        auto bad = source;
        bad["pages"][0]["objects"][0]["kind"] = badKind;
        TEST_CHECK(!Document::fromJson(bad.dump()));
    }
    auto bad = source;
    bad["pages"][1]["objects"][0]["id"] = text.object.id;
    TEST_CHECK(!Document::fromJson(bad.dump()));
    bad = source;
    bad["pages"][0]["epoch"] = "18446744073709551615";
    TEST_CHECK(!Document::fromJson(bad.dump()));
    bad = source;
    bad["pages"][0]["objects"][0]["color"] = 1.5;
    TEST_CHECK(!Document::fromJson(bad.dump()));
    TEST_CHECK(doc.toJson() == bytes);
    auto invalid = command(doc, CommandKind::Add);
    invalid.object.points[0].x = std::numeric_limits<double>::quiet_NaN();
    TEST_CHECK(doc.apply(invalid).status == Status::Invalid);
    invalid.object.points[0].x = -1;
    TEST_CHECK(doc.apply(invalid).status == Status::Invalid);
    invalid.object.points[0].x = 0;
    invalid.object.points.resize(Document::MaxStrokePoints + 1);
    TEST_CHECK(doc.apply(invalid).status == Status::Invalid);
    invalid.object = text.object;
    invalid.object.id = "bad-text";
    invalid.object.text = std::string("\xc0\xaf", 2);
    TEST_CHECK(doc.apply(invalid).status == Status::Invalid);
    invalid.object.text = std::string(2001, 'a');
    TEST_CHECK(doc.apply(invalid).status == Status::Invalid);
}
void imagePagesAndBackgroundBarriers() {
    Document doc("images");
    const auto first = doc.page().id;
    auto pending = command(doc, CommandKind::Add);
    auto image = command(doc, CommandKind::AddImagePage, "local", "image-page");
    image.assetId = assetContentId("image-one");
    image.pageWidth = 1600;
    image.pageHeight = 900;
    TEST_CHECK(doc.apply(image).changed());
    TEST_CHECK(doc.page().id == "image-page" && doc.page().backgroundAssetId == image.assetId);
    TEST_CHECK(doc.page().width == 1600 && doc.page().height == 900);
    TEST_CHECK(doc.apply(pending).status == Status::Stale);

    auto ink = command(doc, CommandKind::Add);
    TEST_CHECK(doc.apply(ink).changed());
    const auto background = doc.page().backgroundAssetId;
    TEST_CHECK(doc.apply(command(doc, CommandKind::Clear)).changed());
    TEST_CHECK(doc.page().objects.empty() && doc.page().backgroundAssetId == background);

    auto late = command(doc, CommandKind::Add);
    auto replace = command(doc, CommandKind::SetBackground);
    replace.assetId = assetContentId("image-two");
    replace.pageWidth = 1024;
    replace.pageHeight = 768;
    TEST_CHECK(doc.apply(replace).changed());
    TEST_CHECK(doc.page().backgroundAssetId == replace.assetId && doc.page().objects.empty());
    TEST_CHECK(doc.apply(late).status == Status::Stale);
    const auto snapshot = doc.toJson();
    TEST_CHECK(snapshot.find("image-one") == std::string::npos);
    const auto restored = Document::fromJson(snapshot);
    TEST_CHECK(restored && restored->toJson() == snapshot);
    TEST_CHECK(restored->pages().front().id == first);

    auto unauthorized = command(doc, CommandKind::SetBackground, "guest");
    unauthorized.assetId = assetContentId("image-three");
    unauthorized.pageWidth = 800;
    unauthorized.pageHeight = 600;
    TEST_CHECK(doc.apply(unauthorized).status == Status::Forbidden);
}
void limits() {
    Document doc("limits");
    for (std::size_t i = 0; i < Document::MaxPageObjects; ++i)
        TEST_CHECK(doc.apply(command(doc, CommandKind::Add)).changed());
    TEST_CHECK(doc.apply(command(doc, CommandKind::Add)).status == Status::LimitReached);
    TEST_CHECK(doc.toJson().size() <= Document::MaxSnapshotBytes);
    for (std::size_t i = 0; i < Document::MaxHistory; ++i)
        TEST_CHECK(doc.apply(command(doc, CommandKind::Undo)).changed());
    TEST_CHECK(!doc.canUndo("local"));
    TEST_CHECK(doc.apply(command(doc, CommandKind::Add)).changed());
    TEST_CHECK(!doc.canRedo("local"));
    for (std::size_t i = 1; i < Document::MaxPages; ++i)
        TEST_CHECK(doc.apply(command(doc, CommandKind::AddPage, "local", "page-" + std::to_string(i + 1))).changed());
    TEST_CHECK(doc.apply(command(doc, CommandKind::AddPage, "local", "overflow")).status == Status::LimitReached);
}
}
int main() {
    historyAndBarriers(); snapshotsAndValidation(); imagePagesAndBackgroundBarriers(); limits();
    std::cout << "WHITEBOARD_DOCUMENT PASS: apply, ownership, undo/redo, page/clear barriers, snapshots, limits\n";
}
