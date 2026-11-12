/* A platform without window, sound or input, for test hosts that only run the game headless (SS_FRAMES). */
#include "shiningsource/runtime.h"

int ss_plat_video_init(const char *title) { (void)title; return -1; }
int ss_plat_view_width(void) { return SS_SCREEN_W; }
uint16_t ss_plat_present(const uint32_t *pixels, int width) { (void)pixels; (void)width; return 0; }
void ss_plat_audio(const int16_t *samples, int frames) { (void)samples; (void)frames; }
void ss_plat_pump(void) {}
void ss_plat_status(const char *text) { (void)text; }
