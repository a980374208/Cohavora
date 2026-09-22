#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace livekit::whiteboard {

enum class ObjectKind { Pen, Highlighter, Line, Rectangle, Ellipse, Arrow, Text };
struct Point { double x = 0, y = 0; };
struct Object {
    std::string id;
    std::string author;
    ObjectKind kind = ObjectKind::Pen;
    std::uint32_t color = 0x1f2329;
    double width = 4;
    double fontSize = 28;
    std::vector<Point> points;
    std::string text;
};
struct Page {
    std::string id;
    std::uint64_t epoch = 1;
    double width = 1920, height = 1080;
    std::string backgroundAssetId;
    std::vector<Object> objects;
};
struct Context {
    std::string documentId;
    std::string pageId;
    std::uint64_t pageEpoch = 0;
    std::uint64_t interactionEpoch = 0;
};
enum class CommandKind {
    Add,
    Erase,
    Undo,
    Redo,
    Clear,
    AddPage,
    SelectPage,
    AddImagePage,
    SetBackground,
};
struct Command {
    std::string id;
    std::string actor;
    Context context;
    CommandKind kind = CommandKind::Add;
    Object object;
    // Erase/SelectPage target; fresh page ID for AddPage/AddImagePage,
    // fresh object ID for Redo.
    std::string target;
    std::string assetId;
    double pageWidth = 0;
    double pageHeight = 0;
};
enum class Status { Applied, Duplicate, NoChange, Stale, Invalid, Forbidden, LimitReached };
struct Result {
    Status status = Status::Invalid;
    std::string objectId;
    bool changed() const { return status == Status::Applied; }
};

// Value-only, deterministic model. No widget/native resources and no implicit
// threads. M1's local panel is its sole UI-thread owner; M2 can host the same
// apply API on the Session Strand and project immutable views instead.
class Document final {
public:
    static constexpr std::size_t MaxPages = 8;
    static constexpr std::size_t MaxPageObjects = 2000;
    static constexpr std::size_t MaxObjects = 8000;
    static constexpr std::size_t MaxStrokePoints = 4096;
    static constexpr std::size_t MaxPoints = 250000;
    static constexpr std::size_t MaxHistory = 100;
    static constexpr std::size_t MaxSnapshotBytes = 8 * 1024 * 1024;

    explicit Document(std::string id, std::string owner = "local",
                      double pageWidth = 1920, double pageHeight = 1080);
    const std::string &id() const { return id_; }
    const std::string &owner() const { return owner_; }
    const std::vector<Page> &pages() const { return pages_; }
    const Page &page() const { return pages_[active_]; }
    std::size_t activePageIndex() const { return active_; }
    std::uint64_t revision() const { return revision_; }
    Context context() const;
    Result apply(const Command &command);
    void advanceInteractionBarrier();
    bool canUndo(const std::string &actor) const;
    bool canRedo(const std::string &actor) const;

    // Content snapshots intentionally exclude transient gestures and undo/redo
    // history. Loading is transactional and bounded; invalid input returns null.
    std::string toJson() const;
    static std::optional<Document> fromJson(std::string_view bytes);

private:
    struct History { std::string pageId; std::uint64_t epoch; Object object; };
    bool validObject(const Object &object, const Page &page) const;
    bool canAdd(const Object &object) const;
    void remember(const Object &object);
    void advanceInteraction();
    std::string id_, owner_;
    std::vector<Page> pages_;
    std::size_t active_ = 0;
    std::uint64_t interactionEpoch_ = 1, revision_ = 0;
    std::vector<History> undo_, redo_;
    std::unordered_set<std::string> acceptedCommands_, usedObjectIds_;
};

} // namespace livekit::whiteboard
