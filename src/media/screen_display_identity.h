#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace livekit::screen_display_identity {
struct Child {
    bool active = false;
    std::wstring name, key, id;
    bool operator==(const Child&) const = default;
};
struct Display {
    intptr_t index = 0;
    std::uint32_t session = 0;
    bool active = false, remote = false, rdp = false;
    std::wstring name, root_key;
    std::vector<Child> children;
};
struct Identity {
    intptr_t index = 0;
    std::uint32_t session = 0;
    std::wstring name, root_key;
    bool remote_child = false;
    Child child;
    bool operator==(const Identity&) const = default;
};
struct Geometry {
    int x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Geometry&) const = default;
};
inline std::optional<Identity> Select(const Display& display) {
    if (!display.active || display.name.empty()) return std::nullopt;
    Identity result{display.index, display.session, display.name, display.root_key};
    if (!display.root_key.empty()) return result;
    if (!display.remote || !display.rdp) return std::nullopt;
    const Child* selected = nullptr;
    for (const auto& child : display.children) {
        if (!child.active) continue;
        if (selected || child.name.empty() || child.key.empty() || child.id.empty()) return std::nullopt;
        selected = &child;
    }
    if (!selected) return std::nullopt;
    result.remote_child = true;
    result.child = *selected;
    return result;
}
struct Observation { Identity identity; Geometry geometry; };
inline bool MatchesSelectedDisplay(const Observation& observation, const std::wstring& selectedName) {
    return !selectedName.empty() && selectedName == observation.identity.name;
}
// Both callbacks are injected: tests use inert enumeration/geometry fixtures.
// The geometry callback receives the actual root key, never the child key.
template<class Enumerate, class ReadGeometry>
std::optional<Observation> Observe(Enumerate enumerate, ReadGeometry geometry) {
    const auto before = enumerate();
    if (!before) return std::nullopt;
    const auto identity = Select(*before);
    if (!identity) return std::nullopt;
    const auto rect = geometry(identity->index, identity->root_key);
    if (!rect || rect->width <= 0 || rect->height <= 0)
        return std::nullopt;
    const auto after = enumerate();
    if (!after) return std::nullopt;
    const auto confirmed = Select(*after);
    if (!confirmed || *confirmed != *identity) return std::nullopt;
    return Observation{*identity, *rect};
}
} // namespace livekit::screen_display_identity
