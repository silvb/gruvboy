// Phase 0 test app: plays a click when a face button is pressed and reports
// audio timing, so that press-to-sound latency can be measured on the device.
#include "SDL.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <sched.h>
#include <signal.h>

#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

constexpr int kBufferSizes[] = {128, 256, 512, 1024, 2048};
constexpr int kNumBufferSizes = sizeof(kBufferSizes) / sizeof(kBufferSizes[0]);
constexpr int kRates[] = {48000, 44100};
constexpr int kNumRates = sizeof(kRates) / sizeof(kRates[0]);
constexpr int kAudioPriority = 70;
constexpr float kPi = 3.14159265f;

enum Button : uint32_t {
    kA = 1u << 0,
    kB = 1u << 1,
    kX = 1u << 2,
    kY = 1u << 3,
    kL = 1u << 4,
    kR = 1u << 5,
    kUp = 1u << 6,
    kDown = 1u << 7,
    kLeft = 1u << 8,
    kRight = 1u << 9,
    kStart = 1u << 10,
    kSelect = 1u << 11,
};
constexpr uint32_t kPads = kA | kB | kX | kY;

std::atomic<bool> g_running{true};
std::atomic<uint32_t> g_held{0};

// Written by the input thread, read by the audio callback.
std::atomic<uint32_t> g_pressCount{0};
std::atomic<uint32_t> g_pressTimeUs{0};

// Written by the audio callback, read by the main thread.
std::atomic<uint32_t> g_pressLatencyUs{0};
std::atomic<uint32_t> g_pressLatencyMaxUs{0};
std::atomic<uint32_t> g_gapMaxUs{0};
std::atomic<uint32_t> g_renderMaxUs{0};
std::atomic<uint32_t> g_lateCallbacks{0};
std::atomic<uint32_t> g_callbacks{0};
std::atomic<int> g_realtimeResult{-1};
std::atomic<bool> g_toneOn{false};

struct AudioState {
    int rate = 48000;
    int channels = 2;
    int frames = 256;
    uint32_t seenPress = 0;
    uint32_t lastCallbackUs = 0;
    bool hasLastCallback = false;
    bool priorityRequested = false;
    int clickPos = -1;
    float clickPhase = 0.0f;
    float tonePhase = 0.0f;
} g_audio;

uint32_t nowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000);
}

void raiseMax(std::atomic<uint32_t>& target, uint32_t value) {
    if (value > target.load(std::memory_order_relaxed)) {
        target.store(value, std::memory_order_relaxed);
    }
}

int requestRealtime(int priority) {
#ifdef __linux__
    sched_param param{};
    param.sched_priority = priority;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#else
    (void)priority;
    return -1;
#endif
}

void audioCallback(void*, Uint8* stream, int bytes) {
    AudioState& a = g_audio;
    const uint32_t start = nowUs();

    if (!a.priorityRequested) {
        a.priorityRequested = true;
        g_realtimeResult.store(requestRealtime(kAudioPriority), std::memory_order_relaxed);
    }

    const float bufferUs = 1e6f * a.frames / a.rate;
    if (a.hasLastCallback) {
        const uint32_t gap = start - a.lastCallbackUs;
        raiseMax(g_gapMaxUs, gap);
        if (gap > bufferUs * 1.5f) {
            g_lateCallbacks.fetch_add(1, std::memory_order_relaxed);
        }
    }
    a.lastCallbackUs = start;
    a.hasLastCallback = true;

    const uint32_t press = g_pressCount.load(std::memory_order_acquire);
    if (press != a.seenPress) {
        a.seenPress = press;
        a.clickPos = 0;
        a.clickPhase = 0.0f;
        const uint32_t latency = start - g_pressTimeUs.load(std::memory_order_relaxed);
        g_pressLatencyUs.store(latency, std::memory_order_relaxed);
        raiseMax(g_pressLatencyMaxUs, latency);
    }

    const bool toneOn = g_toneOn.load(std::memory_order_relaxed);
    const int clickLength = a.rate / 100;
    const float clickStep = 2.0f * kPi * 2000.0f / a.rate;
    const float toneStep = 2.0f * kPi * 440.0f / a.rate;

    auto* out = reinterpret_cast<int16_t*>(stream);
    const int frames = bytes / (sizeof(int16_t) * a.channels);
    for (int i = 0; i < frames; i++) {
        float sample = 0.0f;
        if (a.clickPos >= 0) {
            // Starting at the sine's peak gives a sharp onset that is easy to
            // find in a recording.
            const float env = 1.0f - static_cast<float>(a.clickPos) / clickLength;
            sample += 0.7f * env * env * std::cos(a.clickPhase);
            a.clickPhase += clickStep;
            if (++a.clickPos >= clickLength) {
                a.clickPos = -1;
            }
        }
        if (toneOn) {
            sample += 0.15f * std::sin(a.tonePhase);
            a.tonePhase += toneStep;
            if (a.tonePhase > 2.0f * kPi) {
                a.tonePhase -= 2.0f * kPi;
            }
        }
        const auto value = static_cast<int16_t>(sample * 32767.0f);
        for (int c = 0; c < a.channels; c++) {
            *out++ = value;
        }
    }

    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    raiseMax(g_renderMaxUs, nowUs() - start);
}

// Called from whichever thread currently owns input.
void onButton(uint32_t button, bool pressed, uint32_t timeUs) {
    if (pressed) {
        g_held.fetch_or(button, std::memory_order_relaxed);
        if ((button & kPads) != 0) {
            g_pressTimeUs.store(timeUs, std::memory_order_relaxed);
            g_pressCount.fetch_add(1, std::memory_order_release);
        }
    } else {
        g_held.fetch_and(~button, std::memory_order_relaxed);
    }
}

#ifdef __linux__
constexpr int kInputPriority = 60;
std::atomic<bool> g_evdevActive{false};
std::atomic<bool> g_evdevEnabled{true};

uint32_t evdevButton(int code) {
    switch (code) {
        case KEY_SPACE: return kA;
        case KEY_LEFTCTRL: return kB;
        case KEY_LEFTSHIFT: return kX;
        case KEY_LEFTALT: return kY;
        case KEY_E: return kL;
        case KEY_T: return kR;
        case KEY_UP: return kUp;
        case KEY_DOWN: return kDown;
        case KEY_LEFT: return kLeft;
        case KEY_RIGHT: return kRight;
        case KEY_ENTER: return kStart;
        case KEY_RIGHTCTRL: return kSelect;
        default: return 0;
    }
}

// Reads buttons directly, so that input does not wait for the UI thread. Does
// not grab the device, which leaves Onion's key monitor working.
void* evdevThread(void*) {
    const int fd = open("/dev/input/event0", O_RDONLY);
    if (fd < 0) {
        return nullptr;
    }
    int clock = CLOCK_MONOTONIC;
    const bool monotonic = ioctl(fd, EVIOCSCLOCKID, &clock) == 0;
    requestRealtime(kInputPriority);
    g_evdevActive.store(true);

    pollfd pfd{fd, POLLIN, 0};
    while (g_running.load(std::memory_order_relaxed)) {
        if (poll(&pfd, 1, 100) <= 0) {
            continue;
        }
        input_event ev;
        if (read(fd, &ev, sizeof(ev)) != sizeof(ev) || ev.type != EV_KEY || ev.value == 2) {
            continue;
        }
        const uint32_t button = evdevButton(ev.code);
        if (button != 0 && g_evdevEnabled.load(std::memory_order_relaxed)) {
            const uint32_t time =
                monotonic ? static_cast<uint32_t>(ev.time.tv_sec * 1000000ull + ev.time.tv_usec)
                          : nowUs();
            onButton(button, ev.value == 1, time);
        }
    }
    close(fd);
    return nullptr;
}
#endif

uint32_t sdlButton(SDLKey key) {
    switch (key) {
        case SDLK_SPACE: return kA;
        case SDLK_LCTRL: return kB;
        case SDLK_LSHIFT: return kX;
        case SDLK_LALT: return kY;
        case SDLK_e: return kL;
        case SDLK_t: return kR;
        case SDLK_UP: return kUp;
        case SDLK_DOWN: return kDown;
        case SDLK_LEFT: return kLeft;
        case SDLK_RIGHT: return kRight;
        case SDLK_RETURN: return kStart;
        case SDLK_RCTRL: return kSelect;
#ifdef __APPLE__
        case SDLK_BACKSPACE: return kSelect;  // Most Mac keyboards have no right Ctrl.
#endif
        default: return 0;
    }
}

// Classic 5x7 font covering ASCII 32-95. Each byte is one column.
const uint8_t kFont[64][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40},
};

constexpr int kScale = 3;
constexpr int kRowHeight = 24;
constexpr int kMarginX = 14;

void drawText(SDL_Surface* screen, int x, int y, Uint32 color, const char* text) {
    for (; *text != '\0'; text++, x += 6 * kScale) {
        int c = static_cast<unsigned char>(*text);
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        if (c < 32 || c > 95) {
            continue;
        }
        const uint8_t* glyph = kFont[c - 32];
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if ((glyph[col] >> row) & 1) {
                    SDL_Rect r;
                    r.x = static_cast<Sint16>(x + col * kScale);
                    r.y = static_cast<Sint16>(y + row * kScale);
                    r.w = kScale;
                    r.h = kScale;
                    SDL_FillRect(screen, &r, color);
                }
            }
        }
    }
}

struct AppState {
    int bufferIndex = 1;
    int rateIndex = 0;
    bool audioOpen = false;
    int obtainedFrames = 0;
    int obtainedRate = 0;
    int obtainedChannels = 0;
    char driver[32] = "NONE";
    char audioError[64] = "";
    bool uiStress = false;
    bool useEvdev = false;
    uint32_t gapShownUs = 0;
    uint32_t renderShownUs = 0;
    FILE* log = nullptr;
};

void logLine(AppState& app, const char* format, ...) {
    if (app.log == nullptr) {
        return;
    }
    va_list args;
    va_start(args, format);
    vfprintf(app.log, format, args);
    va_end(args);
    fputc('\n', app.log);
    fflush(app.log);
}

void resetStats() {
    g_pressLatencyUs = 0;
    g_pressLatencyMaxUs = 0;
    g_gapMaxUs = 0;
    g_renderMaxUs = 0;
    g_lateCallbacks = 0;
}

void openAudio(AppState& app) {
    if (app.audioOpen) {
        SDL_CloseAudio();
        app.audioOpen = false;
    }
    SDL_AudioSpec desired{};
    SDL_AudioSpec obtained{};
    desired.freq = kRates[app.rateIndex];
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = static_cast<Uint16>(kBufferSizes[app.bufferIndex]);
    desired.callback = audioCallback;

    const uint32_t seen = g_pressCount.load();
    g_audio = AudioState{};
    g_audio.seenPress = seen;

    if (SDL_OpenAudio(&desired, &obtained) < 0) {
        snprintf(app.audioError, sizeof(app.audioError), "%s", SDL_GetError());
        logLine(app, "audio open failed: %s", SDL_GetError());
        return;
    }
    app.audioError[0] = '\0';
    app.obtainedFrames = obtained.samples;
    app.obtainedRate = obtained.freq;
    app.obtainedChannels = obtained.channels;
    g_audio.rate = obtained.freq;
    g_audio.channels = obtained.channels;
    g_audio.frames = obtained.samples;
    if (SDL_AudioDriverName(app.driver, sizeof(app.driver)) == nullptr) {
        snprintf(app.driver, sizeof(app.driver), "UNKNOWN");
    }
    app.audioOpen = true;
    resetStats();
    SDL_PauseAudio(0);
    logLine(app, "audio open: driver=%s requested=%d/%d obtained=%d/%d channels=%d format=0x%x",
            app.driver, desired.samples, desired.freq, obtained.samples, obtained.freq,
            obtained.channels, obtained.format);
}

void render(SDL_Surface* screen, const AppState& app, uint32_t hits) {
    const Uint32 background = SDL_MapRGB(screen->format, 0x1D, 0x20, 0x21);
    const Uint32 text = SDL_MapRGB(screen->format, 0xEB, 0xDB, 0xB2);
    const Uint32 accent = SDL_MapRGB(screen->format, 0xFA, 0xBD, 0x2F);
    const Uint32 error = SDL_MapRGB(screen->format, 0xFB, 0x49, 0x34);
    SDL_FillRect(screen, nullptr, background);

    char line[64];
    int y = 12;
    auto row = [&](Uint32 color) {
        drawText(screen, kMarginX, y, color, line);
        y += kRowHeight;
    };

    snprintf(line, sizeof(line), "GRUVBOY CLICK TEST");
    row(accent);
    snprintf(line, sizeof(line), "DRIVER %s", app.driver);
    row(text);
    if (app.audioOpen) {
        snprintf(line, sizeof(line), "BUFFER %d/%d = %.1f MS", kBufferSizes[app.bufferIndex],
                 app.obtainedFrames, 1000.0 * app.obtainedFrames / app.obtainedRate);
        row(text);
        snprintf(line, sizeof(line), "RATE %d  CHANNELS %d", app.obtainedRate,
                 app.obtainedChannels);
        row(text);
    } else {
        snprintf(line, sizeof(line), "AUDIO FAILED");
        row(error);
        snprintf(line, sizeof(line), "%.32s", app.audioError);
        row(error);
    }
    snprintf(line, sizeof(line), "INPUT %s  RT %s", app.useEvdev ? "EVDEV" : "SDL",
             g_realtimeResult.load() == 0 ? "YES" : "NO");
    row(text);
    y += 8;
    snprintf(line, sizeof(line), "PRESS>AUDIO %.1f MAX %.1f", g_pressLatencyUs.load() / 1000.0,
             g_pressLatencyMaxUs.load() / 1000.0);
    row(accent);
    snprintf(line, sizeof(line), "CB GAP MAX %.1f MS", app.gapShownUs / 1000.0);
    row(text);
    snprintf(line, sizeof(line), "CB RENDER MAX %.2f MS", app.renderShownUs / 1000.0);
    row(text);
    snprintf(line, sizeof(line), "LATE CALLBACKS %u", g_lateCallbacks.load());
    row(g_lateCallbacks.load() == 0 ? text : error);
    snprintf(line, sizeof(line), "HITS %u", hits);
    row(text);
    snprintf(line, sizeof(line), "TONE %s  UI STRESS %s", g_toneOn.load() ? "ON" : "OFF",
             app.uiStress ? "ON" : "OFF");
    row(text);
    y += 8;
    snprintf(line, sizeof(line), "PADS CLICK     L/R BUFFER");
    row(accent);
    snprintf(line, sizeof(line), "LEFT/RIGHT RATE  UP STRESS");
    row(accent);
    snprintf(line, sizeof(line), "DOWN INPUT     START TONE");
    row(accent);
    snprintf(line, sizeof(line), "SELECT+START QUIT");
    row(accent);
    SDL_Flip(screen);
}

void handleSignal(int) {
    g_running.store(false);
}

// Exercises the click and UI without audio or input hardware.
int selftest(const char* outPath) {
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Surface* screen = SDL_SetVideoMode(640, 480, 32, SDL_SWSURFACE);
    if (screen == nullptr) {
        fprintf(stderr, "SDL_SetVideoMode failed: %s\n", SDL_GetError());
        return 1;
    }

    int16_t buffer[512];
    auto peakOf = [&](int from, int to) {
        int peak = 0;
        for (int i = from; i < to; i++) {
            const int v = buffer[i] < 0 ? -buffer[i] : buffer[i];
            peak = v > peak ? v : peak;
        }
        return peak;
    };
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    };

    g_audio.frames = 256;
    g_audio.rate = 48000;
    audioCallback(nullptr, reinterpret_cast<Uint8*>(buffer), sizeof(buffer));
    check(peakOf(0, 512) == 0, "silent before any press");

    onButton(kA, true, nowUs());
    audioCallback(nullptr, reinterpret_cast<Uint8*>(buffer), sizeof(buffer));
    check(buffer[0] > 20000, "click starts on the first sample");
    check(buffer[0] == buffer[1], "click is identical in both channels");
    check(g_held.load() == kA, "pad press is held");

    audioCallback(nullptr, reinterpret_cast<Uint8*>(buffer), sizeof(buffer));
    check(peakOf(0, 512) > 0 && peakOf(0, 512) < 20000, "click decays in the next buffer");
    audioCallback(nullptr, reinterpret_cast<Uint8*>(buffer), sizeof(buffer));
    check(peakOf(0, 512) == 0, "click ends after 10 ms");

    onButton(kA, false, nowUs());
    onButton(kL, true, nowUs());
    audioCallback(nullptr, reinterpret_cast<Uint8*>(buffer), sizeof(buffer));
    check(peakOf(0, 512) == 0, "shoulder button does not click");
    check(g_held.load() == kL, "pad release is tracked");
    onButton(kL, false, nowUs());

    g_toneOn.store(true);
    audioCallback(nullptr, reinterpret_cast<Uint8*>(buffer), sizeof(buffer));
    check(peakOf(0, 512) > 4000 && peakOf(0, 512) < 6000, "tone plays at the expected level");
    g_toneOn.store(false);

    AppState app;
    snprintf(app.driver, sizeof(app.driver), "SELFTEST");
    app.audioOpen = true;
    app.obtainedFrames = 256;
    app.obtainedRate = 48000;
    app.obtainedChannels = 2;
    render(screen, app, 42);
    check(SDL_SaveBMP(screen, outPath) == 0, "screen saved");

    SDL_Quit();
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "--selftest") == 0) {
        return selftest(argv[2]);
    }

    AppState app;
    app.log = fopen("clicktest.log", "w");

    signal(SIGTERM, handleSignal);
    signal(SIGINT, handleSignal);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        logLine(app, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Surface* screen = SDL_SetVideoMode(640, 480, 32, SDL_HWSURFACE);
    if (screen == nullptr) {
        logLine(app, "SDL_SetVideoMode failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    logLine(app, "video: %dx%d bpp=%d", screen->w, screen->h, screen->format->BitsPerPixel);

#ifdef __linux__
    pthread_t inputThread;
    const bool inputThreadStarted = pthread_create(&inputThread, nullptr, evdevThread, nullptr) == 0;
    SDL_Delay(50);
    app.useEvdev = g_evdevActive.load();
#endif
    logLine(app, "input: %s", app.useEvdev ? "evdev" : "sdl");

    openAudio(app);

    uint32_t previousHeld = 0;
    uint32_t hits = 0;
    uint32_t seenPress = g_pressCount.load();
    uint32_t lastRenderUs = nowUs();
    uint32_t lastStatsUs = lastRenderUs;
    bool dirty = true;

    while (g_running.load(std::memory_order_relaxed)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                g_running.store(false);
            } else if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) && !app.useEvdev) {
                const uint32_t button = sdlButton(event.key.keysym.sym);
                if (button != 0) {
                    onButton(button, event.type == SDL_KEYDOWN, nowUs());
                }
            }
        }

        const uint32_t held = g_held.load(std::memory_order_relaxed);
        const uint32_t pressed = held & ~previousHeld;
        previousHeld = held;

        if ((pressed & kStart) != 0 && (held & kSelect) != 0) {
            g_running.store(false);
        } else if ((pressed & kStart) != 0) {
            g_toneOn.store(!g_toneOn.load());
            dirty = true;
        }

        int bufferIndex = app.bufferIndex;
        if ((pressed & kL) != 0 && bufferIndex > 0) bufferIndex--;
        if ((pressed & kR) != 0 && bufferIndex < kNumBufferSizes - 1) bufferIndex++;
        int rateIndex = app.rateIndex;
        if ((pressed & (kLeft | kRight)) != 0) rateIndex = (rateIndex + 1) % kNumRates;
        if (bufferIndex != app.bufferIndex || rateIndex != app.rateIndex) {
            app.bufferIndex = bufferIndex;
            app.rateIndex = rateIndex;
            openAudio(app);
            dirty = true;
        }

        if ((pressed & kUp) != 0) {
            app.uiStress = !app.uiStress;
            logLine(app, "ui stress: %s", app.uiStress ? "on" : "off");
            dirty = true;
        }
#ifdef __linux__
        if ((pressed & kDown) != 0 && g_evdevActive.load()) {
            app.useEvdev = !app.useEvdev;
            g_evdevEnabled.store(app.useEvdev);
            g_held.store(0);
            logLine(app, "input: %s", app.useEvdev ? "evdev" : "sdl");
            dirty = true;
        }
#endif

        const uint32_t pressCount = g_pressCount.load();
        if (pressCount != seenPress) {
            hits += pressCount - seenPress;
            seenPress = pressCount;
            dirty = true;
        }

        const uint32_t now = nowUs();
        if (now - lastStatsUs >= 1000000) {
            lastStatsUs = now;
            app.gapShownUs = g_gapMaxUs.exchange(0);
            app.renderShownUs = g_renderMaxUs.exchange(0);
            logLine(app,
                    "stats: buffer=%d rate=%d input=%s stress=%d callbacks=%u gap_max_us=%u "
                    "render_max_us=%u late=%u press_us=%u press_max_us=%u hits=%u",
                    app.obtainedFrames, app.obtainedRate, app.useEvdev ? "evdev" : "sdl",
                    app.uiStress, g_callbacks.exchange(0), app.gapShownUs, app.renderShownUs,
                    g_lateCallbacks.load(), g_pressLatencyUs.load(), g_pressLatencyMaxUs.load(),
                    hits);
            dirty = true;
        }

        if (app.uiStress || (dirty && now - lastRenderUs >= 33000)) {
            render(screen, app, hits);
            lastRenderUs = now;
            dirty = false;
        }
        if (!app.uiStress) {
            SDL_Delay(1);
        }
    }

    logLine(app, "exiting");
    if (app.audioOpen) {
        SDL_CloseAudio();
    }
#ifdef __linux__
    if (inputThreadStarted) {
        pthread_join(inputThread, nullptr);
    }
#endif
    SDL_Quit();
    if (app.log != nullptr) {
        fclose(app.log);
    }
    return 0;
}
