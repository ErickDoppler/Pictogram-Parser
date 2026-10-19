// Pictogram Parser - extracts pictograms from a sheet image into C arrays or bitmaps.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <vector>

#include "image_io.h"
#include "image_proc.h"

namespace {

// ---------------------------------------------------------------------------------------------
// Control ids
// ---------------------------------------------------------------------------------------------
enum : int {
    IDC_EFFECT_FIRST = 100,  // + (int)Effect
    IDC_STRENGTH = 200,
    IDC_OPEN = 300,
    IDC_FIT_VIEW,
    IDC_ZOOM_100,
    IDC_ROWS,
    IDC_COLS,
    IDC_CELLW,
    IDC_CELLH,
    IDC_OFFX,
    IDC_OFFY,
    IDC_APPLY_GRID,
    IDC_FIT_GRID,
    IDC_MARK_ALL,
    IDC_CLEAR_MARKS,
    IDC_OUTW,
    IDC_OUTH,
    IDC_THRESHOLD,
    IDC_INSET,
    IDC_INVERT,
    IDC_NAME,
    IDC_EXTRACT,
    IDC_LIST,
    IDC_DELETE,
    IDC_CLEAR_LIST,
    IDC_SAVE_C,
    IDC_SAVE_BMP,
    IDC_STATUS,
};

constexpr UINT_PTR TIMER_REPROCESS = 1;
constexpr UINT_PTR TIMER_INSET_PREVIEW = 2;
constexpr UINT INSET_PREVIEW_MS = 1500;  // how long the trimmed margins stay visible after the slider moves
constexpr int MAX_INSET = 64;
constexpr int EFFECT_COUNT = (int)Effect::Count;

const wchar_t* const HINT_TEXT =
    L"Left click / drag over cells: mark pictograms  \x2022  Drag a grid line: move that border "
    L"(Shift = respace all evenly)  \x2022  Right drag, Ctrl+drag or arrow keys: move the grid  \x2022  "
    L"Wheel: zoom  \x2022  Middle drag or drag outside the grid: pan";

// ---------------------------------------------------------------------------------------------
// Grid model: absolute line positions in image pixels. Cell (r,c) = [xs[c], xs[c+1]) x [ys[r], ys[r+1]).
// ---------------------------------------------------------------------------------------------
struct Grid {
    std::vector<int> xs;
    std::vector<int> ys;
    std::vector<uint8_t> marks;

    int cols() const { return xs.size() > 1 ? (int)xs.size() - 1 : 0; }
    int rows() const { return ys.size() > 1 ? (int)ys.size() - 1 : 0; }
    bool valid() const { return cols() > 0 && rows() > 0; }

    uint8_t& mark(int r, int c) { return marks[(size_t)r * cols() + c]; }

    int markedCount() const {
        int n = 0;
        for (uint8_t m : marks) n += m ? 1 : 0;
        return n;
    }

    // Rebuilds a uniform grid, keeping the marks of cells that still exist.
    void build(int nRows, int nCols, int x0, int y0, int cw, int ch) {
        const int oldRows = rows(), oldCols = cols();
        std::vector<uint8_t> nm((size_t)nRows * nCols, 0);
        for (int r = 0; r < std::min(nRows, oldRows); ++r)
            for (int c = 0; c < std::min(nCols, oldCols); ++c)
                nm[(size_t)r * nCols + c] = marks[(size_t)r * oldCols + c];
        xs.resize(nCols + 1);
        ys.resize(nRows + 1);
        for (int i = 0; i <= nCols; ++i) xs[i] = x0 + i * cw;
        for (int i = 0; i <= nRows; ++i) ys[i] = y0 + i * ch;
        marks.swap(nm);
    }

    void translate(int dx, int dy) {
        for (int& x : xs) x += dx;
        for (int& y : ys) y += dy;
    }

    static int indexAt(const std::vector<int>& v, double pos) {
        if (v.size() < 2 || pos < v.front() || pos >= v.back()) return -1;
        auto it = std::upper_bound(v.begin(), v.end(), pos, [](double p, int line) { return p < line; });
        return (int)(it - v.begin()) - 1;
    }

    // Moves one line, keeping it between its neighbours.
    static void moveLine(std::vector<int>& v, int idx, int pos) {
        const int n = (int)v.size() - 1;
        const int lo = idx > 0 ? v[idx - 1] + 1 : INT_MIN / 2;
        const int hi = idx < n ? v[idx + 1] - 1 : INT_MAX / 2;
        v[idx] = std::clamp(pos, lo, hi);
    }

    // Moves line idx to pos and spaces all lines evenly (every cell gets the same size).
    static void respace(std::vector<int>& v, int idx, int pos) {
        const int n = (int)v.size() - 1;
        if (n <= 0) return;
        if (idx > 0) {
            const double pitch = std::max(1.0, double(pos - v[0]) / idx);
            const int start = v[0];
            for (int k = 0; k <= n; ++k) v[k] = start + (int)std::lround(k * pitch);
        } else {
            const double pitch = std::max(1.0, double(v[n] - pos) / n);
            const int end = v[n];
            for (int k = 0; k <= n; ++k) v[k] = end - (int)std::lround((n - k) * pitch);
        }
    }
};

struct NumField {
    HWND label = nullptr;
    HWND edit = nullptr;
    HWND spin = nullptr;
};

enum class Drag { None, Paint, Lines, MoveGrid, Pan };

// ---------------------------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------------------------
struct App {
    HINSTANCE hInst = nullptr;
    HWND hwnd = nullptr;
    HWND canvas = nullptr;
    HWND status = nullptr;
    int dpi = 96;
    HFONT font = nullptr;
    HFONT boldFont = nullptr;
    HFONT smallFont = nullptr;
    int minWidth = 1000;

    // toolbar
    HWND effectBtn[EFFECT_COUNT] = {};
    int effectBtnW[EFFECT_COUNT] = {};
    HWND lblStrength = nullptr, trkStrength = nullptr, lblStrengthVal = nullptr, lblHint = nullptr;

    // panel
    HWND hdrImage = nullptr, btnOpen = nullptr, lblFile = nullptr, btnFitView = nullptr, btnZoom100 = nullptr;
    HWND hdrGrid = nullptr, btnApplyGrid = nullptr, btnFitGrid = nullptr, btnMarkAll = nullptr, btnClearMarks = nullptr;
    NumField fRows, fCols, fCellW, fCellH, fOffX, fOffY;
    HWND hdrExtract = nullptr, lblOutHint = nullptr, lblThreshold = nullptr, trkThreshold = nullptr, lblThresholdVal = nullptr;
    HWND lblInset = nullptr, trkInset = nullptr, lblInsetVal = nullptr;
    NumField fOutW, fOutH;
    HWND chkInvert = nullptr, lblName = nullptr, editName = nullptr, btnExtract = nullptr;
    HWND hdrList = nullptr, list = nullptr, btnDelete = nullptr, btnClearList = nullptr, btnSaveC = nullptr, btnSaveBmp = nullptr;

    // image
    std::wstring imagePath;
    Image source;
    Image display;
    std::vector<uint8_t> grey;
    Effect effect = Effect::Original;
    int strength = 50;

    // GDI objects for the preview
    HDC imgDC = nullptr;
    HBITMAP imgBmp = nullptr;
    HGDIOBJ imgOldBmp = nullptr;
    void* imgBits = nullptr;
    int imgBmpW = 0, imgBmpH = 0;
    HDC overlayDC = nullptr;
    HBITMAP overlayBmp = nullptr;
    HDC cutDC = nullptr;        // 1x1 colour used to shade the trimmed cell margins
    HBITMAP cutBmp = nullptr;

    // view
    double zoom = 1.0;
    double panX = 0.0, panY = 0.0;

    // grid & interaction
    Grid grid;
    Drag drag = Drag::None;
    int dragCol = -1, dragRow = -1;
    bool dragUniform = false;
    uint8_t paintValue = 1;
    int dragStartX = 0, dragStartY = 0;
    double dragStartPanX = 0, dragStartPanY = 0;
    int movedX = 0, movedY = 0;
    int hoverCol = -1, hoverRow = -1;
    int cursorImgX = INT_MIN, cursorImgY = INT_MIN;

    // results
    std::vector<Pictogram> pictos;

    // cell inset: pixels cut off every side of a cell before extraction
    int inset = 0;
    bool insetPreview = false;
};

App g;

int S(int v) { return MulDiv(v, g.dpi, 96); }

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------
std::wstring getText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n, L'\0');
    if (n > 0) GetWindowTextW(h, &s[0], n + 1);
    return s;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

void message(const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
    MessageBoxW(g.hwnd, text.c_str(), L"Pictogram Parser", MB_OK | icon);
}

void setStatus(int part, const std::wstring& text) {
    SendMessageW(g.status, SB_SETTEXTW, (WPARAM)part, (LPARAM)text.c_str());
}

HWND makeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0, HFONT font = nullptr) {
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.hwnd,
                             (HMENU)(INT_PTR)id, g.hInst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)(font ? font : g.font), TRUE);
    return h;
}

HWND makeButton(const wchar_t* text, int id, DWORD extra = 0) {
    return makeCtl(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP | extra, id);
}

HWND makeLabel(const wchar_t* text, DWORD extra = 0, HFONT font = nullptr) {
    return makeCtl(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | extra, -1, 0, font);
}

HWND makeTrackbar(int id, int minV, int maxV, int pos) {
    HWND h = makeCtl(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, id);
    SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(minV, maxV));
    SendMessageW(h, TBM_SETPAGESIZE, 0, (maxV - minV) / 10);
    SendMessageW(h, TBM_SETPOS, TRUE, pos);
    return h;
}

NumField makeNumField(const wchar_t* label, int id, int minV, int maxV, int value) {
    NumField f;
    f.label = makeLabel(label);
    f.edit = makeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | (minV >= 0 ? ES_NUMBER : 0), id, WS_EX_CLIENTEDGE);
    f.spin = CreateWindowExW(0, UPDOWN_CLASSW, nullptr,
                             WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                             0, 0, 10, 10, g.hwnd, nullptr, g.hInst, nullptr);
    SendMessageW(f.spin, UDM_SETBUDDY, (WPARAM)f.edit, 0);
    SendMessageW(f.spin, UDM_SETRANGE32, (WPARAM)minV, (LPARAM)maxV);
    SendMessageW(f.spin, UDM_SETPOS32, 0, (LPARAM)value);
    SetWindowTextW(f.edit, std::to_wstring(value).c_str());
    return f;
}

int getNum(const NumField& f, int fallback) {
    const std::wstring t = getText(f.edit);
    try {
        size_t used = 0;
        const int v = std::stoi(t, &used);
        return used > 0 ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

void setNum(const NumField& f, int v) {
    SetWindowTextW(f.edit, std::to_wstring(v).c_str());
}

void moveCtl(HWND h, int x, int y, int w, int hgt) {
    MoveWindow(h, x, y, std::max(1, w), std::max(1, hgt), TRUE);
}

// Places "label [edit^v]" into the given column.
void placeNumField(const NumField& f, int x, int y, int w, int h) {
    const int lw = S(56);
    moveCtl(f.label, x, y, lw, h);
    moveCtl(f.edit, x + lw, y, w - lw, h);
    SendMessageW(f.spin, UDM_SETBUDDY, (WPARAM)f.edit, 0);  // re-attaches the arrows to the moved edit
}

std::string pictogramName(size_t index) {
    return toCIdentifier(toUtf8(getText(g.editName)) + std::to_string(index));
}

// ---------------------------------------------------------------------------------------------
// View <-> image coordinates
// ---------------------------------------------------------------------------------------------
double toImgX(int sx) { return (sx - g.panX) / g.zoom; }
double toImgY(int sy) { return (sy - g.panY) / g.zoom; }
int toScrX(double ix) { return (int)std::lround(ix * g.zoom + g.panX); }
int toScrY(double iy) { return (int)std::lround(iy * g.zoom + g.panY); }

void invalidateCanvas() { InvalidateRect(g.canvas, nullptr, FALSE); }

void updateStatus() {
    if (g.source.empty()) {
        setStatus(0, L"No image");
        setStatus(1, L"");
        setStatus(2, L"");
        setStatus(3, L"");
        return;
    }
    setStatus(0, std::to_wstring(g.source.w) + L" x " + std::to_wstring(g.source.h) + L" px");
    setStatus(1, L"Zoom " + std::to_wstring((int)std::lround(g.zoom * 100)) + L" %");
    if (g.cursorImgX != INT_MIN)
        setStatus(2, L"X " + std::to_wstring(g.cursorImgX) + L"   Y " + std::to_wstring(g.cursorImgY));
    else
        setStatus(2, L"");
    setStatus(3, std::to_wstring(g.grid.cols()) + L" x " + std::to_wstring(g.grid.rows()) + L" cells, " +
                     std::to_wstring(g.grid.markedCount()) + L" marked");
}

// Writes the current grid geometry back into the Grid fields.
void updateGridFields() {
    if (!g.grid.valid()) return;
    const int cols = g.grid.cols(), rows = g.grid.rows();
    setNum(g.fRows, rows);
    setNum(g.fCols, cols);
    setNum(g.fCellW, (int)std::lround(double(g.grid.xs.back() - g.grid.xs.front()) / cols));
    setNum(g.fCellH, (int)std::lround(double(g.grid.ys.back() - g.grid.ys.front()) / rows));
    setNum(g.fOffX, g.grid.xs.front());
    setNum(g.fOffY, g.grid.ys.front());
}

void updateStrengthLabel() {
    SetWindowTextW(g.lblStrengthVal,
                   (std::to_wstring(g.strength) + L"   (" + strengthMeaning(g.effect) + L")").c_str());
}

void updateInsetLabel() {
    SetWindowTextW(g.lblInsetVal, (std::to_wstring(g.inset) + L" px").c_str());
}

// The part of cell (r,c) that is extracted, in image pixels: the cell minus the inset on every side.
// The inset is limited per axis so at least one pixel always remains.
RECT innerCell(int r, int c) {
    const int x0 = g.grid.xs[c], x1 = g.grid.xs[c + 1];
    const int y0 = g.grid.ys[r], y1 = g.grid.ys[r + 1];
    const int ix = std::min(g.inset, (x1 - x0 - 1) / 2);
    const int iy = std::min(g.inset, (y1 - y0 - 1) / 2);
    return RECT{ x0 + ix, y0 + iy, x1 - ix, y1 - iy };
}

void updateThresholdLabel() {
    const int t = (int)SendMessageW(g.trkThreshold, TBM_GETPOS, 0, 0);
    SetWindowTextW(g.lblThresholdVal, std::to_wstring(t).c_str());
}

void updateListHeader() {
    SetWindowTextW(g.hdrList, (L"PICTOGRAMS  (" + std::to_wstring(g.pictos.size()) + L")").c_str());
}

// ---------------------------------------------------------------------------------------------
// Image processing / preview bitmap
// ---------------------------------------------------------------------------------------------
void uploadPreview() {
    const int w = g.display.w, h = g.display.h;
    if (w <= 0 || h <= 0) return;
    if (!g.imgBmp || g.imgBmpW != w || g.imgBmpH != h) {
        if (g.imgBmp) {
            SelectObject(g.imgDC, g.imgOldBmp);
            DeleteObject(g.imgBmp);
            g.imgBmp = nullptr;
        }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g.imgBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &g.imgBits, nullptr, 0);
        if (!g.imgBmp) {
            g.imgBits = nullptr;
            g.imgBmpW = g.imgBmpH = 0;
            return;
        }
        g.imgOldBmp = SelectObject(g.imgDC, g.imgBmp);
        g.imgBmpW = w;
        g.imgBmpH = h;
    }
    GdiFlush();
    std::copy(g.display.px.begin(), g.display.px.end(), (uint32_t*)g.imgBits);
}

void reprocess() {
    if (g.source.empty()) return;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    applyEffect(g.source, g.effect, g.strength, g.display, g.grey);
    uploadPreview();
    SetCursor(old);
    invalidateCanvas();
}

void fitView() {
    if (g.source.empty()) return;
    RECT rc;
    GetClientRect(g.canvas, &rc);
    const double cw = std::max(1L, rc.right), ch = std::max(1L, rc.bottom);
    g.zoom = std::clamp(std::min(cw / g.source.w, ch / g.source.h) * 0.95, 0.02, 64.0);
    g.panX = (cw - g.source.w * g.zoom) / 2.0;
    g.panY = (ch - g.source.h * g.zoom) / 2.0;
    updateStatus();
    invalidateCanvas();
}

void zoomAt(double newZoom, int sx, int sy) {
    newZoom = std::clamp(newZoom, 0.02, 64.0);
    const double ix = toImgX(sx), iy = toImgY(sy);
    g.zoom = newZoom;
    g.panX = sx - ix * g.zoom;
    g.panY = sy - iy * g.zoom;
    updateStatus();
    invalidateCanvas();
}

void zoomCentered(double newZoom) {
    RECT rc;
    GetClientRect(g.canvas, &rc);
    zoomAt(newZoom, rc.right / 2, rc.bottom / 2);
}

// ---------------------------------------------------------------------------------------------
// Grid commands
// ---------------------------------------------------------------------------------------------
void applyGridFromFields() {
    if (g.source.empty()) return;
    const int rows = std::clamp(getNum(g.fRows, 8), 1, 1000);
    const int cols = std::clamp(getNum(g.fCols, 8), 1, 1000);
    const int cw = std::clamp(getNum(g.fCellW, 16), 1, 100000);
    const int ch = std::clamp(getNum(g.fCellH, 16), 1, 100000);
    const int ox = getNum(g.fOffX, 0);
    const int oy = getNum(g.fOffY, 0);
    g.grid.build(rows, cols, ox, oy, cw, ch);
    updateGridFields();
    updateStatus();
    invalidateCanvas();
}

void fitGridToImage() {
    if (g.source.empty()) return;
    const int rows = std::clamp(getNum(g.fRows, 8), 1, 1000);
    const int cols = std::clamp(getNum(g.fCols, 8), 1, 1000);
    setNum(g.fCellW, std::max(1, g.source.w / cols));
    setNum(g.fCellH, std::max(1, g.source.h / rows));
    setNum(g.fOffX, 0);
    setNum(g.fOffY, 0);
    applyGridFromFields();
}

void setAllMarks(uint8_t v) {
    std::fill(g.grid.marks.begin(), g.grid.marks.end(), v);
    updateStatus();
    invalidateCanvas();
}

// ---------------------------------------------------------------------------------------------
// File commands
// ---------------------------------------------------------------------------------------------
void openImage() {
    wchar_t buf[4096] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter =
        L"Images (*.png;*.bmp;*.jpg;*.jpeg;*.gif;*.tif;*.tiff;*.ico)\0*.png;*.bmp;*.jpg;*.jpeg;*.gif;*.tif;*.tiff;*.ico\0"
        L"All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrTitle = L"Open pictogram sheet";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    Image img;
    std::wstring err;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const bool ok = loadImage(buf, img, err);
    SetCursor(old);
    if (!ok) {
        message(err, MB_ICONERROR);
        return;
    }

    g.imagePath = buf;
    g.source = std::move(img);
    const size_t slash = g.imagePath.find_last_of(L"\\/");
    const std::wstring fileName = slash == std::wstring::npos ? g.imagePath : g.imagePath.substr(slash + 1);
    SetWindowTextW(g.lblFile, (fileName + L"  (" + std::to_wstring(g.source.w) + L" x " +
                               std::to_wstring(g.source.h) + L")").c_str());
    SetWindowTextW(g.hwnd, (L"Pictogram Parser - " + fileName).c_str());

    reprocess();
    g.grid = Grid();
    fitGridToImage();
    fitView();
}

void refreshList(bool scrollToEnd) {
    SendMessageW(g.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g.list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g.pictos.size(); ++i) SendMessageW(g.list, LB_ADDSTRING, 0, (LPARAM)i);
    if (scrollToEnd && !g.pictos.empty()) SendMessageW(g.list, LB_SETTOPINDEX, (WPARAM)(g.pictos.size() - 1), 0);
    SendMessageW(g.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.list, nullptr, TRUE);
    updateListHeader();
}

void extract() {
    if (g.source.empty()) {
        message(L"Open an image first.");
        return;
    }
    if (!g.grid.valid() || g.grid.markedCount() == 0) {
        message(L"No cells are marked.\n\nClick on the grid cells that contain pictograms, then press EXTRACT.");
        return;
    }
    const int outW = std::clamp(getNum(g.fOutW, 0), 0, 4096);
    const int outH = std::clamp(getNum(g.fOutH, 0), 0, 4096);
    const int threshold = (int)SendMessageW(g.trkThreshold, TBM_GETPOS, 0, 0);
    const bool invert = SendMessageW(g.chkInvert, BM_GETCHECK, 0, 0) == BST_CHECKED;

    int added = 0;
    for (int r = 0; r < g.grid.rows(); ++r) {
        for (int c = 0; c < g.grid.cols(); ++c) {
            if (!g.grid.mark(r, c)) continue;
            const RECT in = innerCell(r, c);
            g.pictos.push_back(extractCell(g.grey, g.display.w, g.display.h,
                                           in.left, in.top, in.right, in.bottom,
                                           outW, outH, threshold, invert));
            ++added;
        }
    }
    refreshList(true);
    setStatus(4, L"Extracted " + std::to_wstring(added) + L" pictogram(s). Total: " + std::to_wstring(g.pictos.size()));
}

void deleteSelected() {
    const int count = (int)SendMessageW(g.list, LB_GETSELCOUNT, 0, 0);
    if (count <= 0) return;
    std::vector<int> sel((size_t)count);
    SendMessageW(g.list, LB_GETSELITEMS, (WPARAM)count, (LPARAM)sel.data());
    std::sort(sel.rbegin(), sel.rend());
    for (int i : sel)
        if (i >= 0 && i < (int)g.pictos.size()) g.pictos.erase(g.pictos.begin() + i);
    refreshList(false);
}

void clearList() {
    if (g.pictos.empty()) return;
    if (MessageBoxW(g.hwnd, L"Remove all extracted pictograms from the list?", L"Pictogram Parser",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    g.pictos.clear();
    refreshList(false);
}

void saveC() {
    if (g.pictos.empty()) {
        message(L"The list is empty. Extract some pictograms first.");
        return;
    }
    wchar_t buf[4096] = L"pictograms.h";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"C header (*.h)\0*.h\0C source (*.c)\0*.c\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrDefExt = L"h";
    ofn.lpstrTitle = L"Save pictograms as C array";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return;

    std::vector<std::string> names;
    for (size_t i = 0; i < g.pictos.size(); ++i) names.push_back(pictogramName(i));
    std::wstring err;
    if (!saveAsCArray(buf, g.pictos, names, err)) {
        message(err, MB_ICONERROR);
        return;
    }
    setStatus(4, L"Saved " + std::to_wstring(g.pictos.size()) + L" pictogram(s) to " + buf);
}

void saveBmp() {
    if (g.pictos.empty()) {
        message(L"The list is empty. Extract some pictograms first.");
        return;
    }
    BROWSEINFOW bi = {};
    bi.hwndOwner = g.hwnd;
    bi.lpszTitle = L"Choose a folder for the bitmaps (one .bmp file per pictogram)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t folder[MAX_PATH] = L"";
    const BOOL ok = SHGetPathFromIDListW(pidl, folder);
    CoTaskMemFree(pidl);
    if (!ok) {
        message(L"Please choose a folder on disk.", MB_ICONWARNING);
        return;
    }

    std::wstring dir = folder;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    for (size_t i = 0; i < g.pictos.size(); ++i) {
        const std::wstring path = dir + toWide(pictogramName(i)) + L".bmp";
        std::wstring err;
        if (!saveAsBitmap(path, g.pictos[i], err)) {
            message(err, MB_ICONERROR);
            return;
        }
    }
    setStatus(4, L"Saved " + std::to_wstring(g.pictos.size()) + L" bitmap(s) to " + dir);
}

// ---------------------------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------------------------

// Finds grid lines under the mouse (within a few screen pixels).
void hitLines(int mx, int my, int& col, int& row) {
    col = row = -1;
    if (!g.grid.valid()) return;
    const int tol = S(4);
    const auto& xs = g.grid.xs;
    const auto& ys = g.grid.ys;

    if (my >= toScrY(ys.front()) - tol && my <= toScrY(ys.back()) + tol) {
        int best = tol + 1;
        for (int i = 0; i < (int)xs.size(); ++i) {
            const int d = std::abs(toScrX(xs[i]) - mx);
            if (d < best || (d == best && i > col)) {  // prefer the later line when lines overlap
                best = d;
                col = i;
            }
        }
    }
    if (mx >= toScrX(xs.front()) - tol && mx <= toScrX(xs.back()) + tol) {
        int best = tol + 1;
        for (int i = 0; i < (int)ys.size(); ++i) {
            const int d = std::abs(toScrY(ys[i]) - my);
            if (d < best || (d == best && i > row)) {
                best = d;
                row = i;
            }
        }
    }
}

bool cellAt(int mx, int my, int& r, int& c) {
    c = Grid::indexAt(g.grid.xs, toImgX(mx));
    r = Grid::indexAt(g.grid.ys, toImgY(my));
    return r >= 0 && c >= 0;
}

void paintCanvas(HDC hdc, const RECT& rc) {
    const int w = rc.right, h = rc.bottom;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max(1, w), std::max(1, h));
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(RGB(48, 48, 52));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    if (g.source.empty() || !g.imgBmp) {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(200, 200, 200));
        HGDIOBJ oldFont = SelectObject(mem, g.font);
        RECT tr = rc;
        DrawTextW(mem, L"Click \"Open image...\" to load a sheet of pictograms", -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, oldFont);
    } else {
        // Image: blit only the visible part.
        const int iw = g.imgBmpW, ih = g.imgBmpH;
        const int sx0 = std::clamp((int)std::floor(toImgX(0)), 0, iw);
        const int sy0 = std::clamp((int)std::floor(toImgY(0)), 0, ih);
        const int sx1 = std::clamp((int)std::ceil(toImgX(w)), 0, iw);
        const int sy1 = std::clamp((int)std::ceil(toImgY(h)), 0, ih);
        if (sx1 > sx0 && sy1 > sy0) {
            const int dx0 = toScrX(sx0), dy0 = toScrY(sy0);
            const int dx1 = toScrX(sx1), dy1 = toScrY(sy1);
            if (g.zoom >= 1.0) {
                SetStretchBltMode(mem, COLORONCOLOR);
            } else {
                SetStretchBltMode(mem, HALFTONE);
                SetBrushOrgEx(mem, 0, 0, nullptr);
            }
            StretchBlt(mem, dx0, dy0, dx1 - dx0, dy1 - dy0, g.imgDC, sx0, sy0, sx1 - sx0, sy1 - sy0, SRCCOPY);
        }

        if (g.grid.valid()) {
            const auto& xs = g.grid.xs;
            const auto& ys = g.grid.ys;
            const int cols = g.grid.cols(), rows = g.grid.rows();

            // Marked cells: translucent green fill over the extracted (inset) area + extraction order number.
            // Unmarked cells, right after the inset slider moved: the margins that will be cut off are shaded.
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 110, 0 };
            BLENDFUNCTION cutBf = { AC_SRC_OVER, 0, 150, 0 };
            HGDIOBJ oldFont = SelectObject(mem, g.smallFont);
            SetBkMode(mem, TRANSPARENT);
            const bool showCut = g.insetPreview && g.inset > 0;
            auto shade = [&](int l, int t, int rgt, int b) {
                if (rgt > l && b > t) AlphaBlend(mem, l, t, rgt - l, b - t, g.cutDC, 0, 0, 1, 1, cutBf);
            };
            int order = 0;
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    const bool marked = g.grid.mark(r, c) != 0;
                    if (!marked && !showCut) continue;
                    const int idx = marked ? order++ : -1;
                    const int cx0 = toScrX(xs[c]), cx1 = toScrX(xs[c + 1]);
                    const int cy0 = toScrY(ys[r]), cy1 = toScrY(ys[r + 1]);
                    if (cx1 < 0 || cy1 < 0 || cx0 > w || cy0 > h) continue;
                    const RECT in = innerCell(r, c);
                    const int x0 = toScrX(in.left), x1 = toScrX(in.right);
                    const int y0 = toScrY(in.top), y1 = toScrY(in.bottom);
                    if (!marked) {
                        shade(cx0, cy0, cx1, y0);   // top
                        shade(cx0, y1, cx1, cy1);   // bottom
                        shade(cx0, y0, x0, y1);     // left
                        shade(x1, y0, cx1, y1);     // right
                        continue;
                    }
                    if (x1 <= x0 || y1 <= y0) continue;
                    AlphaBlend(mem, x0, y0, x1 - x0, y1 - y0, g.overlayDC, 0, 0, 1, 1, bf);
                    if (x1 - x0 >= S(18) && y1 - y0 >= S(14)) {
                        RECT tr = { x0 + S(2), y0 + 1, x1 - 1, y1 - 1 };
                        const std::wstring num = std::to_wstring(idx);
                        SetTextColor(mem, RGB(0, 0, 0));
                        OffsetRect(&tr, 1, 1);
                        DrawTextW(mem, num.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
                        OffsetRect(&tr, -1, -1);
                        SetTextColor(mem, RGB(255, 255, 120));
                        DrawTextW(mem, num.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
                    }
                }
            }
            SelectObject(mem, oldFont);

            // Grid lines
            HPEN penLine = CreatePen(PS_SOLID, 1, RGB(0, 170, 255));
            HPEN penBorder = CreatePen(PS_SOLID, 2, RGB(0, 120, 255));
            HPEN penActive = CreatePen(PS_SOLID, 2, RGB(255, 200, 0));
            HGDIOBJ oldPen = SelectObject(mem, penLine);
            const int gx0 = toScrX(xs.front()), gx1 = toScrX(xs.back());
            const int gy0 = toScrY(ys.front()), gy1 = toScrY(ys.back());
            const int activeCol = g.drag == Drag::Lines ? g.dragCol : (g.drag == Drag::None ? g.hoverCol : -1);
            const int activeRow = g.drag == Drag::Lines ? g.dragRow : (g.drag == Drag::None ? g.hoverRow : -1);
            for (int i = 0; i <= cols; ++i) {
                SelectObject(mem, i == activeCol ? penActive : ((i == 0 || i == cols) ? penBorder : penLine));
                const int x = toScrX(xs[i]);
                MoveToEx(mem, x, gy0, nullptr);
                LineTo(mem, x, gy1);
            }
            for (int i = 0; i <= rows; ++i) {
                SelectObject(mem, i == activeRow ? penActive : ((i == 0 || i == rows) ? penBorder : penLine));
                const int y = toScrY(ys[i]);
                MoveToEx(mem, gx0, y, nullptr);
                LineTo(mem, gx1, y);
            }
            SelectObject(mem, oldPen);
            DeleteObject(penLine);
            DeleteObject(penBorder);
            DeleteObject(penActive);
        }
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void beginDrag(HWND hwnd, Drag mode, int mx, int my) {
    g.drag = mode;
    g.dragStartX = mx;
    g.dragStartY = my;
    g.dragStartPanX = g.panX;
    g.dragStartPanY = g.panY;
    g.movedX = g.movedY = 0;
    SetCapture(hwnd);
}

void endDrag() {
    if (g.drag == Drag::None) return;
    const bool gridChanged = g.drag == Drag::Lines || g.drag == Drag::MoveGrid;
    g.drag = Drag::None;
    if (GetCapture() == g.canvas) ReleaseCapture();
    if (gridChanged) updateGridFields();
    updateStatus();
    invalidateCanvas();
}

void nudgeGrid(int dx, int dy) {
    if (!g.grid.valid()) return;
    g.grid.translate(dx, dy);
    updateGridFields();
    invalidateCanvas();
}

LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        paintCanvas(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        invalidateCanvas();
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        if (g.source.empty()) return 0;
        const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (wp & MK_CONTROL) {
            if (g.grid.valid()) beginDrag(hwnd, Drag::MoveGrid, mx, my);
            return 0;
        }
        int lc, lr;
        hitLines(mx, my, lc, lr);
        if (lc >= 0 || lr >= 0) {
            g.dragCol = lc;
            g.dragRow = lr;
            g.dragUniform = (wp & MK_SHIFT) != 0;
            beginDrag(hwnd, Drag::Lines, mx, my);
        } else {
            int r, c;
            if (g.grid.valid() && cellAt(mx, my, r, c)) {
                g.paintValue = g.grid.mark(r, c) ? 0 : 1;
                g.grid.mark(r, c) = g.paintValue;
                beginDrag(hwnd, Drag::Paint, mx, my);
                updateStatus();
            } else {
                beginDrag(hwnd, Drag::Pan, mx, my);
            }
        }
        invalidateCanvas();
        return 0;
    }

    case WM_RBUTTONDOWN:
        SetFocus(hwnd);
        if (g.grid.valid()) beginDrag(hwnd, Drag::MoveGrid, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MBUTTONDOWN:
        SetFocus(hwnd);
        if (!g.source.empty()) beginDrag(hwnd, Drag::Pan, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        endDrag();
        return 0;

    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd && g.drag != Drag::None) {
            g.drag = Drag::None;
            updateGridFields();
            invalidateCanvas();
        }
        return 0;

    case WM_MOUSEMOVE: {
        const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (!g.source.empty()) {
            g.cursorImgX = (int)std::floor(toImgX(mx));
            g.cursorImgY = (int)std::floor(toImgY(my));
        }
        switch (g.drag) {
        case Drag::Lines:
            if (g.dragCol >= 0) {
                const int pos = (int)std::lround(toImgX(mx));
                if (g.dragUniform) Grid::respace(g.grid.xs, g.dragCol, pos);
                else Grid::moveLine(g.grid.xs, g.dragCol, pos);
            }
            if (g.dragRow >= 0) {
                const int pos = (int)std::lround(toImgY(my));
                if (g.dragUniform) Grid::respace(g.grid.ys, g.dragRow, pos);
                else Grid::moveLine(g.grid.ys, g.dragRow, pos);
            }
            invalidateCanvas();
            break;
        case Drag::Paint: {
            int r, c;
            if (cellAt(mx, my, r, c) && g.grid.mark(r, c) != g.paintValue) {
                g.grid.mark(r, c) = g.paintValue;
                invalidateCanvas();
            }
            break;
        }
        case Drag::MoveGrid: {
            const int tx = (int)std::lround((mx - g.dragStartX) / g.zoom);
            const int ty = (int)std::lround((my - g.dragStartY) / g.zoom);
            if (tx != g.movedX || ty != g.movedY) {
                g.grid.translate(tx - g.movedX, ty - g.movedY);
                g.movedX = tx;
                g.movedY = ty;
                invalidateCanvas();
            }
            break;
        }
        case Drag::Pan:
            g.panX = g.dragStartPanX + (mx - g.dragStartX);
            g.panY = g.dragStartPanY + (my - g.dragStartY);
            invalidateCanvas();
            break;
        case Drag::None: {
            int lc, lr;
            hitLines(mx, my, lc, lr);
            if (lc != g.hoverCol || lr != g.hoverRow) {
                g.hoverCol = lc;
                g.hoverRow = lr;
                invalidateCanvas();
            }
            break;
        }
        }
        updateStatus();
        return 0;
    }

    case WM_SETCURSOR: {
        if (LOWORD(lp) != HTCLIENT) break;
        LPCWSTR cur = IDC_ARROW;
        switch (g.drag) {
        case Drag::Lines:
            cur = (g.dragCol >= 0 && g.dragRow >= 0) ? IDC_SIZEALL : (g.dragCol >= 0 ? IDC_SIZEWE : IDC_SIZENS);
            break;
        case Drag::MoveGrid:
        case Drag::Pan:
            cur = IDC_SIZEALL;
            break;
        case Drag::Paint:
            cur = IDC_HAND;
            break;
        case Drag::None:
            if (!g.source.empty()) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                int lc, lr, r, c;
                hitLines(pt.x, pt.y, lc, lr);
                if (lc >= 0 && lr >= 0) cur = IDC_SIZEALL;
                else if (lc >= 0) cur = IDC_SIZEWE;
                else if (lr >= 0) cur = IDC_SIZENS;
                else if (GetKeyState(VK_CONTROL) < 0) cur = IDC_SIZEALL;
                else if (g.grid.valid() && cellAt(pt.x, pt.y, r, c)) cur = IDC_HAND;
            }
            break;
        }
        SetCursor(LoadCursor(nullptr, cur));
        return TRUE;
    }

    case WM_MOUSEWHEEL: {
        if (g.source.empty()) return 0;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        zoomAt(g.zoom * std::pow(1.2, delta / (double)WHEEL_DELTA), pt.x, pt.y);
        return 0;
    }

    case WM_KEYDOWN: {
        const int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
        switch (wp) {
        case VK_LEFT:  nudgeGrid(-step, 0); return 0;
        case VK_RIGHT: nudgeGrid(step, 0); return 0;
        case VK_UP:    nudgeGrid(0, -step); return 0;
        case VK_DOWN:  nudgeGrid(0, step); return 0;
        case VK_ADD:
        case VK_OEM_PLUS:  zoomCentered(g.zoom * 1.25); return 0;
        case VK_SUBTRACT:
        case VK_OEM_MINUS: zoomCentered(g.zoom / 1.25); return 0;
        case 'F':          fitView(); return 0;
        }
        break;
    }

    case WM_MOUSELEAVE:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------------------------
int textWidth(HWND h, const wchar_t* text) {
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, g.font);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return sz.cx;
}

void createFonts() {
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    g.font = CreateFontIndirectW(&lf);
    LOGFONTW bold = lf;
    bold.lfWeight = FW_BOLD;
    g.boldFont = CreateFontIndirectW(&bold);
    LOGFONTW small = lf;
    small.lfHeight = lf.lfHeight * 4 / 5;
    small.lfWeight = FW_BOLD;
    g.smallFont = CreateFontIndirectW(&small);
}

// A memory DC holding a single pixel of the given colour (0x00RRGGBB), stretched by AlphaBlend as a tint.
void createTint(uint32_t rgb, HDC& dc, HBITMAP& bmp) {
    dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 1;
    bi.bmiHeader.biHeight = 1;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bits) *(uint32_t*)bits = rgb;
    SelectObject(dc, bmp);
}

void createOverlay() {
    createTint(0x0020D060, g.overlayDC, g.overlayBmp);  // green: marked cells
    createTint(0x00E03050, g.cutDC, g.cutBmp);          // red: margins cut off by the inset
}

void createControls() {
    // --- toolbar above the preview ---
    for (int i = 0; i < EFFECT_COUNT; ++i) {
        DWORD style = BS_AUTORADIOBUTTON | BS_PUSHLIKE;
        if (i == 0) style |= WS_GROUP | WS_TABSTOP;
        g.effectBtn[i] = makeCtl(L"BUTTON", effectName((Effect)i), style, IDC_EFFECT_FIRST + i);
        std::wstring shown = effectName((Effect)i);
        for (size_t p = shown.find(L"&&"); p != std::wstring::npos; p = shown.find(L"&&", p + 1)) shown.erase(p, 1);
        g.effectBtnW[i] = textWidth(g.effectBtn[i], shown.c_str()) + S(20);
    }
    SendMessageW(g.effectBtn[0], BM_SETCHECK, BST_CHECKED, 0);
    g.lblStrength = makeLabel(L"Strength");
    g.trkStrength = makeTrackbar(IDC_STRENGTH, 0, 100, 50);
    SetWindowLongPtrW(g.trkStrength, GWL_STYLE, GetWindowLongPtrW(g.trkStrength, GWL_STYLE) | WS_GROUP);
    g.lblStrengthVal = makeLabel(L"");
    g.lblHint = makeLabel(HINT_TEXT, SS_ENDELLIPSIS);

    // --- right panel ---
    g.hdrImage = makeLabel(L"IMAGE", 0, g.boldFont);
    g.btnOpen = makeButton(L"Open image...", IDC_OPEN, WS_GROUP);
    g.lblFile = makeLabel(L"No image loaded", SS_ENDELLIPSIS);
    g.btnFitView = makeButton(L"Fit view", IDC_FIT_VIEW);
    g.btnZoom100 = makeButton(L"Zoom 100 %", IDC_ZOOM_100);

    g.hdrGrid = makeLabel(L"GRID", 0, g.boldFont);
    g.fRows = makeNumField(L"Rows", IDC_ROWS, 1, 1000, 8);
    g.fCols = makeNumField(L"Columns", IDC_COLS, 1, 1000, 8);
    g.fCellW = makeNumField(L"Cell W", IDC_CELLW, 1, 100000, 32);
    g.fCellH = makeNumField(L"Cell H", IDC_CELLH, 1, 100000, 32);
    g.fOffX = makeNumField(L"Offset X", IDC_OFFX, -100000, 100000, 0);
    g.fOffY = makeNumField(L"Offset Y", IDC_OFFY, -100000, 100000, 0);
    g.btnApplyGrid = makeButton(L"Apply grid", IDC_APPLY_GRID);
    g.btnFitGrid = makeButton(L"Fit grid to image", IDC_FIT_GRID);
    g.btnMarkAll = makeButton(L"Mark all", IDC_MARK_ALL);
    g.btnClearMarks = makeButton(L"Clear marks", IDC_CLEAR_MARKS);

    g.hdrExtract = makeLabel(L"EXTRACTION", 0, g.boldFont);
    g.fOutW = makeNumField(L"Output W", IDC_OUTW, 0, 4096, 0);
    g.fOutH = makeNumField(L"Output H", IDC_OUTH, 0, 4096, 0);
    g.lblOutHint = makeLabel(L"0 = keep the cell size in pixels");
    g.lblInset = makeLabel(L"Cell inset");
    g.trkInset = makeTrackbar(IDC_INSET, 0, MAX_INSET, 0);
    SendMessageW(g.trkInset, TBM_SETPAGESIZE, 0, 4);
    g.lblInsetVal = makeLabel(L"");
    updateInsetLabel();
    g.lblThreshold = makeLabel(L"Threshold");
    g.trkThreshold = makeTrackbar(IDC_THRESHOLD, 1, 255, 128);
    g.lblThresholdVal = makeLabel(L"128");
    g.chkInvert = makeCtl(L"BUTTON", L"Invert (light pixels become ink)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_INVERT);
    g.lblName = makeLabel(L"Name");
    g.editName = makeCtl(L"EDIT", L"icon_", ES_AUTOHSCROLL | WS_TABSTOP, IDC_NAME, WS_EX_CLIENTEDGE);
    g.btnExtract = makeCtl(L"BUTTON", L"EXTRACT", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_EXTRACT, 0, g.boldFont);

    g.hdrList = makeLabel(L"PICTOGRAMS  (0)", 0, g.boldFont);
    g.list = makeCtl(L"LISTBOX", L"",
                     WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_EXTENDEDSEL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                     IDC_LIST, WS_EX_CLIENTEDGE);
    g.btnDelete = makeButton(L"Delete selected", IDC_DELETE);
    g.btnClearList = makeButton(L"Clear list", IDC_CLEAR_LIST);
    g.btnSaveC = makeButton(L"Save as C array...", IDC_SAVE_C);
    g.btnSaveBmp = makeButton(L"Save as Bitmap...", IDC_SAVE_BMP);

    // --- preview canvas & status bar ---
    g.canvas = CreateWindowExW(WS_EX_CLIENTEDGE, L"PictogramCanvas", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
                               g.hwnd, nullptr, g.hInst, nullptr);
    g.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                               g.hwnd, (HMENU)(INT_PTR)IDC_STATUS, g.hInst, nullptr);
    SendMessageW(g.status, WM_SETFONT, (WPARAM)g.font, TRUE);

    // Minimum width: the whole toolbar plus the panel must fit.
    int tb = S(6);
    for (int i = 0; i < EFFECT_COUNT; ++i) tb += g.effectBtnW[i] + S(2);
    tb += S(10) + S(62) + S(172) + S(150);
    g.minWidth = tb + S(300) + GetSystemMetrics(SM_CXSIZEFRAME) * 2;

    updateStrengthLabel();
    updateListHeader();
}

void layout() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;

    SendMessageW(g.status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g.status, &sr);
    const int statusH = sr.bottom - sr.top;

    const int panelW = S(300);
    const int canvasW = std::max(S(100), W - panelW);
    const int btnH = S(28);
    const int toolbarH = S(64);

    // toolbar
    int x = S(6);
    const int y = S(6);
    for (int i = 0; i < EFFECT_COUNT; ++i) {
        moveCtl(g.effectBtn[i], x, y, g.effectBtnW[i], btnH);
        x += g.effectBtnW[i] + S(2);
    }
    x += S(10);
    moveCtl(g.lblStrength, x, y, S(60), btnH);
    x += S(62);
    moveCtl(g.trkStrength, x, y, S(170), btnH);
    x += S(172);
    moveCtl(g.lblStrengthVal, x, y, S(150), btnH);
    moveCtl(g.lblHint, S(8), y + btnH + S(6), canvasW - S(16), S(20));
    moveCtl(g.canvas, 0, toolbarH, canvasW, std::max(S(50), H - toolbarH - statusH));

    // right panel
    const int px = canvasW + S(10);
    const int pw = panelW - S(20);
    const int half = (pw - S(8)) / 2;
    const int px2 = px + half + S(8);
    const int fieldH = S(24);
    int py = S(6);

    moveCtl(g.hdrImage, px, py, pw, S(18));               py += S(20);
    moveCtl(g.btnOpen, px, py, pw, btnH);                  py += btnH + S(4);
    moveCtl(g.lblFile, px, py, pw, S(18));                 py += S(22);
    moveCtl(g.btnFitView, px, py, half, btnH);
    moveCtl(g.btnZoom100, px2, py, half, btnH);            py += btnH + S(10);

    moveCtl(g.hdrGrid, px, py, pw, S(18));                 py += S(20);
    placeNumField(g.fRows, px, py, half, fieldH);
    placeNumField(g.fCols, px2, py, half, fieldH);         py += fieldH + S(4);
    placeNumField(g.fCellW, px, py, half, fieldH);
    placeNumField(g.fCellH, px2, py, half, fieldH);        py += fieldH + S(4);
    placeNumField(g.fOffX, px, py, half, fieldH);
    placeNumField(g.fOffY, px2, py, half, fieldH);         py += fieldH + S(6);
    moveCtl(g.btnApplyGrid, px, py, half, btnH);
    moveCtl(g.btnFitGrid, px2, py, half, btnH);            py += btnH + S(4);
    moveCtl(g.btnMarkAll, px, py, half, btnH);
    moveCtl(g.btnClearMarks, px2, py, half, btnH);         py += btnH + S(10);

    moveCtl(g.hdrExtract, px, py, pw, S(18));              py += S(20);
    placeNumField(g.fOutW, px, py, half, fieldH);
    placeNumField(g.fOutH, px2, py, half, fieldH);         py += fieldH + S(2);
    moveCtl(g.lblOutHint, px, py, pw, S(18));              py += S(20);
    moveCtl(g.lblInset, px, py, S(62), fieldH);
    moveCtl(g.trkInset, px + S(62), py, pw - S(62) - S(44), fieldH);
    moveCtl(g.lblInsetVal, px + pw - S(40), py, S(40), fieldH); py += fieldH + S(4);
    moveCtl(g.lblThreshold, px, py, S(62), fieldH);
    moveCtl(g.trkThreshold, px + S(62), py, pw - S(62) - S(36), fieldH);
    moveCtl(g.lblThresholdVal, px + pw - S(32), py, S(32), fieldH); py += fieldH + S(4);
    moveCtl(g.chkInvert, px, py, pw, S(20));               py += S(24);
    moveCtl(g.lblName, px, py, S(56), fieldH);
    moveCtl(g.editName, px + S(56), py, pw - S(56), fieldH); py += fieldH + S(6);
    moveCtl(g.btnExtract, px, py, pw, S(36));              py += S(36) + S(10);

    moveCtl(g.hdrList, px, py, pw, S(18));                 py += S(20);
    const int bottom = H - statusH - S(6);
    const int buttonsH = 2 * btnH + S(8);
    const int listH = std::max(S(40), bottom - buttonsH - py);
    moveCtl(g.list, px, py, pw, listH);                    py += listH + S(4);
    moveCtl(g.btnDelete, px, py, half, btnH);
    moveCtl(g.btnClearList, px2, py, half, btnH);          py += btnH + S(4);
    moveCtl(g.btnSaveC, px, py, half, btnH);
    moveCtl(g.btnSaveBmp, px2, py, half, btnH);

    // status bar parts
    int parts[5];
    parts[0] = S(120);
    parts[1] = parts[0] + S(100);
    parts[2] = parts[1] + S(130);
    parts[3] = parts[2] + S(200);
    parts[4] = -1;
    SendMessageW(g.status, SB_SETPARTS, 5, (LPARAM)parts);

    InvalidateRect(g.hwnd, nullptr, TRUE);
}

void drawListItem(const DRAWITEMSTRUCT* dis) {
    if (dis->itemID == (UINT)-1) {
        if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &dis->rcItem);
        return;
    }
    const size_t idx = (size_t)dis->itemData;
    if (idx >= g.pictos.size()) return;
    const Pictogram& p = g.pictos[idx];
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;

    FillRect(dc, &rc, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));

    // Thumbnail, scaled to fit while keeping the aspect ratio (integer scale when it fits).
    const int box = rc.bottom - rc.top - S(6);
    RECT boxRc = { rc.left + S(4), rc.top + S(3), rc.left + S(4) + box, rc.top + S(3) + box };
    FillRect(dc, &boxRc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    FrameRect(dc, &boxRc, GetSysColorBrush(COLOR_BTNSHADOW));
    if (p.w > 0 && p.h > 0) {
        const int inner = box - 4;
        double scale = std::min(inner / (double)p.w, inner / (double)p.h);
        if (scale >= 1.0) scale = std::floor(scale);
        const int tw = std::max(1, (int)(p.w * scale));
        const int th = std::max(1, (int)(p.h * scale));
        const int tx = boxRc.left + (box - tw) / 2;
        const int ty = boxRc.top + (box - th) / 2;

        std::vector<uint32_t> px((size_t)p.w * p.h);
        for (size_t i = 0; i < px.size(); ++i) px[i] = p.bits[i] ? 0x00000000u : 0x00FFFFFFu;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = p.w;
        bi.bmiHeader.biHeight = -p.h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, scale >= 1.0 ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, tx, ty, tw, th, 0, 0, p.w, p.h, px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }

    // Name and size
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
    HGDIOBJ oldFont = SelectObject(dc, g.font);
    RECT tr = { boxRc.right + S(8), rc.top + S(4), rc.right - S(4), rc.bottom - S(2) };
    const std::wstring text = toWide(pictogramName(idx)) + L"\n" + std::to_wstring(p.w) + L" x " + std::to_wstring(p.h) + L" px";
    DrawTextW(dc, text.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);

    if (dis->itemState & ODS_FOCUS) DrawFocusRect(dc, &rc);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        createControls();
        layout();
        updateStatus();
        setStatus(4, L"Ready");
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = g.minWidth;
        mmi->ptMinTrackSize.y = S(790);
        return 0;
    }

    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lp;
        if (mis->CtlID == IDC_LIST) {
            mis->itemHeight = S(46);
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (dis->CtlID == IDC_LIST) {
            drawListItem(dis);
            return TRUE;
        }
        break;
    }

    case WM_HSCROLL:
        if ((HWND)lp == g.trkStrength) {
            g.strength = (int)SendMessageW(g.trkStrength, TBM_GETPOS, 0, 0);
            updateStrengthLabel();
            SetTimer(hwnd, TIMER_REPROCESS, 40, nullptr);  // coalesce fast slider movement
            return 0;
        }
        if ((HWND)lp == g.trkThreshold) {
            updateThresholdLabel();
            return 0;
        }
        if ((HWND)lp == g.trkInset) {
            g.inset = (int)SendMessageW(g.trkInset, TBM_GETPOS, 0, 0);
            updateInsetLabel();
            // Show the cut-off margins on the unmarked cells for a moment; every move restarts the timer.
            g.insetPreview = true;
            SetTimer(hwnd, TIMER_INSET_PREVIEW, INSET_PREVIEW_MS, nullptr);
            invalidateCanvas();
            return 0;
        }
        break;

    case WM_TIMER:
        if (wp == TIMER_REPROCESS) {
            KillTimer(hwnd, TIMER_REPROCESS);
            reprocess();
            return 0;
        }
        if (wp == TIMER_INSET_PREVIEW) {
            KillTimer(hwnd, TIMER_INSET_PREVIEW);
            g.insetPreview = false;
            invalidateCanvas();
            return 0;
        }
        break;

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id >= IDC_EFFECT_FIRST && id < IDC_EFFECT_FIRST + EFFECT_COUNT) {
            if (code == BN_CLICKED) {
                g.effect = (Effect)(id - IDC_EFFECT_FIRST);
                updateStrengthLabel();
                reprocess();
            }
            return 0;
        }
        if (id == IDC_NAME && code == EN_CHANGE) {
            InvalidateRect(g.list, nullptr, FALSE);
            return 0;
        }
        if (code != BN_CLICKED) break;
        switch (id) {
        case IDC_OPEN:        openImage(); break;
        case IDC_FIT_VIEW:    fitView(); break;
        case IDC_ZOOM_100:    zoomCentered(1.0); break;
        case IDC_APPLY_GRID:  applyGridFromFields(); break;
        case IDC_FIT_GRID:    fitGridToImage(); break;
        case IDC_MARK_ALL:    setAllMarks(1); break;
        case IDC_CLEAR_MARKS: setAllMarks(0); break;
        case IDC_EXTRACT:     extract(); break;
        case IDC_DELETE:      deleteSelected(); break;
        case IDC_CLEAR_LIST:  clearList(); break;
        case IDC_SAVE_C:      saveC(); break;
        case IDC_SAVE_BMP:    saveBmp(); break;
        default:              return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g.hInst = hInst;
    OleInitialize(nullptr);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    if (!initImageIO()) {
        MessageBoxW(nullptr, L"GDI+ could not be initialised.", L"Pictogram Parser", MB_OK | MB_ICONERROR);
        return 1;
    }

    HDC screen = GetDC(nullptr);
    g.dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    createFonts();
    createOverlay();
    g.imgDC = CreateCompatibleDC(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CanvasProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PictogramCanvas";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = MainProc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"PictogramParserMain";
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"PictogramParserMain", L"Pictogram Parser",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(1440), S(900),
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Global shortcuts: Ctrl+O open, Ctrl+E extract.
        if (msg.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0) {
            if (msg.wParam == 'O') { openImage(); continue; }
            if (msg.wParam == 'E') { extract(); continue; }
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g.imgBmp) {
        SelectObject(g.imgDC, g.imgOldBmp);
        DeleteObject(g.imgBmp);
    }
    DeleteDC(g.imgDC);
    DeleteDC(g.overlayDC);
    DeleteObject(g.overlayBmp);
    DeleteDC(g.cutDC);
    DeleteObject(g.cutBmp);
    DeleteObject(g.font);
    DeleteObject(g.boldFont);
    DeleteObject(g.smallFont);
    shutdownImageIO();
    OleUninitialize();
    return (int)msg.wParam;
}
