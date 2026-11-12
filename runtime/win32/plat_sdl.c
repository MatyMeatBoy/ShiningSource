/* Platform services through SDL3: window, presentation, keyboard and controller, sound output, frame pacing.
 * Used natively by the dev32 build and by the 64-bit host; the game side only sees the ss_plat_* calls. */
#include "shiningsource/runtime.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_Gamepad *gamepad;
static SDL_AudioStream *audio;
static uint64_t next_frame_ns;

/* GBA buttons, bit order of KEYINPUT: A B Select Start Right Left Up Down R L */
static const SDL_Scancode keys[10] = {
    SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_V, SDL_SCANCODE_C,
};

int ss_plat_video_init(const char *title)
{
    SDL_AudioSpec spec;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = 48000;
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) {
        const char *volume = getenv("SS_VOLUME");      /* 0..100, set by the launcher */
        if (volume)
            SDL_SetAudioStreamGain(audio, (float)atoi(volume) / 100.0f);
        SDL_ResumeAudioStreamDevice(audio);
    }
    else
        fprintf(stderr, "no audio device: %s\n", SDL_GetError());
    if (!SDL_CreateWindowAndRenderer(title, SS_SCREEN_W * 4, SS_SCREEN_H * 4, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return -1;
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SS_MAX_VIEW_W, SS_SCREEN_H);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    if (getenv("SS_FULLSCREEN"))
        SDL_SetWindowFullscreen(window, true);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);                /* come to the front even when started from a console or a dialog */
    next_frame_ns = SDL_GetTicksNS();
    return 0;
}

void ss_plat_audio(const int16_t *samples, int frames)
{
    /* never let more than about a tenth of a second queue up: late sound is worse than a dropped frame */
    if (audio && SDL_GetAudioStreamQueued(audio) < 48000 * 4 / 10)
        SDL_PutAudioStreamData(audio, samples, frames * 4);
}

/* The picture keeps a whole-number scale (the largest that fits the height of the window) and takes as many
 * columns as the width then allows, so a 16:9 window shows 284 columns and an ultrawide one up to 480. */
int ss_plat_view_width(void)
{
    int w = 0, h = 0, scale, width;
    if (!window || !SDL_GetWindowSizeInPixels(window, &w, &h) || h < SS_SCREEN_H)
        return SS_SCREEN_W;
    scale = h / SS_SCREEN_H;
    width = (w / scale) & ~1;
    if (width < SS_SCREEN_W) width = SS_SCREEN_W;
    if (width > SS_MAX_VIEW_W) width = SS_MAX_VIEW_W;
    return width;
}

/* Show one `width` x 160 ARGB frame, wait until the next 1/59.7275 s, and return the buttons held (bit order above). */
uint16_t ss_plat_present(const uint32_t *pixels, int width)
{
    static int shown_width;
    SDL_FRect source = { 0, 0, (float)width, SS_SCREEN_H };
    SDL_Rect update = { 0, 0, width, SS_SCREEN_H };
    const bool *state;
    uint16_t held = 0;
    uint64_t now;
    SDL_Event event;
    int i;

    if (!window)
        return 0;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE))
            exit(0);
        if (event.type == SDL_EVENT_GAMEPAD_ADDED && !gamepad)
            gamepad = SDL_OpenGamepad(event.gdevice.which);
        if (event.type == SDL_EVENT_GAMEPAD_REMOVED && gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
            SDL_CloseGamepad(gamepad);
            gamepad = NULL;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F11)
            SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
    }
    state = SDL_GetKeyboardState(NULL);
    for (i = 0; i < 10; i++)
        if (state[keys[i]])
            held |= (uint16_t)(1 << i);
    if (gamepad) {
        /* same bit order as `keys`; the east button is A as on the console's layout, the stick acts as the d-pad */
        static const SDL_GamepadButton buttons[10] = {
            SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
            SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
            SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        };
        int x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        for (i = 0; i < 10; i++)
            if (SDL_GetGamepadButton(gamepad, buttons[i]))
                held |= (uint16_t)(1 << i);
        if (x > 12000) held |= 1 << 4;
        if (x < -12000) held |= 1 << 5;
        if (y < -12000) held |= 1 << 6;
        if (y > 12000) held |= 1 << 7;
    }
    if ((held & 0x30) == 0x30) held &= ~0x30;       /* left+right or up+down cannot happen on the console's pad */
    if ((held & 0xC0) == 0xC0) held &= ~0xC0;

    if (width != shown_width) {
        SDL_SetRenderLogicalPresentation(renderer, width, SS_SCREEN_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        shown_width = width;
    }
    SDL_UpdateTexture(texture, &update, pixels, width * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, &source, NULL);
    SDL_RenderPresent(renderer);

    /* the console refreshes at 59.7275 Hz; game speed must not depend on the monitor */
    next_frame_ns += 16742706;
    now = SDL_GetTicksNS();
    if (next_frame_ns > now)
        SDL_DelayPrecise(next_frame_ns - now);
    else if (now - next_frame_ns > 100000000)
        next_frame_ns = now;                        /* fell far behind (debugger, window drag): do not catch up */
    return held;
}
