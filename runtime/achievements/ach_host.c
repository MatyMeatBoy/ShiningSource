/* Achievements in the game process (Windows host).
 *
 * The host sees the game's memory at the console's addresses, so it evaluates the achievements itself, once per
 * frame, with rcheevos (third_party/rcheevos). Three things can be running:
 *
 * The account (normal runs only, and only when the launcher's sign-in is remembered). The RetroAchievements
 * set of the loaded ROM is evaluated by rcheevos' client through launcher/achievements/ra_client.c, the wrapper
 * the launcher signs in with, and unlocks go to the account (softcore). The session token is read from SS_RA_DIR,
 * protected for this Windows user; the password never reaches this program. All requests run on the wrapper's
 * own threads: this thread only starts them and collects the answers, so the game never waits.
 *
 * The local evaluation (every run). It needs no account and no connection:
 *   - the RetroAchievements set as the site once answered it, if ra_set_<hash>.json is in SS_RA_DIR (the wrapper
 *     keeps it, with the badge images, whenever a signed-in launcher or game receives it), evaluated by
 *     rcheevos' runtime alone;
 *   - the port's own short list (port_list.h), whose conditions are the C predicates below.
 * Unlocks are appended to a record in SS_RA_DIR, "id <tab> unix time <tab> title" per line: local_record.txt in
 * a normal run, rebel_record.txt in a Rebel run (started with cheats, core/rebel.c). Nothing of a record is
 * ever sent anywhere: RetroAchievements takes unlocks only as they happen in a signed-in session.
 *
 * A Rebel run never creates the client: no connection is made at all.
 *
 * Environment:
 *   SS_RA_DIR                the launcher's folder. Not set: nothing is evaluated.
 *   SS_LANG=es               notices in Spanish.
 *   SS_RA_TEST_NOTICE=text   tests: show this as an unlocked achievement two seconds into the run, with the
 *                            image SS_RA_TEST_ICON (a PNG file, or "port" for one of the port's jewels) if given.
 *   SS_RA_TEST_SERVER=dir    tests: no connection; every request is answered at once from <dir>/<request>.json
 *                            (login2, achievementsets, startsession, awardachievement, unlocks, ping; an image
 *                            from <dir>/badge.png) and noted in <dir>/requests.log. SS_RA_TEST_LOGIN=1 signs in
 *                            as a pretend user first.
 *
 * A run whose input is scripted or that has no window (SS_INPUT, SS_INPUT_FILE, SS_FRAMES) is a test: the
 * account's set is evaluated and shown, and nothing is submitted.
 */
#include "ach_host.h"
#include "port_list.h"
#include "ra_client.h"
#include "rc_api_runtime.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "rc_runtime.h"
#include "shiningsource/runtime.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

static wchar_t folder[900];             /* SS_RA_DIR */
static char folder_utf8[2048];
static const uint8_t *rom_bytes;
static size_t rom_size;
static int active, rebel_run, spanish, announced, was_online;
static unsigned long frames, online_frames;

static const char *say(const char *english, const char *spanish_text)
{
    return spanish ? spanish_text : english;
}

static FILE *open_in_folder(const char *name, const wchar_t *mode)
{
    wchar_t path[1024];
    _snwprintf(path, 1024, L"%ls\\%hs", folder, name);
    path[1023] = 0;
    return _wfopen(path, mode);
}

/* ---- the game's memory as RetroAchievements numbers it for the console ----
 * 0x00000-0x07FFF is the 32 KiB of internal work RAM (0x03000000) and 0x08000-0x47FFF the 256 KiB of external
 * work RAM (0x02000000): rcheevos' own table (rc_consoles, "GameBoy Advance"). Its third range, the cartridge's
 * save memory, is a file in this port and reads as absent, so an achievement that needs it is disabled. */
static const uint8_t *real_address(uint32_t address)
{
    if (address < 0x8000)
        return (const uint8_t *)(uintptr_t)(SS_IWRAM + address);
    if (address < 0x48000)
        return (const uint8_t *)(uintptr_t)(SS_EWRAM + address - 0x8000);
    return NULL;
}

static uint32_t read_memory(uint32_t address, uint8_t *out, uint32_t size, void *unused)
{
    uint32_t i;
    (void)unused;
    for (i = 0; i < size; i++) {
        const uint8_t *byte = real_address(address + i);
        if (!byte)
            break;
        out[i] = *byte;
    }
    return i;
}

/* ---- icons ----
 * A RetroAchievements badge is a PNG the wrapper keeps in SS_RA_DIR; SDL's own PNG loader reads it (SDL 3.4,
 * which is why this is here and not in the shared window code: other platforms may build with an older SDL).
 * The port's achievements get a jewel drawn here. Both end as a square of ARGB pixels for ss_plat_notice. */
static uint32_t icon_pixels[128 * 128];

static int icon_from_png(const char *utf8_path)
{
    SDL_Surface *loaded = SDL_LoadPNG(utf8_path), *argb;
    int size = 0, y;
    if (!loaded)
        return 0;
    argb = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(loaded);
    if (argb && argb->w == argb->h && argb->w > 0 && argb->w <= 128) {
        size = argb->w;
        for (y = 0; y < size; y++)
            memcpy(icon_pixels + y * size, (const uint8_t *)argb->pixels + (size_t)y * argb->pitch, (size_t)size * 4);
    }
    if (argb)
        SDL_DestroySurface(argb);
    return size;
}

static int icon_of_badge(const char *badge_name)
{
    char path[2200];
    const char *c;
    for (c = badge_name; *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_'))
            return 0;
    if (!badge_name[0])
        return 0;
    snprintf(path, sizeof path, "%s\\badge_%s.png", folder_utf8, badge_name);
    return icon_from_png(path);
}

/* A jewel of the given colour: a diamond with a light upper-left face, a dark lower-right one and a gold rim. */
static int icon_of_port(const struct ss_port_achievement *a)
{
    enum { SIZE = 32 };
    int x, y;
    for (y = 0; y < SIZE; y++)
        for (x = 0; x < SIZE; x++) {
            int dx = x - SIZE / 2, dy = y - SIZE / 2, d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            int shade = d >= 14 ? 0 : 100 - (dx + dy) * 3;         /* percent of the colour */
            uint32_t r = a->red * shade / 100, g = a->green * shade / 100, b = a->blue * shade / 100;
            if (d > 15)
                icon_pixels[y * SIZE + x] = 0;
            else if (d >= 14)
                icon_pixels[y * SIZE + x] = 0xFFE8A838u;
            else
                icon_pixels[y * SIZE + x] = 0xFF000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
    for (x = 0; x < 6; x++)                                         /* the glint */
        icon_pixels[(9 + x / 2) * SIZE + 11 + x] = 0xFFFFFADCu;
    return SIZE;
}

/* ---- the account ---- */

static KhRa *ra;
static wchar_t test_server[900];

/* Tests: the part of the site. The request is named by "r=" in what would have been posted. */
static void test_transport(const rc_api_request_t *request, rc_client_server_callback_t callback, void *callback_data)
{
    rc_api_server_response_t response = { "", 0, RC_API_SERVER_RESPONSE_CLIENT_ERROR };
    char name[40] = "", award[24] = "", *body = NULL;
    const char *at = request->post_data;
    wchar_t path[1024];
    FILE *file;
    while (at && *at) {
        const char *end = strchr(at, '&');
        size_t length = end ? (size_t)(end - at) : strlen(at), i;
        if (length > 2 && at[0] == 'r' && at[1] == '=' && length - 2 < sizeof name) {
            for (i = 2; i < length; i++)
                name[i - 2] = (at[i] >= 'a' && at[i] <= 'z') || (at[i] >= '0' && at[i] <= '9') ? at[i] : '_';
            name[length - 2] = 0;
        }
        if (length > 2 && at[0] == 'a' && at[1] == '=' && length - 2 < sizeof award) {
            memcpy(award, at + 2, length - 2);
            award[length - 2] = 0;
        }
        at = end ? end + 1 : NULL;
    }
    _snwprintf(path, 1024, L"%ls\\requests.log", test_server);
    if ((file = _wfopen(path, L"a")) != NULL) {         /* the request's name and the achievement, never the token */
        fprintf(file, "%s %s\n", name[0] ? name : request->url, award);
        fclose(file);
    }
    if (name[0])
        _snwprintf(path, 1024, L"%ls\\%hs.json", test_server, name);
    else
        _snwprintf(path, 1024, L"%ls\\badge.png", test_server);
    if ((file = _wfopen(path, L"rb")) != NULL) {
        body = malloc(1 << 20);
        if (body) {
            response.body_length = fread(body, 1, (1 << 20) - 1, file);
            body[response.body_length] = 0;
            response.body = body;
            response.http_status_code = 200;
        }
        fclose(file);
    }
    callback(&response, callback_data);
    free(body);
}

static void account_start(void)
{
    int scripted = getenv("SS_FRAMES") || getenv("SS_INPUT") || getenv("SS_INPUT_FILE");
    int pretend = GetEnvironmentVariableW(L"SS_RA_TEST_SERVER", test_server, 900) > 0;
    ra = KhRaCreate(folder_utf8, read_memory, NULL, pretend ? test_transport : NULL);
    if (!ra)
        return;
    /* Unlocks are sent to the account only when the player asked for it (SS_RA_SUBMIT=1, a switch in the
     * launcher). RetroAchievements counts unlocks from the programs it has approved, the set was written for the
     * cartridge in an emulator and has not been checked against this port, and an unlock on an account cannot be
     * taken back. Without the switch the set is still evaluated and kept in the local record. */
    if (pretend || (!scripted && getenv("SS_RA_SUBMIT") && getenv("SS_RA_SUBMIT")[0] == '1'))
        KhRaStartOfficial(ra, NULL, 0);         /* the whole set; unlocks are submitted, softcore */
    if (pretend && getenv("SS_RA_TEST_LOGIN")) {
        KhRaSetEnabled(ra, 1);
        KhRaLogin(ra, "tester", "not-a-password", 1);
    }
    KhRaSetRom(ra, rom_bytes, rom_size);
}

static int account_loaded(void)
{
    return ra && rc_client_get_user_info(KhRaClient(ra)) && rc_client_is_game_loaded(KhRaClient(ra));
}

static void account_frame(void)
{
    const char *notice;
    KhRaPump(ra, 1);
    while ((notice = KhRaTakeNotice(ra)) != NULL) {     /* "title <newline> description" */
        const rc_client_achievement_t *info = rc_client_get_achievement_info(KhRaClient(ra), KhRaNoticeId(ra));
        char title[200];
        snprintf(title, sizeof title, "%s", notice);
        title[strcspn(title, "\n")] = 0;
        ss_plat_notice(say("ACHIEVEMENT UNLOCKED", "LOGRO DESBLOQUEADO"), title, 0, icon_pixels, info ? icon_of_badge(info->badge_name) : 0);
        fprintf(stderr, "achievements: unlocked \"%s\" (%u)\n", title, (unsigned)KhRaNoticeId(ra));
    }
}

/* ---- the local evaluation ---- */

struct local_entry { uint32_t id; int earned; char title[120], badge[16]; };
static struct local_entry *local_list;
static unsigned local_total, local_earned, set_total;
static rc_runtime_t local_runtime;
static int have_set;

static struct local_entry *local_find(uint32_t id)
{
    unsigned i;
    for (i = 0; i < local_total; i++)
        if (local_list[i].id == id)
            return &local_list[i];
    return NULL;
}

static void local_unlock(struct local_entry *entry)
{
    int port = entry->id >= SS_PORT_ID, size;
    FILE *file;
    if (entry->earned)
        return;
    entry->earned = 1;
    local_earned++;
    if ((file = open_in_folder(rebel_run ? "rebel_record.txt" : "local_record.txt", L"a")) != NULL) {
        fprintf(file, "%u\t%lld\t%s\n", (unsigned)entry->id, (long long)time(NULL), entry->title);
        fclose(file);
    }
    fprintf(stderr, "achievements: %s record, \"%s\" (%u)\n", rebel_run ? "Rebel" : "local", entry->title, (unsigned)entry->id);
    if (!port && !rebel_run && account_loaded())
        return;                 /* the account evaluates the same achievement and gives its own notice */
    size = port ? icon_of_port(&ss_port_list[entry->id - SS_PORT_ID]) : icon_of_badge(entry->badge);
    ss_plat_notice(rebel_run ? say("REBEL ACHIEVEMENT", "LOGRO REBELDE") : port ? say("PORT ACHIEVEMENT", "LOGRO DEL PORT")
                                                                          : say("ACHIEVEMENT UNLOCKED", "LOGRO DESBLOQUEADO"),
                   entry->title, rebel_run, icon_pixels, size);
}

static uint32_t local_peek(uint32_t address, uint32_t size, void *unused)
{
    uint8_t bytes[4] = { 0, 0, 0, 0 };
    (void)unused;
    if (size > 4 || read_memory(address, bytes, size, NULL) < size)
        return 0;
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static int local_valid_address(uint32_t address)
{
    return real_address(address) != NULL;
}

static void local_event(const rc_runtime_event_t *event)
{
    struct local_entry *entry;
    if (event->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED && (entry = local_find(event->id)) != NULL)
        local_unlock(entry);
}

static void local_start(void)
{
    char hash[33] = "", name[64], line[400], *body = NULL;
    rc_hash_iterator_t iterator;
    rc_api_server_response_t answer;
    rc_api_fetch_game_sets_response_t sets;
    uint32_t earned[1024];
    unsigned earned_count = 0, count = SS_PORT_COUNT, s, a, i;
    int parsed = 0;
    FILE *file;

    rc_runtime_init(&local_runtime);
    if ((file = open_in_folder(rebel_run ? "rebel_record.txt" : "local_record.txt", L"r")) != NULL) {  /* earned in earlier runs */
        while (earned_count < 1024 && fgets(line, sizeof line, file))
            if ((earned[earned_count] = (uint32_t)strtoul(line, NULL, 10)) != 0)
                earned_count++;
        fclose(file);
    }
    rc_hash_initialize_iterator(&iterator, NULL, rom_bytes, rom_size);
    rc_hash_generate(hash, RC_CONSOLE_GAMEBOY_ADVANCE, &iterator);
    rc_hash_destroy_iterator(&iterator);
    snprintf(name, sizeof name, "ra_set_%s.json", hash);
    memset(&answer, 0, sizeof answer);
    if (hash[0] && (file = open_in_folder(name, L"rb")) != NULL) {
        if ((body = malloc(8u << 20)) != NULL) {
            answer.body_length = fread(body, 1, (8u << 20) - 1, file);
            body[answer.body_length] = 0;
            answer.body = body;
            answer.http_status_code = 200;
            parsed = rc_api_process_fetch_game_sets_server_response(&sets, &answer) == RC_OK && sets.response.succeeded;
            for (s = 0; parsed && s < sets.num_sets; s++)
                count += sets.sets[s].num_achievements;
        }
        fclose(file);
    }
    local_list = calloc(count, sizeof *local_list);
    for (s = 0; local_list && parsed && s < sets.num_sets; s++) {
        for (a = 0; a < sets.sets[s].num_achievements; a++) {
            const rc_api_achievement_definition_t *definition = &sets.sets[s].achievements[a];
            struct local_entry *entry = &local_list[local_total];
            if (definition->category != RC_ACHIEVEMENT_CATEGORY_CORE || definition->id >= SS_PORT_ID || local_find(definition->id))
                continue;               /* only the published ones, as the account's evaluation */
            entry->id = definition->id;
            snprintf(entry->title, sizeof entry->title, "%s", definition->title ? definition->title : "");
            snprintf(entry->badge, sizeof entry->badge, "%s", definition->badge_name ? definition->badge_name : "");
            local_total++;
            if (rc_runtime_activate_achievement(&local_runtime, entry->id, definition->definition, NULL, 0) != RC_OK)
                fprintf(stderr, "achievements: %u cannot be read by this rcheevos, left out\n", (unsigned)entry->id);
        }
    }
    if (body) {
        rc_api_destroy_fetch_game_sets_response(&sets);
        free(body);
    }
    set_total = local_total;
    have_set = set_total > 0;
    for (i = 0; local_list && i < SS_PORT_COUNT; i++) {
        struct local_entry *entry = &local_list[local_total++];
        entry->id = SS_PORT_ID + i;
        snprintf(entry->title, sizeof entry->title, "%s", spanish ? ss_port_list[i].title_es : ss_port_list[i].title_en);
    }
    for (i = 0; i < local_total; i++) {
        for (a = 0; a < earned_count; a++)
            local_list[i].earned |= earned[a] == local_list[i].id;
        if (local_list[i].earned) {
            local_earned++;
            if (local_list[i].id < SS_PORT_ID)
                rc_runtime_deactivate_achievement(&local_runtime, local_list[i].id);
        }
    }
    if (have_set)
        rc_runtime_validate_addresses(&local_runtime, local_event, local_valid_address);
}

/* The port's own list. The fields are the ones core/rebel.c names, from the decompilation: the local player's
 * record (0x03003CE0, 0x14C bytes each): +0x62 maximum health, +0xC4 health, +0x20 state (11 = defeated); the
 * character's shared block (*(u32 *)0x030034E0): +0x20 the level, counted from 0 (AddExperienceAndAdvanceOneLevel,
 * sub_0801E4CC), the bag of 25 records at +0x150 and the storage of 75 at +0x2E0, 16 bytes each, kind 10 = money
 * with its amount at +4 (SumBagCurrency sub_0805D3D4, SumStashCurrency sub_0805D404). */
#define U16(address) (*(const uint16_t *)(uintptr_t)(address))
#define U32(address) (*(const uint32_t *)(uintptr_t)(address))

static void port_frame(int run_flags)
{
    uint32_t seat = (U32(0x030027D0) & 2) ? (U32(0x04000128) >> 4) & 3 : 0;
    uint32_t player = 0x03003CE0u + seat * 0x14C, shared = U32(0x030034E0), level, gold = 0, i;
    uint16_t most = U16(player + 0x62), health = U16(player + 0xC4);
    int held = 0, met[SS_PORT_COUNT] = { 0 };

    if (run_flags & SS_RUN_ONLINE) {
        /* with somebody: two or more of the link's connected bits (the game's link state, bits 8 to 11) */
        uint32_t state = *(const uint32_t *)(uintptr_t)0x03002730u, seats = (state >> 8) & 15;
        if (seats & (seats - 1))
            online_frames++;
    }
    if (!local_list || frames % 30 || !most || most > 0x7FFF || shared < SS_EWRAM || shared > SS_EWRAM + 0x40000 - 0xCA4)
        return;                 /* twice a second is enough, and only with a character in play */
    level = U32(shared + 0x20) + 1;
    for (i = 0; i < 25 + 75; i++) {
        uint32_t record = shared + (i < 25 ? 0x150 + i * 16 : 0x2E0 + (i - 25) * 16);
        if (U16(record) == 10)
            gold += U32(record + 4) > 100000 ? 100000 : U32(record + 4);
        if (i < 25 && U16(record))
            held++;
    }
    met[SS_PORT_FIRST_STEPS] = 1;
    met[SS_PORT_LEVEL_5] = level >= 5 && level <= 200;
    met[SS_PORT_LEVEL_10] = level >= 10 && level <= 200;
    met[SS_PORT_LEVEL_25] = level >= 25 && level <= 200;
    met[SS_PORT_LEVEL_50] = level >= 50 && level <= 200;
    met[SS_PORT_GOLD_1K] = gold >= 1000;
    met[SS_PORT_GOLD_10K] = gold >= 10000;
    met[SS_PORT_GOLD_100K] = gold >= 100000;
    met[SS_PORT_FULL_PACK] = held == 25;
    met[SS_PORT_CLOSE_CALL] = most >= 20 && health > 0 && health <= most / 10 && U16(player + 0x20) != 11;
    met[SS_PORT_STURDY] = most >= 200;
    met[SS_PORT_ONLINE] = online_frames >= 5 * 60 * 60;        /* counted only with a companion, see below */
    for (i = 0; i < SS_PORT_COUNT; i++)
        if (met[i])
            local_unlock(&local_list[set_total + i]);
}

/* ---- the two calls of the host ---- */

void ss_ach_start(const uint8_t *rom, size_t size)
{
    const char *text = getenv("SS_LANG"), *cheats = getenv("SS_CHEATS");
    rom_bytes = rom;
    rom_size = size;
    spanish = text && text[0] == 'e' && text[1] == 's';
    rebel_run = cheats && cheats[0];            /* the rule of core/rebel.c */
    active = GetEnvironmentVariableW(L"SS_RA_DIR", folder, 900) > 0
          && WideCharToMultiByte(CP_UTF8, 0, folder, -1, folder_utf8, sizeof folder_utf8, NULL, NULL) > 0;
    if (!active)
        return;
    local_start();
    if (!rebel_run)
        account_start();
}

/* What this run counts for, once: after the sign-in was answered, or two seconds in when there is none. */
static void announce(void)
{
    char line[200];
    if (announced || (ra ? KhRaBusy(ra) : frames < 120))
        return;
    announced = 1;
    if (rebel_run) {
        snprintf(line, sizeof line, say("CHEATS ON  REBEL RECORD %u/%u", "TRUCOS ACTIVOS  REGISTRO REBELDE %u/%u"), local_earned, local_total);
        ss_plat_notice(say("REBEL MODE", "MODO REBELDE"), active ? line : say("CHEATS ON", "TRUCOS ACTIVOS"), 1, NULL, 0);
    } else if (account_loaded()) {
        rc_client_user_game_summary_t summary;
        rc_client_get_user_game_summary(KhRaClient(ra), &summary);
        snprintf(line, sizeof line, say("SIGNED IN AS %s  %u/%u", "SESION DE %s  %u/%u"), rc_client_get_user_info(KhRaClient(ra))->display_name,
                 (unsigned)summary.num_unlocked_achievements, (unsigned)summary.num_core_achievements);
        ss_plat_notice("RETROACHIEVEMENTS", line, 0, NULL, 0);
    } else {
        /* no account in this run: not signed in, no connection, or the site does not know this ROM */
        if (ra && KhRaError(ra))
            fprintf(stderr, "achievements: %s\n", KhRaStatus(ra));
        snprintf(line, sizeof line, say("NOT SIGNED IN  LOCAL RECORD %u/%u", "SIN SESION  REGISTRO LOCAL %u/%u"), local_earned, local_total);
        ss_plat_notice(say("ACHIEVEMENTS", "LOGROS"), line, 0, NULL, 0);
    }
    fprintf(stderr, "achievements: %s\n", line);
}

void ss_ach_frame(int run_flags)
{
    static const char *test_notice;
    frames++;
    if (frames == 1)
        test_notice = getenv("SS_RA_TEST_NOTICE");
    if (test_notice && frames == 120) {
        const char *png = getenv("SS_RA_TEST_ICON");
        ss_plat_notice((run_flags & SS_RUN_REBEL) ? say("REBEL ACHIEVEMENT", "LOGRO REBELDE") : say("ACHIEVEMENT UNLOCKED", "LOGRO DESBLOQUEADO"),
                       test_notice, run_flags & SS_RUN_REBEL, icon_pixels,
                       !png ? 0 : !strcmp(png, "port") ? icon_of_port(&ss_port_list[SS_PORT_GOLD_10K]) : icon_from_png(png));
    }
    if ((run_flags & SS_RUN_REBEL) && !rebel_run) {     /* cannot happen: both read SS_CHEATS. The image has the last word */
        rebel_run = 1;
        ra = NULL;
    }
    if (rebel_run && ((run_flags & SS_RUN_ONLINE) != 0) != was_online) {    /* cheats stop while online and come back after */
        was_online = (run_flags & SS_RUN_ONLINE) != 0;
        ss_plat_notice(say("REBEL MODE", "MODO REBELDE"),
                       was_online ? say("CHEATS PAUSED WHILE ONLINE", "TRUCOS EN PAUSA EN LINEA") : say("CHEATS ON AGAIN", "TRUCOS ACTIVOS DE NUEVO"), 1, NULL, 0);
    }
    if (active) {
        if (ra)
            account_frame();
        if (have_set)
            rc_runtime_do_frame(&local_runtime, local_event, local_peek, NULL, NULL);
        port_frame(run_flags);
    }
    if (active || rebel_run)
        announce();
}
