#include "ui_layout_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

namespace layout = codex_monitor::ui_layout;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void ExpectNear(double actual, double expected, const char* message) {
    Expect(std::fabs(actual - expected) < 0.000001, message);
}

void ExpectSize(
    const layout::PixelSize& actual,
    std::int32_t width,
    std::int32_t height,
    const char* message) {
    Expect(actual.width == width && actual.height == height, message);
}

void ExpectRect(
    const layout::PixelRect& actual,
    std::int32_t left,
    std::int32_t top,
    std::int32_t right,
    std::int32_t bottom,
    const char* message) {
    Expect(actual.left == left && actual.top == top &&
               actual.right == right && actual.bottom == bottom,
           message);
}

void ExpectAspectWithinOnePixel(
    const layout::PixelRect& frame,
    layout::PixelSize base,
    const char* message) {
    const std::int64_t width =
        static_cast<std::int64_t>(frame.right) - frame.left;
    const std::int64_t height =
        static_cast<std::int64_t>(frame.bottom) - frame.top;
    const std::int64_t crossDifference =
        std::llabs(width * base.height - height * base.width);
    Expect(crossDifference <=
               std::max<std::int64_t>(base.width, base.height),
           message);
}

void TestMinimumAndDynamicMaximumScale() {
    const auto limits = layout::ComputeUniformScaleLimits(
        {400, 600}, {0, 0, 1920, 1040}, 20);
    Expect(limits.has_value(), "a valid work area must produce scale limits");
    ExpectNear(limits->minimumScale, 0.75,
               "the minimum drag scale must remain 75 percent");
    ExpectNear(limits->maximumScale, 1000.0 / 600.0,
               "the maximum must follow the limiting work-area axis");
    Expect(limits->minimumFitsWorkArea,
           "a normal work area must fit the 75 percent minimum");

    const auto wideLimits = layout::ComputeUniformScaleLimits(
        {400, 200}, {-1920, 0, 0, 1040}, 20);
    Expect(wideLimits.has_value(), "negative monitor coordinates must be valid");
    ExpectNear(wideLimits->maximumScale, 1880.0 / 400.0,
               "a wide layout must use work-area width as its maximum");

    const auto tinyLimits = layout::ComputeUniformScaleLimits(
        {400, 600}, {0, 0, 200, 200}, 0);
    Expect(tinyLimits.has_value(), "a tiny but valid work area must be explicit");
    ExpectNear(tinyLimits->minimumScale, 0.75,
               "a tiny monitor must not silently lower the 75 percent minimum");
    ExpectNear(tinyLimits->maximumScale, 0.75,
               "contradictory scale limits must collapse safely to the minimum");
    Expect(!tinyLimits->minimumFitsWorkArea,
           "callers must be told when even the minimum cannot fully fit");

    Expect(!layout::ComputeUniformScaleLimits(
               {400, 600}, {0, 0, 1920, 1040}, -1),
           "negative margins must be rejected");
    Expect(!layout::ComputeUniformScaleLimits(
               {400, 600}, {0, 0, 30, 30}, 20),
           "a margin that consumes the work area must be rejected");
    Expect(!layout::ComputeUniformScaleLimits(
               {0, 600}, {0, 0, 1920, 1040}, 20),
           "zero base dimensions must be rejected");
}

void TestDpiAndRoundedUniformSizes() {
    const auto oneHundred = layout::ScaleLogicalSizeForDpi({430, 680}, 96);
    Expect(oneHundred.has_value(), "96 DPI must be accepted");
    ExpectSize(*oneHundred, 430, 680, "96 DPI must preserve logical pixels");

    const auto oneFifty = layout::ScaleLogicalSizeForDpi({430, 680}, 144);
    Expect(oneFifty.has_value(), "150 percent DPI must be accepted");
    ExpectSize(*oneFifty, 645, 1020,
               "DPI conversion must use integer nearest-pixel rounding");

    const auto oneTwentyFive = layout::ScaleLogicalSizeForDpi({1, 3}, 120);
    Expect(oneTwentyFive.has_value(), "fractional DPI scaling must be accepted");
    ExpectSize(*oneTwentyFive, 1, 4,
               "DPI conversion must round half pixels predictably");

    const auto minimum = layout::FrameSizeForUniformScale({430, 680}, 0.75);
    Expect(minimum.has_value(), "the 75 percent size must be representable");
    ExpectSize(*minimum, 323, 510,
               "uniform sizing must round both axes to whole pixels");

    Expect(!layout::ScaleLogicalSizeForDpi({430, 680}, 0),
           "zero DPI must be rejected");
    Expect(!layout::ScaleLogicalSizeForDpi(
               {std::numeric_limits<std::int32_t>::max(), 1},
               std::numeric_limits<std::uint32_t>::max()),
           "DPI multiplication must reject signed-coordinate overflow");
    Expect(!layout::FrameSizeForUniformScale(
               {std::numeric_limits<std::int32_t>::max(), 1}, 2.0),
           "uniform scale multiplication must reject overflow");
    Expect(!layout::FrameSizeForUniformScale(
               {400, 600}, std::numeric_limits<double>::infinity()),
           "non-finite scale values must be rejected");
}

void TestScaleSelectionFromEveryDragAxis() {
    const layout::UniformScaleLimits limits{0.75, 1.5, true};
    const layout::PixelSize base{400, 600};

    const auto left = layout::ComputeUniformScaleForProposedFrame(
        {-20, 200, 500, 800}, base, layout::ResizeHandle::kLeft, 1.0, limits);
    Expect(left.has_value(), "a left-edge proposal must produce a scale");
    ExpectNear(*left, 1.3, "left and right edges must follow width");

    const auto top = layout::ComputeUniformScaleForProposedFrame(
        {100, 80, 500, 800}, base, layout::ResizeHandle::kTop, 1.0, limits);
    Expect(top.has_value(), "a top-edge proposal must produce a scale");
    ExpectNear(*top, 1.2, "top and bottom edges must follow height");

    const auto heightDominantCorner =
        layout::ComputeUniformScaleForProposedFrame(
            {100, 20, 540, 800}, base,
            layout::ResizeHandle::kTopRight, 1.0, limits);
    Expect(heightDominantCorner.has_value(),
           "a corner proposal must produce a scale");
    ExpectNear(*heightDominantCorner, 1.3,
               "a corner drag must follow the axis changed farther from current scale");

    const auto widthDominantCorner =
        layout::ComputeUniformScaleForProposedFrame(
            {-20, 140, 500, 800}, base,
            layout::ResizeHandle::kBottomLeft, 1.0, limits);
    Expect(widthDominantCorner.has_value(),
           "a width-dominant corner proposal must produce a scale");
    ExpectNear(*widthDominantCorner, 1.3,
               "corner width movement must remain directly controllable");

    const auto minimum = layout::ComputeUniformScaleForProposedFrame(
        {100, 200, 180, 320}, base, layout::ResizeHandle::kBottomRight,
        1.0, limits);
    Expect(minimum.has_value(), "a small proposal must still be constrained");
    ExpectNear(*minimum, 0.75, "dragging smaller must stop at 75 percent");

    const auto maximum = layout::ComputeUniformScaleForProposedFrame(
        {100, 200, 1700, 2600}, base, layout::ResizeHandle::kBottomRight,
        1.0, limits);
    Expect(maximum.has_value(), "a large proposal must still be constrained");
    ExpectNear(*maximum, 1.5,
               "dragging larger must stop at the dynamic work-area maximum");

    Expect(!layout::ComputeUniformScaleForProposedFrame(
               {100, 200, 100, 800}, base, layout::ResizeHandle::kRight,
               1.0, limits),
           "a zero-width proposed rectangle must be rejected");
    Expect(!layout::ComputeUniformScaleForProposedFrame(
               {100, 200, 500, 800}, base, layout::ResizeHandle::kRight,
               0.0, limits),
           "an invalid current scale must be rejected");
}

void TestFixedCornerAndOppositeEdgeAnchors() {
    const layout::PixelRect original{100, 200, 500, 800};
    const layout::PixelSize base{400, 600};

    const auto topLeft = layout::FrameForUniformScale(
        original, base, layout::ResizeHandle::kTopLeft, 1.25);
    Expect(topLeft.has_value(), "top-left resizing must succeed");
    ExpectRect(*topLeft, 0, 50, 500, 800,
               "top-left resizing must keep the opposite corner fixed");

    const auto topRight = layout::FrameForUniformScale(
        original, base, layout::ResizeHandle::kTopRight, 1.25);
    Expect(topRight.has_value(), "top-right resizing must succeed");
    ExpectRect(*topRight, 100, 50, 600, 800,
               "top-right resizing must keep the opposite corner fixed");

    const auto bottomLeft = layout::FrameForUniformScale(
        original, base, layout::ResizeHandle::kBottomLeft, 1.25);
    Expect(bottomLeft.has_value(), "bottom-left resizing must succeed");
    ExpectRect(*bottomLeft, 0, 200, 500, 950,
               "bottom-left resizing must keep the opposite corner fixed");

    const auto bottomRight = layout::FrameForUniformScale(
        original, base, layout::ResizeHandle::kBottomRight, 1.25);
    Expect(bottomRight.has_value(), "bottom-right resizing must succeed");
    ExpectRect(*bottomRight, 100, 200, 600, 950,
               "bottom-right resizing must keep the opposite corner fixed");

    const auto left = layout::FrameForUniformScale(
        original, base, layout::ResizeHandle::kLeft, 1.25);
    Expect(left.has_value(), "left-edge resizing must succeed");
    ExpectRect(*left, 0, 125, 500, 875,
               "left resizing must fix the right edge and vertical center");

    const auto right = layout::FrameForUniformScale(
        original, base, layout::ResizeHandle::kRight, 1.25);
    Expect(right.has_value(), "right-edge resizing must succeed");
    ExpectRect(*right, 100, 125, 600, 875,
               "right resizing must fix the left edge and vertical center");

    const auto top = layout::FrameForUniformScale(
        original, base, layout::ResizeHandle::kTop, 1.25);
    Expect(top.has_value(), "top-edge resizing must succeed");
    ExpectRect(*top, 50, 50, 550, 800,
               "top resizing must fix the bottom edge and horizontal center");

    const auto bottom = layout::FrameForUniformScale(
        original, base, layout::ResizeHandle::kBottom, 1.25);
    Expect(bottom.has_value(), "bottom-edge resizing must succeed");
    ExpectRect(*bottom, 50, 200, 550, 950,
               "bottom resizing must fix the top edge and horizontal center");

    ExpectAspectWithinOnePixel(*left, base,
                               "side resizing must preserve the base aspect ratio");
    ExpectAspectWithinOnePixel(*bottomRight, base,
                               "corner resizing must preserve the base aspect ratio");
}

void TestConstrainedResizeComposition() {
    const layout::PixelRect original{100, 200, 500, 800};
    const layout::UniformScaleLimits limits{0.75, 1.5, true};
    const auto constrained = layout::ConstrainUniformResize(
        original, {-20, 140, 500, 800}, {400, 600},
        layout::ResizeHandle::kTopLeft, 1.0, limits);
    Expect(constrained.has_value(), "the resize composition must succeed");
    ExpectRect(*constrained, -20, 20, 500, 800,
               "the composition must derive one scale and retain the opposite corner");
}

void TestFourCornerPlacementAndWorkAreaClamping() {
    const layout::PixelRect work{-1920, 0, 0, 1040};
    const layout::PixelSize size{400, 600};

    const auto topLeft = layout::PlaceFrameInWorkAreaCorner(
        size, work, layout::WorkAreaCorner::kTopLeft, 24);
    const auto topRight = layout::PlaceFrameInWorkAreaCorner(
        size, work, layout::WorkAreaCorner::kTopRight, 24);
    const auto bottomLeft = layout::PlaceFrameInWorkAreaCorner(
        size, work, layout::WorkAreaCorner::kBottomLeft, 24);
    const auto bottomRight = layout::PlaceFrameInWorkAreaCorner(
        size, work, layout::WorkAreaCorner::kBottomRight, 24);
    Expect(topLeft && topRight && bottomLeft && bottomRight,
           "all four work-area corners must be placeable");
    ExpectRect(*topLeft, -1896, 24, -1496, 624,
               "top-left placement must respect the edge margin");
    ExpectRect(*topRight, -424, 24, -24, 624,
               "top-right placement must respect the edge margin");
    ExpectRect(*bottomLeft, -1896, 416, -1496, 1016,
               "bottom-left placement must respect the edge margin");
    ExpectRect(*bottomRight, -424, 416, -24, 1016,
               "bottom-right placement must respect the edge margin");

    const auto clampedTop = layout::ClampFrameToWorkArea(
        {100, -50, 500, 550}, {0, 0, 1000, 800});
    Expect(clampedTop.has_value(), "an offscreen frame must be clampable");
    ExpectRect(*clampedTop, 100, 0, 500, 600,
               "clamping one axis must not disturb the other axis");

    const auto clampedBottomRight = layout::ClampFrameToWorkArea(
        {900, 500, 1300, 1100}, {0, 0, 1000, 800});
    Expect(clampedBottomRight.has_value(),
           "a frame past two work-area edges must be clampable");
    ExpectRect(*clampedBottomRight, 600, 200, 1000, 800,
               "clamping must preserve size while bringing the frame onscreen");

    const auto oversized = layout::ClampFrameToWorkArea(
        {-500, -500, 1500, 1500}, {0, 0, 1000, 800});
    Expect(oversized.has_value(), "an oversized frame must degrade predictably");
    ExpectRect(*oversized, 0, 0, 2000, 2000,
               "oversized clamping must keep aspect and align to work origin");

    Expect(!layout::PlaceFrameInWorkAreaCorner(
               {980, 780}, {0, 0, 1000, 800},
               layout::WorkAreaCorner::kBottomRight, 20),
           "corner placement must reject a frame that does not fit its margins");
}

void TestCoordinateOverflowIsRejected() {
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
    const layout::PixelSize base{400, 600};

    const auto safeTopLeft = layout::FrameForUniformScale(
        {maximum - 400, maximum - 600, maximum, maximum}, base,
        layout::ResizeHandle::kTopLeft, 1.25);
    Expect(safeTopLeft.has_value(),
           "subtracting from a fixed maximum corner must remain safe");
    Expect(safeTopLeft->right == maximum && safeTopLeft->bottom == maximum,
           "the fixed maximum corner must remain exact");

    Expect(!layout::FrameForUniformScale(
               {maximum - 400, maximum - 600, maximum, maximum}, base,
               layout::ResizeHandle::kBottomRight, 1.25),
           "adding a scaled size past signed coordinates must be rejected");
    Expect(!layout::FrameForUniformScale(
               {minimum, minimum, minimum + 400, minimum + 600}, base,
               layout::ResizeHandle::kTopLeft, 1.25),
           "subtracting a scaled size past signed coordinates must be rejected");

    Expect(!layout::ClampFrameToWorkArea(
               {minimum, 0, maximum, 100}, {0, 0, 1000, 800}),
           "a RECT whose mathematical width exceeds signed range must be rejected");
}

}  // namespace

int main() {
    TestMinimumAndDynamicMaximumScale();
    TestDpiAndRoundedUniformSizes();
    TestScaleSelectionFromEveryDragAxis();
    TestFixedCornerAndOppositeEdgeAnchors();
    TestConstrainedResizeComposition();
    TestFourCornerPlacementAndWorkAreaClamping();
    TestCoordinateOverflowIsRejected();
    std::cout << "windows_ui_layout_math_tests=pass\n";
    return 0;
}
