#include "ui_layout_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace codex_monitor::ui_layout {
namespace {

constexpr std::int64_t kInt32Minimum =
    static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
constexpr std::int64_t kInt32Maximum =
    static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());

bool IsPositiveSize(PixelSize size) {
    return size.width > 0 && size.height > 0;
}

std::optional<PixelSize> RectSize(PixelRect rect) {
    const std::int64_t width =
        static_cast<std::int64_t>(rect.right) - rect.left;
    const std::int64_t height =
        static_cast<std::int64_t>(rect.bottom) - rect.top;
    if (width <= 0 || height <= 0 || width > kInt32Maximum ||
        height > kInt32Maximum) {
        return std::nullopt;
    }
    return PixelSize{static_cast<std::int32_t>(width),
                     static_cast<std::int32_t>(height)};
}

bool IsFinitePositive(double value) {
    return std::isfinite(value) && value > 0.0;
}

std::optional<std::int32_t> RoundPositiveToInt32(long double value) {
    if (!std::isfinite(value) || value <= 0.0L ||
        value > static_cast<long double>(kInt32Maximum) + 0.499L) {
        return std::nullopt;
    }
    const long double rounded = std::floor(value + 0.5L);
    if (rounded < 1.0L || rounded > static_cast<long double>(kInt32Maximum)) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(rounded);
}

std::optional<PixelRect> CheckedRect(
    std::int64_t left,
    std::int64_t top,
    std::int64_t width,
    std::int64_t height) {
    if (width <= 0 || height <= 0 || width > kInt32Maximum ||
        height > kInt32Maximum) {
        return std::nullopt;
    }
    const std::int64_t right = left + width;
    const std::int64_t bottom = top + height;
    if (left < kInt32Minimum || left > kInt32Maximum ||
        top < kInt32Minimum || top > kInt32Maximum ||
        right < kInt32Minimum || right > kInt32Maximum ||
        bottom < kInt32Minimum || bottom > kInt32Maximum) {
        return std::nullopt;
    }
    return PixelRect{static_cast<std::int32_t>(left),
                     static_cast<std::int32_t>(top),
                     static_cast<std::int32_t>(right),
                     static_cast<std::int32_t>(bottom)};
}

std::int64_t CenteredOrigin(
    std::int32_t firstEdge,
    std::int32_t secondEdge,
    std::int32_t length) {
    // Keeping twice the center avoids both floating-point drift and overflow.
    return (static_cast<std::int64_t>(firstEdge) + secondEdge - length) / 2;
}

}  // namespace

std::optional<PixelSize> ScaleLogicalSizeForDpi(
    PixelSize logicalSize,
    std::uint32_t dpi) {
    if (!IsPositiveSize(logicalSize) || dpi == 0) return std::nullopt;

    const auto scaleDimension = [dpi](std::int32_t logical)
        -> std::optional<std::int32_t> {
        const std::uint64_t product =
            static_cast<std::uint64_t>(logical) * dpi;
        const std::uint64_t physical = (product + 48U) / 96U;
        if (physical == 0 || physical >
                static_cast<std::uint64_t>(kInt32Maximum)) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(physical);
    };

    const auto width = scaleDimension(logicalSize.width);
    const auto height = scaleDimension(logicalSize.height);
    if (!width || !height) return std::nullopt;
    return PixelSize{*width, *height};
}

std::optional<UniformScaleLimits> ComputeUniformScaleLimits(
    PixelSize baseFrameSize,
    PixelRect workArea,
    std::int32_t marginPixels,
    double minimumScale) {
    const auto workSize = RectSize(workArea);
    if (!IsPositiveSize(baseFrameSize) || !workSize || marginPixels < 0 ||
        !IsFinitePositive(minimumScale)) {
        return std::nullopt;
    }

    const std::int64_t doubleMargin =
        static_cast<std::int64_t>(marginPixels) * 2;
    const std::int64_t availableWidth =
        static_cast<std::int64_t>(workSize->width) - doubleMargin;
    const std::int64_t availableHeight =
        static_cast<std::int64_t>(workSize->height) - doubleMargin;
    if (availableWidth <= 0 || availableHeight <= 0) return std::nullopt;

    const double fitScale = std::min(
        static_cast<double>(availableWidth) / baseFrameSize.width,
        static_cast<double>(availableHeight) / baseFrameSize.height);
    if (!IsFinitePositive(fitScale)) return std::nullopt;

    UniformScaleLimits limits;
    limits.minimumScale = minimumScale;
    limits.minimumFitsWorkArea = fitScale >= minimumScale;
    limits.maximumScale = std::max(minimumScale, fitScale);
    return limits;
}

std::optional<double> ComputeUniformScaleForProposedFrame(
    PixelRect proposedFrame,
    PixelSize baseFrameSize,
    ResizeHandle handle,
    double currentScale,
    UniformScaleLimits limits) {
    const auto proposedSize = RectSize(proposedFrame);
    if (!proposedSize || !IsPositiveSize(baseFrameSize) ||
        !IsFinitePositive(currentScale) ||
        !IsFinitePositive(limits.minimumScale) ||
        !IsFinitePositive(limits.maximumScale) ||
        limits.maximumScale < limits.minimumScale) {
        return std::nullopt;
    }

    const double widthScale =
        static_cast<double>(proposedSize->width) / baseFrameSize.width;
    const double heightScale =
        static_cast<double>(proposedSize->height) / baseFrameSize.height;
    double requestedScale = currentScale;
    switch (handle) {
        case ResizeHandle::kLeft:
        case ResizeHandle::kRight:
            requestedScale = widthScale;
            break;
        case ResizeHandle::kTop:
        case ResizeHandle::kBottom:
            requestedScale = heightScale;
            break;
        case ResizeHandle::kTopLeft:
        case ResizeHandle::kTopRight:
        case ResizeHandle::kBottomLeft:
        case ResizeHandle::kBottomRight:
            requestedScale =
                std::fabs(widthScale - currentScale) >=
                        std::fabs(heightScale - currentScale)
                    ? widthScale
                    : heightScale;
            break;
        default:
            return std::nullopt;
    }
    if (!IsFinitePositive(requestedScale)) return std::nullopt;
    return std::clamp(
        requestedScale, limits.minimumScale, limits.maximumScale);
}

std::optional<PixelSize> FrameSizeForUniformScale(
    PixelSize baseFrameSize,
    double scale) {
    if (!IsPositiveSize(baseFrameSize) || !IsFinitePositive(scale)) {
        return std::nullopt;
    }
    const auto width = RoundPositiveToInt32(
        static_cast<long double>(baseFrameSize.width) * scale);
    const auto height = RoundPositiveToInt32(
        static_cast<long double>(baseFrameSize.height) * scale);
    if (!width || !height) return std::nullopt;
    return PixelSize{*width, *height};
}

std::optional<PixelRect> FrameForUniformScale(
    PixelRect originalFrame,
    PixelSize baseFrameSize,
    ResizeHandle handle,
    double scale) {
    if (!RectSize(originalFrame)) return std::nullopt;
    const auto size = FrameSizeForUniformScale(baseFrameSize, scale);
    if (!size) return std::nullopt;

    const std::int64_t width = size->width;
    const std::int64_t height = size->height;
    std::int64_t left = originalFrame.left;
    std::int64_t top = originalFrame.top;

    switch (handle) {
        case ResizeHandle::kTopLeft:
            left = static_cast<std::int64_t>(originalFrame.right) - width;
            top = static_cast<std::int64_t>(originalFrame.bottom) - height;
            break;
        case ResizeHandle::kTopRight:
            left = originalFrame.left;
            top = static_cast<std::int64_t>(originalFrame.bottom) - height;
            break;
        case ResizeHandle::kBottomLeft:
            left = static_cast<std::int64_t>(originalFrame.right) - width;
            top = originalFrame.top;
            break;
        case ResizeHandle::kBottomRight:
            left = originalFrame.left;
            top = originalFrame.top;
            break;
        case ResizeHandle::kLeft:
            left = static_cast<std::int64_t>(originalFrame.right) - width;
            top = CenteredOrigin(
                originalFrame.top, originalFrame.bottom, size->height);
            break;
        case ResizeHandle::kRight:
            left = originalFrame.left;
            top = CenteredOrigin(
                originalFrame.top, originalFrame.bottom, size->height);
            break;
        case ResizeHandle::kTop:
            left = CenteredOrigin(
                originalFrame.left, originalFrame.right, size->width);
            top = static_cast<std::int64_t>(originalFrame.bottom) - height;
            break;
        case ResizeHandle::kBottom:
            left = CenteredOrigin(
                originalFrame.left, originalFrame.right, size->width);
            top = originalFrame.top;
            break;
        default:
            return std::nullopt;
    }
    return CheckedRect(left, top, width, height);
}

std::optional<PixelRect> ConstrainUniformResize(
    PixelRect originalFrame,
    PixelRect proposedFrame,
    PixelSize baseFrameSize,
    ResizeHandle handle,
    double currentScale,
    UniformScaleLimits limits) {
    const auto scale = ComputeUniformScaleForProposedFrame(
        proposedFrame, baseFrameSize, handle, currentScale, limits);
    if (!scale) return std::nullopt;
    return FrameForUniformScale(originalFrame, baseFrameSize, handle, *scale);
}

std::optional<PixelRect> ClampFrameToWorkArea(
    PixelRect frame,
    PixelRect workArea) {
    const auto frameSize = RectSize(frame);
    const auto workSize = RectSize(workArea);
    if (!frameSize || !workSize) return std::nullopt;

    const auto clampOrigin = [](
        std::int32_t frameOrigin,
        std::int32_t frameLength,
        std::int32_t workOrigin,
        std::int32_t workLength) -> std::int64_t {
        if (frameLength >= workLength) return workOrigin;
        const std::int64_t maximumOrigin =
            static_cast<std::int64_t>(workOrigin) + workLength - frameLength;
        return std::clamp(
            static_cast<std::int64_t>(frameOrigin),
            static_cast<std::int64_t>(workOrigin), maximumOrigin);
    };

    const std::int64_t left = clampOrigin(
        frame.left, frameSize->width, workArea.left, workSize->width);
    const std::int64_t top = clampOrigin(
        frame.top, frameSize->height, workArea.top, workSize->height);
    return CheckedRect(left, top, frameSize->width, frameSize->height);
}

std::optional<PixelRect> PlaceFrameInWorkAreaCorner(
    PixelSize frameSize,
    PixelRect workArea,
    WorkAreaCorner corner,
    std::int32_t marginPixels) {
    const auto workSize = RectSize(workArea);
    if (!IsPositiveSize(frameSize) || !workSize || marginPixels < 0) {
        return std::nullopt;
    }

    const std::int64_t doubleMargin =
        static_cast<std::int64_t>(marginPixels) * 2;
    if (static_cast<std::int64_t>(frameSize.width) + doubleMargin >
            workSize->width ||
        static_cast<std::int64_t>(frameSize.height) + doubleMargin >
            workSize->height) {
        return std::nullopt;
    }

    const std::int64_t nearLeft =
        static_cast<std::int64_t>(workArea.left) + marginPixels;
    const std::int64_t nearTop =
        static_cast<std::int64_t>(workArea.top) + marginPixels;
    const std::int64_t farLeft =
        static_cast<std::int64_t>(workArea.right) - marginPixels -
        frameSize.width;
    const std::int64_t farTop =
        static_cast<std::int64_t>(workArea.bottom) - marginPixels -
        frameSize.height;

    std::int64_t left = nearLeft;
    std::int64_t top = nearTop;
    switch (corner) {
        case WorkAreaCorner::kTopLeft:
            break;
        case WorkAreaCorner::kTopRight:
            left = farLeft;
            break;
        case WorkAreaCorner::kBottomLeft:
            top = farTop;
            break;
        case WorkAreaCorner::kBottomRight:
            left = farLeft;
            top = farTop;
            break;
        default:
            return std::nullopt;
    }
    return CheckedRect(left, top, frameSize.width, frameSize.height);
}

std::optional<VariableHeightGridRows> BuildVariableHeightGridRows(
    const std::vector<std::int32_t>& cardHeights,
    std::int32_t columns,
    std::int32_t gapPixels) {
    if (columns <= 0 || gapPixels < 0) return std::nullopt;
    VariableHeightGridRows result;
    if (cardHeights.empty()) return result;

    std::int64_t offset = 0;
    for (std::size_t start = 0; start < cardHeights.size();
         start += static_cast<std::size_t>(columns)) {
        std::int32_t rowHeight = 0;
        const std::size_t end = std::min(
            cardHeights.size(),
            start + static_cast<std::size_t>(columns));
        for (std::size_t index = start; index < end; ++index) {
            if (cardHeights[index] <= 0) return std::nullopt;
            rowHeight = std::max(rowHeight, cardHeights[index]);
        }
        if (offset > kInt32Maximum) return std::nullopt;
        result.offsets.push_back(static_cast<std::int32_t>(offset));
        result.heights.push_back(rowHeight);
        offset += rowHeight;
        if (end < cardHeights.size()) offset += gapPixels;
        if (offset > kInt32Maximum) return std::nullopt;
    }
    result.totalHeight = static_cast<std::int32_t>(offset);
    return result;
}

std::int32_t ClampPixelScrollOffset(
    std::int32_t requestedOffset,
    std::int32_t contentHeight,
    std::int32_t viewportHeight) {
    if (contentHeight <= 0 || viewportHeight <= 0 ||
        contentHeight <= viewportHeight) {
        return 0;
    }
    const std::int32_t maximum = contentHeight - viewportHeight;
    return std::clamp(requestedOffset, 0, maximum);
}

}  // namespace codex_monitor::ui_layout
