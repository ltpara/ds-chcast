#pragma once

// The host's pointer, installed as the local Windows cursor.
//
// During remote control the local pointer drives the host, so the pointer the
// user should see is the *host's shape* at the local hand's position. Installing
// it as the system cursor is what makes that free: the OS composites it at the
// hardware cursor rate, it cannot lag behind the hand, and it cannot drift from
// the pointer the user is actually moving. Painting the sprite at host
// coordinates would do all three wrong while the user is in control.
//
// Sprites arrive premultiplied BGRA from Desktop Duplication. Windows wants a
// 32-bit colour DIB plus a 1-bit AND mask; the mask is built from alpha, so an
// opaque black pixel (an outlined pointer's shadow) stays visible instead of
// being punched out.

#include <windows.h>

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace chast_viewer {

class RemoteCursor {
public:
    RemoteCursor() = default;
    ~RemoteCursor();
    RemoteCursor(const RemoteCursor&) = delete;
    RemoteCursor& operator=(const RemoteCursor&) = delete;

    // Keeps `bgra` (premultiplied, `width` x `height`) as the sprite for
    // `shape_id`. Called once per shape; a repeated id is ignored.
    bool Register(uint64_t shape_id, int width, int height, int hotspot_x, int hotspot_y,
                  const std::vector<uint8_t>& bgra, std::string* error);
    // Makes `shape_id` the cursor, drawn at `scale` device pixels per host
    // pixel. The scaled handle is built on first use and cached, so later
    // activations of the same shape and scale cost nothing.
    bool Activate(uint64_t shape_id, double scale, std::string* error);
    // Stops overriding the system cursor; registered sprites stay cached.
    void Deactivate();
    // Frees every handle and sprite. Call when a session ends.
    void Clear();

    bool active() const { return cursor() != nullptr; }
    HCURSOR cursor() const;

    // Scale is quantised (quarter steps below 2x, whole steps above) so a
    // resize does not rebuild the sprite for changes nobody can see.
    static double QuantiseScale(double scale);

private:
    struct Shape {
        int width = 0;
        int height = 0;
        int hotspot_x = 0;
        int hotspot_y = 0;
        std::vector<uint8_t> bgra;
        std::map<int, HCURSOR> scaled;  // scale percent -> handle
    };

    HCURSOR build_locked(const Shape& shape, int width, int height, int percent,
                         std::string* error);
    void release_shape(Shape& shape);
    void evict_locked();

    std::map<uint64_t, Shape> shapes_;
    // Registration order, oldest first, so eviction is a host that keeps
    // inventing shapes (an animated pointer under a buggy driver) and not one
    // that cycles through the pointers a desktop actually uses.
    std::deque<uint64_t> order_;
    uint64_t active_shape_ = 0;
    int active_scale_ = 0;
};

}  // namespace chast_viewer
