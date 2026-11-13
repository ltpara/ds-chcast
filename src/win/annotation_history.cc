#include "win/annotation_history.h"

#include <algorithm>

namespace chast::detail {

bool AnnotationHistory::is_active(uint32_t id) const {
    return std::find(active_ids_.begin(), active_ids_.end(), id) != active_ids_.end();
}

bool AnnotationHistory::begin(uint32_t id, const AnnotationPoint& point) {
    if (id == 0 || is_active(id)) {
        return false;
    }
    strokes_.push_back(Stroke{id, {point}});
    active_ids_.push_back(id);
    ++point_count_;
    return true;
}

bool AnnotationHistory::append(uint32_t id, const AnnotationPoint& point) {
    if (!is_active(id)) {
        return false;
    }
    for (auto stroke = strokes_.rbegin(); stroke != strokes_.rend(); ++stroke) {
        if (stroke->id == id) {
            stroke->points.push_back(point);
            ++point_count_;
            return true;
        }
    }
    return false;
}

bool AnnotationHistory::end(uint32_t id) {
    if (!is_active(id)) {
        return false;
    }
    deactivate(id);
    return true;
}

void AnnotationHistory::clear() {
    strokes_.clear();
    active_ids_.clear();
    point_count_ = 0;
}

bool AnnotationHistory::undo() {
    if (strokes_.empty()) {
        return false;
    }
    deactivate(strokes_.back().id);
    point_count_ -= strokes_.back().points.size();
    strokes_.pop_back();
    return true;
}

bool AnnotationHistory::trim(size_t max_strokes, size_t max_points) {
    bool trimmed = false;
    while (strokes_.size() > max_strokes) {
        remove_front();
        trimmed = true;
    }
    while (point_count_ > max_points && strokes_.size() > 1) {
        remove_front();
        trimmed = true;
    }
    if (point_count_ > max_points && !strokes_.empty()) {
        std::vector<AnnotationPoint>& points = strokes_.front().points;
        const size_t excess =
            (std::min)(point_count_ - max_points,
                       points.size() > 1 ? points.size() - 1 : size_t{0});
        if (excess != 0) {
            points.erase(points.begin(), points.begin() + static_cast<ptrdiff_t>(excess));
            point_count_ -= excess;
            trimmed = true;
        }
    }
    return trimmed;
}

void AnnotationHistory::deactivate(uint32_t id) {
    active_ids_.erase(std::remove(active_ids_.begin(), active_ids_.end(), id),
                      active_ids_.end());
}

void AnnotationHistory::remove_front() {
    deactivate(strokes_.front().id);
    point_count_ -= strokes_.front().points.size();
    strokes_.erase(strokes_.begin());
}

}  // namespace chast::detail
