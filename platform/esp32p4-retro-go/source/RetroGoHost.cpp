// FAKE-08 host backend for retro-go on ESP32-P4.
//
// Unlike every other backend in this tree, there is no operating system here and no window
// manager -- this is firmware. retro-go owns the display, the audio codec and the buttons, so
// each Host method below is a thin adapter onto an rg_* call rather than an SDL one.
//
// Two things fall out of that and shape the whole file:
//
//   * retro-go can blit an 8-bit paletted surface, which is exactly what PICO-8 produces.
//     drawFrame therefore does no colour conversion at all: it unpacks the 4bpp frame buffer
//     into one byte per pixel and hands over a palette. A cart calling pal() changes an index
//     table, not pixels, so palette swaps cost nothing here.
//
//   * The frame pacing and audio pacing belong to retro-go, which wants to be told when a
//     frame ended so it can report FPS and busy time. waitForTargetFps is where that happens.

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../../../source/host.h"
#include "../../../source/hostVmShared.h"
#include "../../../source/nibblehelpers.h"
#include "../../../source/logger.h"

// rg_system.h carries its own extern "C" guard and pulls in the display, audio, input,
// storage and gui headers with it.
#include <rg_system.h>

#include "RetroGoHost.h"

using namespace std;

static const int PicoScreenWidth = 128;
static const int PicoScreenHeight = 128;

// PICO-8 mixes at 22050Hz and FAKE-08's synth is written around that number, so the codec is
// told to match rather than resampling anything.
static const int SampleRate = 22050;

// PICO-8 indexes its screen palette with values up to 143 (16 base colours plus the 16 of the
// alternate palette at 128..143), which is why this is not 16 entries.
static const int PaletteEntries = 144;

static rg_app_t *_app = NULL;
static rg_surface_t *_surface = NULL;
static Audio *_audio = NULL;

static rg_audio_frame_t *_audioBuffer = NULL;
static size_t _audioFrames = 0;

// Set to 1 to log where a frame goes: Lua, audio synthesis, display submit, audio submit.
// Left in because that split is what found the bring-up bug written up in docs/BRINGUP.md, and
// it will be wanted again the first time a real cart runs slowly.
#define FAKE08_TIMING 0
static int64_t _tDraw, _tAudio, _tWork, _tSynth, _tFillStart;
static int _tFrames;

static int _targetFps = 60;
static int64_t _frameDeadline = 0;
static int64_t _frameStarted = 0;
static double _deltaMs = 0;
static bool _quit = false;

rg_surface_t *retrogo_host_screen(void)
{
    return _surface;
}

Host::Host(int windowWidth, int windowHeight)
{
    (void)windowWidth;
    (void)windowHeight;
    // Carts live where every other system's ROMs live, so the launcher finds them with no
    // special case: /sd/roms/p8.
    _cartDirectory = RG_BASE_PATH_ROMS "/p8";
}

void Host::oneTimeSetup(Audio *audio)
{
    _app = rg_system_get_app();
    _audio = audio;

    _surface = rg_surface_create(PicoScreenWidth, PicoScreenHeight, RG_PIXEL_PAL565_LE, MEM_FAST);
    RG_ASSERT(_surface, "Failed to allocate the PICO-8 screen surface");

    // The 144 colours are fixed for the life of the process; only the index table a cart
    // points at them through changes. So this conversion happens once.
    Color *colors = GetPaletteColors();
    for (int i = 0; i < PaletteEntries; ++i)
        _surface->palette[i] = ((colors[i].Red & 0xF8) << 8) | ((colors[i].Green & 0xFC) << 3) | (colors[i].Blue >> 3);

    // One frame's worth of stereo frames. Submitting roughly what a frame consumes keeps the
    // emulator paced by audio without the buffer either starving or running away.
    _audioFrames = SampleRate / _targetFps;
    _audioBuffer = (rg_audio_frame_t *)calloc(_audioFrames, sizeof(rg_audio_frame_t));
    RG_ASSERT(_audioBuffer, "Failed to allocate the audio buffer");

    rg_audio_set_sample_rate(SampleRate);

    _frameDeadline = rg_system_timer();
    _frameStarted = _frameDeadline;

    RG_LOGI("FAKE-08 host ready: %dx%d paletted, %dHz, carts in %s", PicoScreenWidth, PicoScreenHeight, SampleRate,
            _cartDirectory.c_str());
}

void Host::oneTimeCleanup()
{
    rg_surface_free(_surface);
    _surface = NULL;
    free(_audioBuffer);
    _audioBuffer = NULL;
}

void Host::setTargetFps(int targetFps)
{
    // Carts declare 30 or 60 by which update function they define, and the audio buffer is
    // sized per frame, so both have to move together.
    _targetFps = targetFps > 0 ? targetFps : 60;

    size_t frames = SampleRate / _targetFps;
    if (frames != _audioFrames && _audioBuffer)
    {
        rg_audio_frame_t *resized = (rg_audio_frame_t *)realloc(_audioBuffer, frames * sizeof(rg_audio_frame_t));
        if (resized)
        {
            _audioBuffer = resized;
            _audioFrames = frames;
        }
    }
}

// Scaling is retro-go's business -- it has its own aspect ratio setting that applies to every
// emulator on the device, and a PICO-8 specific stretch control would only disagree with it.
void Host::changeStretch()
{
}

void Host::forceStretch(StretchOption newStretch)
{
    (void)newStretch;
}

InputState_t Host::scanInput()
{
    uint32_t keys = rg_input_read_gamepad();

    // MENU is retro-go's own chord and belongs to it: volume, brightness, quitting back to the
    // launcher. Handled here rather than passed to the cart, and the cart is not told the
    // frame took several seconds either -- see waitForTargetFps.
    if (keys & (RG_KEY_MENU | RG_KEY_OPTION))
    {
        rg_gui_game_menu();
        _frameDeadline = rg_system_timer();
        keys = 0;
    }

    uint8_t held = 0;
    if (keys & RG_KEY_LEFT)
        held |= P8_KEY_LEFT;
    if (keys & RG_KEY_RIGHT)
        held |= P8_KEY_RIGHT;
    if (keys & RG_KEY_UP)
        held |= P8_KEY_UP;
    if (keys & RG_KEY_DOWN)
        held |= P8_KEY_DOWN;
    // A is the button under the thumb on a GBA, so it gets PICO-8's primary (O). B is X.
    if (keys & RG_KEY_A)
        held |= P8_KEY_O;
    if (keys & RG_KEY_B)
        held |= P8_KEY_X;
    if (keys & RG_KEY_START)
        held |= P8_KEY_PAUSE;

    // KDown means "newly pressed this frame", which the cart's btnp() and FAKE-08's own menus
    // both rely on. rg_input reports state, not edges, so the edge is computed here.
    static uint8_t previous = 0;
    uint8_t down = held & ~previous;
    previous = held;

    InputState_t state = {};
    state.KDown = down;
    state.KHeld = held;
    return state;
}

bool Host::shouldQuit()
{
    return _quit;
}

void Host::waitForTargetFps()
{
    int64_t now = rg_system_timer();
    int64_t frameTime = 1000000 / _targetFps;

    // Tell retro-go how much of the frame was spent working before we sleep. This is what
    // feeds the BUSY/FPS line in the log, which is the only way to see how a cart is doing on
    // a device with no display attached yet.
    rg_system_tick(now - _frameStarted);

    _deltaMs = (now - _frameStarted) / 1000.0;
    _frameDeadline += frameTime;

    // A frame that overran (or a menu that took a second) must not leave the deadline in the
    // past, or every following frame tries to catch up and the cart runs fast.
    if (_frameDeadline < now)
        _frameDeadline = now;
    else
        rg_usleep(_frameDeadline - now);

#if FAKE08_TIMING
    // Everything between two frames' ends, minus the two pieces measured above, is Step() --
    // the cart's own _update and _draw plus the audio synthesis.
    _tWork += now - _frameStarted;
    if (++_tFrames >= 10)
    {
        RG_LOGI("10 frames: work %dms = lua %dms + synth %dms + submit %dms + audio %dms", (int)(_tWork / 1000),
                (int)((_tWork - _tDraw - _tAudio - _tSynth) / 1000), (int)(_tSynth / 1000), (int)(_tDraw / 1000),
                (int)(_tAudio / 1000));
        _tWork = _tDraw = _tAudio = _tSynth = 0;
        _tFrames = 0;
    }
#endif
    _frameStarted = rg_system_timer();
}

double Host::deltaTMs()
{
    return _deltaMs;
}

void Host::drawFrame(uint8_t *picoFb, uint8_t *screenPaletteMap, uint8_t drawMode)
{
    // drawMode carries PICO-8's 64x64 and mirrored screen modes. Not handled yet: the common
    // case is mode 0, and a cart using another one draws its top-left quarter rather than
    // nothing at all.
    (void)drawMode;

    uint8_t *dest = (uint8_t *)_surface->data;

    for (int y = 0; y < PicoScreenHeight; ++y)
    {
        for (int x = 0; x < PicoScreenWidth; ++x)
        {
            uint8_t index = getPixelNibble(x, y, picoFb);
            // 0x8f keeps the alternate-palette bit and the colour, which is the range the
            // palette built in oneTimeSetup covers.
            *dest++ = screenPaletteMap[index] & 0x8f;
        }
    }

#if FAKE08_TIMING
    int64_t started = rg_system_timer();
#endif
    rg_display_submit(_surface, 0);
#if FAKE08_TIMING
    _tDraw += rg_system_timer() - started;
#endif
}

bool Host::shouldFillAudioBuff()
{
#if FAKE08_TIMING
    // GameLoop calls this, then FillAudioBuffer, then playFilledAudioBuffer. So the gap
    // between here and there is the synthesis itself -- the part written with doubles.
    _tFillStart = rg_system_timer();
#endif
    return _audioBuffer != NULL;
}

void *Host::getAudioBufferPointer()
{
    return _audioBuffer;
}

size_t Host::getAudioBufferSize()
{
    // Counted in stereo frames: Audio::FillAudioBuffer writes one uint32 per unit, which is
    // exactly one rg_audio_frame_t.
    return _audioFrames;
}

void Host::playFilledAudioBuffer()
{
#if FAKE08_TIMING
    int64_t started = rg_system_timer();
    _tSynth += started - _tFillStart;
#endif
    rg_audio_submit(_audioBuffer, _audioFrames);
#if FAKE08_TIMING
    _tAudio += rg_system_timer() - started;
#endif
}

bool Host::shouldRunMainLoop()
{
    return !_quit;
}

vector<string> Host::listcarts()
{
    vector<string> carts;

    DIR *dir = opendir(_cartDirectory.c_str());
    if (!dir)
    {
        RG_LOGW("No cart directory at %s", _cartDirectory.c_str());
        return carts;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;
        carts.push_back(_cartDirectory + "/" + entry->d_name);
    }
    closedir(dir);

    return carts;
}

std::vector<std::string> Host::listdirs()
{
    std::vector<std::string> dirs;

    DIR *dir = opendir(_cartDirectory.c_str());
    if (!dir)
        return dirs;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;
        std::string full = _cartDirectory + "/" + entry->d_name;
        DIR *test = opendir(full.c_str());
        if (test)
        {
            closedir(test);
            dirs.push_back(entry->d_name);
        }
    }
    closedir(dir);

    return dirs;
}

const char *Host::logFilePrefix()
{
    // Empty: a log file written to the card on every boot is flash wear for something the
    // serial console already shows.
    return "";
}

std::string Host::customBiosLua()
{
    // Shown by the built-in BIOS cart, so it should describe this device's buttons rather
    // than a keyboard's.
    return "cartpath = \"roms/p8/\"\n"
           "selectbtn = \"a\"\n"
           "pausebtn = \"start\"\n"
           "exitbtn = \"menu\"\n"
           "sizebtn = \"\"";
}

std::string Host::getCartDirectory()
{
    return _cartDirectory;
}

void Host::overrideLogFilePrefix(const char *newPrefix)
{
    (void)newPrefix;
}

void Host::setPlatformParams(int windowWidth, int windowHeight, uint32_t sdlWindowFlags, uint32_t sdlRendererFlags,
                             uint32_t sdlPixelFormat, std::string logFilePrefix, std::string customBiosLua,
                             std::string cartDirectory)
{
    (void)windowWidth;
    (void)windowHeight;
    (void)sdlWindowFlags;
    (void)sdlRendererFlags;
    (void)sdlPixelFormat;
    (void)logFilePrefix;
    (void)customBiosLua;
    if (cartDirectory.length() > 0)
        _cartDirectory = cartDirectory;
}
