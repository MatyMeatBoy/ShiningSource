/* Native MusicPlayer2000 ("m4a") sound engine.
 *
 * The cartridge's engine is hand-tuned ARM that mixes into DMA buffers. This is a re-implementation of what it
 * does, not of how: it reads the same song table, song headers, track byte code, voice groups and samples from
 * the ROM, runs the sequencer once per frame from the game's own m4aSoundMain call, and mixes that frame at the
 * host's sample rate.
 *
 * First version. Implemented: all track commands the sequencer needs to stay in step, direct-sound voices with
 * envelopes, key splits and rhythm sets, the four synthesised channels (two squares, wave, noise), volume, pan,
 * pitch bend, tune, key shift, fades and the m4aMPlay controls. Not yet: LFO modulation, reverb, MEMACC
 * conditionals, and the exact envelope timing of the synthesised channels.
 */
#include "shiningsource/runtime.h"

#include <math.h>
#include <string.h>

#define P8(a)   ((const uint8_t *)(uintptr_t)(a))
#define RD32(a) (*(const uint32_t *)(uintptr_t)(a))

enum { RATE = 48000, MAX_PLAYERS = 8, MAX_TRACKS = 16, MAX_VOICES = 48 };

/* ROM tables with their SDK names (addresses come from the game's symbols) */
extern const uint8_t gSongTable[];      /* { SongHeader *header; u16 player; u16 player2; } */
extern const uint8_t gMPlayTable[];     /* { MusicPlayerInfo *info; MusicPlayerTrack *tracks; u8 count; u16 unk; } */

static const uint8_t clock_table[49] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60, 64, 66, 68, 72, 76, 78, 80, 84, 88, 90, 92, 96,
};

struct Track {
    int active;
    const uint8_t *cursor, *stack[3];
    int depth, wait, repeat, running, key, velocity;
    int key_shift, volume, pan, bend, bend_range, tune, priority, mod, lfo_speed;
    int vol_x, pan_x, key_shift_x, pitch_x;        /* set by the m4aMPlay* controls */
    uint8_t tone[12];
};

struct Player {
    uint32_t info;                  /* MusicPlayerInfo in game memory: songHeader at +0, status at +4 */
    uint32_t header;
    int playing, paused, tempo_d, tempo_u, tempo_c, fade_interval, fade_counter, fade_level, tracks_count;
    struct Track tracks[MAX_TRACKS];
};

struct Voice {
    int active, player, track, key, gate, releasing, kind;      /* kind: 0 sample, 1/2 square, 3 wave, 4 noise */
    const int8_t *data;
    uint32_t size, loop_start;
    int loops;
    double position, base_step;
    int envelope, attack, decay, sustain, release, phase;       /* sample envelopes run 0..255 */
    int velocity, fixed_pan, age;
    double synth_phase, synth_freq;
    int duty, lfsr, short_noise;
    const uint8_t *wave;
};

static struct Player players[MAX_PLAYERS];
static int player_count;
static struct Voice voices[MAX_VOICES];
static int age_counter, initialised;
static double sample_carry;

static void write_status(struct Player *p)
{
    uint32_t mask = 0;
    int i;
    if (!p->info)
        return;
    for (i = 0; i < p->tracks_count; i++)
        if (p->tracks[i].active)
            mask |= 1u << i;
    *(uint32_t *)(uintptr_t)p->info = p->header;
    *(uint32_t *)(uintptr_t)(p->info + 4) = (p->playing && !p->paused && mask) ? mask : 0x80000000u;
}

static struct Player *player_of(uint32_t info)
{
    int i;
    for (i = 0; i < player_count; i++)
        if (players[i].info == info)
            return &players[i];
    if (player_count < MAX_PLAYERS) {
        memset(&players[player_count], 0, sizeof players[0]);
        players[player_count].info = info;
        players[player_count].tempo_u = 256;
        return &players[player_count++];
    }
    return &players[0];
}

static void release_voices(int player, int track, int key)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++) {
        struct Voice *v = &voices[i];
        if (v->active && v->player == player && (track < 0 || v->track == track) && (key < 0 || v->key == key))
            v->releasing = 1;
    }
}

static void stop_player(struct Player *p)
{
    int i;
    p->playing = 0;
    for (i = 0; i < MAX_TRACKS; i++)
        p->tracks[i].active = 0;
    release_voices((int)(p - players), -1, -1);
    write_status(p);
}

static void start_player(struct Player *p, uint32_t header)
{
    const uint8_t *song = P8(header);
    int i, count = song[0];

    release_voices((int)(p - players), -1, -1);
    p->header = header;
    p->tracks_count = count > MAX_TRACKS ? MAX_TRACKS : count;
    p->playing = 1;
    p->paused = 0;
    p->tempo_d = 150;
    p->tempo_c = 0;
    p->fade_interval = 0;
    p->fade_level = 64;
    for (i = 0; i < MAX_TRACKS; i++) {
        struct Track *t = &p->tracks[i];
        int vol_x = t->vol_x, pan_x = t->pan_x;
        memset(t, 0, sizeof *t);
        if (i >= p->tracks_count)
            continue;
        t->active = 1;
        t->cursor = P8(RD32(header + 8 + 4 * i));
        t->volume = 100;
        t->bend_range = 2;
        t->velocity = 127;
        t->vol_x = 64;
        (void)vol_x; (void)pan_x;
    }
    write_status(p);
}

static void start_note(struct Player *p, int track_index, int key, int velocity, int gate)
{
    struct Track *t = &p->tracks[track_index];
    const uint8_t *tone = t->tone;
    struct Voice *v = NULL;
    int i, oldest = 0, type, play_key, pan = -1000;

    key += t->key_shift + t->key_shift_x;
    if (key < 0) key = 0;
    if (key > 127) key = 127;
    play_key = key;
    type = tone[0];
    if (type & 0x40) {                              /* key split: a table picks the voice */
        const uint8_t *sub = P8(RD32((uintptr_t)tone + 4)), *split = P8(RD32((uintptr_t)tone + 8));
        tone = sub + split[key] * 12;
        type = tone[0];
    } else if (type & 0x80) {                       /* rhythm set: one voice per key, at its own pitch */
        tone = P8(RD32((uintptr_t)tone + 4)) + key * 12;
        type = tone[0];
        play_key = tone[1];
        if (tone[3] & 0x80)
            pan = ((tone[3] & 0x7F) - 64) * 2;
    }
    if (type & 0xC0)
        return;
    for (i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) { v = &voices[i]; break; }
        if (voices[i].age < voices[oldest].age) oldest = i;
    }
    if (!v) v = &voices[oldest];
    memset(v, 0, sizeof *v);
    v->active = 1;
    v->player = (int)(p - players);
    v->track = track_index;
    v->key = key;
    v->gate = gate;
    v->velocity = velocity;
    v->fixed_pan = pan;
    v->age = ++age_counter;
    v->kind = type & 7;
    v->attack = tone[8]; v->decay = tone[9]; v->sustain = tone[10]; v->release = tone[11];
    if (v->kind == 0) {
        const uint8_t *wave = P8(RD32((uintptr_t)tone + 4));
        uint32_t frequency = RD32((uintptr_t)wave + 4);
        v->loops = (wave[3] & 0x40) != 0;
        v->loop_start = RD32((uintptr_t)wave + 8);
        v->size = RD32((uintptr_t)wave + 12);
        v->data = (const int8_t *)wave + 16;
        v->base_step = (type & 8) ? 13379.0 / RATE      /* fixed-rate voices play at the engine's mixing rate */
                                  : (frequency / 1024.0) * pow(2.0, (play_key - 60) / 12.0) / RATE;
    } else {
        v->synth_freq = 440.0 * pow(2.0, (play_key - 69) / 12.0);
        v->duty = (int)(RD32((uintptr_t)tone + 4) & 3);
        v->wave = P8(RD32((uintptr_t)tone + 4));
        v->short_noise = (int)(RD32((uintptr_t)tone + 4) & 1);
        v->lfsr = 0x7FFF;
        v->sustain = (tone[10] & 15) * 17;                      /* synthesised envelopes use 0..15 */
    }
    t->key = key - t->key_shift - t->key_shift_x;
}

static void run_track(struct Player *p, int index)
{
    struct Track *t = &p->tracks[index];
    int guard = 0;

    while (t->active && t->wait == 0 && guard++ < 4096) {
        int command = *t->cursor;
        if (command < 0x80) {
            command = t->running;
        } else {
            t->cursor++;
            if (command >= 0xBD)
                t->running = command;
        }
        if (command >= 0xCF) {                                  /* note: TIE or N01..N96 */
            int gate = command == 0xCF ? -1 : clock_table[command - 0xCF];
            if (*t->cursor < 0x80) {
                t->key = *t->cursor++;
                if (*t->cursor < 0x80) {
                    t->velocity = *t->cursor++;
                    if (*t->cursor < 0x80)
                        gate += *t->cursor++;
                }
            }
            start_note(p, index, t->key, t->velocity, gate);
        } else if (command >= 0x80 && command <= 0xB0) {
            t->wait = clock_table[command - 0x80];
        } else switch (command) {
        case 0xB1:                                              /* FINE */
            t->active = 0;
            release_voices((int)(p - players), index, -1);
            break;
        case 0xB2: t->cursor = P8(RD32(t->cursor)); break;      /* GOTO */
        case 0xB3:                                              /* PATT */
            if (t->depth < 3) {
                t->stack[t->depth++] = t->cursor + 4;
                t->cursor = P8(RD32(t->cursor));
            } else {
                t->active = 0;
            }
            break;
        case 0xB4: if (t->depth) t->cursor = t->stack[--t->depth]; break;   /* PEND */
        case 0xB5:                                              /* REPT */
            if (t->cursor[0] == 0 || ++t->repeat < t->cursor[0]) {
                t->cursor = P8(RD32(t->cursor + 1));
            } else {
                t->repeat = 0;
                t->cursor += 5;
            }
            break;
        case 0xB9: t->cursor += 3; break;                       /* MEMACC */
        case 0xBA: t->priority = *t->cursor++; break;
        case 0xBB: p->tempo_d = *t->cursor++ * 2; break;
        case 0xBC: t->key_shift = (int8_t)*t->cursor++; break;
        case 0xBD: {
            uint32_t group = RD32(p->header + 4);
            memcpy(t->tone, P8(group + *t->cursor++ * 12), 12);
            break;
        }
        case 0xBE: t->volume = *t->cursor++; break;
        case 0xBF: t->pan = *t->cursor++ - 64; break;
        case 0xC0: t->bend = *t->cursor++ - 64; break;
        case 0xC1: t->bend_range = *t->cursor++; break;
        case 0xC2: t->lfo_speed = *t->cursor++; break;
        case 0xC3: t->cursor++; break;                          /* LFODL */
        case 0xC4: t->mod = *t->cursor++; break;
        case 0xC5: t->cursor++; break;                          /* MODT */
        case 0xC8: t->tune = *t->cursor++ - 64; break;
        case 0xCD: t->cursor += 2; break;                       /* XCMD: sub-command and one argument */
        case 0xCE:                                              /* EOT */
            if (*t->cursor < 0x80)
                t->key = *t->cursor++;
            release_voices((int)(p - players), index, t->key + t->key_shift + t->key_shift_x);
            break;
        default:                                                /* unknown: the stream cannot be followed */
            t->active = 0;
            break;
        }
    }
    if (t->wait > 0)
        t->wait--;
}

static void tick(struct Player *p)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++) {
        struct Voice *v = &voices[i];
        if (v->active && v->player == (int)(p - players) && v->gate > 0 && --v->gate == 0)
            v->releasing = 1;
    }
    for (i = 0; i < p->tracks_count; i++)
        if (p->tracks[i].active)
            run_track(p, i);
}

static void sequence(void)
{
    int i, j, any;
    for (i = 0; i < player_count; i++) {
        struct Player *p = &players[i];
        if (!p->playing || p->paused)
            continue;
        if (p->fade_interval && --p->fade_counter <= 0) {
            p->fade_counter = p->fade_interval;
            p->fade_level -= 4;
            if (p->fade_level <= 0) {
                stop_player(p);
                continue;
            }
        }
        p->tempo_c += p->tempo_d * p->tempo_u / 256;
        while (p->tempo_c >= 150) {
            p->tempo_c -= 150;
            tick(p);
        }
        for (any = 0, j = 0; j < p->tracks_count; j++)
            any |= p->tracks[j].active;
        if (!any)
            p->playing = 0;
        write_status(p);
    }
}

static void envelopes(void)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++) {
        struct Voice *v = &voices[i];
        if (!v->active)
            continue;
        if (v->kind == 0) {
            if (v->releasing) {
                v->envelope = v->envelope * v->release >> 8;
                if (v->envelope <= 0) v->active = 0;
            } else if (v->phase == 0) {
                v->envelope += v->attack;
                if (v->envelope >= 255) { v->envelope = 255; v->phase = 1; }
            } else if (v->phase == 1) {
                v->envelope = v->envelope * v->decay >> 8;
                if (v->envelope <= v->sustain) { v->envelope = v->sustain; v->phase = 2; }
                if (v->envelope == 0) v->active = 0;
            }
        } else {                                    /* synthesised channels: steps of 1/15, one every N frames */
            int rate = v->releasing ? (v->release & 7) : v->phase == 0 ? (v->attack & 7) : (v->decay & 7);
            int step = rate ? (17 + rate - 1) / rate : 255;
            if (v->releasing) {
                v->envelope -= step;
                if (v->envelope <= 0) v->active = 0;
            } else if (v->phase == 0) {
                v->envelope += step;
                if (v->envelope >= 255) { v->envelope = 255; v->phase = 1; }
            } else if (v->phase == 1) {
                v->envelope -= step;
                if (v->envelope <= v->sustain) { v->envelope = v->sustain; v->phase = 2; }
                if (v->envelope <= 0 && v->sustain == 0) v->active = 0;
            }
        }
    }
}

static void mix(int16_t *out, int frames)
{
    static int32_t sum[2048 * 2];
    int i, n;

    if (frames > 2048) frames = 2048;
    memset(sum, 0, sizeof(int32_t) * 2 * (size_t)frames);
    for (i = 0; i < MAX_VOICES; i++) {
        struct Voice *v = &voices[i];
        struct Player *p;
        struct Track *t;
        int level, pan, left, right;
        double step;

        if (!v->active)
            continue;
        p = &players[v->player];
        t = &p->tracks[v->track];
        level = t->volume * t->vol_x >> 5;                              /* 0..254 */
        level = level * v->velocity >> 7;
        level = level * v->envelope >> 8;
        level = level * p->fade_level >> 6;
        pan = v->fixed_pan > -1000 ? v->fixed_pan : t->pan * 2 + t->pan_x;
        if (pan < -128) pan = -128;
        if (pan > 127) pan = 127;
        right = level * (pan + 128) >> 8;
        left = level * (127 - pan) >> 8;
        /* pitch offset in semitones: bend (-64..63 over the bend range), tune (1/64 semitone), pitch control (1/256) */
        step = pow(2.0, ((t->bend * t->bend_range) / 64.0 + t->tune / 64.0 + t->pitch_x / 256.0) / 12.0);
        if (v->kind == 0) {
            double position = v->position, advance = v->base_step * step;
            for (n = 0; n < frames; n++) {
                uint32_t at = (uint32_t)position;
                int sample;
                if (at >= v->size) {
                    if (!v->loops || v->size <= v->loop_start) { v->active = 0; break; }
                    position = v->loop_start + fmod(position - v->loop_start, (double)(v->size - v->loop_start));
                    at = (uint32_t)position;
                }
                {
                    /* between this sample and the next (the loop's first one at the end of a loop): stepping from
                     * one 8-bit sample to the next at 48 kHz adds a hard edge the console's own output filters away */
                    uint32_t after = at + 1 < v->size ? at + 1 : (v->loops && v->size > v->loop_start ? v->loop_start : at);
                    int fraction = (int)((position - (double)at) * 256.0);
                    sample = (v->data[at] * (256 - fraction) + v->data[after] * fraction) >> 4;      /* 4 more bits */
                }
                sum[n * 2] += sample * left >> 4;
                sum[n * 2 + 1] += sample * right >> 4;
                position += advance;
            }
            v->position = position;
        } else {
            double phase = v->synth_phase, advance = v->synth_freq * step / RATE;
            static const double duty[4] = { 0.125, 0.25, 0.5, 0.75 };
            for (n = 0; n < frames; n++) {
                int sample;
                if (v->kind == 3) {
                    int index = (int)(phase * 32) & 31, byte = v->wave[index >> 1];
                    sample = (((index & 1) ? byte & 15 : byte >> 4) - 8) * 8;
                } else if (v->kind == 4) {
                    sample = (v->lfsr & 1) ? 36 : -36;
                } else {
                    sample = phase < duty[v->duty] ? 36 : -36;
                }
                sum[n * 2] += sample * left;
                sum[n * 2 + 1] += sample * right;
                phase += advance;
                if (phase >= 1.0) {
                    phase -= 1.0;
                    if (v->kind == 4) {
                        int bit = (v->lfsr ^ (v->lfsr >> 1)) & 1;
                        v->lfsr = (v->lfsr >> 1) | (bit << (v->short_noise ? 6 : 14));
                    }
                }
            }
            v->synth_phase = phase;
        }
    }
    for (n = 0; n < frames * 2; n++) {
        /* The console's amplifier rolls the top off; without that the square and noise channels sound brittle.
         * One-pole low-pass at about 14 kHz, per channel. */
        static int32_t filtered[2];
        int32_t s = sum[n] >> 2, magnitude;
        filtered[n & 1] += (s - filtered[n & 1]) * 5 / 6;
        s = filtered[n & 1];
        /* Loud passages: above three quarters of full scale the signal is bent towards the limit instead of
         * being cut flat, which is what was heard as saturation. */
        magnitude = s < 0 ? -s : s;
        if (magnitude > 24576) {
            int32_t over = magnitude - 24576;
            magnitude = 24576 + (int32_t)((int64_t)over * 8191 / (over + 8191));
            s = s < 0 ? -magnitude : magnitude;
        }
        out[n] = (int16_t)s;
    }
}

/* ---- entry points with the SDK's names ---- */

void m4aSoundInit(void)
{
    int i;
    memset(players, 0, sizeof players);
    memset(voices, 0, sizeof voices);
    player_count = 0;
    for (i = 0; i < 4; i++) {
        uint32_t info = RD32((uintptr_t)gMPlayTable + i * 12);
        if (info >= SS_EWRAM && info < SS_IWRAM + 0x8000)
            write_status(player_of(info));
    }
    initialised = 1;
}

void m4aSoundMain(void)
{
    static int16_t buffer[2048 * 2];
    int frames;

    if (!initialised)
        return;
    sequence();
    envelopes();
    sample_carry += RATE / 59.7275;
    frames = (int)sample_carry;
    sample_carry -= frames;
    mix(buffer, frames);
    ss_audio_push(buffer, frames);
}

void m4aSoundVSync(void) {}
void m4aSoundVSyncOn(void) {}
void m4aSoundVSyncOff(void) {}
void m4aSoundMode(uint32_t mode) { (void)mode; }
void m4aMPlayImmInit(uint32_t info) { (void)info; }

void MPlayStart(uint32_t info, uint32_t header)
{
    start_player(player_of(info), header);
}

static struct Player *song_player(uint16_t number, uint32_t *header)
{
    const uint8_t *entry = gSongTable + number * 8;
    *header = RD32(entry);
    return player_of(RD32((uintptr_t)gMPlayTable + (entry[4] | entry[5] << 8) * 12));
}

void m4aSongNumStart(uint16_t number)
{
    uint32_t header;
    struct Player *p = song_player(number, &header);
    start_player(p, header);
}

void m4aSongNumStartOrChange(uint16_t number)
{
    uint32_t header;
    struct Player *p = song_player(number, &header);
    if (p->header != header || !p->playing || p->paused)
        start_player(p, header);
}

void m4aSongNumStartOrContinue(uint16_t number)
{
    uint32_t header;
    struct Player *p = song_player(number, &header);
    if (p->header != header || !p->playing)
        start_player(p, header);
    else if (p->paused) {
        p->paused = 0;
        write_status(p);
    }
}

void m4aSongNumStop(uint16_t number)
{
    uint32_t header;
    struct Player *p = song_player(number, &header);
    if (p->header == header)
        stop_player(p);
}

void m4aSongNumContinue(uint16_t number)
{
    uint32_t header;
    struct Player *p = song_player(number, &header);
    if (p->header == header && p->paused) {
        p->paused = 0;
        write_status(p);
    }
}

void m4aMPlayStop(uint32_t info)
{
    stop_player(player_of(info));
}

void m4aMPlayAllStop(void)
{
    int i;
    for (i = 0; i < player_count; i++)
        stop_player(&players[i]);
}

void m4aMPlayAllContinue(void)
{
    int i;
    for (i = 0; i < player_count; i++) {
        players[i].paused = 0;
        write_status(&players[i]);
    }
}

void m4aMPlayFadeOut(uint32_t info, uint16_t speed)
{
    struct Player *p = player_of(info);
    p->fade_interval = speed ? speed : 1;
    p->fade_counter = p->fade_interval;
}

void m4aMPlayTempoControl(uint32_t info, uint16_t tempo)
{
    player_of(info)->tempo_u = tempo;
}

#define FOR_TRACKS(info, bits) \
    struct Player *p = player_of(info); int i; \
    for (i = 0; i < MAX_TRACKS; i++) if ((bits) & (1u << i))

void m4aMPlayVolumeControl(uint32_t info, uint16_t bits, uint16_t volume) { FOR_TRACKS(info, bits) p->tracks[i].vol_x = volume / 4; }
void m4aMPlayPitchControl(uint32_t info, uint16_t bits, int16_t pitch) { FOR_TRACKS(info, bits) p->tracks[i].pitch_x = pitch; }
void m4aMPlayPanpotControl(uint32_t info, uint16_t bits, int8_t pan) { FOR_TRACKS(info, bits) p->tracks[i].pan_x = pan; }
void m4aMPlayModDepthSet(uint32_t info, uint16_t bits, uint8_t depth) { FOR_TRACKS(info, bits) p->tracks[i].mod = depth; }
void m4aMPlayLFOSpeedSet(uint32_t info, uint16_t bits, uint8_t speed) { FOR_TRACKS(info, bits) p->tracks[i].lfo_speed = speed; }
