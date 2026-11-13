/* Native MusicPlayer2000 ("m4a") sound engine.
 *
 * The cartridge's engine is hand-tuned ARM that mixes into DMA buffers and drives the four Game Boy sound
 * channels through their registers. This file does what that engine does, with the same state and the same
 * decisions, but mixes at the host's sample rate: it reads the game's song table, song headers, track byte code,
 * voice groups and samples from the ROM, and runs once per frame from the game's own m4aSoundMain call, in the
 * engine's order: the sequencer of every music player (MPlayMain), the Game Boy channels (CgbSound), then the
 * envelopes and the mix of the sampled channels (SoundMainRAM).
 *
 * What follows the engine exactly (the game's decompiled driver, games/<game>/decomp/src/m4a*.c and
 * asm/nonmatchings/m4a_1_*; the same routines are public in the pret decompilations' m4a.c / m4a_1.s):
 *   - music players: MPlayStart (tracks limited to the player's own count, the song's reverb), m4aMPlayStop
 *     (a pause: the tracks stay where they are), MPlayContinue, the fade (FadeOutBody), the status word the
 *     m4aSongNum* calls decide on, and the m4aMPlay* controls, which only touch tracks that exist;
 *   - tracks: every command (notes, ties, rests, FINE GOTO PATT PEND REPT MEMACC PRIO TEMPO KEYSH VOICE VOL PAN
 *     BEND BENDR LFOS LFODL MOD MODT TUNE XCMD EOT), running status, a track's first-tick reset, the LFO
 *     (pitch, volume or pan modulation) and TrkVolPitSet;
 *   - notes (ply_note): key splits and rhythm sets, priorities, the choice of one of the sampled channels (as
 *     many as the sound mode allows, 8 for this game) or of the one Game Boy channel of that kind, with the
 *     engine's rules for stealing a channel or dropping the note;
 *   - sampled channels: attack/decay/sustain/release and pseudo-echo per frame, master volume, pitch from the
 *     sample's rate (MidiKeyToFreq), fixed-rate voices at the engine's mixing rate, loops, reverb;
 *   - Game Boy channels: CgbSound's envelope program, volume and pan rules (CgbModVol), pitch
 *     (MidiKeyToCgbFreq), and the registers it writes, which drive a model of the hardware: duty cycles, the
 *     volume envelope, length counter and sweep, the 32-step 4-bit wave with its four volumes, the noise
 *     shift register in both widths.
 * What is the port's own: the mix is made at 48 kHz instead of the engine's rate (so samples are interpolated
 * and never clipped to 8 bits), channel gains glide over a frame instead of stepping, a channel that is cut
 * fades over a few milliseconds, the Game Boy channels are averaged over each output sample and carry no
 * constant offset, and the output passes the low-pass and the soft limiter at the end of mix().
 * Not done: the PORT command (a raw register write; no song of the game has it), the scan-line budget
 * (maxLines), m4aSoundVSyncOff muting the sampled channels.
 */
#include "shiningsource/runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P8(a) ((const uint8_t *)(uintptr_t)(a))

/* The mode the game's m4aSoundInit sets (it is part of the module this file replaces): 8-bit output, 10512 Hz,
 * master volume 13, 8 sampled channels. Shining Soul II: decomp/src/m4a.c. */
#ifndef SS_M4A_INIT_MODE
#define SS_M4A_INIT_MODE 0x0093D800u
#endif

enum {
    RATE = 48000, MAX_PLAYERS = 8, MAX_TRACKS = 16, PCM_CHANNELS = 12, CHANNELS = 16, TAILS = 16, TAIL_LENGTH = 128,
    TRACK_BYTES = 0x50,                     /* sizeof(struct MusicPlayerTrack): tracks are compared by address */
    EXIST = 0x80, START = 0x40, VOLCHG = 0x03, PITCHG = 0x0C,               /* track flags */
    CH_START = 0x80, CH_STOP = 0x40, CH_LOOP = 0x10, CH_IEC = 0x04, CH_ENV = 0x03, CH_ON = 0xC7,
    PAUSED = (int)0x80000000
};
#define PCM_UNIT 1.125f     /* one sampled channel: sample * volume (0..255) * this, in units of 1/4 output step */
#define PSG_UNIT 72.0f      /* one Game Boy channel spans a quarter of a sampled channel's range (GBATEK) */

/* ROM tables with their SDK names (addresses come from the game's symbols) */
extern const uint8_t gSongTable[];      /* { SongHeader *header; u16 player; u16 player2; } */
extern const uint8_t gMPlayTable[];     /* { MusicPlayerInfo *info; MusicPlayerTrack *tracks; u8 count; u16 unk; } */

static const uint8_t clock_table[49] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60, 64, 66, 68, 72, 76, 78, 80, 84, 88, 90, 92, 96,
};
static const uint16_t samples_per_vblank[12] = { 96, 132, 176, 224, 264, 304, 352, 448, 528, 608, 672, 704 };
static const int16_t cgb_freq_table[12] = { -2004, -1891, -1785, -1685, -1591, -1501, -1417, -1337, -1262, -1192, -1125, -1062 };
static const uint8_t cgb3_volume[16] = { 0, 0, 96, 96, 96, 96, 64, 64, 64, 64, 128, 128, 128, 128, 32, 32 };

struct Track {
    uint8_t flags, wait, pattern_level, repeat, gate, key, velocity, running;
    uint8_t bend_range, vol, vol_x, mod, mod_type, lfo_speed, lfo_speed_c, lfo_delay, lfo_delay_c;
    uint8_t priority, echo_volume, echo_length, vol_r, vol_l, pit_m;
    int8_t key_m, key_shift, key_shift_x, tune, bend, pan, pan_x, mod_m;
    int16_t pit_x;
    uint8_t tone[12];               /* ToneData: type, key, length, pan_sweep, wav, attack, decay, sustain, release */
    const uint8_t *cursor, *stack[3];
};

struct Player {
    uint32_t info;                  /* MusicPlayerInfo in game memory: songHeader at +0, status at +4 */
    uint32_t tracks_address;        /* its MusicPlayerTrack array: only the order of the addresses is used */
    uint32_t header, tone;
    int32_t status;                 /* bit 31: paused; low bits: the tracks that exist */
    int track_count, priority;
    unsigned tempo_d, tempo_u, tempo_i, tempo_c, fade_interval, fade_counter;
    int fade_volume;
    struct Track tracks[MAX_TRACKS];
};

struct Channel {
    uint8_t status, type, right, left, attack, decay, sustain, release;
    uint8_t ev, echo_volume, echo_length, gate, midi_key, velocity, priority, key;
    int player, track;              /* the track it is chained to, -1 after ClearChain */
    int stat_player, stat_track;    /* who started it (test report) */
    uint32_t serial, wav;
    /* sampled */
    double frequency, position;
    const int8_t *data;
    uint32_t size, loop_start;
    int loops, sounding;
    float gain_l, gain_r;
    /* Game Boy (struct CgbChannel) */
    uint8_t modify, length, sweep, n4, env_goal, sus_goal, env_counter, pan, pan_mask, step_dir;
    uint32_t cgb_frequency, current_wave;
};

struct Psg {                        /* one hardware channel as its registers leave it */
    int on, dac, nrx2, volume, env_add, env_period, env_timer;
    int frequency, duty, length, length_on;
    int sweep_reg, sweep_timer, sweep_on, shadow;
    int wave_code, noise_reg, left, right;
    unsigned lfsr;
    uint8_t ram[16];
    double mean, phase, level;
};

struct Tail {                       /* the last few milliseconds of a sampled channel that was cut */
    const int8_t *data;
    uint32_t size, loop_start;
    int loops, left;
    double position, advance;
    float gain_l, gain_r;
};

static struct Player players[MAX_PLAYERS];
static int player_count;
static struct Channel channels[CHANNELS];           /* 12 sampled, then square 1, square 2, wave, noise */
static struct Psg psg[4];
static struct Tail tails[TAILS];
static struct { int reverb, max_channels, master_volume, pcm_rate, dma_period, soundbias_h, c15; } sound;
static uint8_t mem_acc_area[0x80];
static uint32_t serial_counter;
static int initialised;
static double sample_carry, sequencer_carry;
static int sequencer_step;
static float reverb_l[16384], reverb_r[16384];
static unsigned reverb_at;
static double semitone[12];

static uint32_t rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* ---- test hook (inert unless SS_TEST_SONG or SS_M4A_LOG is set) ----
 * SS_TEST_SONG=n      start song n of the song table through m4aSongNumStart once the engine is initialised and
 *                     ignore the game's own sound calls, so that only that song is heard;
 * SS_TEST_SONG_AT=f   ... at the f-th call of m4aSoundMain instead of the first;
 * SS_M4A_LOG=1        one line on stderr for every sound call the game makes (number of the frame, call,
 *                     arguments in hexadecimal), and what a start-or-continue did.
 * With SS_TEST_SONG the run ends with one line per track: notes started, notes that found no channel, the kind
 * of voice, and the energy that track put into the mix. tools/tests/m4a_audit.py does this for every song. */
void m4aSongNumStart(uint16_t number);

static int test_song = -1, test_at, test_log, test_checked, test_inside;
static unsigned long main_calls;
static struct { unsigned long notes, dropped, kinds[5]; double energy; } test_stats[MAX_PLAYERS][MAX_TRACKS];

static void test_report(void)
{
    int i, j;
    for (i = 0; i < MAX_PLAYERS; i++)
        for (j = 0; j < MAX_TRACKS; j++)
            if (test_stats[i][j].notes || test_stats[i][j].dropped)
                fprintf(stderr, "m4a: player %d track %d notes %lu dropped %lu pcm %lu sq1 %lu sq2 %lu wave %lu noise %lu energy %.0f%c",
                        i, j, test_stats[i][j].notes, test_stats[i][j].dropped, test_stats[i][j].kinds[0],
                        test_stats[i][j].kinds[1], test_stats[i][j].kinds[2], test_stats[i][j].kinds[3],
                        test_stats[i][j].kinds[4], test_stats[i][j].energy, 10);
}

static void test_setup(void)
{
    const char *text;
    test_checked = 1;
    text = getenv("SS_TEST_SONG");
    if (text && *text) {
        test_song = (int)strtol(text, NULL, 0);
        text = getenv("SS_TEST_SONG_AT");
        test_at = text ? (int)strtol(text, NULL, 0) : 0;
        atexit(test_report);
    }
    test_log = getenv("SS_M4A_LOG") != NULL;
}

/* 1 when a call of the game is to be ignored (a song is under test) */
static int test_call(const char *name, uint32_t a, uint32_t b, uint32_t c)
{
    if (!test_checked)
        test_setup();
    if (test_log && !test_inside)
        fprintf(stderr, "m4a call %lu %s %X %X %X%c", main_calls, name, (unsigned)a, (unsigned)b, (unsigned)c, 10);
    return test_song >= 0 && !test_inside;
}

/* ---- players, tracks and channels ---- */

static void write_status(struct Player *p)
{
    *(uint32_t *)(uintptr_t)p->info = p->header;
    *(uint32_t *)(uintptr_t)(p->info + 4) = (uint32_t)p->status;
}

/* The player the game means; one that was never opened does nothing, as in the engine (its ident is not set). */
static struct Player *player_of(uint32_t info)
{
    int i;
    for (i = 0; i < player_count; i++)
        if (players[i].info == info)
            return &players[i];
    return NULL;
}

static uint32_t track_order(int player, int track)
{
    return track < 0 ? 0 : players[player].tracks_address + (uint32_t)track * TRACK_BYTES;
}

static void psg_off(int ch);

/* A sampled channel that still sounds is not cut flat: its last gain fades out in a tail. */
static void channel_silence(struct Channel *c)
{
    int index = (int)(c - channels);
    if (index >= PCM_CHANNELS) {
        if (c->status)
            psg_off(index - PCM_CHANNELS + 1);
    } else if (c->sounding && c->data && (c->gain_l > 0 || c->gain_r > 0)) {
        int i, slot = 0;
        for (i = 0; i < TAILS; i++) {
            if (tails[i].left == 0) { slot = i; break; }
            if (tails[i].left < tails[slot].left) slot = i;
        }
        tails[slot].data = c->data;
        tails[slot].size = c->size;
        tails[slot].loop_start = c->loop_start;
        tails[slot].loops = c->loops;
        tails[slot].position = c->position;
        tails[slot].advance = ((c->type & 8) ? sound.pcm_rate : c->frequency) / RATE;
        tails[slot].gain_l = c->gain_l;
        tails[slot].gain_r = c->gain_r;
        tails[slot].left = TAIL_LENGTH;
    }
    c->status = 0;
    c->sounding = 0;
}

/* TrackStop: every channel of the track goes off at once and leaves the chain. */
static void track_stop(struct Player *p, int index)
{
    int i, player = (int)(p - players);
    if (!(p->tracks[index].flags & EXIST))
        return;
    for (i = 0; i < CHANNELS; i++) {
        struct Channel *c = &channels[i];
        if (c->player == player && c->track == index) {
            channel_silence(c);
            c->track = -1;
        }
    }
}

static void sound_mode(uint32_t mode)
{
    int i;
    if (mode & 0xFF)
        sound.reverb = (int)(mode & 0x7F);
    if (mode & 0xF00) {
        sound.max_channels = (int)(mode & 0xF00) >> 8;
        for (i = 0; i < PCM_CHANNELS; i++)
            channel_silence(&channels[i]);
    }
    if (mode & 0xF000)
        sound.master_volume = (int)(mode & 0xF000) >> 12;
    if (mode & 0xF00000)
        sound.soundbias_h = (sound.soundbias_h & 0x3F) | (int)((mode & 0x300000) >> 14);
    if (mode & 0xF0000) {
        int samples = samples_per_vblank[((mode & 0xF0000) >> 16) - 1 < 12 ? ((mode & 0xF0000) >> 16) - 1 : 3];
        sound.pcm_rate = (597275 * samples + 5000) / 10000;
        sound.dma_period = 1584 / samples;
    }
}

static void start_player(struct Player *p, uint32_t header)
{
    const uint8_t *song = P8(header);
    int i;

    p->status = 0;
    p->header = header;
    p->tone = rd32(song + 4);
    p->priority = song[2];
    p->tempo_d = 150;
    p->tempo_i = 150;
    p->tempo_u = 0x100;
    p->tempo_c = 0;
    p->fade_interval = 0;
    for (i = 0; i < p->track_count; i++) {
        struct Track *t = &p->tracks[i];
        track_stop(p, i);
        if (i < song[0]) {
            t->flags = EXIST | START;
            t->cursor = P8(rd32(song + 8 + 4 * i));
        } else {
            t->flags = 0;
        }
    }
    if (song[3] & 0x80)
        sound_mode(song[3]);
    write_status(p);
}

static void stop_player(struct Player *p)
{
    int i;
    p->status |= PAUSED;
    for (i = 0; i < p->track_count; i++)
        track_stop(p, i);
    write_status(p);
}

static void continue_player(struct Player *p)
{
    p->status &= ~PAUSED;
    write_status(p);
}

static void clear_mod(struct Track *t)
{
    t->lfo_speed_c = 0;
    t->mod_m = 0;
    t->flags |= t->mod_type == 0 ? PITCHG : VOLCHG;
}

static void track_init(struct Track *t)
{
    const uint8_t *cursor = t->cursor;
    memset(t, 0, sizeof *t);
    t->cursor = cursor;
    t->flags = EXIST;
    t->bend_range = 2;
    t->vol_x = 64;
    t->lfo_speed = 22;
    t->tone[0] = 1;
}

/* TrkVolPitSet */
static void track_vol_pit(struct Track *t)
{
    if (t->flags & VOLCHG) {
        int x = (t->vol * t->vol_x) >> 5, y;
        unsigned v;
        if (t->mod_type == 1)
            x = (x * (t->mod_m + 128)) >> 7;
        y = 2 * t->pan + t->pan_x;
        if (t->mod_type == 2)
            y += t->mod_m;
        if (y < -128) y = -128;
        else if (y > 127) y = 127;
        v = (unsigned)((y + 128) * x) >> 8;
        t->vol_r = (uint8_t)v;
        v = (unsigned)((127 - y) * x) >> 8;
        t->vol_l = (uint8_t)v;
    }
    if (t->flags & PITCHG) {
        int x = (t->tune + t->bend * t->bend_range) * 4 + (t->key_shift << 8) + (t->key_shift_x << 8) + t->pit_x;
        if (t->mod_type == 0)
            x += 16 * t->mod_m;
        t->key_m = (int8_t)(x >> 8);
        t->pit_m = (uint8_t)x;
    }
    t->flags &= (uint8_t)~(PITCHG | VOLCHG);
}

/* MidiKeyToFreq: the rate a sample plays at, in Hz. The engine's table is 2^(n/12) to 32 bits, and the fine
 * part goes in a straight line from one semitone to the next. */
static double key_frequency(uint32_t wav, int key, int fine)
{
    double base = rd32(P8(wav) + 4) / 1024.0, low, high;
    if (key > 178) { key = 178; fine = 255; }
    low = base * semitone[key % 12] * pow(2.0, key / 12 - 5);
    high = base * semitone[(key + 1) % 12] * pow(2.0, (key + 1) / 12 - 5);
    return low + (high - low) * fine / 256.0;
}

/* MidiKeyToCgbFreq: the 11-bit frequency register of a tone channel, or the noise channel's NR43 */
static uint32_t cgb_frequency(int kind, int key, int fine)
{
    int low, high;
    if (kind == 4) {
        if (key <= 20) key = 0;
        else { key -= 21; if (key > 59) key = 59; }
        return (uint32_t)(key < 56 ? ((13 - key / 4) << 4) | (7 - key % 4) : 59 - key);    /* gNoiseTable */
    }
    if (key <= 35) { fine = 0; key = 0; }
    else { key -= 36; if (key > 130) { key = 130; fine = 255; } }
    low = cgb_freq_table[key % 12] >> (key / 12);                                             /* gCgbScaleTable */
    high = cgb_freq_table[(key + 1) % 12] >> ((key + 1) / 12);
    return (uint32_t)(low + ((fine * (high - low)) >> 8) + 2048);
}

/* ply_note */
static void note(struct Player *p, int index, int command)
{
    struct Track *t = &p->tracks[index];
    const uint8_t *tone = t->tone;
    struct Channel *c = NULL;
    int player = (int)(p - players), key, kind, priority, rhythm = 0, i, final_key;
    uint32_t order = track_order(player, index);

    t->gate = clock_table[command];
    if (*t->cursor < 0x80) {
        t->key = *t->cursor++;
        if (*t->cursor < 0x80) {
            t->velocity = *t->cursor++;
            if (*t->cursor < 0x80)
                t->gate += *t->cursor++;
        }
    }
    key = t->key;
    if (tone[0] & 0xC0) {                           /* key split or rhythm set: the voice comes from a second group */
        int sub = (tone[0] & 0x40) ? P8(rd32(t->tone + 8))[t->key] : t->key;
        tone = P8(rd32(t->tone + 4)) + sub * 12;
        if (tone[0] & 0xC0)
            goto dropped;
        if (t->tone[0] & 0x80) {
            if (tone[3] & 0x80)
                rhythm = tone[3];
            key = tone[1];
        }
    }
    priority = p->priority + t->priority;
    if (priority > 255) priority = 255;
    kind = tone[0] & 7;
    if (kind) {
        if (kind > 4)
            goto dropped;
        c = &channels[PCM_CHANNELS + kind - 1];
        if ((c->status & CH_ON) && !(c->status & CH_STOP)) {
            if (c->priority > priority || (c->priority == priority && track_order(c->player, c->track) < order))
                goto dropped;
        }
    } else {
        int best_priority = priority, releasing = 0;
        uint32_t best_order = order;
        for (i = 0; i < sound.max_channels && i < PCM_CHANNELS; i++) {
            struct Channel *x = &channels[i];
            uint32_t x_order = track_order(x->player, x->track);
            if (!(x->status & CH_ON)) { c = x; break; }
            if (x->status & CH_STOP) {
                if (!releasing) {
                    releasing = 1;
                    best_priority = x->priority;
                    best_order = x_order;
                    c = x;
                    continue;
                }
            } else if (releasing) {
                continue;
            }
            if (x->priority < best_priority) {
                best_priority = x->priority;
                best_order = x_order;
                c = x;
            } else if (x->priority == best_priority && x_order >= best_order) {
                best_order = x_order;
                c = x;
            }
        }
        if (!c)
            goto dropped;
    }
    if (!kind) {
        uint32_t wav = rd32(tone + 4);
        if (wav < 0x08000000u || wav >= 0x0A000000u)
            goto dropped;                           /* a voice with no sample */
        channel_silence(c);                         /* the channel is taken over */
    }
    c->player = player;
    c->track = index;
    c->stat_player = player;
    c->stat_track = index;
    c->serial = ++serial_counter;
    t->lfo_delay_c = t->lfo_delay;
    if (t->lfo_delay)
        clear_mod(t);
    if (rhythm) {
        t->pan_x = (int8_t)((rhythm - 0xC0) << 1);
        t->flags |= VOLCHG;
    }
    track_vol_pit(t);
    c->gate = t->gate;
    c->midi_key = t->key;
    c->velocity = t->velocity;
    c->priority = (uint8_t)priority;
    c->key = (uint8_t)key;
    c->type = tone[0];
    c->wav = rd32(tone + 4);
    c->attack = tone[8]; c->decay = tone[9]; c->sustain = tone[10]; c->release = tone[11];
    c->echo_volume = t->echo_volume;
    c->echo_length = t->echo_length;
    c->right = (uint8_t)((t->vol_r * c->velocity) >> 7);
    c->left = (uint8_t)((t->vol_l * c->velocity) >> 7);
    final_key = key + t->key_m;
    if (final_key < 0) final_key = 0;
    if (kind) {
        c->length = tone[2];
        c->sweep = ((tone[3] & 0x80) || !(tone[3] & 0x70)) ? 8 : tone[3];
        c->cgb_frequency = cgb_frequency(kind, final_key, t->pit_m);
    } else {
        c->frequency = key_frequency(c->wav, final_key, t->pit_m);
    }
    c->status = CH_START;
    t->flags &= 0xF0;
    test_stats[player][index].notes++;
    test_stats[player][index].kinds[kind]++;
    return;
dropped:
    test_stats[player][index].dropped++;
}

static void track_fine(struct Player *p, int index)
{
    int i, player = (int)(p - players);
    for (i = 0; i < CHANNELS; i++) {
        struct Channel *c = &channels[i];
        if (c->player == player && c->track == index) {
            if (c->status & CH_ON)
                c->status |= CH_STOP;
            c->track = -1;
        }
    }
    p->tracks[index].flags = 0;
}

static void command(struct Player *p, int index, int code)
{
    struct Track *t = &p->tracks[index];
    int player = (int)(p - players), i, value;

    switch (code) {
    case 0xB2: t->cursor = P8(rd32(t->cursor)); break;                      /* GOTO */
    case 0xB3:                                                              /* PATT */
        if (t->pattern_level < 3) {
            t->stack[t->pattern_level++] = t->cursor + 4;
            t->cursor = P8(rd32(t->cursor));
        } else {
            track_fine(p, index);
        }
        break;
    case 0xB4:                                                              /* PEND */
        if (t->pattern_level)
            t->cursor = t->stack[--t->pattern_level];
        break;
    case 0xB5:                                                              /* REPT */
        if (t->cursor[0] == 0) {
            t->cursor = P8(rd32(t->cursor + 1));
        } else if (++t->repeat < t->cursor[0]) {
            t->cursor = P8(rd32(t->cursor + 1));
        } else {
            t->repeat = 0;
            t->cursor += 5;
        }
        break;
    case 0xB9: {                                                            /* MEMACC */
        int op = t->cursor[0], data = t->cursor[2], jump = -1;
        uint8_t *at = &mem_acc_area[t->cursor[1] & 0x7F], other = mem_acc_area[data & 0x7F];
        t->cursor += 3;
        switch (op) {
        case 0: *at = (uint8_t)data; break;
        case 1: *at = (uint8_t)(*at + data); break;
        case 2: *at = (uint8_t)(*at - data); break;
        case 3: *at = other; break;
        case 4: *at = (uint8_t)(*at + other); break;
        case 5: *at = (uint8_t)(*at - other); break;
        case 6: jump = *at == data; break;
        case 7: jump = *at != data; break;
        case 8: jump = *at > data; break;
        case 9: jump = *at >= data; break;
        case 10: jump = *at <= data; break;
        case 11: jump = *at < data; break;
        case 12: jump = *at == other; break;
        case 13: jump = *at != other; break;
        case 14: jump = *at > other; break;
        case 15: jump = *at >= other; break;
        case 16: jump = *at <= other; break;
        case 17: jump = *at < other; break;
        }
        if (jump == 1) t->cursor = P8(rd32(t->cursor));
        else if (jump == 0) t->cursor += 4;
        break;
    }
    case 0xBA: t->priority = *t->cursor++; break;
    case 0xBB:                                                              /* TEMPO */
        p->tempo_d = *t->cursor++ * 2u;
        p->tempo_i = (p->tempo_d * p->tempo_u) >> 8;
        break;
    case 0xBC: t->key_shift = (int8_t)*t->cursor++; t->flags |= PITCHG; break;
    case 0xBD: memcpy(t->tone, P8(p->tone + *t->cursor++ * 12u), 12); break;
    case 0xBE: t->vol = *t->cursor++; t->flags |= VOLCHG; break;
    case 0xBF: t->pan = (int8_t)(*t->cursor++ - 64); t->flags |= VOLCHG; break;
    case 0xC0: t->bend = (int8_t)(*t->cursor++ - 64); t->flags |= PITCHG; break;
    case 0xC1: t->bend_range = *t->cursor++; t->flags |= PITCHG; break;
    case 0xC2:                                                              /* LFOS */
        t->lfo_speed = *t->cursor++;
        if (!t->lfo_speed) clear_mod(t);
        break;
    case 0xC3: t->lfo_delay = *t->cursor++; break;
    case 0xC4:                                                              /* MOD */
        t->mod = *t->cursor++;
        if (!t->mod) clear_mod(t);
        break;
    case 0xC5:                                                              /* MODT */
        value = *t->cursor++;
        if (value != t->mod_type) {
            t->mod_type = (uint8_t)value;
            t->flags |= VOLCHG | PITCHG;
        }
        break;
    case 0xC8: t->tune = (int8_t)(*t->cursor++ - 64); t->flags |= PITCHG; break;
    case 0xCC: t->cursor += 2; break;                                       /* PORT: a register write, not done */
    case 0xCD:                                                              /* XCMD */
        value = *t->cursor++;
        switch (value) {
        case 0: memcpy(t->tone + 4, t->cursor, 4); t->cursor += 4; break;   /* xWAVE */
        case 1: t->tone[0] = *t->cursor++; break;                           /* xTYPE */
        case 3: t->tone[8] = *t->cursor++; break;                           /* xATTA */
        case 4: t->tone[9] = *t->cursor++; break;                           /* xDECA */
        case 5: t->tone[10] = *t->cursor++; break;                          /* xSUST */
        case 6: t->tone[11] = *t->cursor++; break;                          /* xRELE */
        case 7: t->echo_volume = *t->cursor++; break;                       /* xIECV */
        case 8: t->echo_length = *t->cursor++; break;                       /* xIECL */
        case 9: t->tone[2] = *t->cursor++; break;                           /* xLENG */
        case 10: t->tone[3] = *t->cursor++; break;                          /* xSWEE */
        default: track_fine(p, index); break;
        }
        break;
    case 0xCE: {                                                            /* EOT: the newest note of that key */
        struct Channel *found = NULL;
        if (*t->cursor < 0x80)
            t->key = *t->cursor++;
        for (i = 0; i < CHANNELS; i++) {
            struct Channel *c = &channels[i];
            if (c->player == player && c->track == index && (c->status & 0x83) && !(c->status & CH_STOP)
                && c->midi_key == t->key && (!found || c->serial > found->serial))
                found = c;
        }
        if (found)
            found->status |= CH_STOP;
        break;
    }
    default:                                                                /* FINE, and the codes the engine ends on */
        track_fine(p, index);
        break;
    }
}

/* FadeOutBody */
static void fade_out_body(struct Player *p)
{
    int i;
    if (p->fade_interval == 0)
        return;
    p->fade_counter = (p->fade_counter - 1) & 0xFFFF;
    if (p->fade_counter != 0)
        return;
    p->fade_volume -= 16;
    if ((int16_t)p->fade_volume <= 0) {
        for (i = 0; i < p->track_count; i++) {
            track_stop(p, i);
            p->tracks[i].flags = 0;
        }
        return;
    }
    p->fade_counter = p->fade_interval;
    for (i = 0; i < p->track_count; i++) {
        if (p->tracks[i].flags & EXIST) {
            p->tracks[i].vol_x = (uint8_t)(p->fade_volume >> 2);
            p->tracks[i].flags |= VOLCHG;
        }
    }
}

/* MPlayMain */
static void player_main(struct Player *p)
{
    int player = (int)(p - players), i, j, guard;
    unsigned tempo;

    if (p->status < 0)
        return;
    fade_out_body(p);
    tempo = p->tempo_c + p->tempo_i;
    while (tempo >= 150) {
        int32_t active = 0;
        for (i = 0; i < p->track_count; i++) {
            struct Track *t = &p->tracks[i];
            if (!(t->flags & EXIST))
                continue;
            active |= 1 << i;
            for (j = 0; j < CHANNELS; j++) {
                struct Channel *c = &channels[j];
                if (c->player != player || c->track != i)
                    continue;
                if (!(c->status & CH_ON))
                    c->track = -1;
                else if (c->gate != 0 && --c->gate == 0)
                    c->status |= CH_STOP;
            }
            if (t->flags & START)
                track_init(t);
            for (guard = 0; t->wait == 0; guard++) {
                int code = *t->cursor;
                if (code < 0x80) {
                    code = t->running;
                } else {
                    t->cursor++;
                    if (code >= 0xBD)
                        t->running = (uint8_t)code;
                }
                if (code >= 0xCF) {
                    note(p, i, code - 0xCF);
                } else if (code > 0xB0) {
                    command(p, i, code);
                    if (t->flags == 0)
                        break;
                } else if (code >= 0x80) {
                    t->wait = clock_table[code - 0x80];
                } else {
                    track_fine(p, i);               /* data with no command before it */
                    break;
                }
                if (guard > 20000) {                /* a stream that never rests would hang the game */
                    track_fine(p, i);
                    break;
                }
            }
            if (t->flags == 0)
                continue;
            t->wait--;
            if (t->lfo_speed && t->mod) {
                if (t->lfo_delay_c) {
                    t->lfo_delay_c--;
                } else {
                    int wave;
                    t->lfo_speed_c = (uint8_t)(t->lfo_speed_c + t->lfo_speed);
                    if ((int8_t)(t->lfo_speed_c - 0x40) < 0)
                        wave = (int8_t)t->lfo_speed_c;
                    else
                        wave = 0x80 - t->lfo_speed_c;
                    wave = (t->mod * wave) >> 6;
                    if ((uint8_t)(t->mod_m ^ wave)) {
                        t->mod_m = (int8_t)wave;
                        t->flags |= t->mod_type == 0 ? PITCHG : VOLCHG;
                    }
                }
            }
        }
        if (active == 0) {
            p->status = PAUSED;
            p->tempo_c = tempo;
            write_status(p);
            return;
        }
        p->status = active;
        tempo -= 150;
    }
    p->tempo_c = tempo;
    write_status(p);
}

/* The last part of MPlayMain: a track whose volume or pitch changed passes it on to its channels. */
static void player_update_channels(struct Player *p)
{
    int player = (int)(p - players), i, j;
    for (i = 0; i < p->track_count; i++) {
        struct Track *t = &p->tracks[i];
        int flags = t->flags;
        if (!(flags & EXIST) || !(flags & 0x0F))
            continue;
        track_vol_pit(t);
        for (j = 0; j < CHANNELS; j++) {
            struct Channel *c = &channels[j];
            int kind;
            if (c->player != player || c->track != i)
                continue;
            if (!(c->status & CH_ON)) {
                c->track = -1;
                continue;
            }
            kind = c->type & 7;
            if (flags & VOLCHG) {
                c->right = (uint8_t)((t->vol_r * c->velocity) >> 7);
                c->left = (uint8_t)((t->vol_l * c->velocity) >> 7);
                if (kind)
                    c->modify |= 1;
            }
            if (flags & PITCHG) {
                int key = c->key + t->key_m;
                if (key < 0) key = 0;
                if (kind) {
                    c->cgb_frequency = cgb_frequency(kind, key, t->pit_m);
                    c->modify |= 2;
                } else {
                    c->frequency = key_frequency(c->wav, key, t->pit_m);
                }
            }
        }
    }
}

/* ---- the Game Boy channels: the registers CgbSound writes, and what the hardware makes of them ---- */

static void psg_trigger(int ch)
{
    struct Psg *h = &psg[ch - 1];
    if (ch == 3) {
        if (!h->dac)
            return;
        h->on = 1;
        h->phase = 0;
        if (h->length == 0) h->length = 256;
        return;
    }
    h->dac = (h->nrx2 & 0xF8) != 0;
    h->on = h->dac;
    h->volume = h->nrx2 >> 4;
    h->env_add = (h->nrx2 & 8) ? 1 : -1;
    h->env_period = h->nrx2 & 7;
    h->env_timer = h->env_period ? h->env_period : 8;
    if (h->length == 0) h->length = 64;
    if (ch == 4)
        h->lfsr = (h->noise_reg & 8) ? 0x7F : 0x7FFF;
    if (ch == 1) {
        int period = (h->sweep_reg >> 4) & 7, shift = h->sweep_reg & 7;
        h->shadow = h->frequency;
        h->sweep_timer = period ? period : 8;
        h->sweep_on = period || shift;
        if (shift && !(h->sweep_reg & 8) && h->shadow + (h->shadow >> shift) > 2047)
            h->on = 0;
    }
}

/* NRx4: the top of the frequency, the length switch and the trigger */
static void psg_nrx4(int ch, int value)
{
    struct Psg *h = &psg[ch - 1];
    if (ch != 4)
        h->frequency = (h->frequency & 0xFF) | ((value & 7) << 8);
    h->length_on = (value & 0x40) != 0;
    if (value & 0x80)
        psg_trigger(ch);
}

/* CgbOscOff */
static void psg_off(int ch)
{
    struct Psg *h = &psg[ch - 1];
    if (ch == 3) {
        h->dac = 0;
        h->on = 0;
    } else {
        h->nrx2 = 8;
        psg_trigger(ch);
    }
}

/* The 512 Hz frame sequencer: length at 256 Hz, sweep at 128 Hz, volume envelope at 64 Hz */
static void psg_sequencer(void)
{
    int i, step = sequencer_step++ & 7;
    if (!(step & 1)) {
        for (i = 0; i < 4; i++) {
            struct Psg *h = &psg[i];
            if (h->length_on && h->length > 0 && --h->length == 0)
                h->on = 0;
        }
    }
    if (step == 2 || step == 6) {
        struct Psg *h = &psg[0];
        if (h->sweep_on && --h->sweep_timer <= 0) {
            int period = (h->sweep_reg >> 4) & 7, shift = h->sweep_reg & 7;
            h->sweep_timer = period ? period : 8;
            if (period) {
                int next = h->shadow + ((h->sweep_reg & 8) ? -(h->shadow >> shift) : (h->shadow >> shift));
                if (next > 2047) {
                    h->on = 0;
                } else if (shift) {
                    if (next < 0) next = 0;
                    h->shadow = h->frequency = next;
                }
            }
        }
    }
    if (step == 7) {
        for (i = 0; i < 4; i++) {
            struct Psg *h = &psg[i];
            if (i == 2 || !h->env_period)
                continue;
            if (--h->env_timer <= 0) {
                h->env_timer = h->env_period;
                if (h->env_add > 0 && h->volume < 15) h->volume++;
                else if (h->env_add < 0 && h->volume > 0) h->volume--;
            }
        }
    }
}

/* CgbModVol with CgbPan */
static void cgb_mod_vol(struct Channel *c)
{
    unsigned right = c->right, left = c->left;
    int hard = 0;
    if (right >= left) {
        if (right / 2 >= left) { c->pan = 0x0F; hard = 1; }
    } else if (left / 2 >= right) {
        c->pan = 0xF0;
        hard = 1;
    }
    c->env_goal = (uint8_t)((left + right) / 16);
    if (hard) {
        if (c->env_goal > 15) c->env_goal = 15;
    } else {
        c->pan = 0xFF;
    }
    c->sus_goal = (uint8_t)((c->env_goal * c->sustain + 15) >> 4);
    c->pan &= c->pan_mask;
}

/* CgbSound. The envelope program is the engine's, label for label; where it writes a sound register the
 * hardware model above takes the value. (The engine leaves the envelope byte of NRx2 as it last was when a
 * change of volume alone makes it write the register; here each channel remembers its own.) */
static void cgb_sound(void)
{
    int ch;

    if (sound.c15) sound.c15--;
    else sound.c15 = 14;
    for (ch = 1; ch <= 4; ch++) {
        struct Channel *c = &channels[PCM_CHANNELS + ch - 1];
        struct Psg *h = &psg[ch - 1];
        int previous = sound.c15;

        if (!(c->status & CH_ON))
            continue;
        if (c->status & CH_START) {
            if (c->status & CH_STOP)
                goto oscillator_off;
            c->status = 3;
            c->modify = 3;
            cgb_mod_vol(c);
            switch (ch) {
            case 1:
                h->sweep_reg = c->sweep;
                /* fall through */
            case 2:
                h->duty = (int)(c->wav & 3);
                h->length = 64 - (c->length & 63);
                goto init_step;
            case 3:
                if (c->wav != c->current_wave) {
                    int i, total = 0;
                    memcpy(h->ram, P8(c->wav), 16);
                    for (i = 0; i < 16; i++)
                        total += (h->ram[i] >> 4) + (h->ram[i] & 15);
                    h->mean = total / 32.0;
                    c->current_wave = c->wav;
                }
                h->dac = 0;
                h->on = 0;
                h->length = 256 - c->length;
                c->n4 = c->length ? 0xC0 : 0x80;
                break;
            default:
                h->length = 64 - (c->length & 63);
                h->noise_reg = (int)((c->wav << 3) & 0xFF);
            init_step:
                c->step_dir = (uint8_t)(c->attack + 8);
                c->n4 = c->length ? 0x40 : 0x00;
                break;
            }
            c->env_counter = c->attack;
            if ((int8_t)c->attack) {
                c->ev = 0;
                goto step_complete;
            }
            goto decay_start;
        } else if (c->status & CH_IEC) {
            c->echo_length--;
            if ((int8_t)c->echo_length <= 0) {
            oscillator_off:
                psg_off(ch);
                c->status = 0;
                goto channel_complete;
            }
            goto envelope_complete;
        } else if ((c->status & CH_STOP) && (c->status & CH_ENV)) {
            c->status &= (uint8_t)~CH_ENV;
            c->env_counter = c->release;
            if ((int8_t)c->release) {
                c->modify |= 1;
                if (ch != 3)
                    c->step_dir = c->release;
                goto step_complete;
            }
            goto pseudo_echo_start;
        } else {
        step_repeat:
            if (c->env_counter == 0) {
                if (ch == 3)
                    c->modify |= 1;
                cgb_mod_vol(c);
                if ((c->status & CH_ENV) == 0) {                    /* release */
                    c->ev--;
                    if ((int8_t)c->ev <= 0) {
                    pseudo_echo_start:
                        c->ev = (uint8_t)((c->env_goal * c->echo_volume + 0xFF) >> 8);
                        if (c->ev) {
                            c->status |= CH_IEC;
                            c->modify |= 1;
                            goto envelope_complete;
                        }
                        goto oscillator_off;
                    }
                    c->env_counter = c->release;
                } else if ((c->status & CH_ENV) == 1) {             /* sustain */
                sustain:
                    c->ev = c->sus_goal;
                    c->env_counter = 7;
                } else if ((c->status & CH_ENV) == 2) {             /* decay */
                    c->ev--;
                    if ((int8_t)c->ev <= (int8_t)c->sus_goal) {
                    sustain_start:
                        if (c->sustain == 0) {
                            c->status &= (uint8_t)~CH_ENV;
                            goto pseudo_echo_start;
                        }
                        c->status--;
                        c->modify |= 1;
                        if (ch != 3)
                            c->step_dir = 8;
                        goto sustain;
                    }
                    c->env_counter = c->decay;
                } else {                                            /* attack */
                    c->ev++;
                    if (c->ev >= c->env_goal) {
                    decay_start:
                        c->status--;
                        c->env_counter = c->decay;
                        if (c->env_counter) {
                            c->modify |= 1;
                            c->ev = c->env_goal;
                            if (ch != 3)
                                c->step_dir = c->decay;
                        } else {
                            goto sustain_start;
                        }
                    } else {
                        c->env_counter = c->attack;
                    }
                }
            }
        }
    step_complete:
        /* every 15th frame the step is done twice, to keep up with the hardware's 64 Hz envelope */
        c->env_counter--;
        if (previous == 0) {
            previous--;
            goto step_repeat;
        }
    envelope_complete:
        if (c->modify & 2) {
            if (ch < 4 && (c->type & 8)) {
                if (sound.soundbias_h < 0x40)
                    c->cgb_frequency = (c->cgb_frequency + 2) & 0x7FC;
                else if (sound.soundbias_h < 0x80)
                    c->cgb_frequency = (c->cgb_frequency + 1) & 0x7FE;
            }
            if (ch != 4)
                h->frequency = (h->frequency & 0x700) | (int)(c->cgb_frequency & 0xFF);
            else
                h->noise_reg = (h->noise_reg & 0x08) | (int)(c->cgb_frequency & 0xFF);
            c->n4 = (uint8_t)((c->n4 & 0xC0) + ((c->cgb_frequency >> 8) & 0xFF));
            psg_nrx4(ch, c->n4);
        }
        if (c->modify & 1) {
            h->left = (c->pan & 0xF0) != 0;
            h->right = (c->pan & 0x0F) != 0;
            if (ch == 3) {
                h->wave_code = cgb3_volume[c->ev & 15];
                if (c->n4 & 0x80) {
                    h->dac = 1;
                    psg_nrx4(ch, c->n4);
                    c->n4 &= 0x7F;
                }
            } else {
                h->nrx2 = ((c->step_dir & 0x0F) + (c->ev << 4)) & 0xFF;
                psg_nrx4(ch, c->n4 | 0x80);
            }
        }
    channel_complete:
        c->modify = 0;
    }
}

/* Time the square wave is high between two phases (in periods), for a duty d: the mean over an output sample
 * instead of one point of it, which is what keeps a 48 kHz mix of a square wave from whistling. */
static double square_high(double phase, double duty)
{
    double whole = floor(phase), part = phase - whole;
    return whole * duty + (part < duty ? part : duty);
}

static void psg_render(float *sum, int frames)
{
    static const double duties[4] = { 0.125, 0.25, 0.5, 0.75 };
    int ch, n;

    for (n = 0; n < frames; n++) {
        sequencer_carry += 512.0 / RATE;
        if (sequencer_carry >= 1.0) {
            sequencer_carry -= 1.0;
            psg_sequencer();
        }
        for (ch = 0; ch < 4; ch++) {
            struct Psg *h = &psg[ch];
            struct Channel *c = &channels[PCM_CHANNELS + ch];
            double target = 0, value = 0, advance;

            if (h->on) {
                if (ch != 2)
                    target = h->volume / 15.0;
                else if (h->wave_code & 0x80)
                    target = 0.75;
                else
                    target = ((h->wave_code >> 5) & 3) ? 1.0 / (1 << (((h->wave_code >> 5) & 3) - 1)) : 0.0;
            }
            h->level += (target - h->level) * 0.02;         /* about a millisecond: no click at a step of volume */
            if (h->level < 1e-4 && target == 0)
                continue;
            if (ch < 2) {
                advance = 131072.0 / (2048 - h->frequency) / RATE;
                if (advance >= 0.5) {
                    value = 0;                              /* above what the output can carry: its mean */
                } else {
                    double duty = duties[h->duty];
                    value = (square_high(h->phase + advance, duty) - square_high(h->phase, duty)) / advance - duty;
                }
                h->phase += advance;
                if (h->phase >= 1.0) h->phase -= floor(h->phase);
            } else if (ch == 2) {
                advance = 2097152.0 / (2048 - h->frequency) / RATE;        /* in wave steps: 32 to a period */
                if (advance < 16.0) {
                    double left = advance, at = h->phase, total = 0;
                    while (left > 1e-9) {
                        int step = (int)at & 31, byte = h->ram[step >> 1];
                        double take = 1.0 - (at - floor(at));
                        if (take > left) take = left;
                        total += (((step & 1) ? byte & 15 : byte >> 4) - h->mean) * take;
                        at += take;
                        left -= take;
                    }
                    value = total / advance / 15.0;
                }
                h->phase += advance;
                if (h->phase >= 32.0) h->phase -= 32.0 * floor(h->phase / 32.0);
            } else {
                int ratio = h->noise_reg & 7, shift = h->noise_reg >> 4, clocks = 0;
                double high = 0;
                advance = shift >= 14 ? 0 : 524288.0 / (ratio ? ratio : 0.5) / (double)(1 << (shift + 1)) / RATE;
                h->phase += advance;
                while (h->phase >= 1.0 && clocks < 64) {
                    unsigned bit = (h->lfsr ^ (h->lfsr >> 1)) & 1;
                    h->lfsr = (h->lfsr >> 1) | (bit << 14);
                    if (h->noise_reg & 8)
                        h->lfsr = (h->lfsr & ~0x40u) | (bit << 6);
                    high += (~h->lfsr & 1);
                    h->phase -= 1.0;
                    clocks++;
                }
                if (h->phase >= 1.0) h->phase -= floor(h->phase);
                value = (clocks ? high / clocks : (double)(~h->lfsr & 1)) - 0.5;
            }
            value *= h->level * 256.0 * PSG_UNIT;
            if (h->left) sum[n * 2] += (float)value;
            if (h->right) sum[n * 2 + 1] += (float)value;
            if (test_song >= 0 && c->stat_player >= 0)
                test_stats[c->stat_player][c->stat_track].energy += value * value * ((h->left != 0) + (h->right != 0)) / 16e6;
        }
    }
}

/* ---- the sampled channels: SoundMainRAM ---- */

/* One frame of a channel's envelope. Returns 0 when the channel is off. */
static int pcm_envelope(struct Channel *c)
{
    int ev = c->ev, status = c->status;

    if (!(status & CH_ON))
        return 0;
    if (status & CH_START) {
        const uint8_t *wave = P8(c->wav);
        if (status & CH_STOP) {
            c->status = 0;
            return 0;
        }
        status = 3;
        c->data = (const int8_t *)wave + 16;
        c->size = rd32(wave + 12);
        c->loop_start = rd32(wave + 8);
        c->loops = (wave[3] & 0xC0) != 0 && c->loop_start < c->size;
        c->position = 0;
        c->sounding = 0;
        if (wave[3] & 0xC0)
            status |= CH_LOOP;
        ev = c->attack;
        if (ev >= 0xFF) { ev = 0xFF; status--; }
    } else if (status & CH_IEC) {
        if (--c->echo_length == 0 || c->echo_length == 0xFF) {
            c->echo_length = 0;
            channel_silence(c);
            return 0;
        }
    } else if (status & CH_STOP) {
        ev = (ev * c->release) >> 8;
        if (ev <= c->echo_volume) {
        echo:
            ev = c->echo_volume;
            if (ev == 0) {
                channel_silence(c);
                return 0;
            }
            status |= CH_IEC;
        }
    } else if ((status & CH_ENV) == 2) {
        ev = (ev * c->decay) >> 8;
        if (ev <= c->sustain) {
            ev = c->sustain;
            if (ev == 0)
                goto echo;
            status--;
        }
    } else if ((status & CH_ENV) == 3) {
        ev += c->attack;
        if (ev >= 0xFF) { ev = 0xFF; status--; }
    }
    c->ev = (uint8_t)ev;
    c->status = (uint8_t)status;
    return 1;
}

static void pcm_render(struct Channel *c, float *sum, int frames)
{
    int level = ((sound.master_volume + 1) * c->ev) >> 4, n;
    float target_r = (float)((level * c->right) >> 8) * PCM_UNIT, target_l = (float)((level * c->left) >> 8) * PCM_UNIT;
    float gain_r, gain_l, step_r, step_l;
    double position = c->position, advance = ((c->type & 8) ? sound.pcm_rate : c->frequency) / RATE;
    double energy = 0;

    if (!c->sounding) {                             /* a new note starts at its volume */
        c->gain_r = target_r;
        c->gain_l = target_l;
        c->sounding = 1;
    }
    gain_r = c->gain_r; gain_l = c->gain_l;
    step_r = (target_r - gain_r) / (float)frames;
    step_l = (target_l - gain_l) / (float)frames;
    for (n = 0; n < frames; n++) {
        uint32_t at = (uint32_t)position, after;
        float fraction, sample, l, r;
        if (at >= c->size) {
            if (!c->loops) {                        /* the sample is over */
                c->status = 0;
                c->sounding = 0;
                break;
            }
            position = c->loop_start + fmod(position - c->loop_start, (double)(c->size - c->loop_start));
            at = (uint32_t)position;
        }
        /* between this sample and the next (the loop's first one at the end of a loop): stepping from one 8-bit
         * sample to the next at 48 kHz adds a hard edge the console's own output filters away */
        after = at + 1 < c->size ? at + 1 : (c->loops ? c->loop_start : at);
        fraction = (float)(position - (double)at);
        sample = (float)c->data[at] + ((float)c->data[after] - (float)c->data[at]) * fraction;
        gain_l += step_l;
        gain_r += step_r;
        l = sample * gain_l;
        r = sample * gain_r;
        sum[n * 2] += l;
        sum[n * 2 + 1] += r;
        energy += (double)l * l + (double)r * r;
        position += advance;
    }
    c->position = position;
    c->gain_l = target_l;
    c->gain_r = target_r;
    if (test_song >= 0 && c->stat_player >= 0)
        test_stats[c->stat_player][c->stat_track].energy += energy / 16e6;
}

static void tails_render(float *sum, int frames)
{
    int i, n;
    for (i = 0; i < TAILS; i++) {
        struct Tail *t = &tails[i];
        for (n = 0; n < frames && t->left > 0; n++, t->left--) {
            uint32_t at = (uint32_t)t->position;
            float fade = (float)t->left / TAIL_LENGTH, sample;
            if (at >= t->size) {
                if (!t->loops) { t->left = 0; break; }
                t->position = t->loop_start + fmod(t->position - t->loop_start, (double)(t->size - t->loop_start));
                at = (uint32_t)t->position;
            }
            sample = (float)t->data[at] * fade;
            sum[n * 2] += sample * t->gain_l;
            sum[n * 2 + 1] += sample * t->gain_r;
            t->position += t->advance;
        }
    }
}

static void mix(int16_t *out, int frames)
{
    static float sum[2048 * 2];
    int i, n;

    if (frames > 2048) frames = 2048;
    memset(sum, 0, sizeof(float) * 2 * (size_t)frames);
    for (i = 0; i < PCM_CHANNELS; i++)
        if (pcm_envelope(&channels[i]))
            pcm_render(&channels[i], sum, frames);
    tails_render(sum, frames);
    /* Reverb: the engine adds to each frame what its buffer held one buffer length ago and one frame less than
     * that, both sides together, times reverb/512; the sampled channels are mixed on top, so it feeds back. */
    {
        unsigned size = sizeof reverb_l / sizeof reverb_l[0];
        unsigned far = (unsigned)(sound.dma_period * (RATE / 59.7275)), near = (unsigned)((sound.dma_period - 1) * (RATE / 59.7275));
        float amount = (float)sound.reverb / 512.0f;
        if (far >= size) far = size - 1;
        for (n = 0; n < frames; n++) {
            float echo = 0;
            if (sound.reverb) {
                unsigned a = (reverb_at - far) & (size - 1), b = (reverb_at - near) & (size - 1);
                echo = (reverb_l[a] + reverb_r[a] + reverb_l[b] + reverb_r[b]) * amount;
            }
            sum[n * 2] += echo;
            sum[n * 2 + 1] += echo;
            reverb_l[reverb_at] = sum[n * 2];
            reverb_r[reverb_at] = sum[n * 2 + 1];
            reverb_at = (reverb_at + 1) & (size - 1);
        }
    }
    psg_render(sum, frames);
    for (n = 0; n < frames * 2; n++) {
        /* The console's amplifier rolls the top off; without that the square and noise channels sound brittle.
         * One-pole low-pass at about 14 kHz, per channel. */
        static int32_t filtered[2];
        int32_t s = (int32_t)sum[n] >> 2, magnitude;
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
    memset(channels, 0, sizeof channels);
    memset(psg, 0, sizeof psg);
    memset(tails, 0, sizeof tails);
    memset(&sound, 0, sizeof sound);
    memset(mem_acc_area, 0, sizeof mem_acc_area);
    memset(reverb_l, 0, sizeof reverb_l);
    memset(reverb_r, 0, sizeof reverb_r);
    for (i = 0; i < 12; i++)
        semitone[i] = pow(2.0, i / 12.0);
    for (i = 0; i < CHANNELS; i++) {
        channels[i].player = channels[i].stat_player = -1;
        channels[i].track = -1;
    }
    for (i = 0; i < 4; i++) {                       /* MPlayExtender */
        channels[PCM_CHANNELS + i].type = (uint8_t)(i + 1);
        channels[PCM_CHANNELS + i].pan_mask = (uint8_t)(0x11 << i);
        psg[i].lfsr = 0x7FFF;
    }
    sound.max_channels = 8;                         /* SoundInit, then the game's mode */
    sound.master_volume = 15;
    sound.soundbias_h = 0x42;
    sound_mode(0x00040000);
    sound_mode(SS_M4A_INIT_MODE);
    player_count = 0;
    for (i = 0; i < MAX_PLAYERS; i++) {             /* MPlayOpen for every entry of gMPlayTable */
        uint32_t info = rd32(gMPlayTable + i * 12);
        struct Player *p;
        if (info < SS_EWRAM || info >= SS_IWRAM + 0x8000)
            break;
        p = &players[player_count++];
        p->info = info;
        p->tracks_address = rd32(gMPlayTable + i * 12 + 4);
        p->track_count = gMPlayTable[i * 12 + 8] > MAX_TRACKS ? MAX_TRACKS : gMPlayTable[i * 12 + 8];
        p->status = PAUSED;
        p->tempo_u = 0x100;
        write_status(p);
    }
    initialised = 1;
}

void m4aSoundMain(void)
{
    static int16_t buffer[2048 * 2];
    int frames, i;

    if (!initialised)
        return;
    ss_prof(SS_PROF_SOUND, 1);
    if (!test_checked)
        test_setup();
    if (test_song >= 0 && main_calls == (unsigned long)test_at) {
        test_inside = 1;
        m4aSongNumStart((uint16_t)test_song);
        test_inside = 0;
    }
    main_calls++;
    for (i = 0; i < player_count; i++) {            /* each MPlayMain calls the player opened before it first */
        if (players[i].status < 0)
            continue;
        player_main(&players[i]);
        if (players[i].status >= 0)
            player_update_channels(&players[i]);
    }
    cgb_sound();
    sample_carry += RATE / 59.7275;
    frames = (int)sample_carry;
    sample_carry -= frames;
    mix(buffer, frames);
    ss_audio_push(buffer, frames);
    ss_prof(SS_PROF_SOUND, 0);
}

void m4aSoundVSync(void) {}
void m4aSoundVSyncOn(void) { test_call("m4aSoundVSyncOn", 0, 0, 0); }
void m4aSoundVSyncOff(void) { test_call("m4aSoundVSyncOff", 0, 0, 0); }

void m4aSoundMode(uint32_t mode)
{
    if (test_call("m4aSoundMode", mode, 0, 0) || !initialised)
        return;
    sound_mode(mode);
}

void m4aMPlayImmInit(uint32_t info)
{
    struct Player *p = player_of(info);
    int i;
    if (test_call("m4aMPlayImmInit", info, 0, 0) || !p)
        return;
    for (i = 0; i < p->track_count; i++)
        if ((p->tracks[i].flags & EXIST) && (p->tracks[i].flags & START))
            track_init(&p->tracks[i]);
}

void MPlayStart(uint32_t info, uint32_t header)
{
    struct Player *p = player_of(info);
    if (test_call("MPlayStart", info, header, 0) || !p)
        return;
    start_player(p, header);
}

static struct Player *song_player(uint16_t number, uint32_t *header)
{
    const uint8_t *entry = gSongTable + number * 8;
    *header = rd32(entry);
    return player_of(rd32(gMPlayTable + (entry[4] | entry[5] << 8) * 12));
}

void m4aSongNumStart(uint16_t number)
{
    uint32_t header;
    struct Player *p;
    if (test_call("m4aSongNumStart", number, 0, 0) || !initialised)
        return;
    p = song_player(number, &header);
    if (p)
        start_player(p, header);
}

void m4aSongNumStartOrChange(uint16_t number)
{
    uint32_t header;
    struct Player *p;
    if (test_call("m4aSongNumStartOrChange", number, 0, 0) || !initialised)
        return;
    p = song_player(number, &header);
    if (p && (p->header != header || !(p->status & 0xFFFF) || p->status < 0))
        start_player(p, header);
}

void m4aSongNumStartOrContinue(uint16_t number)
{
    uint32_t header;
    struct Player *p;
    if (test_call("m4aSongNumStartOrContinue", number, 0, 0) || !initialised)
        return;
    p = song_player(number, &header);
    if (!p)
        return;
    if (test_log)                                   /* what the call will do: the usual question when an area is silent */
        fprintf(stderr, "m4a   song %u on player %d: %s%s%c", number, (int)(p - players),
                p->header != header ? "started (another song was there)" : !(p->status & 0xFFFF) ? "started (it had ended)"
                : p->status < 0 ? "continued from its pause" : "left playing",
                p->header == header && (p->status & 0xFFFF) && p->fade_interval ? ", and it is fading out" : "", 10);
    if (p->header != header || !(p->status & 0xFFFF))
        start_player(p, header);
    else if (p->status < 0)
        continue_player(p);
}

void m4aSongNumStop(uint16_t number)
{
    uint32_t header;
    struct Player *p;
    if (test_call("m4aSongNumStop", number, 0, 0) || !initialised)
        return;
    p = song_player(number, &header);
    if (p && p->header == header)
        stop_player(p);
}

void m4aSongNumContinue(uint16_t number)
{
    uint32_t header;
    struct Player *p;
    if (test_call("m4aSongNumContinue", number, 0, 0) || !initialised)
        return;
    p = song_player(number, &header);
    if (p && p->header == header)
        continue_player(p);
}

void m4aMPlayStop(uint32_t info)
{
    struct Player *p = player_of(info);
    if (test_call("m4aMPlayStop", info, 0, 0) || !p)
        return;
    stop_player(p);
}

void m4aMPlayContinue(uint32_t info)
{
    struct Player *p = player_of(info);
    if (test_call("m4aMPlayContinue", info, 0, 0) || !p)
        return;
    continue_player(p);
}

void m4aMPlayAllStop(void)
{
    int i;
    if (test_call("m4aMPlayAllStop", 0, 0, 0))
        return;
    for (i = 0; i < player_count; i++)
        stop_player(&players[i]);
}

void m4aMPlayAllContinue(void)
{
    int i;
    if (test_call("m4aMPlayAllContinue", 0, 0, 0))
        return;
    for (i = 0; i < player_count; i++)
        continue_player(&players[i]);
}

void m4aMPlayFadeOut(uint32_t info, uint16_t speed)
{
    struct Player *p = player_of(info);
    if (test_call("m4aMPlayFadeOut", info, speed, 0) || !p)
        return;
    p->fade_counter = speed;
    p->fade_interval = speed;
    p->fade_volume = 64 << 2;
}

void m4aMPlayTempoControl(uint32_t info, uint16_t tempo)
{
    struct Player *p = player_of(info);
    if (test_call("m4aMPlayTempoControl", info, tempo, 0) || !p)
        return;
    p->tempo_u = tempo;
    p->tempo_i = (p->tempo_d * p->tempo_u) >> 8;
}

/* The controls reach the tracks of the mask that exist, as in the engine. */
#define FOR_TRACKS(name, info, bits, value) \
    struct Player *p = player_of(info); int i; \
    if (test_call(name, info, bits, (uint32_t)(value)) || !p) return; \
    for (i = 0; i < p->track_count; i++) if (((bits) & (1u << i)) && (p->tracks[i].flags & EXIST))

void m4aMPlayVolumeControl(uint32_t info, uint16_t bits, uint16_t volume)
{
    FOR_TRACKS("m4aMPlayVolumeControl", info, bits, volume) {
        p->tracks[i].vol_x = (uint8_t)(volume / 4);
        p->tracks[i].flags |= VOLCHG;
    }
}

void m4aMPlayPitchControl(uint32_t info, uint16_t bits, int16_t pitch)
{
    FOR_TRACKS("m4aMPlayPitchControl", info, bits, pitch) {
        p->tracks[i].key_shift_x = (int8_t)(pitch >> 8);
        p->tracks[i].pit_x = pitch;
        p->tracks[i].flags |= PITCHG;
    }
}

void m4aMPlayPanpotControl(uint32_t info, uint16_t bits, int8_t pan)
{
    FOR_TRACKS("m4aMPlayPanpotControl", info, bits, pan) {
        p->tracks[i].pan_x = pan;
        p->tracks[i].flags |= VOLCHG;
    }
}

void m4aMPlayModDepthSet(uint32_t info, uint16_t bits, uint8_t depth)
{
    FOR_TRACKS("m4aMPlayModDepthSet", info, bits, depth) {
        p->tracks[i].mod = depth;
        if (!depth)
            clear_mod(&p->tracks[i]);
    }
}

void m4aMPlayLFOSpeedSet(uint32_t info, uint16_t bits, uint8_t speed)
{
    FOR_TRACKS("m4aMPlayLFOSpeedSet", info, bits, speed) {
        p->tracks[i].lfo_speed = speed;
        if (!speed)
            clear_mod(&p->tracks[i]);
    }
}

/* Asked by the game's area-music selector (games/ss2/port/patches.json): in a multiplayer game the cartridge
 * plays music only in the town and the first field. The port plays each stage's own song there too, unless
 * SS_MP_MUSIC=0 asks for the cartridge's silence. */
int ss_music_everywhere(void)
{
    static int wanted = -1;
    if (wanted < 0)
        wanted = !(getenv("SS_MP_MUSIC") && getenv("SS_MP_MUSIC")[0] == '0');
    return wanted;
}
