#pragma once

#include <Windows.h>
#include <cstdint>
#include <vector>

// 0xRRGGBB. Внутри всё равно BGRA, но так читать код проще.
using Color = std::uint32_t;

constexpr Color MakeColor(int r, int g, int b) {
    return (static_cast<Color>(r) << 16) | (static_cast<Color>(g) << 8) | static_cast<Color>(b);
}

constexpr COLORREF ToColorRef(Color color) {
    return RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
}

Color MixColor(Color from, Color to, float t);

struct RectF {
    float x{};
    float y{};
    float w{};
    float h{};

    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    bool Contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    RectF Inset(float amount) const {
        return RectF{ x + amount, y + amount, w - amount * 2.0f, h - amount * 2.0f };
    }
};

// 32-битный top-down DIB. Текст в него пишет GDI, фигуры рисую сам.
// Сглаживание считаю через знаковое расстояние до скруглённого прямоугольника -
// GDI'шный RoundRect так не умеет, из-за чего у старого окна были рваные углы.
class Canvas final {
public:
    Canvas() = default;
    ~Canvas();

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    bool Resize(int width, int height);
    int Width() const noexcept { return width_; }
    int Height() const noexcept { return height_; }
    HDC Dc() const noexcept { return dc_; }

    void Clear(Color color);
    void FillRect(const RectF& rect, Color color, float alpha = 1.0f);
    void FillRoundRect(const RectF& rect, float radius, Color color, float alpha = 1.0f);
    void StrokeRoundRect(const RectF& rect, float radius, float thickness, Color color,
                         float alpha = 1.0f);
    void FillCircle(float cx, float cy, float radius, Color color, float alpha = 1.0f);

    // Скруглённый прямоугольник с градиентом по первым gradientHeight пикселям.
    // Обычная прямоугольная полоска оставляла бы шов на верхних углах.
    void FillRoundRectGradient(const RectF& rect, float radius, Color top, Color bottom,
                               float gradientHeight);

    void StrokeLine(float x0, float y0, float x1, float y1, float thickness, Color color,
                    float alpha = 1.0f);

    // Ставит альфу из маски и домножает цвет. Строго последним шагом перед выводом.
    void ApplyAlphaMask(const std::vector<std::uint8_t>& mask);

private:
    void BlendPixel(int x, int y, Color color, float coverage);
    void BlendSpan(int y, float x0, float x1, Color color, float alpha);
    void Release();

    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    std::uint32_t* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

// Покрытие скруглённого прямоугольника в точке, 0..1.
float RoundRectCoverage(float px, float py, const RectF& rect, float radius);

// Маска окна: сама панель плюс мягкая тень вокруг. Считается один раз на размер.
std::vector<std::uint8_t> BuildWindowMask(int width, int height, const RectF& panel, float radius,
                                          float shadowRadius, float shadowOffsetY,
                                          float shadowStrength);
