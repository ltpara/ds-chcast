#include "win/annotation_paint.h"
#include "win/annotation_history.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

#define EXPECT(condition)                                                          \
    do {                                                                           \
        if (!(condition)) {                                                        \
            std::fprintf(stderr, "annotation paint failure at %s:%d: %s\n", __FILE__, \
                         __LINE__, #condition);                                       \
            ++failures;                                                            \
        }                                                                          \
    } while (false)

uint32_t alpha_at(const std::vector<uint32_t>& pixels, int width, int x, int y) {
    return pixels[static_cast<size_t>(y) * width + x] >> 24;
}

void expect_premultiplied(const std::vector<uint32_t>& pixels) {
    for (const uint32_t pixel : pixels) {
        const uint32_t alpha = pixel >> 24;
        EXPECT(((pixel >> 16) & 0xFFu) <= alpha);
        EXPECT(((pixel >> 8) & 0xFFu) <= alpha);
        EXPECT((pixel & 0xFFu) <= alpha);
    }
}

void test_segment_and_clip() {
    constexpr int width = 80;
    constexpr int height = 60;
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0);
    chast::annotation_draw_segment(pixels.data(), width, height, -30, 30, 110, 30, 4.0,
                                   0x00FF00, 220);
    expect_premultiplied(pixels);
    EXPECT(alpha_at(pixels, width, 0, 30) > 0);
    EXPECT(alpha_at(pixels, width, width - 1, 30) > 0);
    EXPECT(alpha_at(pixels, width, width / 2, 30) > 180);
    EXPECT(alpha_at(pixels, width, width / 2, 3) == 0);
}

void test_dot_and_color() {
    constexpr int width = 32;
    constexpr int height = 32;
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0);
    chast::annotation_draw_segment(pixels.data(), width, height, 16, 16, 16, 16, 3.5,
                                   0xFF0000, 240);
    const uint32_t center = pixels[16 * width + 16];
    EXPECT((center >> 24) > 200);
    EXPECT(((center >> 16) & 0xFFu) > 180);
    EXPECT(((center >> 8) & 0xFFu) == 0);
    EXPECT((center & 0xFFu) == 0);
    EXPECT(alpha_at(pixels, width, 0, 0) == 0);
    expect_premultiplied(pixels);
}

void test_overlay_blend() {
    constexpr int width = 24;
    constexpr int height = 24;
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0);
    chast::annotation_draw_segment(pixels.data(), width, height, 4, 12, 20, 12, 4.0,
                                   0x0000FF, 255);
    chast::annotation_draw_segment(pixels.data(), width, height, 12, 4, 12, 20, 4.0,
                                   0xFF0000, 120);
    EXPECT(alpha_at(pixels, width, 12, 12) >= 250);
    expect_premultiplied(pixels);
}

void test_interleaved_strokes() {
    chast::detail::AnnotationHistory history;
    EXPECT(history.begin(1, chast::AnnotationPoint{10, 10}));
    EXPECT(history.begin(2, chast::AnnotationPoint{20, 20}));
    EXPECT(history.append(1, chast::AnnotationPoint{11, 11}));
    EXPECT(history.append(2, chast::AnnotationPoint{21, 21}));
    EXPECT(history.end(1));
    EXPECT(!history.append(1, chast::AnnotationPoint{12, 12}));
    EXPECT(history.append(2, chast::AnnotationPoint{22, 22}));
    EXPECT(history.end(2));
    EXPECT(history.strokes().size() == 2);
    EXPECT(history.strokes()[0].id == 1 && history.strokes()[0].points.size() == 2);
    EXPECT(history.strokes()[1].id == 2 && history.strokes()[1].points.size() == 3);
    EXPECT(history.strokes()[1].points.back().x == 22);
    EXPECT(history.point_count() == 5);
    EXPECT(!history.is_active(1) && !history.is_active(2));
}

void test_undo_removes_newest() {
    chast::detail::AnnotationHistory history;
    EXPECT(history.begin(7, chast::AnnotationPoint{1, 2}));
    EXPECT(history.end(7));
    EXPECT(history.begin(8, chast::AnnotationPoint{3, 4}));
    EXPECT(history.append(8, chast::AnnotationPoint{5, 6}));
    EXPECT(history.end(8));
    EXPECT(history.strokes().size() == 2);
    EXPECT(history.undo());
    EXPECT(history.strokes().size() == 1 && history.strokes().front().id == 7);
    EXPECT(history.point_count() == 1);
    EXPECT(!history.is_active(8));
    EXPECT(!history.append(8, chast::AnnotationPoint{9, 10}));
}

void test_history_bounds() {
    chast::detail::AnnotationHistory history;
    EXPECT(history.begin(10, chast::AnnotationPoint{0, 0}));
    EXPECT(history.begin(11, chast::AnnotationPoint{1, 1}));
    EXPECT(history.begin(12, chast::AnnotationPoint{2, 2}));
    EXPECT(history.trim(2, 64));
    EXPECT(history.strokes().size() == 2);
    EXPECT(history.strokes().front().id == 11 && history.strokes().back().id == 12);
    EXPECT(!history.is_active(10) && history.is_active(11) && history.is_active(12));

    history.clear();
    EXPECT(history.begin(20, chast::AnnotationPoint{0, 0}));
    for (int i = 1; i <= 6; ++i) {
        EXPECT(history.append(20, chast::AnnotationPoint{i, i}));
    }
    EXPECT(history.trim(2, 3));
    EXPECT(history.strokes().size() == 1 && history.point_count() == 3);
    EXPECT(history.strokes().front().points.front().x == 4);
    EXPECT(history.is_active(20));
    EXPECT(history.append(20, chast::AnnotationPoint{7, 7}));
}

}  // namespace

int main() {
    test_segment_and_clip();
    test_dot_and_color();
    test_overlay_blend();
    test_interleaved_strokes();
    test_undo_removes_newest();
    test_history_bounds();
    if (failures != 0) {
        std::fprintf(stderr, "%d annotation paint expectation(s) failed\n", failures);
        return 1;
    }
    std::printf("annotation paint tests passed\n");
    return 0;
}
