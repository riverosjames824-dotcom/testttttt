// TriggerBot - High-performance color detection trigger bot
// Compile: cl /std:c++20 /O2 /Wall TriggerBot.cpp user32.lib gdi32.lib
// Or in Visual Studio: Create empty C++ project, add this file, set C++20, link user32.lib gdi32.lib
//
// Operation: Captures centered screen zone (default 40x40), scans for 4 target colors with tolerance.
// When any target color detected, simulates human-like key press with randomized delays.
// Adjust configuration constants at top to customize behavior.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>
#include <random>
#include <chrono>
#include <cstdint>

// ============================================================
// CONFIGURATION
// ============================================================
constexpr int CAPTURE_WIDTH       = 40;
constexpr int CAPTURE_HEIGHT      = 40;
constexpr int COLOR_TOLERANCE     = 10;
constexpr DWORD TARGET_KEY        = VK_SPACE;
constexpr int CAPTURE_INTERVAL_MS = 1;  // Sleep between captures (ms)

// Humanization timing ranges (milliseconds)
constexpr int DELAY_BEFORE_PRESS_MIN = 10;
constexpr int DELAY_BEFORE_PRESS_MAX = 50;
constexpr int HOLD_TIME_MIN          = 20;
constexpr int HOLD_TIME_MAX          = 80;
constexpr int COOLDOWN_MIN           = 500;
constexpr int COOLDOWN_MAX           = 1500;

struct RGB_COLOR {
    uint8_t r, g, b;
};

const RGB_COLOR TARGET_COLORS[] = {
    {222, 132, 255},
    {238, 143, 211},
    {253, 118, 255},
    {255, 150, 235}
};
constexpr size_t TARGET_COLOR_COUNT = sizeof(TARGET_COLORS) / sizeof(RGB_COLOR);

// ============================================================
// UTILITIES
// ============================================================

// Fast squared distance check (avoids sqrt)
inline bool ColorMatch(uint8_t r, uint8_t g, uint8_t b, const RGB_COLOR& target, int tolerance) {
    return (abs(int(r) - int(target.r)) <= tolerance &&
            abs(int(g) - int(target.g)) <= tolerance &&
            abs(int(b) - int(target.b)) <= tolerance);
}

// Scan captured bitmap for any target color match
// Returns true immediately on first match (early exit)
bool ScanForTargetColors(const BYTE* bits, int width, int height, int stride) {
    for (int y = 0; y < height; ++y) {
        const BYTE* row = bits + y * stride;
        for (int x = 0; x < width; ++x) {
            // Bitmap is BGRX (32bpp), so byte order: B, G, R, X
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];

            for (size_t i = 0; i < TARGET_COLOR_COUNT; ++i) {
                if (ColorMatch(r, g, b, TARGET_COLORS[i], COLOR_TOLERANCE)) {
                    return true;
                }
            }
        }
    }
    return false;
}

// High-quality random number generator (thread-local for performance)
inline std::mt19937& GetRNG() {
    thread_local std::mt19937 rng([](){
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(),
                          (uint32_t)GetTickCount64(),
                          (uint32_t)(GetCurrentThreadId() << 16)};
        return std::mt19937(seq);
    }());
    return rng;
}

inline int RandomInt(int min, int max) {
    std::uniform_int_distribution<int> dist(min, max);
    return dist(GetRNG());
}

// Sleep with high-resolution timer for precision
void PreciseSleep(int ms) {
    if (ms <= 0) return;
    auto start = std::chrono::high_resolution_clock::now();
    auto target = start + std::chrono::milliseconds(ms);
    while (std::chrono::high_resolution_clock::now() < target) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// Simulate human-like key press with randomized delays
void HumanKeyPress(DWORD vkCode) {
    // Random delay before press
    PreciseSleep(RandomInt(DELAY_BEFORE_PRESS_MIN, DELAY_BEFORE_PRESS_MAX));

    // Key down
    INPUT inputDown = {};
    inputDown.type = INPUT_KEYBOARD;
    inputDown.ki.wVk = (WORD)vkCode;
    inputDown.ki.dwFlags = 0;
    SendInput(1, &inputDown, sizeof(INPUT));

    // Random hold time
    PreciseSleep(RandomInt(HOLD_TIME_MIN, HOLD_TIME_MAX));

    // Key up
    INPUT inputUp = {};
    inputUp.type = INPUT_KEYBOARD;
    inputUp.ki.wVk = (WORD)vkCode;
    inputUp.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &inputUp, sizeof(INPUT));
}

// ============================================================
// SCREEN CAPTURE (BitBlt - lowest latency for small regions)
// ============================================================

class ScreenCapture {
    HDC hScreenDC = nullptr;
    HDC hMemDC = nullptr;
    HBITMAP hBitmap = nullptr;
    BITMAPINFO bmi = {};
    std::vector<BYTE> pixels;
    int width = 0;
    int height = 0;
    int stride = 0;

public:
    ScreenCapture() = default;

    ~ScreenCapture() {
        Cleanup();
    }

    bool Init(int w, int h) {
        width = w;
        height = h;
        stride = ((width * 32 + 31) / 32) * 4;  // 32bpp aligned to 4 bytes
        pixels.resize(stride * height);

        hScreenDC = GetDC(nullptr);
        if (!hScreenDC) return false;

        hMemDC = CreateCompatibleDC(hScreenDC);
        if (!hMemDC) return false;

        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;  // Negative = top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hBitmap = CreateDIBSection(hMemDC, &bmi, DIB_RGB_COLORS, (void**)&pixels[0], nullptr, 0);
        if (!hBitmap) return false;

        SelectObject(hMemDC, hBitmap);
        return true;
    }

    void Cleanup() {
        if (hBitmap) { DeleteObject(hBitmap); hBitmap = nullptr; }
        if (hMemDC)  { DeleteDC(hMemDC);    hMemDC  = nullptr; }
        if (hScreenDC) { ReleaseDC(nullptr, hScreenDC); hScreenDC = nullptr; }
    }

    // Capture centered screen region
    bool Capture() {
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int x = (screenW - width) / 2;
        int y = (screenH - height) / 2;

        return BitBlt(hMemDC, 0, 0, width, height, hScreenDC, x, y, SRCCOPY | CAPTUREBLT) != 0;
    }

    const BYTE* GetPixels() const { return pixels.data(); }
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
    int GetStride() const { return stride; }
};

// ============================================================
// MAIN
// ============================================================

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    ScreenCapture capture;
    if (!capture.Init(CAPTURE_WIDTH, CAPTURE_HEIGHT)) {
        return 1;
    }

    // Cooldown tracking
    auto lastTriggerTime = std::chrono::steady_clock::now();
    int currentCooldown = 0;

    // Main loop
    while (true) {
        // Check cooldown
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTriggerTime).count();

        if (elapsed >= currentCooldown) {
            // Capture screen
            if (capture.Capture()) {
                // Scan for target colors
                if (ScanForTargetColors(capture.GetPixels(),
                                        capture.GetWidth(),
                                        capture.GetHeight(),
                                        capture.GetStride())) {
                    // Color detected - trigger key press
                    HumanKeyPress(TARGET_KEY);

                    // Set new random cooldown
                    currentCooldown = RandomInt(COOLDOWN_MIN, COOLDOWN_MAX);
                    lastTriggerTime = std::chrono::steady_clock::now();
                }
            }
        }

        // Small sleep to prevent 100% CPU
        if (CAPTURE_INTERVAL_MS > 0) {
            Sleep(CAPTURE_INTERVAL_MS);
        }
    }

    // Unreachable, but cleanup for completeness
    capture.Cleanup();
    return 0;
}