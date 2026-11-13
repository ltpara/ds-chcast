#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "chast/protocol.h"

namespace chast::detail {

// Ordered, bounded stroke history shared by the renderer and its unit tests.
// Several display windows may draw at once, so activity is tracked per stroke
// id instead of assuming that the newest stroke is the only one still moving.
class AnnotationHistory {
public:
    struct Stroke {
        uint32_t id = 0;
        std::vector<AnnotationPoint> points;
    };

    bool begin(uint32_t id, const AnnotationPoint& point);
    bool append(uint32_t id, const AnnotationPoint& point);
    bool end(uint32_t id);
    void clear();
    bool undo();
    bool trim(size_t max_strokes, size_t max_points);

    const std::vector<Stroke>& strokes() const { return strokes_; }
    size_t point_count() const { return point_count_; }
    bool is_active(uint32_t id) const;

private:
    void deactivate(uint32_t id);
    void remove_front();

    std::vector<Stroke> strokes_;
    std::vector<uint32_t> active_ids_;
    size_t point_count_ = 0;
};

}  // namespace chast::detail
