#ifndef SHININGSOURCE_COUCH_WIN32_H
#define SHININGSOURCE_COUCH_WIN32_H

#include <stdint.h>

int ss_couch_parent(const wchar_t *rom);
int ss_couch_worker_init(void);
int ss_couch_worker(void);
int ss_couch_worker_width(void);
uint16_t ss_couch_worker_present(const uint32_t *pixels, int width);
void ss_couch_worker_admission(int open);
void ss_couch_worker_audio(const int16_t *samples, int frames);
void ss_couch_worker_pump(void);
void ss_couch_worker_overlay(int darkness, const char *text);

#endif
