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

/* GBA buttons, bit order of KEYINPUT: A B Select Start Right Left Up Down R L */
static const SDL_Scancode keys[10] = {
    SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_V, SDL_SCANCODE_C,
};

int ss_video_init(const char *title)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    if (!SDL_CreateWindowAndRenderer(title, SS_SCREEN_W * 4, SS_SCREEN_H * 4, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderLogicalPresentation(renderer, SS_SCREEN_W, SS_SCREEN_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SS_SCREEN_W, SS_SCREEN_H);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
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
    }
    state = SDL_GetKeyboardState(NULL);
    for (i = 0; i < 10; i++)
        if (state[keys[i]])
            held |= (uint16_t)(1 << i);
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
