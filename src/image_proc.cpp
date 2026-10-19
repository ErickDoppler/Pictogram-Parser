#include "image_proc.h"

#include <algorithm>
#include <cmath>

namespace {

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
inline int clamp255(double v) { return v < 0.0 ? 0 : (v > 255.0 ? 255 : (int)(v + 0.5)); }

inline int chR(uint32_t p) { return (p >> 16) & 0xFF; }
inline int chG(uint32_t p) { return (p >> 8) & 0xFF; }
inline int chB(uint32_t p) { return p & 0xFF; }
inline int luma(uint32_t p) { return (chR(p) * 299 + chG(p) * 587 + chB(p) * 114) / 1000; }
inline uint32_t rgb(int r, int g, int b) { return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }
inline uint32_t greyPx(int v) { return rgb(v, v, v); }

// Maps strength 0..100 to a multiplicative factor 1/range .. range, 50 -> 1.0
double expFactor(int strength, double range) {
    return std::pow(range, (strength - 50) / 50.0);
}

// One horizontal + vertical box blur pass with the given radius.
void boxBlur(std::vector<uint8_t>& img, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<uint8_t> tmp(img.size());
    // horizontal
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = &img[(size_t)y * w];
        uint8_t* dst = &tmp[(size_t)y * w];
        int sum = 0;
        for (int i = -r; i <= r; ++i) sum += src[std::clamp(i, 0, w - 1)];
        const int n = 2 * r + 1;
        for (int x = 0; x < w; ++x) {
            dst[x] = (uint8_t)(sum / n);
            sum += src[std::min(x + r + 1, w - 1)];
            sum -= src[std::max(x - r, 0)];
        }
    }
    // vertical
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int i = -r; i <= r; ++i) sum += tmp[(size_t)std::clamp(i, 0, h - 1) * w + x];
        const int n = 2 * r + 1;
        for (int y = 0; y < h; ++y) {
            img[(size_t)y * w + x] = (uint8_t)(sum / n);
            sum += tmp[(size_t)std::min(y + r + 1, h - 1) * w + x];
            sum -= tmp[(size_t)std::max(y - r, 0) * w + x];
        }
    }
}

} // namespace

const wchar_t* effectName(Effect e) {
    switch (e) {
    case Effect::Original:     return L"Original";
    case Effect::Greyscale:    return L"Greyscale";
    case Effect::BlackWhite:   return L"Black && White";
    case Effect::RedChannel:   return L"Red channel";
    case Effect::GreenChannel: return L"Green channel";
    case Effect::BlueChannel:  return L"Blue channel";
    case Effect::Sketch:       return L"Sketch";
    case Effect::Emboss:       return L"Emboss";
    case Effect::Edges:        return L"Edges";
    default:                   return L"?";
    }
}

const wchar_t* strengthMeaning(Effect e) {
    switch (e) {
    case Effect::Original:     return L"saturation";
    case Effect::Greyscale:    return L"contrast";
    case Effect::BlackWhite:   return L"threshold";
    case Effect::RedChannel:
    case Effect::GreenChannel:
    case Effect::BlueChannel:  return L"brightness";
    case Effect::Sketch:       return L"pencil width";
    case Effect::Emboss:       return L"depth";
    case Effect::Edges:        return L"sensitivity";
    default:                   return L"";
    }
}

void applyEffect(const Image& src, Effect effect, int strength, Image& display, std::vector<uint8_t>& grey) {
    const int w = src.w, h = src.h;
    const size_t n = (size_t)w * h;
    display.w = w;
    display.h = h;
    display.px.assign(n, 0xFFFFFFFFu);
    grey.assign(n, 255);
    if (n == 0) return;

    strength = std::clamp(strength, 0, 100);

    // Plain luminance is the base of most effects.
    std::vector<uint8_t> lum(n);
    for (size_t i = 0; i < n; ++i) lum[i] = (uint8_t)luma(src.px[i]);

    auto lumAt = [&](int x, int y) -> int {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return lum[(size_t)y * w + x];
    };

    switch (effect) {
    case Effect::Original: {
        // Strength = saturation: 0 -> grey, 50 -> unchanged, 100 -> double saturation.
        const double sat = strength / 50.0;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t p = src.px[i];
            const double l = lum[i];
            const int r = clamp255(l + (chR(p) - l) * sat);
            const int g = clamp255(l + (chG(p) - l) * sat);
            const int b = clamp255(l + (chB(p) - l) * sat);
            display.px[i] = rgb(r, g, b);
            grey[i] = lum[i];
        }
        break;
    }
    case Effect::Greyscale: {
        // Strength = contrast around mid grey.
        const double c = expFactor(strength, 4.0);
        for (size_t i = 0; i < n; ++i) {
            const int v = clamp255((lum[i] - 128.0) * c + 128.0);
            grey[i] = (uint8_t)v;
            display.px[i] = greyPx(v);
        }
        break;
    }
    case Effect::BlackWhite: {
        // Strength = threshold. Higher strength -> more pixels become black.
        const int t = std::clamp((int)std::lround(strength * 2.55), 1, 255);
        for (size_t i = 0; i < n; ++i) {
            const int v = lum[i] < t ? 0 : 255;
            grey[i] = (uint8_t)v;
            display.px[i] = greyPx(v);
        }
        break;
    }
    case Effect::RedChannel:
    case Effect::GreenChannel:
    case Effect::BlueChannel: {
        // Strength = brightness gain of the channel.
        const double gain = expFactor(strength, 3.0);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t p = src.px[i];
            int c = effect == Effect::RedChannel ? chR(p) : (effect == Effect::GreenChannel ? chG(p) : chB(p));
            c = clamp255(c * gain);
            grey[i] = (uint8_t)c;
            if (effect == Effect::RedChannel)        display.px[i] = rgb(c, 0, 0);
            else if (effect == Effect::GreenChannel) display.px[i] = rgb(0, c, 0);
            else                                     display.px[i] = rgb(0, 0, c);
        }
        break;
    }
    case Effect::Sketch: {
        // Colour dodge of the luminance with its blurred negative. Strength = blur radius.
        const int radius = 1 + strength * 15 / 100;
        std::vector<uint8_t> inv(n);
        for (size_t i = 0; i < n; ++i) inv[i] = (uint8_t)(255 - lum[i]);
        for (int pass = 0; pass < 3; ++pass) boxBlur(inv, w, h, std::max(1, radius / 2));
        for (size_t i = 0; i < n; ++i) {
            const int base = lum[i];
            const int blend = inv[i];
            const int v = blend >= 255 ? 255 : std::min(255, base * 255 / (255 - blend));
            grey[i] = (uint8_t)v;
            display.px[i] = greyPx(v);
        }
        break;
    }
    case Effect::Emboss: {
        // 3x3 emboss kernel, strength = depth.
        const double depth = 0.1 + strength / 100.0 * 3.0;
        static const int k[3][3] = { { -2, -1, 0 }, { -1, 0, 1 }, { 0, 1, 2 } };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int sum = 0;
                for (int ky = -1; ky <= 1; ++ky)
                    for (int kx = -1; kx <= 1; ++kx)
                        sum += k[ky + 1][kx + 1] * lumAt(x + kx, y + ky);
                const int v = clamp255(128.0 + sum * depth);
                const size_t i = (size_t)y * w + x;
                grey[i] = (uint8_t)v;
                display.px[i] = greyPx(v);
            }
        }
        break;
    }
    case Effect::Edges: {
        // Sobel magnitude drawn as black edges on white. Strength = sensitivity.
        const double gain = expFactor(strength, 8.0) * 0.5;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int gx = -lumAt(x - 1, y - 1) - 2 * lumAt(x - 1, y) - lumAt(x - 1, y + 1)
                               + lumAt(x + 1, y - 1) + 2 * lumAt(x + 1, y) + lumAt(x + 1, y + 1);
                const int gy = -lumAt(x - 1, y - 1) - 2 * lumAt(x, y - 1) - lumAt(x + 1, y - 1)
                               + lumAt(x - 1, y + 1) + 2 * lumAt(x, y + 1) + lumAt(x + 1, y + 1);
                const double mag = std::sqrt((double)gx * gx + (double)gy * gy);
                const int v = 255 - clamp255(mag * gain);
                const size_t i = (size_t)y * w + x;
                grey[i] = (uint8_t)v;
                display.px[i] = greyPx(v);
            }
        }
        break;
    }
    default:
        for (size_t i = 0; i < n; ++i) {
            display.px[i] = src.px[i] | 0xFF000000u;
            grey[i] = lum[i];
        }
        break;
    }
}

Pictogram extractCell(const std::vector<uint8_t>& grey, int imgW, int imgH,
                      int x0, int y0, int x1, int y1,
                      int outW, int outH, int threshold, bool invert) {
    Pictogram p;
    const int cw = std::max(1, x1 - x0);
    const int ch = std::max(1, y1 - y0);
    p.w = outW > 0 ? outW : cw;
    p.h = outH > 0 ? outH : ch;
    p.bits.assign((size_t)p.w * p.h, 0);

    for (int oy = 0; oy < p.h; ++oy) {
        int sy0 = y0 + (int)((long long)oy * ch / p.h);
        int sy1 = y0 + (int)((long long)(oy + 1) * ch / p.h);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int ox = 0; ox < p.w; ++ox) {
            int sx0 = x0 + (int)((long long)ox * cw / p.w);
            int sx1 = x0 + (int)((long long)(ox + 1) * cw / p.w);
            if (sx1 <= sx0) sx1 = sx0 + 1;

            // Area average; pixels outside of the image count as white paper.
            long long sum = 0;
            long long cnt = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    if (sx >= 0 && sy >= 0 && sx < imgW && sy < imgH)
                        sum += grey[(size_t)sy * imgW + sx];
                    else
                        sum += 255;
                    ++cnt;
                }
            }
            const int avg = (int)(sum / std::max(1LL, cnt));
            bool ink = avg < threshold;
            if (invert) ink = !ink;
            p.bits[(size_t)oy * p.w + ox] = ink ? 1 : 0;
        }
    }
    return p;
}
