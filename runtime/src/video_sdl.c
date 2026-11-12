/* Window, presentation, keyboard and frame pacing through SDL3. */
#include "shiningsource/runtime.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define REG_KEYINPUT (*(volatile uint16_t *)(uintptr_t)(SS_IO + 0x130))

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint64_t next_frame_ns;
static SDL_Gamepad *gamepad;

/* GBA buttons, bit order of KEYINPUT: A B Select Start Right Left Up Down R L */
static const SDL_Scancode keys[10] = {
    SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_V, SDL_SCANCODE_C,
};

static SDL_AudioStream *audio;
static unsigned long audio_frames, audio_loud_frames;
static int audio_peak;
static FILE *audio_file;

/* One game frame of 48 kHz stereo sound. Without a window the samples are only measured (and written to
 * ss_audio.raw when SS_AUDIO_FILE is set), so a headless test can tell that the engine produces a signal. */
void ss_audio_push(const int16_t *samples, int frames)
{
    int i;
    static int file_checked;
    for (i = 0; i < frames * 2; i++) {
        int magnitude = samples[i] < 0 ? -samples[i] : samples[i];
        if (magnitude > audio_peak) audio_peak = magnitude;
        if (magnitude > 256) audio_loud_frames++;
    }
    audio_frames += (unsigned long)frames;
    if (!file_checked) {
        file_checked = 1;
        if (getenv("SS_AUDIO_FILE"))
            audio_file = fopen("ss_audio.raw", "wb");
    }
    if (audio_file)
        fwrite(samples, 4, (size_t)frames, audio_file);
    if (audio) {
        /* never let more than about a tenth of a second queue up: late sound is worse than a dropped frame */
        if (SDL_GetAudioStreamQueued(audio) < 48000 * 4 / 10)
            SDL_PutAudioStreamData(audio, samples, frames * 4);
    }
}

void ss_audio_report(void)
{
    fprintf(stderr, "audio: %lu sample frames, peak %d, %lu samples above the noise floor\n", audio_frames, audio_peak, audio_loud_frames);
    if (audio_file)
        fclose(audio_file);
}

int ss_video_init(const char *title)
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
    if (audio)
        SDL_ResumeAudioStreamDevice(audio);
    else
        fprintf(stderr, "no audio device: %s\n", SDL_GetError());
    if (!SDL_CreateWindowAndRenderer(title, SS_SCREEN_W * 4, SS_SCREEN_H * 4, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderLogicalPresentation(renderer, SS_SCREEN_W, SS_SCREEN_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SS_SCREEN_W, SS_SCREEN_H);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);                /* come to the front even when started from a console or a dialog */
    next_frame_ns = SDL_GetTicksNS();
    return 0;
}

void ss_video_frame(void)
{
    static uint32_t pixels[SS_SCREEN_W * SS_SCREEN_H];
    const bool *state;
    uint16_t held = 0;
    uint64_t now;
    SDL_Event event;
    int i;

    if (!window)
        return;
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
        /* same bit order as `keys`; the south button is A as on the console's layout, sticks act as the d-pad */
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
    REG_KEYINPUT = (uint16_t)(~held & 0x03FF);     /* the register is active low */

    ss_ppu_render(pixels, SS_SCREEN_W);
    SDL_UpdateTexture(texture, NULL, pixels, SS_SCREEN_W * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    /* the console refreshes at 59.7275 Hz; game speed must not depend on the monitor */
    next_frame_ns += 16742706;
    now = SDL_GetTicksNS();
    if (next_frame_ns > now)
        SDL_DelayPrecise(next_frame_ns - now);
    else if (now - next_frame_ns > 100000000)
        next_frame_ns = now;                        /* fell far behind (debugger, window drag): do not catch up */
}
