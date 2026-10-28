#include <doctest/doctest.h>

#include "ui/fork_pages.h"

using namespace babble::ui;

namespace {

bool same(Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

// True if the box lies inside a w x h image.
bool inside(Rect r, float w, float h) { return r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h; }

}

TEST_CASE("crop corner drag moves one corner and keeps the opposite one fixed") {
    const Rect b{80, 80, 80, 80};   // In a 240 x 240 image.
    // Unlocked: each corner moves with the pointer and w and h change on their own.
    CHECK(same(drag_corner(b, 0, -20, -10, 240, 240, false), {60, 70, 100, 90}));
    CHECK(same(drag_corner(b, 1, 20, -10, 240, 240, false), {80, 70, 100, 90}));
    CHECK(same(drag_corner(b, 2, -20, 10, 240, 240, false), {60, 80, 100, 90}));
    CHECK(same(drag_corner(b, 3, 20, 10, 240, 240, false), {80, 80, 100, 90}));
    // Locked: the larger side wins (here the one asked by dx) and the box stays square.
    CHECK(same(drag_corner(b, 0, -20, -10, 240, 240, true), {60, 60, 100, 100}));
    CHECK(same(drag_corner(b, 1, 20, -10, 240, 240, true), {80, 60, 100, 100}));
    CHECK(same(drag_corner(b, 2, -20, 10, 240, 240, true), {60, 80, 100, 100}));
    CHECK(same(drag_corner(b, 3, 10, 20, 240, 240, true), {80, 80, 100, 100}));
    // A shrinking drag asks for 50 x 70, and the larger side wins.
    CHECK(same(drag_corner(b, 3, -30, -10, 240, 240, true), {80, 80, 70, 70}));
}

TEST_CASE("crop corner drag keeps the minimum size") {
    const Rect b{80, 80, 80, 80};
    for (int c = 0; c < 4; ++c) {
        const float sx = c & 1 ? -1.f : 1.f, sy = c & 2 ? -1.f : 1.f;   // Toward the fixed corner.
        const Rect s = drag_corner(b, c, sx * 500, sy * 500, 240, 240, true);
        CHECK(s.w == kMinCrop);
        CHECK(s.h == kMinCrop);
    }
    // Unlocked, only the side that is squeezed stops at the minimum.
    CHECK(same(drag_corner(b, 3, -100, 50, 240, 240, false), {80, 80, 32, 130}));
    CHECK(same(drag_corner(b, 0, 100, -50, 240, 240, false), {128, 30, 32, 130}));
    // With less room than the minimum the box takes what there is.
    CHECK(same(drag_corner({0, 0, 20, 20}, 0, -5, -5, 240, 240, true), {0, 0, 20, 20}));
    CHECK(same(drag_corner({0, 0, 20, 20}, 0, 5, 5, 240, 240, false), {0, 0, 20, 20}));
}

TEST_CASE("crop corner drag stays inside the image") {
    const Rect b{80, 80, 80, 80};
    CHECK(same(drag_corner(b, 3, 500, 500, 240, 240, true), {80, 80, 160, 160}));
    CHECK(same(drag_corner(b, 0, -500, -500, 240, 240, true), {0, 0, 160, 160}));
    CHECK(same(drag_corner(b, 1, 500, -500, 240, 240, false), {80, 0, 160, 160}));
    CHECK(same(drag_corner(b, 2, -500, 500, 240, 240, false), {0, 80, 160, 160}));
    CHECK(same(drag_corner(b, 3, 500, 20, 240, 240, false), {80, 80, 160, 100}));
    // Locked, the smaller room limits both sides: 140 high, 220 wide.
    CHECK(same(drag_corner({20, 100, 60, 60}, 3, 500, 0, 240, 240, true), {20, 100, 140, 140}));
    // A wide image: the box can be wider than the image is high only when unlocked.
    CHECK(same(drag_corner({10, 10, 40, 40}, 3, 500, 500, 320, 240, true), {10, 10, 230, 230}));
    CHECK(same(drag_corner({10, 10, 40, 40}, 3, 500, 500, 320, 240, false), {10, 10, 310, 230}));
    for (int c = 0; c < 4; ++c)
        for (float d : {-500.f, -40.f, 0.f, 40.f, 500.f})
            for (bool sq : {false, true})
                CHECK(inside(drag_corner(b, c, d, -d / 2, 240, 240, sq), 240, 240));
}

TEST_CASE("crop move drag keeps the size and stays inside the image") {
    const Rect b{80, 80, 80, 80};
    CHECK(same(drag_move(b, 30, -10, 240, 240), {110, 70, 80, 80}));
    CHECK(same(drag_move(b, -500, 500, 240, 240), {0, 160, 80, 80}));
    CHECK(same(drag_move(b, 500, -500, 240, 240), {160, 0, 80, 80}));
    // A box as large as the image cannot move.
    CHECK(same(drag_move({0, 0, 240, 240}, 30, 30, 240, 240), {0, 0, 240, 240}));
}

TEST_CASE("crop drag under a rotate maps back to a source box") {
    // A 320 x 240 source turned one quarter, so the preview is 240 x 320.
    PreviewTransform t;
    t.w = 320;
    t.h = 240;
    t.q = 1;
    REQUIRE(t.view_w() == 240);
    REQUIRE(t.view_h() == 320);
    const Rect v0{40, 60, 100, 100};

    // Locked: the view box grows to the room on the narrow side.
    const Rect vs = drag_corner(v0, 3, 500, 500, t.view_w(), t.view_h(), true);
    CHECK(same(vs, {40, 60, 200, 200}));
    CHECK(same(t.box_to_source(vs), {60, 40, 200, 200}));
    CHECK(same(t.box_to_view(t.box_to_source(vs)), vs));

    // Unlocked: the view box is 200 x 260, so the source box is 260 x 200 and fills the source height.
    const Rect vu = drag_corner(v0, 3, 500, 500, t.view_w(), t.view_h(), false);
    CHECK(same(vu, {40, 60, 200, 260}));
    const Rect su = t.box_to_source(vu);
    CHECK(same(su, {0, 40, 260, 200}));
    CHECK(inside(su, t.w, t.h));
    CHECK(same(t.box_to_view(su), vu));

    // A top left drag keeps the bottom right corner of the view box where it was.
    const Rect vt = drag_corner(v0, 0, -20, -10, t.view_w(), t.view_h(), false);
    CHECK(vt.x + vt.w == v0.x + v0.w);
    CHECK(vt.y + vt.h == v0.y + v0.h);
    CHECK(inside(t.box_to_source(vt), t.w, t.h));

    // The drag stays in the source frame in every turn and flip.
    for (int q = 0; q < 4; ++q)
        for (int f = 0; f < 4; ++f) {
            PreviewTransform u;
            u.w = 320;
            u.h = 240;
            u.q = q;
            u.fh = f & 1;
            u.fv = f & 2;
            for (int c = 0; c < 4; ++c) {
                const Rect v = drag_corner({50, 50, 90, 90}, c, c * 70.f - 100, 300.f - c * 200, u.view_w(), u.view_h(), false);
                const Rect s = u.box_to_source(v);
                CHECK(inside(s, u.w, u.h));
                CHECK(same(u.box_to_view(s), v));
                CHECK(s.w == (q % 2 ? v.h : v.w));
                CHECK(s.h == (q % 2 ? v.w : v.h));
            }
        }
}

TEST_CASE("crop drag under a flip maps back to a source box") {
    PreviewTransform t;
    t.w = 320;
    t.h = 240;
    t.fh = true;
    const Rect v{10, 20, 100, 50};
    CHECK(same(t.box_to_source(v), {210, 20, 100, 50}));
    // Dragging the view's top left corner moves the source's top right corner.
    const Rect d = drag_corner(v, 0, -10, -20, t.view_w(), t.view_h(), false);
    CHECK(same(d, {0, 0, 110, 70}));
    CHECK(same(t.box_to_source(d), {210, 0, 110, 70}));
    CHECK(same(t.box_to_view(t.box_to_source(d)), d));

    t.fh = false;
    t.fv = true;
    t.q = 3;
    const Rect s = t.box_to_source(d);
    CHECK(inside(s, t.w, t.h));
    CHECK(same(t.box_to_view(s), d));
}

TEST_CASE("turn_crop follows a USB camera turn and flip") {
    // A 432 x 240 frame with a box in it; a clockwise turn of the whole frame moves the box with it.
    const Rect b0{76, 56, 225, 140};
    const Rect b90 = turn_crop(b0, 432, 240, {}, {90, false, false});
    CHECK(same(b90, {44, 76, 140, 225}));   // The turned frame is 240 x 432.
    CHECK(inside(b90, 240, 432));
    const Rect b180 = turn_crop(b90, 432, 240, {90, false, false}, {180, false, false});
    CHECK(same(b180, {131, 44, 225, 140}));
    const Rect b270 = turn_crop(b180, 432, 240, {180, false, false}, {270, false, false});
    CHECK(same(b270, {56, 131, 140, 225}));
    CHECK(same(turn_crop(b270, 432, 240, {270, false, false}, {}), b0));

    // A flip twice comes back, and a flip alone mirrors the box in the frame.
    const Rect fh = turn_crop(b0, 432, 240, {}, {0, true, false});
    CHECK(same(fh, {131, 56, 225, 140}));
    CHECK(same(turn_crop(fh, 432, 240, {0, true, false}, {}), b0));
    const Rect fv = turn_crop(b0, 432, 240, {}, {0, false, true});
    CHECK(same(fv, {76, 44, 225, 140}));
    CHECK(same(turn_crop(fv, 432, 240, {0, false, true}, {}), b0));

    // The same turn and flip together are one change, and no change leaves the box alone.
    CHECK(same(turn_crop(b0, 432, 240, {}, {90, true, false}), {44, 131, 140, 225}));
    CHECK(same(turn_crop(b0, 432, 240, {90, true, true}, {90, true, true}), b0));
}
