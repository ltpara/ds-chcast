#pragma once

// Temporary freehand annotations drawn on the host desktop for one viewer.
//
// Each monitor gets a layered, click-through, never-activated tool window, so
// the overlay cannot take focus or intercept input. A stroke is kept only in
// memory and dies with this object, which the host owns for the lifetime of
// the viewer session. Points use virtual-desktop pixels; monitor planes clip
// them locally, including negative coordinates and lines that cross outputs.

#include <cstdint>
#include <memory>
#include <string>

#include "chast/protocol.h"

namespace chast {

class AnnotationOverlay {
public:
    // Starts the overlay thread. `label` is the viewer's device name and is
    // hashed into the ink colour. Returns nullptr with *error set when no
    // interactive monitor surface can be created.
    static std::unique_ptr<AnnotationOverlay> create(std::string* error,
                                                     const std::string& label = {});

    ~AnnotationOverlay();
    AnnotationOverlay(const AnnotationOverlay&) = delete;
    AnnotationOverlay& operator=(const AnnotationOverlay&) = delete;

    // Applies one begin/move/end/clear/undo message in order. Returns false
    // with *error set when rendering a changed monitor surface failed.
    bool apply(const AnnotationMessage& annotation, std::string* error);

private:
    AnnotationOverlay();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chast
