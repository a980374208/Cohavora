#pragma once

#include "video_demand_types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace livekit {

struct SpeakerStripMetrics {
    int width = 0;
    int height = 0;
    uint32_t columns = 0;
    uint32_t rows = 0;
};

// Capacity and placement use the same fixed thumbnail size. In particular,
// neither a short final page nor a speaking-only list stretches the strip.
inline SpeakerStripMetrics CalculateSpeakerStripMetrics(SpeakerLayout layout,
                                                        LogicalViewport stage) {
    if (stage.width < 240 || stage.height < 144) return {};
    constexpr int gap = 8;
    const int inner_width = stage.width - 16;
    const int inner_height = stage.height - 16;
    int width = std::min(inner_width / 3,
        std::min(260, std::max(96, static_cast<int>(int64_t(inner_width) * 24 / 100))));
    const bool horizontal = layout != SpeakerLayout::Right && layout != SpeakerLayout::Floating;
    if (horizontal) {
        width = std::min(width, static_cast<int>(int64_t(inner_height / 3) * 16 / 9));
    }
    const int height = static_cast<int>(int64_t(width) * 9 / 16);
    if (width <= 0 || height <= 0) return {};
    // Count a full-size card as soon as its first pixel enters the strip. The
    // corresponding clip below removes the remainder; a gap alone adds no seat.
    const auto count = [](int available, int stride) {
        return available <= 0 ? 0u
            : static_cast<uint32_t>(1 + (int64_t(available) - 1) / stride);
    };
    const auto columns = count(stage.width - 8, width + gap);
    const int vertical_height = layout == SpeakerLayout::TopLeft
        ? std::max(0, stage.height - 8 - height - gap)
        : layout == SpeakerLayout::BottomLeft ? std::max(0, inner_height - height - gap)
        : horizontal ? inner_height : stage.height - 8;
    const auto rows = count(vertical_height, height + gap);
    return {width, height, columns, rows};
}

// Logical-pixel geometry shared by demand policy and both UI render backends.
// The result preserves the caller's seat order; main_index identifies the focus.
inline uint32_t SpeakerThumbnailCapacity(SpeakerLayout layout,
                                         LogicalViewport stage) {
    const auto metrics = CalculateSpeakerStripMetrics(layout, stage);
    switch (layout) {
    case SpeakerLayout::Right:
    case SpeakerLayout::Floating:
        return metrics.rows;
    case SpeakerLayout::Top:
        return metrics.columns;
    case SpeakerLayout::TopLeft:
    case SpeakerLayout::BottomLeft:
        return metrics.columns + metrics.rows;
    }
    return 0;
}

inline std::vector<LogicalViewport> CalculateVideoLayout(
    VideoLayoutMode mode, SpeakerLayout speaker_layout, LogicalViewport stage,
    uint32_t grid_capacity, std::size_t total_seats, std::size_t main_index = 0) {
    std::vector<LogicalViewport> result(total_seats);
    if (total_seats == 0 || stage.width <= 0 || stage.height <= 0) return result;
    constexpr int gap = 8;
    const int margin = std::min({8, stage.width / 4, stage.height / 4});
    LogicalViewport inner{stage.x + margin, stage.y + margin,
        stage.width - 2 * margin, stage.height - 2 * margin};
    const auto fit = [](LogicalViewport box) {
        if (box.width <= 0 || box.height <= 0) return LogicalViewport{};
        const int w = static_cast<int>(std::min<int64_t>(box.width,
            int64_t(box.height) * 16 / 9));
        const int h = static_cast<int>(std::min<int64_t>(box.height,
            int64_t(box.width) * 9 / 16));
        return LogicalViewport{box.x + (box.width - w) / 2,
            box.y + (box.height - h) / 2, w, h};
    };
    const bool gallery = mode == VideoLayoutMode::Grid || mode == VideoLayoutMode::Auto;
    if (gallery) {
        // A full page keeps its advertised square grid. Partial pages optimize
        // the actual seat count and center the incomplete final row.
        const int count = static_cast<int>(total_seats);
        const int dimension = static_cast<int>(std::ceil(std::sqrt(
            static_cast<double>(std::max<uint32_t>(1, grid_capacity)))));
        int columns = dimension, rows = (count + columns - 1) / columns;
        int tile_w = 0, tile_h = 0;
        int64_t best_area = -1;
        for (int candidate = 1; candidate <= std::min(count, dimension); ++candidate) {
            if (total_seats == grid_capacity && candidate != dimension) continue;
            const int candidate_rows = (count + candidate - 1) / candidate;
            const auto box = fit({0, 0,
                std::max(0, inner.width - (candidate - 1) * gap) / candidate,
                std::max(0, inner.height - (candidate_rows - 1) * gap) / candidate_rows});
            const int64_t area = int64_t(box.width) * box.height;
            if (area > best_area) {
                best_area = area;
                columns = candidate;
                rows = candidate_rows;
                tile_w = box.width;
                tile_h = box.height;
            }
        }
        if (tile_w == 0 || tile_h == 0) {
            std::fill(result.begin(), result.end(), LogicalViewport{stage.x, stage.y, 0, 0});
            return result;
        }
        const int first_y = inner.y + (inner.height - rows * tile_h - (rows - 1) * gap) / 2;
        for (int row = 0, index = 0; row < rows; ++row) {
            const int row_count = std::min(columns, count - index);
            const int first_x = inner.x +
                (inner.width - row_count * tile_w - (row_count - 1) * gap) / 2;
            for (int col = 0; col < row_count; ++col, ++index) {
                result[index] = {first_x + col * (tile_w + gap),
                    first_y + row * (tile_h + gap), tile_w, tile_h};
            }
        }
        return result;
    }

    main_index = std::min(main_index, total_seats - 1);
    result[main_index] = inner;
    const std::size_t count = total_seats - 1;
    if (count == 0) return result;
    std::vector<LogicalViewport> thumbnails;
    thumbnails.reserve(count);
    if (mode == VideoLayoutMode::PictureInPicture) {
        const int available = std::max(0,
            inner.width - static_cast<int>(count - 1) * gap);
        const int width = std::min(available / static_cast<int>(count),
            std::max(1, static_cast<int>(int64_t(inner.width) * 22 / 100)));
        const int height = std::min(inner.height,
            static_cast<int>(int64_t(width) * 9 / 16));
        const int first_x = inner.x + inner.width -
            static_cast<int>(count) * width - static_cast<int>(count - 1) * gap;
        for (std::size_t index = 0; index < count; ++index) {
            thumbnails.push_back(width == 0 || height == 0
                ? LogicalViewport{stage.x, stage.y, 0, 0}
                : LogicalViewport{first_x + static_cast<int>(index) * (width + gap),
                    inner.y + inner.height - height, width, height});
        }
    } else {
        const auto metrics = CalculateSpeakerStripMetrics(speaker_layout, stage);
        const int width = metrics.width, height = metrics.height;
        const auto capacity = SpeakerThumbnailCapacity(speaker_layout, stage);
        const auto visible = std::min<std::size_t>(count, capacity);
        if (speaker_layout == SpeakerLayout::Right || speaker_layout == SpeakerLayout::Floating) {
            if (visible != 0 && speaker_layout == SpeakerLayout::Right) {
                result[main_index].width = std::max(0, inner.width - width - gap);
            }
            for (std::size_t index = 0; index < visible; ++index) {
                thumbnails.push_back({inner.x + inner.width - width,
                    inner.y + static_cast<int>(index) * (height + gap), width, height});
            }
        } else {
            const auto horizontal_count = std::min<std::size_t>(metrics.columns, visible);
            const auto vertical_count = visible - horizontal_count;
            const bool bottom = speaker_layout == SpeakerLayout::BottomLeft;
            const int row_y = bottom ? inner.y + inner.height - height : inner.y;
            const int content_y = bottom ? inner.y : inner.y + height + gap;
            const int content_height = std::max(0, inner.height - height - gap);
            if (visible != 0) {
                result[main_index] = {inner.x, content_y,
                    std::max(0, inner.width - (vertical_count ? width + gap : 0)), content_height};
            }
            for (std::size_t index = 0; index < horizontal_count; ++index) {
                thumbnails.push_back({inner.x + static_cast<int>(index) * (width + gap),
                    row_y, width, height});
            }
            for (std::size_t index = 0; index < vertical_count; ++index) {
                thumbnails.push_back({inner.x + inner.width - width,
                    content_y + static_cast<int>(index) * (height + gap), width, height});
            }
        }
    }
    for (std::size_t index = 0, thumbnail = 0; index < total_seats; ++index) {
        if (index != main_index && thumbnail < thumbnails.size()) {
            result[index] = thumbnails[thumbnail++];
        }
    }
    return result;
}

// Each clip is in stage coordinates and corresponds to the same-index full
// rectangle from CalculateVideoLayout. Rendering and hit testing use the clip;
// subscription dimensions continue to use the uncropped rectangle.
inline std::vector<LogicalViewport> CalculateVideoLayoutClips(
    VideoLayoutMode mode, SpeakerLayout speaker_layout, LogicalViewport stage,
    std::size_t total_seats, std::size_t main_index = 0) {
    std::vector<LogicalViewport> clips(total_seats, stage);
    if (total_seats <= 1 || stage.width <= 0 || stage.height <= 0 ||
        mode != VideoLayoutMode::Speaker) return clips;
    const bool horizontal = speaker_layout == SpeakerLayout::Top ||
        speaker_layout == SpeakerLayout::TopLeft || speaker_layout == SpeakerLayout::BottomLeft;
    if (!horizontal) return clips;
    const auto metrics = CalculateSpeakerStripMetrics(speaker_layout, stage);
    if (metrics.width == 0 || metrics.height == 0) return clips;
    constexpr int margin = 8, gap = 8;
    const bool bottom = speaker_layout == SpeakerLayout::BottomLeft;
    const int row_y = bottom ? stage.y + stage.height - margin - metrics.height : stage.y + margin;
    const int column_y = bottom ? stage.y + margin : row_y + metrics.height + gap;
    const int column_bottom = bottom ? row_y - gap : stage.y + stage.height;
    const LogicalViewport row_clip{stage.x, row_y, stage.width, metrics.height};
    const LogicalViewport column_clip{stage.x + stage.width - margin - metrics.width,
        column_y, metrics.width, std::max(0, column_bottom - column_y)};
    main_index = std::min(main_index, total_seats - 1);
    const auto horizontal_count = std::min<std::size_t>(metrics.columns, total_seats - 1);
    for (std::size_t index = 0, thumbnail = 0; index < total_seats; ++index) {
        if (index == main_index) continue;
        clips[index] = thumbnail++ < horizontal_count ? row_clip : column_clip;
    }
    return clips;
}

} // namespace livekit
