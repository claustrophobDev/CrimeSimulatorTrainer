#include "Canvas.h"

#include <algorithm>
#include <cmath>

namespace {

inline float Clamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

inline std::uint32_t PackBgr(Color color) {
    const std::uint32_t r = (color >> 16) & 0xFF;
    const std::uint32_t g = (color >> 8) & 0xFF;
    const std::uint32_t b = color & 0xFF;
    return (r << 16) | (g << 8) | b;
}

// Знаковое расстояние до скруглённого прямоугольника. Внутри отрицательное.
inline float RoundBoxDistance(float px, float py, float halfW, float halfH, float radius) {
    const float qx = std::fabs(px) - halfW + radius;
    const float qy = std::fabs(py) - halfH + radius;
    const float ax = qx > 0.0f ? qx : 0.0f;
    const float ay = qy > 0.0f ? qy : 0.0f;
    const float outside = std::sqrt(ax * ax + ay * ay);
    const float inside = std::min(std::max(qx, qy), 0.0f);
    return outside + inside - radius;
}

void BoxBlurHorizontal(const std::vector<std::uint8_t>& src, std::vector<std::uint8_t>& dst,
                       int width, int height, int radius) {
    const int window = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = src.data() + static_cast<std::size_t>(y) * width;
        std::uint8_t* out = dst.data() + static_cast<std::size_t>(y) * width;
        int sum = 0;
        for (int x = -radius; x <= radius; ++x) sum += row[std::clamp(x, 0, width - 1)];
        for (int x = 0; x < width; ++x) {
            out[x] = static_cast<std::uint8_t>(sum / window);
            sum -= row[std::clamp(x - radius, 0, width - 1)];
            sum += row[std::clamp(x + radius + 1, 0, width - 1)];
        }
    }
}

void BoxBlurVertical(const std::vector<std::uint8_t>& src, std::vector<std::uint8_t>& dst,
                     int width, int height, int radius) {
    const int window = radius * 2 + 1;
    for (int x = 0; x < width; ++x) {
        int sum = 0;
        for (int y = -radius; y <= radius; ++y) {
            sum += src[static_cast<std::size_t>(std::clamp(y, 0, height - 1)) * width + x];
        }
        for (int y = 0; y < height; ++y) {
            dst[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(sum / window);
            sum -= src[static_cast<std::size_t>(std::clamp(y - radius, 0, height - 1)) * width + x];
            sum += src[static_cast<std::size_t>(std::clamp(y + radius + 1, 0, height - 1)) * width + x];
        }
    }
}

} // namespace

Color MixColor(Color from, Color to, float t) {
    t = Clamp01(t);
    // Каналы обязательно тащим в знаковый int. Если смешивать в сторону более
    // тёмного цвета, разница отрицательная, а в беззнаковой арифметике она
    // превращается в огромное число - я на этом уже один раз попался.
    const auto channel = [t](Color a, Color b, int shift) {
        const int from8 = static_cast<int>((a >> shift) & 0xFF);
        const int to8 = static_cast<int>((b >> shift) & 0xFF);
        return from8 + static_cast<int>(static_cast<float>(to8 - from8) * t);
    };
    return MakeColor(channel(from, to, 16), channel(from, to, 8), channel(from, to, 0));
}

float RoundRectCoverage(float px, float py, const RectF& rect, float radius) {
    const float halfW = rect.w * 0.5f;
    const float halfH = rect.h * 0.5f;
    const float limit = std::min(halfW, halfH);
    const float r = std::clamp(radius, 0.0f, limit);
    const float distance = RoundBoxDistance(px - (rect.x + halfW), py - (rect.y + halfH), halfW, halfH, r);
    return Clamp01(0.5f - distance);
}

Canvas::~Canvas() {
    Release();
}

void Canvas::Release() {
    if (dc_) {
        if (previousBitmap_) SelectObject(dc_, previousBitmap_);
        DeleteDC(dc_);
        dc_ = nullptr;
        previousBitmap_ = nullptr;
    }
    if (bitmap_) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    pixels_ = nullptr;
    width_ = 0;
    height_ = 0;
}

bool Canvas::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (width == width_ && height == height_) return true;

    Release();

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    bitmap_ = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap_ || !bits) {
        Release();
        return false;
    }

    dc_ = CreateCompatibleDC(nullptr);
    if (!dc_) {
        Release();
        return false;
    }

    previousBitmap_ = SelectObject(dc_, bitmap_);
    pixels_ = static_cast<std::uint32_t*>(bits);
    width_ = width;
    height_ = height;

    // Текст рисует GDI, фигуры пишу прямо в пиксели, и порядок важен. Без
    // batch limit = 1 GDI может отложить текст и нарисовать его уже поверх.
    GdiSetBatchLimit(1);
    SetBkMode(dc_, TRANSPARENT);
    return true;
}

void Canvas::Clear(Color color) {
    if (!pixels_) return;
    const std::uint32_t value = PackBgr(color);
    std::fill_n(pixels_, static_cast<std::size_t>(width_) * height_, value);
}

void Canvas::BlendPixel(int x, int y, Color color, float coverage) {
    if (coverage <= 0.0f || x < 0 || y < 0 || x >= width_ || y >= height_) return;

    std::uint32_t& target = pixels_[static_cast<std::size_t>(y) * width_ + x];
    if (coverage >= 1.0f) {
        target = PackBgr(color);
        return;
    }

    const int sr = (color >> 16) & 0xFF;
    const int sg = (color >> 8) & 0xFF;
    const int sb = color & 0xFF;
    const int dr = (target >> 16) & 0xFF;
    const int dg = (target >> 8) & 0xFF;
    const int db = target & 0xFF;

    const int a = static_cast<int>(coverage * 255.0f + 0.5f);
    const int r = dr + ((sr - dr) * a) / 255;
    const int g = dg + ((sg - dg) * a) / 255;
    const int b = db + ((sb - db) * a) / 255;
    target = (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) |
             static_cast<std::uint32_t>(b);
}

void Canvas::BlendSpan(int y, float x0, float x1, Color color, float alpha) {
    if (!pixels_ || y < 0 || y >= height_ || x1 <= x0 || alpha <= 0.0f) return;

    x0 = std::max(x0, 0.0f);
    x1 = std::min(x1, static_cast<float>(width_));
    if (x1 <= x0) return;

    const int first = static_cast<int>(std::floor(x0));
    const int last = static_cast<int>(std::ceil(x1)) - 1;

    if (alpha >= 1.0f) {
        // Непрозрачную середину просто заливаем, смешивать надо только края.
        const int solidFirst = static_cast<int>(std::ceil(x0));
        const int solidLast = static_cast<int>(std::floor(x1)) - 1;
        if (solidLast >= solidFirst) {
            const std::uint32_t value = PackBgr(color);
            std::uint32_t* row = pixels_ + static_cast<std::size_t>(y) * width_;
            std::fill(row + solidFirst, row + solidLast + 1, value);
            BlendPixel(first, y, color, static_cast<float>(solidFirst) - x0);
            BlendPixel(last, y, color, x1 - static_cast<float>(solidLast + 1));
            return;
        }
    }

    for (int x = first; x <= last; ++x) {
        const float left = std::max(static_cast<float>(x), x0);
        const float right = std::min(static_cast<float>(x + 1), x1);
        BlendPixel(x, y, color, (right - left) * alpha);
    }
}

void Canvas::FillRect(const RectF& rect, Color color, float alpha) {
    const int top = std::max(0, static_cast<int>(std::floor(rect.y)));
    const int bottom = std::min(height_ - 1, static_cast<int>(std::ceil(rect.Bottom())) - 1);
    for (int y = top; y <= bottom; ++y) {
        const float coverY = std::min(static_cast<float>(y + 1), rect.Bottom()) -
                             std::max(static_cast<float>(y), rect.y);
        if (coverY <= 0.0f) continue;
        BlendSpan(y, rect.x, rect.Right(), color, alpha * Clamp01(coverY));
    }
}

void Canvas::FillRoundRect(const RectF& rect, float radius, Color color, float alpha) {
    if (!pixels_ || rect.w <= 0.0f || rect.h <= 0.0f || alpha <= 0.0f) return;

    const float halfW = rect.w * 0.5f;
    const float halfH = rect.h * 0.5f;
    const float r = std::clamp(radius, 0.0f, std::min(halfW, halfH));
    const float cx = rect.x + halfW;
    const float cy = rect.y + halfH;

    const int top = std::max(0, static_cast<int>(std::floor(rect.y - 1.0f)));
    const int bottom = std::min(height_ - 1, static_cast<int>(std::ceil(rect.Bottom() + 1.0f)));
    const int left = std::max(0, static_cast<int>(std::floor(rect.x - 1.0f)));
    const int right = std::min(width_ - 1, static_cast<int>(std::ceil(rect.Right() + 1.0f)));

    for (int y = top; y <= bottom; ++y) {
        const float py = static_cast<float>(y) + 0.5f - cy;
        if (std::fabs(py) <= halfH - r) {
            // Прямой участок - обычная строка пикселей, расстояние тут не нужно.
            BlendSpan(y, rect.x, rect.Right(), color, alpha);
            continue;
        }
        for (int x = left; x <= right; ++x) {
            const float px = static_cast<float>(x) + 0.5f - cx;
            const float coverage = Clamp01(0.5f - RoundBoxDistance(px, py, halfW, halfH, r));
            BlendPixel(x, y, color, coverage * alpha);
        }
    }
}

void Canvas::StrokeRoundRect(const RectF& rect, float radius, float thickness, Color color,
                             float alpha) {
    if (!pixels_ || rect.w <= 0.0f || rect.h <= 0.0f || thickness <= 0.0f) return;

    const float halfW = rect.w * 0.5f;
    const float halfH = rect.h * 0.5f;
    const float r = std::clamp(radius, 0.0f, std::min(halfW, halfH));
    const float cx = rect.x + halfW;
    const float cy = rect.y + halfH;
    const float half = thickness * 0.5f;

    const int top = std::max(0, static_cast<int>(std::floor(rect.y - half - 1.0f)));
    const int bottom = std::min(height_ - 1, static_cast<int>(std::ceil(rect.Bottom() + half + 1.0f)));
    const int left = std::max(0, static_cast<int>(std::floor(rect.x - half - 1.0f)));
    const int right = std::min(width_ - 1, static_cast<int>(std::ceil(rect.Right() + half + 1.0f)));

    for (int y = top; y <= bottom; ++y) {
        const float py = static_cast<float>(y) + 0.5f - cy;
        if (std::fabs(py) <= halfH - r) {
            BlendSpan(y, rect.x - half, rect.x + half, color, alpha);
            BlendSpan(y, rect.Right() - half, rect.Right() + half, color, alpha);
            continue;
        }
        for (int x = left; x <= right; ++x) {
            const float px = static_cast<float>(x) + 0.5f - cx;
            const float distance = std::fabs(RoundBoxDistance(px, py, halfW, halfH, r));
            BlendPixel(x, y, color, Clamp01(0.5f - (distance - half)) * alpha);
        }
    }
}

void Canvas::FillCircle(float cx, float cy, float radius, Color color, float alpha) {
    if (!pixels_ || radius <= 0.0f || alpha <= 0.0f) return;

    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 1.0f)));
    const int bottom = std::min(height_ - 1, static_cast<int>(std::ceil(cy + radius + 1.0f)));
    const int left = std::max(0, static_cast<int>(std::floor(cx - radius - 1.0f)));
    const int right = std::min(width_ - 1, static_cast<int>(std::ceil(cx + radius + 1.0f)));

    for (int y = top; y <= bottom; ++y) {
        const float dy = static_cast<float>(y) + 0.5f - cy;
        for (int x = left; x <= right; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float distance = std::sqrt(dx * dx + dy * dy) - radius;
            BlendPixel(x, y, color, Clamp01(0.5f - distance) * alpha);
        }
    }
}

void Canvas::FillRoundRectGradient(const RectF& rect, float radius, Color top, Color bottom,
                                   float gradientHeight) {
    if (!pixels_ || rect.w <= 0.0f || rect.h <= 0.0f) return;

    const float halfW = rect.w * 0.5f;
    const float halfH = rect.h * 0.5f;
    const float r = std::clamp(radius, 0.0f, std::min(halfW, halfH));
    const float cx = rect.x + halfW;
    const float cy = rect.y + halfH;
    const float span = std::max(gradientHeight, 1.0f);

    const int first = std::max(0, static_cast<int>(std::floor(rect.y - 1.0f)));
    const int last = std::min(height_ - 1, static_cast<int>(std::ceil(rect.Bottom() + 1.0f)));
    const int left = std::max(0, static_cast<int>(std::floor(rect.x - 1.0f)));
    const int right = std::min(width_ - 1, static_cast<int>(std::ceil(rect.Right() + 1.0f)));

    for (int y = first; y <= last; ++y) {
        const float py = static_cast<float>(y) + 0.5f - cy;
        const Color color =
            MixColor(top, bottom, (static_cast<float>(y) + 0.5f - rect.y) / span);

        if (std::fabs(py) <= halfH - r) {
            BlendSpan(y, rect.x, rect.Right(), color, 1.0f);
            continue;
        }
        for (int x = left; x <= right; ++x) {
            const float px = static_cast<float>(x) + 0.5f - cx;
            BlendPixel(x, y, color, Clamp01(0.5f - RoundBoxDistance(px, py, halfW, halfH, r)));
        }
    }
}

void Canvas::StrokeLine(float x0, float y0, float x1, float y1, float thickness, Color color,
                        float alpha) {
    if (!pixels_ || thickness <= 0.0f || alpha <= 0.0f) return;

    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float lengthSq = dx * dx + dy * dy;
    if (lengthSq <= 0.0f) return;

    const float half = thickness * 0.5f;
    const float pad = half + 1.5f;
    const int top = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - pad)));
    const int bottom = std::min(height_ - 1, static_cast<int>(std::ceil(std::max(y0, y1) + pad)));
    const int left = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - pad)));
    const int right = std::min(width_ - 1, static_cast<int>(std::ceil(std::max(x0, x1) + pad)));

    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const float px = static_cast<float>(x) + 0.5f - x0;
            const float py = static_cast<float>(y) + 0.5f - y0;
            // Расстояние до отрезка, концы получаются скруглёнными.
            const float t = Clamp01((px * dx + py * dy) / lengthSq);
            const float ox = px - dx * t;
            const float oy = py - dy * t;
            const float distance = std::sqrt(ox * ox + oy * oy) - half;
            BlendPixel(x, y, color, Clamp01(0.5f - distance) * alpha);
        }
    }
}

void Canvas::ApplyAlphaMask(const std::vector<std::uint8_t>& mask) {
    if (!pixels_) return;
    const std::size_t count = static_cast<std::size_t>(width_) * height_;
    if (mask.size() < count) return;

    // GdiFlush обязателен: иначе можем прочитать пиксели раньше, чем GDI
    // допишет туда текст.
    GdiFlush();

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t alpha = mask[i];
        std::uint32_t& pixel = pixels_[i];
        if (alpha == 255) {
            pixel |= 0xFF000000u;
            continue;
        }
        if (alpha == 0) {
            pixel = 0;
            continue;
        }
        const std::uint32_t r = (((pixel >> 16) & 0xFF) * alpha) / 255;
        const std::uint32_t g = (((pixel >> 8) & 0xFF) * alpha) / 255;
        const std::uint32_t b = ((pixel & 0xFF) * alpha) / 255;
        pixel = (alpha << 24) | (r << 16) | (g << 8) | b;
    }
}

std::vector<std::uint8_t> BuildWindowMask(int width, int height, const RectF& panel, float radius,
                                          float shadowRadius, float shadowOffsetY,
                                          float shadowStrength) {
    const std::size_t count = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> panelMask(count, 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float coverage = RoundRectCoverage(static_cast<float>(x) + 0.5f,
                                                     static_cast<float>(y) + 0.5f, panel, radius);
            panelMask[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint8_t>(coverage * 255.0f + 0.5f);
        }
    }

    if (shadowRadius < 1.0f || shadowStrength <= 0.0f) return panelMask;

    // Тень - это силуэт панели, сдвинутый вниз и размытый. Три box blur'а
    // дают почти гаусс, полос не видно.
    std::vector<std::uint8_t> shadow(count, 0);
    const int offset = static_cast<int>(shadowOffsetY + 0.5f);
    for (int y = 0; y < height; ++y) {
        const int source = std::clamp(y - offset, 0, height - 1);
        std::copy_n(panelMask.begin() + static_cast<std::size_t>(source) * width, width,
                    shadow.begin() + static_cast<std::size_t>(y) * width);
    }

    std::vector<std::uint8_t> scratch(count, 0);
    const int blur = std::max(1, static_cast<int>(shadowRadius / 3.0f));
    for (int pass = 0; pass < 3; ++pass) {
        BoxBlurHorizontal(shadow, scratch, width, height, blur);
        BoxBlurVertical(scratch, shadow, width, height, blur);
    }

    for (std::size_t i = 0; i < count; ++i) {
        const auto shaded = static_cast<std::uint8_t>(shadow[i] * shadowStrength);
        panelMask[i] = std::max(panelMask[i], shaded);
    }
    return panelMask;
}
