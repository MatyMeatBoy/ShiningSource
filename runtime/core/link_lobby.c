/* Game Link's presentation uses the save menu's original sprites and font from the loaded cartridge. */
#include "shiningsource/runtime.h"
#include "shiningsource/language.h"

#include <stdio.h>
#include <string.h>

enum {
    ROM_SIZE = 0x1000000, ARCHIVE = 0x518938, CLASS_TABLE = 0x479140,
    CLASS_SIZE = 0x58, CLASSES = 9, FONT_CELL = 128, FONT_GLYPHS = 256
};

struct canvas {
    uint32_t *pixels;
    const uint8_t *font;
    int width, spanish;
};

enum room_page { ROOM_CLOSED, ROOM_CHOICE, ROOM_CREATE, ROOM_JOIN, ROOM_CONNECTING, ROOM_UNAVAILABLE, ROOM_IN_GAME };
static const char room_alphabet[] = SS_ROOM_CODE_ALPHABET;
static struct {
    enum room_page page;
    int create, selection, key, length;
    char code[SS_ROOM_CODE_CAPACITY], confirmed_code[SS_ROOM_CODE_CAPACITY];
} room_menu;

void sub_080003F0(void);        /* yield to the game scheduler for one frame */

int ss_link_room_select(void)
{
    enum { A = 1, B = 2, START = 8, RIGHT = 16, LEFT = 32, UP = 64, DOWN = 128, L = 512 };
    room_menu.page = ROOM_CHOICE;
    room_menu.selection = 0;
    room_menu.confirmed_code[0] = '\0';
    for (;;) {
        sub_080003F0();
        uint16_t pressed = *(const uint16_t *)(uintptr_t)0x03001324;
        uint16_t move = *(const uint16_t *)(uintptr_t)0x03001308;
        switch (room_menu.page) {
        case ROOM_CHOICE:
            if (pressed & B) {
                ss_net_room_cancel();
                room_menu.page = ROOM_CLOSED;
                return 0;
            }
            if (move & UP)
                room_menu.selection = 0;
            if (move & DOWN)
                room_menu.selection = 1;
            if (pressed & A) {
                room_menu.create = !room_menu.selection;
                room_menu.length = room_menu.create ? SS_ROOM_CODE_LENGTH : 0;
                room_menu.key = 0;
                if (room_menu.create)
                    ss_net_room_generate(room_menu.code);
                else
                    room_menu.code[0] = '\0';
                room_menu.page = room_menu.create ? ROOM_CREATE : ROOM_JOIN;
            }
            break;
        case ROOM_CREATE:
        case ROOM_JOIN:
            if (pressed & B) {
                room_menu.page = ROOM_CHOICE;
                break;
            }
            if (room_menu.page == ROOM_JOIN) {
                int column = room_menu.key % 8, row = room_menu.key / 8;
                if (move & LEFT) column = (column + 7) % 8;
                if (move & RIGHT) column = (column + 1) % 8;
                if (move & UP) row = (row + 3) % 4;
                if (move & DOWN) row = (row + 1) % 4;
                room_menu.key = row * 8 + column;
                if ((pressed & L) && room_menu.length > 0)
                    room_menu.code[--room_menu.length] = '\0';
                if ((pressed & A) && room_menu.length < SS_ROOM_CODE_LENGTH) {
                    room_menu.code[room_menu.length++] = room_alphabet[room_menu.key];
                    room_menu.code[room_menu.length] = '\0';
                }
            }
            if (room_menu.length == SS_ROOM_CODE_LENGTH && (pressed & (room_menu.create ? A : START))) {
                room_menu.page = ss_net_room_begin(room_menu.code, room_menu.create) ? ROOM_CONNECTING : ROOM_UNAVAILABLE;
            }
            break;
        case ROOM_CONNECTING:
            if (pressed & B) {
                ss_net_room_cancel();
                room_menu.page = room_menu.create ? ROOM_CREATE : ROOM_JOIN;
            } else {
                int status = ss_net_room_status();
                if (status == SS_ROOM_READY) {
                    memcpy(room_menu.confirmed_code, room_menu.code, sizeof room_menu.code);
                    room_menu.page = ROOM_CLOSED;
                    return 1;
                }
                if (status == SS_ROOM_UNAVAILABLE)
                    room_menu.page = ROOM_UNAVAILABLE;
                else if (status == SS_ROOM_ACTIVE)
                    room_menu.page = ROOM_IN_GAME;
            }
            break;
        case ROOM_UNAVAILABLE:
        case ROOM_IN_GAME:
            if (pressed & B) {
                ss_net_room_cancel();
                room_menu.page = room_menu.create ? ROOM_CREATE : ROOM_JOIN;
            }
            break;
        default:
            return 0;
        }
    }
}

static unsigned read16(const uint8_t *data)
{
    return data[0] | (unsigned)data[1] << 8;
}

static uint32_t read32(const uint8_t *data)
{
    return read16(data) | (uint32_t)read16(data + 2) << 16;
}

static const uint8_t *rom_span(uint32_t start, uint32_t size)
{
    return start <= ROM_SIZE && size <= ROM_SIZE - start ? (const uint8_t *)(uintptr_t)SS_ROM + start : NULL;
}

static const uint8_t *from_offset(const uint8_t *base, uint32_t offset, uint32_t size)
{
    uint32_t start = (uint32_t)(base - rom_span(0, 0));
    return offset < ROM_SIZE && start <= ROM_SIZE - offset ? rom_span(start + offset, size) : NULL;
}

static const uint8_t *resource(unsigned category, unsigned index, unsigned size)
{
    const uint8_t *archive = rom_span(ARCHIVE, 48);
    const uint8_t *section = from_offset(archive, read32(archive + 8 + category * 4), 4);
    const uint8_t *offset = section ? from_offset(section, 4 + index * 4, 4) : NULL;
    return offset ? from_offset(section, read32(offset), size) : NULL;
}

static uint32_t rgb(unsigned red, unsigned green, unsigned blue)
{
    return 0xff000000u | red << 16 | green << 8 | blue;
}

static void box(const struct canvas *screen, int x, int y, int width, int height, uint32_t color)
{
    for (int row = y; row < y + height; row++)
        for (int col = x; col < x + width; col++)
            screen->pixels[row * screen->width + col] = color;
}

static void panel(const struct canvas *screen, int x, int y, int w, int h, uint32_t edge, uint32_t fill)
{
    box(screen, x + 2, y + 3, w, h, 0xff071016u);
    box(screen, x + 1, y, w - 2, h, edge);
    box(screen, x, y + 1, w, h - 2, edge);
    box(screen, x + 1, y + 1, w - 2, h - 2, 0xff0b2027u);
    box(screen, x + 2, y + 2, w - 4, h - 4, fill);
}

static const char *wording(const struct canvas *screen, const char *english, const char *spanish)
{
    return screen->spanish ? spanish : english;
}

static unsigned next_letter(const char **label)
{
    unsigned first = (unsigned char)*(*label)++;
    unsigned next = (unsigned char)**label;
    if ((first & 0xe0u) == 0xc0u && (next & 0xc0u) == 0x80u) {
        (*label)++;
        return ((first & 31) << 6) | (next & 63);
    }
    return first;
}

static const uint8_t *glyph(const struct canvas *screen, unsigned ch)
{
    static const uint8_t accents[] = {0xc1, 0xc9, 0xcd, 0xd3, 0xda, 0xd1};
    static const uint8_t cells[] = {0x61, 0x65, 0x69, 0x6d, 0x71, 0x75};
    if (ch >= 'A' && ch <= 'Z')
        return screen->font + (ch - 'A') * FONT_CELL;
    if (ch >= 'a' && ch <= 'z')
        return screen->font + (ch - 'a' + 32) * FONT_CELL;
    if (ch >= '0' && ch <= '9')
        return screen->font + (ch - '0' + 64) * FONT_CELL;
    for (unsigned i = 0; i < sizeof accents; i++) {
        if (ch == accents[i])
            return screen->font + cells[i] * FONT_CELL;
        if (ch == accents[i] + 0x20u)
            return screen->font + (cells[i] + 0x20) * FONT_CELL;
    }
    return NULL;
}

static unsigned nibble(const uint8_t *data, int x)
{
    return (data[x / 2] >> ((x & 1) * 4)) & 15;
}

static int glyph_edges(const uint8_t *cell, int *left)
{
    int first = 8, last = -1;
    *left = 0;
    if (!cell)
        return 4;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++)
            if (nibble(cell + y * 4, x)) {
                if (x < first) first = x;
                if (x > last) last = x;
            }
    *left = first;
    return last >= first ? last - first + 1 : 0;
}

static void text(const struct canvas *screen, int center, int y, const char *label, uint32_t color)
{
    int width = 1;
    for (const char *ch = label; *ch;) {
        int left;
        int columns = glyph_edges(glyph(screen, next_letter(&ch)), &left);
        width += columns > 1 ? columns - 1 : columns;
    }
    int x = center - width / 2;
    while (*label) {
        int left;
        const uint8_t *cell = glyph(screen, next_letter(&label));
        int columns = glyph_edges(cell, &left);
        if (cell)
            for (int row = 0; row < 16; row++)
                for (int col = 0; col < columns; col++) {
                    unsigned value = nibble(cell + row * 4, left + col);
                    if (value)
                        screen->pixels[(y + row) * screen->width + x + col] = value == 1 ? color : 0xff071016u;
                }
        x += columns > 1 ? columns - 1 : columns;    /* neighbouring glyphs share the original dark outline */
    }
}

static void background(const struct canvas *screen)
{
    int width = screen->width;
    for (int y = 0; y < SS_SCREEN_H; y++) {
        unsigned light = (unsigned)(80 - (y > 80 ? y - 80 : 80 - y));
        for (int x = 0; x < width; x++) {
            unsigned grain = ((unsigned)(x * 5 + y * 3) & 7) == 0 ? 3 : 0;
            screen->pixels[y * width + x] = rgb(10 + light / 8 + grain, 20 + light / 5 + grain, 28 + light / 5 + grain);
        }
    }
    box(screen, 3, 3, width - 6, 1, 0xff6c543cu);
    box(screen, 3, 156, width - 6, 1, 0xff6c543cu);
    box(screen, 3, 4, 1, 152, 0xff594632u);
    box(screen, width - 4, 4, 1, 152, 0xff594632u);
}

static void draw_room_menu(const struct canvas *screen)
{
    int middle = screen->width / 2;
    int wide = screen->width > 300 ? 272 : 220;
    int left = middle - wide / 2;
    uint32_t copper = 0xff976e46u, gold = 0xfff7d894u, white = 0xffe5eee1u;

    background(screen);
    panel(screen, left, 9, wide, 22, copper, 0xff203940u);
    if (room_menu.page == ROOM_CHOICE) {
        text(screen, middle, 12, wording(screen, "ONLINE MULTIPLAYER", "MULTIJUGADOR EN LÍNEA"), gold);
        for (int i = 0; i < 2; i++) {
            int selected = i == room_menu.selection;
            int y = 52 + 32 * i;
            panel(screen, left + 20, y, wide - 40, 25, selected ? gold : copper,
                  selected ? 0xff35534eu : 0xff213a40u);
            const char *choice = i == 0 ? wording(screen, "Create room", "Crear sala") :
                                         wording(screen, "Join room", "Unirse a sala");
            text(screen, middle, y + 4, choice, selected ? gold : white);
        }
        text(screen, middle, 131, wording(screen, "A Select    B Back", "A Elegir    B Atrás"), white);
        return;
    }

    const char *title = room_menu.create ? wording(screen, "CREATE ROOM", "CREAR SALA") :
                                          wording(screen, "JOIN ROOM", "UNIRSE A SALA");
    text(screen, middle, 12, title, gold);
    if (room_menu.page == ROOM_JOIN) {
        for (int i = 0; i < SS_ROOM_CODE_LENGTH; i++) {
            int x = middle - 79 + i * 20;
            char symbol[2] = {i < room_menu.length ? room_menu.code[i] : ' ', '\0'};
            panel(screen, x, 35, 18, 20, i == room_menu.length ? gold : copper, 0xff192d34u);
            text(screen, x + 9, 37, symbol, gold);
        }
        for (int i = 0; i < (int)sizeof room_alphabet - 1; i++) {
            int x = middle - 87 + i % 8 * 22;
            int y = 61 + i / 8 * 18;
            char symbol[2] = {room_alphabet[i], '\0'};
            int selected = i == room_menu.key;
            box(screen, x, y, 20, 16, selected ? 0xfff5cc7du : 0xff34565au);
            box(screen, x + 1, y + 1, 18, 14, selected ? 0xff496951u : 0xff1e3b43u);
            text(screen, x + 10, y, symbol, selected ? gold : white);
        }
        text(screen, middle, 135, room_menu.length == SS_ROOM_CODE_LENGTH ?
             wording(screen, "Start Join    L Erase    B Back", "Start Unirse    L Borrar    B Atrás") :
             wording(screen, "A Add    L Erase    B Back", "A Añadir    L Borrar    B Atrás"), white);
        return;
    }

    panel(screen, left, 47, wide, 68, copper, 0xff203c44u);
    text(screen, middle, 54, room_menu.code, gold);
    if (room_menu.page == ROOM_CONNECTING)
        text(screen, middle, 79, wording(screen, "Connecting to relay", "Conectando al servidor"), white);
    else if (room_menu.page == ROOM_UNAVAILABLE)
        text(screen, middle, 79, wording(screen, "Room unavailable", "Sala no disponible"), 0xfff0ac86u);
    else if (room_menu.page == ROOM_IN_GAME)
        text(screen, middle, 79, wording(screen, "Room already playing", "Sala ya en juego"), 0xfff0ac86u);
    else
        text(screen, middle, 79, wording(screen, "Share this room code", "Comparte este código"), white);
    text(screen, middle, 131, room_menu.page == ROOM_CREATE ? wording(screen, "A Create    B Back", "A Crear    B Atrás") :
         wording(screen, "B Back", "B Atrás"), white);
}

/* Animation 8 is the front-facing pose used by the original save-slot screen. Like sub_08007CF0,
 * each frame resolves a list of native object offsets, shapes, flips and 4bpp tile indices. */
static int character(const struct canvas *screen, int center, int feet, int class_id, int palette_id)
{
    static const uint8_t widths[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static const uint8_t heights[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    if (class_id < 0 || class_id >= CLASSES)
        return 0;
    const uint8_t *class_record = rom_span(CLASS_TABLE + class_id * CLASS_SIZE, 4);
    unsigned id = read32(class_record) & 0xffffu;
    const uint8_t *meta = resource(5, id, 12);
    const uint8_t *tiles = resource(6, 2 * id, 32);
    const uint8_t *palettes = resource(6, 2 * id + 1, 32);
    const uint8_t *next_tiles = resource(6, 2 * id + 2, 32);
    if (!meta || !tiles || !palettes || !next_tiles || next_tiles <= palettes || read16(meta + 2) <= 8)
        return 0;
    unsigned palette_count = (unsigned)(next_tiles - palettes) / 32;
    if (palette_id < 0 || (unsigned)palette_id >= palette_count)
        palette_id = 0;
    const uint8_t *anim = from_offset(meta, read32(meta + 8), 9 * 12);
    if (!anim)
        return 0;
    anim += 8 * 12;
    const uint8_t *list = from_offset(anim, read32(anim + 8), 2);
    const uint8_t *frames = from_offset(meta, read32(meta + 4), 12);
    const uint8_t *frame = list && frames ? from_offset(frames, read16(list) * 12, 12) : NULL;
    if (!frame || read16(frame + 2) > 32)
        return 0;
    unsigned count = read16(frame + 2);
    const uint8_t *object = from_offset(frame, read32(frame + 8), count * 8);
    if (!object)
        return 0;
    const uint8_t *palette = palettes + palette_id * 32;
    int drew_pixel = 0;
    for (unsigned i = count; i > 0; i--) {
        const uint8_t *part = object + (i - 1) * 8;
        unsigned attr0 = read16(part), attr1 = read16(part + 2);
        unsigned shape = attr0 >> 14, size = attr1 >> 14;
        if (shape > 2 || (attr0 & 0x2100u))
            continue;
        int w = widths[shape][size], h = heights[shape][size];
        unsigned tile = read16(part + 4) & 0xfff;
        const uint8_t *pixels = from_offset(tiles, tile * 32, (unsigned)w * h / 2);
        if (!pixels || pixels + (unsigned)w * h / 2 > palettes)
            continue;
        int ox = (int)(attr1 & 255) - (int)(attr1 & 256);
        int oy = (int)(attr0 & 127) - (int)(attr0 & 128);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                /* Clip only to the portrait cell, so a damaged descriptor cannot cover the name or UI. */
                if (ox + x < -22 || ox + x >= 22 || oy + y < -34 || oy + y >= 3)
                    continue;
                int px = attr1 & 0x1000u ? w - 1 - x : x;
                int py = attr1 & 0x2000u ? h - 1 - y : y;
                unsigned color = nibble(pixels + ((py / 8) * (w / 8) + px / 8) * 32 + (py & 7) * 4, px & 7);
                if (color) {
                    unsigned gba = read16(palette + 2 * color);
                    screen->pixels[(feet + oy + y) * screen->width + center + ox + x] =
                        rgb((gba & 31) * 255 / 31, ((gba >> 5) & 31) * 255 / 31, ((gba >> 10) & 31) * 255 / 31);
                    drew_pixel = 1;
                }
            }
    }
    return drew_pixel;
}

int ss_link_screen_draw(uint32_t *pixels, int width)
{
    static const char *const phase[][2] = {{"Open", "Libre"}, {"Linked", "En línea"}, {"Save slot", "Ranura"},
        {"Creating", "Creando"}, {"Ready", "Listo"}, {"In game", "Jugando"}, {"Rejoining", "Vuelve"}};
    static const uint32_t phase_color[] = {0xff778f8du, 0xff85c3c6u, 0xfff1cc86u, 0xfff1cc86u,
                                           0xffb7e4a5u, 0xffb7e4a5u, 0xffdd986cu};
    if (room_menu.page == ROOM_CLOSED && !ss_net_roster_lobby())
        return 0;
    const uint8_t *rom = rom_span(0, ROM_SIZE);
    if (memcmp(rom + 0xac, "AU2P", 4) || memcmp(rom + ARCHIVE, "SSDT", 4))
        return 0;
    const uint8_t *font = resource(4, 0, FONT_CELL * FONT_GLYPHS);
    if (!font)
        return 0;
    struct canvas screen = { pixels, font, width, ss_game_language_current() == SS_LANG_ES };
    if (room_menu.page != ROOM_CLOSED) {
        draw_room_menu(&screen);
        return 1;
    }

    /* Native WIN0 hides every layer when entering or leaving the lobby. Match the cartridge's blackout. */
    int lit = 0;
    int native_left = (width - SS_SCREEN_W) / 2;
    for (int y = 4; y < SS_SCREEN_H && !lit; y += 8)
        for (int x = 4; x < SS_SCREEN_W; x += 8)
            lit |= (pixels[y * width + native_left + x] & 0xffffffu) != 0;
    if (!lit) {
        box(&screen, 0, 0, width, SS_SCREEN_H, 0xff000000u);
        return 1;
    }

    struct ss_net_roster_entry seats[4];
    int occupied = ss_net_roster(seats);
    int gap = width > 300 ? 8 : 4;
    int card = ((width > 408 ? 400 : width - 16) - 3 * gap) / 4;
    int content = 4 * card + 3 * gap;
    int left = (width - content) / 2;
    uint32_t copper = 0xff976e46u, gold = 0xfff7d894u, white = 0xffe5eee1u;

    background(&screen);

    char room[36];
    if (room_menu.confirmed_code[0])
        snprintf(room, sizeof room, wording(&screen, "%s    %d of 4 linked", "%s    %d de 4 en línea"),
                 room_menu.confirmed_code, occupied);
    else
        snprintf(room, sizeof room, wording(&screen, "%d of 4 linked", "%d de 4 en línea"), occupied);
    panel(&screen, left, 10, content, 33, copper, 0xff203940u);
    text(&screen, width / 2, 11, "GAME LINK", gold);
    text(&screen, width / 2, 26, room, white);

    for (int i = 0; i < 4; i++) {
        const struct ss_net_roster_entry *seat = &seats[i];
        unsigned stage = (unsigned)seat->stage;
        if (stage >= sizeof phase / sizeof *phase)
            stage = SS_ROSTER_EMPTY;
        int x = left + i * (card + gap), center = x + card / 2;
        uint32_t edge = seat->local ? 0xffd7a760u : stage == SS_ROSTER_EMPTY ? 0xff4b6264u : copper;
        panel(&screen, x, 49, card, 76, edge, stage == SS_ROSTER_EMPTY ? 0xff162b31u : 0xff29474du);
        box(&screen, x + 3, 93, card - 6, 1, edge);
        box(&screen, center - 12, 88, 24, 2, 0xff1a343au);
        if (stage == SS_ROSTER_EMPTY || !character(&screen, center, 88, seat->class_id, seat->palette_id)) {
            uint32_t pip = stage == SS_ROSTER_EMPTY ? 0xff476368u : phase_color[stage];
            box(&screen, center - 4, 69, 8, 6, pip);
            box(&screen, center - 2, 67, 4, 2, pip);
            box(&screen, center - 2, 75, 4, 2, pip);
        }
        char player[12];
        snprintf(player, sizeof player, wording(&screen, "Player %d", card < 65 ? "J %d" : "Jugador %d"), i + 1);
        const char *name = seat->local ? wording(&screen, "You", "Tú") :
                           i == 0 ? wording(&screen, "Host", "Creador") : player;
        text(&screen, center, 94, name, seat->local ? gold : white);
        text(&screen, center, 108, phase[stage][screen.spanish], phase_color[stage]);
    }

    const char *action = wording(&screen, "Waiting for host    B Back", "Esperando al anfitrión    B Atrás");
    if (seats[0].local)
        action = ss_net_roster_can_start() ? wording(&screen, "A Start    B Back", "A Jugar    B Atrás") :
                 wording(&screen, "Waiting for players    B Back", "Esperando jugadores    B Atrás");
    panel(&screen, left, 132, content, 16, copper, 0xff182f36u);
    text(&screen, width / 2, 132, action, white);
    return 1;
}
