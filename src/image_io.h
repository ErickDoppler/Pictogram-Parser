// Image loading (GDI+) and pictogram export (C array, monochrome BMP).
#pragma once

#include <string>
#include <vector>

#include "image_proc.h"

bool initImageIO();
void shutdownImageIO();

// Loads BMP/PNG/JPG/GIF/TIFF/ICO. Transparent pixels are composited onto white.
bool loadImage(const std::wstring& path, Image& out, std::wstring& error);

// Turns any text into a valid C identifier ("my icon" -> "my_icon", "1x" -> "_1x").
std::string toCIdentifier(const std::string& name);

// Writes all pictograms into one C header: 1-bit ones as packed MONO1 bytes, colour ones as RGB565.
// names[i] belongs to pictos[i].
bool saveAsCArray(const std::wstring& path, const std::vector<Pictogram>& pictos,
                  const std::vector<std::string>& names, std::wstring& error);

// Writes a 1-bit BMP (black ink on white), or a 24-bit BMP for a colour pictogram.
bool saveAsBitmap(const std::wstring& path, const Pictogram& picto, std::wstring& error);
