#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace codex_monitor::ui_layout {

// The HUD may shrink to 75% of its current page's 100%-scale frame size.
constexpr double kMinimumUniformScale = 0.75;

struct PixelSize {
    std::int32_t width = 0;
    std::int32_t height = 0;
};

// Uses Win32 RECT semantics: right and bottom are exclusive edges.
struct PixelRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

enum class ResizeHandle {
    kLeft,
    kRight,
    kTop,
    kBottom,
    kTopLeft,
    kTopRight,
    kBottomLeft,
    kBottomRight,
};

enum class WorkAreaCorner {
    kTopLeft,
    kTopRight,
    kBottomLeft,
    kBottomRight,
};

struct UniformScaleLimits {
    double minimumScale = kMinimumUniformScale;
    double maximumScale = kMinimumUniformScale;
    // False only when the usable work area is smaller than the 75% minimum.
    // The minimum remains 75%; callers can then keep the frame visible by
    // clamping its origin rather than silently shrinking the HUD further.
    bool minimumFitsWorkArea = false;
};

struct VariableHeightGridRows {
    std::vector<std::int32_t> offsets;
    std::vector<std::int32_t> heights;
    std::int32_t totalHeight = 0;
};

// Converts logical pixels at 96 DPI to physical pixels using integer rounding.
// Invalid dimensions, zero DPI, and results outside signed 32-bit coordinates
// return nullopt instead of wrapping.
std::optional<PixelSize> ScaleLogicalSizeForDpi(
    PixelSize logicalSize,
    std::uint32_t dpi);

// Computes the largest uniform scale that fits inside the monitor work area
// after applying the same non-negative margin on all four sides.
std::optional<UniformScaleLimits> ComputeUniformScaleLimits(
    PixelSize baseFrameSize,
    PixelRect workArea,
    std::int32_t marginPixels,
    double minimumScale = kMinimumUniformScale);

// Chooses one scale from the proposed rectangle. A side drag follows that
// side's axis. A corner drag follows whichever axis changed farther from the
// current scale, so horizontal and vertical mouse movement both feel direct.
std::optional<double> ComputeUniformScaleForProposedFrame(
    PixelRect proposedFrame,
    PixelSize baseFrameSize,
    ResizeHandle handle,
    double currentScale,
    UniformScaleLimits limits);

std::optional<PixelSize> FrameSizeForUniformScale(
    PixelSize baseFrameSize,
    double scale);

// Builds an aspect-preserving rectangle around the original fixed anchor.
// Corner drags keep the opposite corner fixed. Side drags keep the opposite
// edge and the original orthogonal center fixed (to the nearest half pixel).
std::optional<PixelRect> FrameForUniformScale(
    PixelRect originalFrame,
    PixelSize baseFrameSize,
    ResizeHandle handle,
    double scale);

// Convenience operation for a WM_SIZING adapter: derive a bounded scale from
// the proposed RECT and rebuild it around the correct fixed anchor.
std::optional<PixelRect> ConstrainUniformResize(
    PixelRect originalFrame,
    PixelRect proposedFrame,
    PixelSize baseFrameSize,
    ResizeHandle handle,
    double currentScale,
    UniformScaleLimits limits);

// Moves a valid frame into the work area without changing its size or aspect
// ratio. If a frame is larger than the work area, its corresponding origin is
// aligned to the work-area origin; normal callers avoid this through the scale
// limits above.
std::optional<PixelRect> ClampFrameToWorkArea(
    PixelRect frame,
    PixelRect workArea);

// Places a fitting frame at any work-area corner with a uniform edge margin.
std::optional<PixelRect> PlaceFrameInWorkAreaCorner(
    PixelSize frameSize,
    PixelRect workArea,
    WorkAreaCorner corner,
    std::int32_t marginPixels);

// Groups positive card heights into rows. Each row uses the tallest card in
// that row, and offsets are measured from the top of the scrollable content.
// Checked arithmetic prevents malformed text measurements from wrapping.
std::optional<VariableHeightGridRows> BuildVariableHeightGridRows(
    const std::vector<std::int32_t>& cardHeights,
    std::int32_t columns,
    std::int32_t gapPixels);

// Clamps a pixel scroll position against variable-height content. Invalid or
// empty dimensions safely collapse to the top.
std::int32_t ClampPixelScrollOffset(
    std::int32_t requestedOffset,
    std::int32_t contentHeight,
    std::int32_t viewportHeight);

}  // namespace codex_monitor::ui_layout
