#include "whiteboard_document.h"
#include "whiteboard_asset.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace livekit::whiteboard {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxCommands = 20000;
bool identifier(const std::string &value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
        });
}
bool utf8Text(const std::string &text) {
    if (text.empty() || text.size() > 8000) return false;
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++count) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        unsigned code = lead, extra = 0, minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { code &= 0x1f; extra = 1; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { code &= 0x0f; extra = 2; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { code &= 7; extra = 3; minimum = 0x10000; }
        else if (lead >= 0x80 || (lead < 0x20 && lead != '\n' && lead != '\t')) return false;
        if (i + extra > text.size()) return false;
        for (unsigned j = 0; j < extra; ++j) {
            const auto c = static_cast<unsigned char>(text[i++]);
            if ((c & 0xc0) != 0x80) return false;
            code = (code << 6) | (c & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return count <= 2000;
}
bool pageSize(double width, double height) {
    return std::isfinite(width) && std::isfinite(height) &&
        std::floor(width) == width && std::floor(height) == height &&
        width >= 320 && height >= 180 &&
        width <= MaxAssetDimension && height <= MaxAssetDimension &&
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) <= MaxAssetPixels;
}
std::size_t snapshotBudget(const Object &object) {
    // Conservative bound for JSON double formatting, UTF-8/control escaping,
    // identifiers and fixed fields. Keeps every accepted document serializable.
    return 1024 + object.points.size() * 64 + object.text.size() * 6;
}
std::uint64_t counter(const Json &value) {
    const auto &text = value.get_ref<const std::string &>();
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        result == 0 || result == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument("counter");
    }
    return result;
}
} // namespace

Document::Document(std::string id, std::string owner, double pageWidth, double pageHeight)
    : id_(std::move(id)), owner_(std::move(owner)) {
    if (!identifier(id_) || !identifier(owner_) || !pageSize(pageWidth, pageHeight))
        throw std::invalid_argument("document identity or page size");
    Page first;
    first.id = "page-1";
    first.width = pageWidth;
    first.height = pageHeight;
    pages_.push_back(std::move(first));
}

Context Document::context() const {
    return {id_, page().id, page().epoch, interactionEpoch_};
}

bool Document::validObject(const Object &object, const Page &target) const {
    const auto kind = static_cast<int>(object.kind);
    if (!identifier(object.id) || !identifier(object.author) || kind < 0 || kind > 6 ||
        object.color > 0xffffff || !std::isfinite(object.width) || object.width < 1 || object.width > 64 ||
        !std::isfinite(object.fontSize) || object.fontSize < 8 || object.fontSize > 96) return false;
    if (object.points.empty() || object.points.size() > MaxStrokePoints) return false;
    if (object.kind != ObjectKind::Pen && object.kind != ObjectKind::Highlighter && object.points.size() != 2)
        return false;
    for (const auto &p : object.points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
            p.x < 0 || p.y < 0 || p.x > target.width || p.y > target.height) return false;
    }
    if (object.kind == ObjectKind::Text) {
        return utf8Text(object.text) && object.points[1].x > object.points[0].x &&
            object.points[1].y > object.points[0].y;
    }
    return object.text.empty();
}

bool Document::canAdd(const Object &object) const {
    if (page().objects.size() >= MaxPageObjects || usedObjectIds_.size() >= kMaxCommands) return false;
    std::size_t objects = 1, points = object.points.size(), bytes = 4096 + snapshotBudget(object);
    for (const auto &p : pages_) for (const auto &o : p.objects) {
        ++objects;
        points += o.points.size();
        bytes += snapshotBudget(o);
    }
    return objects <= MaxObjects && points <= MaxPoints && bytes <= MaxSnapshotBytes;
}

void Document::remember(const Object &object) {
    if (undo_.size() == MaxHistory) undo_.erase(undo_.begin());
    undo_.push_back({page().id, page().epoch, object});
}

void Document::advanceInteraction() {
    ++interactionEpoch_;
    acceptedCommands_.clear();
    redo_.clear();
}

void Document::advanceInteractionBarrier() {
    if (interactionEpoch_ == std::numeric_limits<std::uint64_t>::max()) return;
    advanceInteraction();
    ++revision_;
}

bool Document::canUndo(const std::string &actor) const {
    return std::any_of(undo_.rbegin(), undo_.rend(), [&](const History &h) {
        return h.pageId == page().id && h.epoch == page().epoch && h.object.author == actor &&
            std::any_of(page().objects.begin(), page().objects.end(), [&](const Object &o) { return o.id == h.object.id; });
    });
}
bool Document::canRedo(const std::string &actor) const {
    return std::any_of(redo_.rbegin(), redo_.rend(), [&](const History &h) {
        return h.pageId == page().id && h.epoch == page().epoch && h.object.author == actor;
    });
}

Result Document::apply(const Command &command) {
    const auto expected = context();
    if (command.context.documentId != expected.documentId || command.context.pageId != expected.pageId ||
        command.context.pageEpoch != expected.pageEpoch || command.context.interactionEpoch != expected.interactionEpoch)
        return {Status::Stale, {}};
    if (!identifier(command.id) || !identifier(command.actor)) return {Status::Invalid, {}};
    // Bound key fields avoid concatenation ambiguity. Actors are supplied by
    // the local caller in M1, not parsed as trusted identities from the wire.
    const auto key = command.actor + "/" + command.id;
    if (acceptedCommands_.count(key)) return {Status::Duplicate, {}};
    if (acceptedCommands_.size() >= kMaxCommands || interactionEpoch_ == std::numeric_limits<std::uint64_t>::max() ||
        page().epoch == std::numeric_limits<std::uint64_t>::max()) return {Status::LimitReached, {}};
    auto &objects = pages_[active_].objects;
    std::string changedId;
    switch (command.kind) {
    case CommandKind::Add: {
        if (command.object.author != command.actor || !validObject(command.object, page()) ||
            usedObjectIds_.count(command.object.id)) return {Status::Invalid, {}};
        if (!canAdd(command.object)) return {Status::LimitReached, {}};
        objects.push_back(command.object);
        usedObjectIds_.insert(command.object.id);
        remember(command.object);
        redo_.erase(std::remove_if(redo_.begin(), redo_.end(), [&](const History &h) {
            return h.object.author == command.actor;
        }), redo_.end());
        changedId = command.object.id;
        break;
    }
    case CommandKind::Erase: {
        const auto it = std::find_if(objects.begin(), objects.end(), [&](const Object &o) { return o.id == command.target; });
        if (it == objects.end()) return {Status::NoChange, {}};
        if (command.actor != owner_ && it->author != command.actor) return {Status::Forbidden, {}};
        changedId = it->id;
        objects.erase(it);
        break;
    }
    case CommandKind::Undo: {
        auto it = undo_.end();
        while (it != undo_.begin()) {
            --it;
            if (it->pageId != page().id || it->epoch != page().epoch || it->object.author != command.actor) continue;
            const auto found = std::find_if(objects.begin(), objects.end(), [&](const Object &o) { return o.id == it->object.id; });
            if (found == objects.end()) continue;
            changedId = found->id;
            if (redo_.size() == MaxHistory) redo_.erase(redo_.begin());
            redo_.push_back(*it);
            undo_.erase(it);
            objects.erase(found);
            break;
        }
        if (changedId.empty()) return {Status::NoChange, {}};
        break;
    }
    case CommandKind::Redo: {
        if (!identifier(command.target) || usedObjectIds_.count(command.target)) return {Status::Invalid, {}};
        auto it = redo_.end();
        while (it != redo_.begin()) {
            --it;
            if (it->pageId != page().id || it->epoch != page().epoch || it->object.author != command.actor) continue;
            auto object = it->object;
            object.id = command.target;
            if (!canAdd(object)) return {Status::LimitReached, {}};
            objects.push_back(object);
            usedObjectIds_.insert(object.id);
            remember(object);
            changedId = object.id;
            redo_.erase(it);
            break;
        }
        if (changedId.empty()) return {Status::NoChange, {}};
        break;
    }
    case CommandKind::Clear:
        if (command.actor != owner_) return {Status::Forbidden, {}};
        objects.clear();
        ++pages_[active_].epoch;
        undo_.erase(std::remove_if(undo_.begin(), undo_.end(), [&](const History &h) { return h.pageId == page().id; }), undo_.end());
        advanceInteraction();
        break;
    case CommandKind::AddPage: {
        if (command.actor != owner_) return {Status::Forbidden, {}};
        if (pages_.size() >= MaxPages) return {Status::LimitReached, {}};
        if (!identifier(command.target) || std::any_of(pages_.begin(), pages_.end(), [&](const Page &p) { return p.id == command.target; }))
            return {Status::Invalid, {}};
        Page next;
        next.id = command.target;
        pages_.push_back(std::move(next));
        active_ = pages_.size() - 1;
        advanceInteraction();
        break;
    }
    case CommandKind::AddImagePage: {
        if (command.actor != owner_) return {Status::Forbidden, {}};
        if (pages_.size() >= MaxPages) return {Status::LimitReached, {}};
        if (!identifier(command.target) || !validAssetId(command.assetId) ||
            !pageSize(command.pageWidth, command.pageHeight) ||
            std::any_of(pages_.begin(), pages_.end(), [&](const Page &p) { return p.id == command.target; }))
            return {Status::Invalid, {}};
        Page next;
        next.id = command.target;
        next.width = command.pageWidth;
        next.height = command.pageHeight;
        next.backgroundAssetId = command.assetId;
        pages_.push_back(std::move(next));
        active_ = pages_.size() - 1;
        advanceInteraction();
        break;
    }
    case CommandKind::SetBackground:
        if (command.actor != owner_) return {Status::Forbidden, {}};
        if (!validAssetId(command.assetId) || !pageSize(command.pageWidth, command.pageHeight))
            return {Status::Invalid, {}};
        objects.clear();
        pages_[active_].backgroundAssetId = command.assetId;
        pages_[active_].width = command.pageWidth;
        pages_[active_].height = command.pageHeight;
        ++pages_[active_].epoch;
        undo_.erase(std::remove_if(undo_.begin(), undo_.end(), [&](const History &h) {
            return h.pageId == page().id;
        }), undo_.end());
        advanceInteraction();
        break;
    case CommandKind::SelectPage: {
        if (command.actor != owner_) return {Status::Forbidden, {}};
        const auto it = std::find_if(pages_.begin(), pages_.end(), [&](const Page &p) { return p.id == command.target; });
        if (it == pages_.end()) return {Status::Invalid, {}};
        if (it->id == page().id) return {Status::NoChange, {}};
        active_ = static_cast<std::size_t>(it - pages_.begin());
        advanceInteraction();
        break;
    }
    default: return {Status::Invalid, {}};
    }
    acceptedCommands_.insert(key);
    ++revision_;
    return {Status::Applied, std::move(changedId)};
}

std::string Document::toJson() const {
    Json value{{"version", 2}, {"id", id_}, {"owner", owner_}, {"active", page().id},
        {"interactionEpoch", std::to_string(interactionEpoch_)}, {"pages", Json::array()}};
    for (const auto &p : pages_) {
        Json pageValue{{"id", p.id}, {"epoch", std::to_string(p.epoch)}, {"width", p.width},
            {"height", p.height}, {"backgroundAssetId", p.backgroundAssetId},
            {"objects", Json::array()}};
        for (const auto &o : p.objects) {
            Json points = Json::array();
            for (const auto &point : o.points) points.push_back({point.x, point.y});
            pageValue["objects"].push_back({{"id", o.id}, {"author", o.author}, {"kind", static_cast<int>(o.kind)},
                {"color", o.color}, {"width", o.width}, {"fontSize", o.fontSize}, {"points", std::move(points)}, {"text", o.text}});
        }
        value["pages"].push_back(std::move(pageValue));
    }
    return value.dump();
}

std::optional<Document> Document::fromJson(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > MaxSnapshotBytes) return std::nullopt;
    try {
        const auto value = Json::parse(bytes, [](int depth, Json::parse_event_t, Json &) {
            if (depth > 12) throw std::invalid_argument("depth");
            return true;
        });
        if (!value.at("version").is_number_integer()) return std::nullopt;
        const auto version = value.at("version").get<int>();
        if (version != 1 && version != 2) return std::nullopt;
        Document result(value.at("id").get<std::string>(), value.at("owner").get<std::string>());
        result.pages_.clear();
        result.interactionEpoch_ = counter(value.at("interactionEpoch"));
        const auto activeId = value.at("active").get<std::string>();
        const auto &pages = value.at("pages");
        if (!pages.is_array() || pages.empty() || pages.size() > MaxPages) return std::nullopt;
        std::unordered_set<std::string> pageIds;
        std::size_t totalObjects = 0, totalPoints = 0, budget = 4096;
        bool foundActive = false;
        for (const auto &p : pages) {
            Page page;
            page.id = p.at("id").get<std::string>();
            page.epoch = counter(p.at("epoch"));
            page.width = p.at("width").get<double>();
            page.height = p.at("height").get<double>();
            if (version >= 2) page.backgroundAssetId = p.at("backgroundAssetId").get<std::string>();
            if (!identifier(page.id) || !pageIds.insert(page.id).second ||
                !pageSize(page.width, page.height) ||
                (!page.backgroundAssetId.empty() && !validAssetId(page.backgroundAssetId)))
                return std::nullopt;
            const auto &objects = p.at("objects");
            if (!objects.is_array() || objects.size() > MaxPageObjects) return std::nullopt;
            for (const auto &o : objects) {
                if (++totalObjects > MaxObjects) return std::nullopt;
                Object object;
                object.id = o.at("id").get<std::string>();
                object.author = o.at("author").get<std::string>();
                if (!o.at("kind").is_number_integer() || o.at("kind") < 0 || o.at("kind") > 6 ||
                    !o.at("color").is_number_integer() || o.at("color") < 0 || o.at("color") > 0xffffff)
                    return std::nullopt;
                const auto kind = o.at("kind").get<int>();
                object.kind = static_cast<ObjectKind>(kind);
                const auto color = o.at("color").get<std::int64_t>();
                if (color < 0 || color > 0xffffff) return std::nullopt;
                object.color = static_cast<std::uint32_t>(color);
                object.width = o.at("width").get<double>();
                object.fontSize = o.at("fontSize").get<double>();
                object.text = o.at("text").get<std::string>();
                const auto &points = o.at("points");
                if (!points.is_array() || points.size() > MaxStrokePoints) return std::nullopt;
                for (const auto &point : points) {
                    if (!point.is_array() || point.size() != 2) return std::nullopt;
                    object.points.push_back({point[0].get<double>(), point[1].get<double>()});
                }
                totalPoints += object.points.size();
                budget += snapshotBudget(object);
                if (totalPoints > MaxPoints || budget > MaxSnapshotBytes || !result.validObject(object, page) ||
                    !result.usedObjectIds_.insert(object.id).second) return std::nullopt;
                page.objects.push_back(std::move(object));
            }
            if (page.id == activeId) { result.active_ = result.pages_.size(); foundActive = true; }
            result.pages_.push_back(std::move(page));
        }
        if (!foundActive) return std::nullopt;
        return result;
    } catch (const std::exception &) { return std::nullopt; }
}

} // namespace livekit::whiteboard
