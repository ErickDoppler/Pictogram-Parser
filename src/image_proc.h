// Image container, preprocessing effects and pictogram extraction.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 32-bit image, each pixel 0xAARRGGBB (in memory B,G,R,A - directly usable as a 32bpp DIB).
struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> px;

    bool empty() const { return w <= 0 || h <= 0; }
};

enum class Effect {
    Original = 0,
    Greyscale,
    BlackWhite,
    RedChannel,
    GreenChannel,
    BlueChannel,
    Sketch,
    Emboss,
    Edges,
    Count
};

// Display name of the effect button.
const wchar_t* effectName(Effect e);
// What the "Strength" slider controls for the given effect.
const wchar_t* strengthMeaning(Effect e);

// Applies the effect to src.
//  display - colour image shown in the preview
//  grey    - 8-bit luminance the extractor works on (w*h bytes)
// strength is 0..100, 50 is the neutral/default value.
void applyEffect(const Image& src, Effect effect, int strength, Image& display, std::vector<uint8_t>& grey);

// A small 1-bit pictogram. bits[y*w + x] == 1 means black (ink) pixel.
struct Pictogram {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> bits;
};

// Extracts the cell [x0,x1) x [y0,y1) of the grey image into an outW x outH monochrome pictogram.
// Pixels darker than threshold become ink (1), unless invert is set.
// outW/outH <= 0 means "use the cell size".
Pictogram extractCell(const std::vector<uint8_t>& grey, int imgW, int imgH,
                      int x0, int y0, int x1, int y1,
                      int outW, int outH, int threshold, bool invert);
