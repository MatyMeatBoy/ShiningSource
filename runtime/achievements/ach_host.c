/* Achievements in the game process (the Windows host, and the Linux hosts: GA36, R36S, Linux x64).
 *
 * The host sees the game's memory (at the console's addresses, or where a rebased image keeps it: ss_ach_set_memory),
 * so it evaluates the achievements itself, once per frame, with rcheevos (third_party/rcheevos). Three things can be
 * running:
 *
 * The account (normal runs only, and only when a sign-in is remembered). The RetroAchievements set of the loaded ROM
 * is evaluated by rcheevos' client through launcher/achievements/ra_client.c, the wrapper the launcher signs in with,
 * and unlocks go to the account (softcore). The session token is read from SS_RA_DIR: on Windows protected for this
 * Windows user (the launcher's sign-in), on Linux the plain retroachievements.auth that a handheld's card holds
 * (platform/ga36/ra_vincular_cuenta.py writes it from a PC; the token of ReKHCoM's folder on the same card, SS_RA_SHARED_AUTH,
 * is used when this game has none). The password never reaches this program. All requests run on the wrapper's own
 * threads: this thread only starts them and collects the answers, so the game never waits.
 *
 * The local evaluation (every run). It needs no account and no connection:
 *   - the RetroAchievements set as the site once answered it, if ra_set_<hash>.json is in SS_RA_DIR (the wrapper
 *     keeps it, with the badge images, whenever a signed-in launcher or game receives it; the packages ship it),
 *     evaluated by rcheevos' runtime alone.
 * Unlocks are appended to a record in SS_RA_DIR, "id <tab> unix time <tab> title" per line: local_record.txt in
 * a normal run, rebel_record.txt in a Rebel run (started with cheats, core/rebel.c). On Windows nothing of a record
 * is ever sent anywhere. On a handheld (SS_RA_HANDHELD) whose card is linked to an account and submits, an unlock of
 * a normal run is also queued for that account (retroachievements_pending.txt): handhelds are mostly offline, and
 * the queue is sent at the next start with a connection, as ReKHCoM's handhelds do.
 *
 * A Rebel run never creates the client: no connection is made at all, nothing is queued.
 *
 * Outside Windows the state is also written for the player to read on the card: retroachievements_estado.txt in
 * SS_RA_DIR (the account's name, connected or not, unlocks waiting, the console's clock). Never the token.
 *
 * Environment:
 *   SS_RA_DIR                the achievements folder (the launcher's on Windows, the port's on a handheld).
 *                            Not set: nothing is evaluated.
 *   SS_RA_SUBMIT=1           unlocks go to the account (the launcher's switch on Windows; on by default on the
 *                            handhelds, ga36-env.sh / r36s-env.sh). Otherwise the account's set is only evaluated.
 *   SS_RA_SHARED_AUTH=file   Linux: another game's retroachievements.auth on the same card, used when SS_RA_DIR has
 *                            none (RetroAchievements tokens belong to the account, not to a game).
 *   SS_LANG=es               notices in Spanish.
 *   SS_RA_TEST_NOTICE=text   tests: show this as an unlocked achievement two seconds into the run, with the
 *                            image SS_RA_TEST_ICON (a PNG file) if given.
 *   SS_RA_TEST_SERVER=dir    tests: no connection; every request is answered at once from <dir>/<request>.json
 *                            (login2, achievementsets, startsession, awardachievement, unlocks, ping; an image
 *                            from <dir>/badge.png) and noted in <dir>/requests.log. While <dir>/offline exists every
 *                            request fails as without a network. SS_RA_TEST_LOGIN=1 signs in as a pretend user first.
 *   SS_RA_TEST_PEEK=a,b,...  tests: every 300 frames print the bytes at these RetroAchievements addresses (hex).
 *
 * A run whose input is scripted or that has no window (SS_INPUT, SS_INPUT_FILE, SS_FRAMES) is a test: the
 * account's set is evaluated and shown, and nothing is submitted.
 */
#include "ach_host.h"
#include "record.h"
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
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char folder_utf8[2048];          /* SS_RA_DIR */
static const uint8_t *rom_bytes;        /* only during ss_ach_start */
static size_t rom_size;
static char local_hash[33];
static int active, rebel_run, spanish, announced, was_online, submitting;
static unsigned long frames;
static const uint8_t *ewram_at, *iwram_at;     /* ss_ach_set_memory; NULL: the console's own addresses */

static const char *say(const char *english, const char *spanish_text)
{
    return spanish ? spanish_text : english;
}

static FILE *open_path(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wide[1200], wide_mode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1200) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, 8))
        return NULL;
    return _wfopen(wide, wide_mode);
#else
    return fopen(path, mode);
#endif
}

static FILE *open_in_folder(const char *name, const char *mode)
{
    char path[2200];
    if (snprintf(path, sizeof path, "%s/%s", folder_utf8, name) >= (int)sizeof path)
        return NULL;
    return open_path(path, mode);
}

/* An environment variable that may hold a path (UTF-8 on every system). */
static int path_from_env(const char *name, char *out, size_t size)
{
#ifdef _WIN32
    wchar_t wide_name[64], value[1024];
    DWORD length;
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, 64);
    length = GetEnvironmentVariableW(wide_name, value, 1024);
    out[0] = 0;
    return length > 0 && length < 1024 && WideCharToMultiByte(CP_UTF8, 0, value, -1, out, (int)size, NULL, NULL) > 0;
#else
    const char *value = getenv(name);
    out[0] = 0;
    if (!value || !value[0] || strlen(value) >= size)
        return 0;
    memcpy(out, value, strlen(value) + 1);
    return 1;
#endif
}

/* ---- the game's memory as RetroAchievements numbers it for the console ----
 * 0x00000-0x07FFF is the 32 KiB of internal work RAM (0x03000000) and 0x08000-0x47FFF the 256 KiB of external
 * work RAM (0x02000000): rcheevos' own table (rc_consoles, "GameBoy Advance"). Its third range, the cartridge's
 * save memory, is a file in this port and reads as absent, so an achievement that needs it is disabled.
 * A rebased image (the ARM handhelds) keeps both RAMs in a block of its own: the host says where (ss_ach_set_memory)
 * before the first frame is evaluated. */
static const uint8_t *real_address(uint32_t address)
{
#ifdef SS_REBASED_HOST
    const uint8_t *iwram = iwram_at, *ewram = ewram_at;
#else
    const uint8_t *iwram = iwram_at ? iwram_at : (const uint8_t *)(uintptr_t)SS_IWRAM;
    const uint8_t *ewram = ewram_at ? ewram_at : (const uint8_t *)(uintptr_t)SS_EWRAM;
#endif
    if (address < 0x8000)
        return iwram ? iwram + address : NULL;
    if (address < 0x48000)
        return ewram ? ewram + address - 0x8000 : NULL;
    return NULL;
}

void ss_ach_set_memory(const uint8_t *ewram, const uint8_t *iwram)
{
    ewram_at = ewram;
    iwram_at = iwram;
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
 * A RetroAchievements badge is a PNG the wrapper keeps under achievements/badges; SDL's own PNG loader reads it (SDL 3.4,
 * which is why this is here and not in the shared window code: other platforms may build with an older SDL).
 * The image becomes a square of ARGB pixels for ss_plat_notice. */
static uint32_t icon_pixels[128 * 128];

static int icon_from_surface(SDL_Surface *loaded)
{
    SDL_Surface *argb;
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
    return icon_from_surface(KhRaLoadBadge(folder_utf8, badge_name));
}

/* ---- the account ---- */

static KhRa *ra;
static char test_server[1024];

/* Tests: the part of the site. The request is named by "r=" in what would have been posted. */
static void test_transport(const rc_api_request_t *request, rc_client_server_callback_t callback, void *callback_data)
{
    rc_api_server_response_t response = { "", 0, RC_API_SERVER_RESPONSE_CLIENT_ERROR };
    char name[40] = "", award[24] = "", *body = NULL, path[1200];
    const char *at = request->post_data;
    int offline;
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
    snprintf(path, sizeof path, "%s/offline", test_server);
    if ((file = open_path(path, "rb")) != NULL)
        fclose(file);
    offline = file != NULL;
    snprintf(path, sizeof path, "%s/requests.log", test_server);
    if ((file = open_path(path, "a")) != NULL) {        /* the request's name and the achievement, never the token */
        fprintf(file, "%s%s %s\n", offline ? "offline " : "", name[0] ? name : request->url, award);
        fclose(file);
    }
    if (offline) {                                      /* as the HTTPS worker answers without a network */
        response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        callback(&response, callback_data);
        return;
    }
    if (name[0])
        snprintf(path, sizeof path, "%s/%s.json", test_server, name);
    else
        snprintf(path, sizeof path, "%s/badge.png", test_server);
    if ((file = open_path(path, "rb")) != NULL) {
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
    int pretend = path_from_env("SS_RA_TEST_SERVER", test_server, sizeof test_server);
    ra = KhRaCreate(folder_utf8, read_memory, NULL, pretend ? test_transport : NULL);
    if (!ra)
        return;
    /* Unlocks are sent to the account only when the player asked for it (SS_RA_SUBMIT=1, a switch in the
     * launcher; the handhelds' start scripts set it unless the player's shiningsource.env says 0). RetroAchievements
     * counts unlocks from the programs it has approved, the set was written for the cartridge in an emulator and has
     * not been checked against this port, and an unlock on an account cannot be taken back. Without the switch the
     * set is still evaluated and kept in the local record. */
    if (pretend || (!scripted && getenv("SS_RA_SUBMIT") && getenv("SS_RA_SUBMIT")[0] == '1')) {
        KhRaStartOfficial(ra, NULL, 0);         /* the whole set; unlocks are submitted, softcore */
        submitting = 1;
    }
    if (pretend && getenv("SS_RA_TEST_LOGIN")) {
        KhRaSetEnabled(ra, 1);
        KhRaLogin(ra, "tester", "not-a-password", 1);
    }
    if (local_hash[0])
        KhRaSetRomHash(ra, local_hash);     /* hashed once already (local_start): 16 MiB is felt on a handheld */
    else
        KhRaSetRom(ra, rom_bytes, rom_size);
}

static int account_sync_enabled(void)
{
    rc_client_t *client = ra ? KhRaClient(ra) : NULL;
    return client && rc_client_get_user_info(client) && rc_client_is_game_loaded(client) &&
           !rc_client_get_spectator_mode_enabled(client);
}

static int have_set;
static int local_notified(uint32_t id);
static void account_notices(void)
{
    const char *notice;
    while ((notice = KhRaTakeNotice(ra)) != NULL) {     /* "title <newline> description" */
        const rc_client_achievement_t *info = rc_client_get_achievement_info(KhRaClient(ra), KhRaNoticeId(ra));
        char title[200];
        snprintf(title, sizeof title, "%s", notice);
        title[strcspn(title, "\n")] = 0;
        if (!local_notified(KhRaNoticeId(ra)))
            ss_plat_notice(say("ACHIEVEMENT UNLOCKED", "LOGRO DESBLOQUEADO"), title, 0, icon_pixels, info ? icon_of_badge(info->badge_name) : 0);
        fprintf(stderr, "achievements: unlocked \"%s\" (%u)\n", title, (unsigned)KhRaNoticeId(ra));
    }
}

/* ---- the local evaluation ---- */

struct local_entry { uint32_t id; int earned; unsigned long notice_frame; char title[120], badge[16]; };
static struct local_entry *local_list;
static unsigned local_total, local_earned;
static rc_runtime_t local_runtime;

static struct local_entry *local_find(uint32_t id)
{
    unsigned i;
    for (i = 0; i < local_total; i++)
        if (local_list[i].id == id)
            return &local_list[i];
    return NULL;
}

static int local_notified(uint32_t id)
{
    struct local_entry *entry = local_find(id);
    return entry && entry->earned && entry->notice_frame == frames;
}

static void status_update(int force);

static void local_unlock(struct local_entry *entry)
{
    int size;
    FILE *file;
    if (entry->earned)
        return;
    entry->earned = 1;
    local_earned++;
    if ((file = open_in_folder(rebel_run ? "rebel_record.txt" : "local_record.txt", "a")) != NULL) {
        fprintf(file, "%u\t%lld\t%s\n", (unsigned)entry->id, (long long)time(NULL), entry->title);
        fclose(file);
    }
    fprintf(stderr, "achievements: %s record, \"%s\" (%u)\n", rebel_run ? "Rebel" : "local", entry->title, (unsigned)entry->id);
#ifdef SS_RA_HANDHELD
    /* a handheld is mostly offline: the account set up on its card gets it at the next start with a connection */
    if (!rebel_run && ra && submitting && KhRaQueueLocal(ra, entry->id, (long long)time(NULL)))
        fprintf(stderr, "achievements: %u queued for the account\n", (unsigned)entry->id);
#endif
    entry->notice_frame = frames;
    size = icon_of_badge(entry->badge);
    ss_plat_notice(rebel_run ? say("REBEL ACHIEVEMENT", "LOGRO REBELDE") : say("ACHIEVEMENT UNLOCKED", "LOGRO DESBLOQUEADO"),
                   entry->title, rebel_run, icon_pixels, size);
    status_update(1);
}

static uint32_t local_peek(uint32_t address, uint32_t size, void *unused)
{
    uint8_t bytes[4] = { 0, 0, 0, 0 };
    (void)unused;
    if (size > 4 || read_memory(address, bytes, size, NULL) < size)
        return 0;
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* the console's ranges (the memory itself may only be known from the first frame on, ss_ach_set_memory) */
static int local_valid_address(uint32_t address)
{
    return address < 0x48000;
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
    unsigned earned_count = 0, count = 0, s, a, i;
    int parsed = 0;
    FILE *file;

    rc_runtime_init(&local_runtime);
    if ((file = open_in_folder(rebel_run ? "rebel_record.txt" : "local_record.txt", "r")) != NULL) {  /* earned in earlier runs */
        while (earned_count < 1024 && fgets(line, sizeof line, file))
            if ((earned[earned_count] = (uint32_t)strtoul(line, NULL, 10)) != 0)
                earned_count++;
        fclose(file);
    }
    if (!local_hash[0] && rom_bytes) {
        rc_hash_initialize_iterator(&iterator, NULL, rom_bytes, rom_size);
        rc_hash_generate(local_hash, RC_CONSOLE_GAMEBOY_ADVANCE, &iterator);
        rc_hash_destroy_iterator(&iterator);
    }
    memcpy(hash, local_hash, sizeof hash);
    snprintf(name, sizeof name, "ra_set_%s.json", hash);
    memset(&answer, 0, sizeof answer);
    if (hash[0] && (file = open_in_folder(name, "rb")) != NULL) {
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
            if (definition->category != RC_ACHIEVEMENT_CATEGORY_CORE || definition->id >= SS_ACH_WARNING_ID || local_find(definition->id))
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
    have_set = local_total > 0;
    for (i = 0; i < local_total; i++) {
        for (a = 0; a < earned_count; a++)
            local_list[i].earned |= earned[a] == local_list[i].id;
        if (local_list[i].earned) {
            local_earned++;
            if (local_list[i].id < SS_ACH_WARNING_ID)
                rc_runtime_deactivate_achievement(&local_runtime, local_list[i].id);
        }
    }
    if (have_set) {
        rc_runtime_validate_addresses(&local_runtime, local_event, local_valid_address);
        fprintf(stderr, "achievements: %u of the set evaluated here, %u earned (%s)\n", local_total, local_earned,
                rebel_run ? "Rebel record" : "local record");
    }
}

/* ---- the state on the card (not on Windows: the launcher shows it there) ---- */

#ifndef _WIN32
static char status_last[1400];
static unsigned status_pending = (unsigned)-1;

static void status_update(int force)
{
    char text[1400], account[200], state[600], *at = text;
    const char *user = ra ? KhRaUser(ra) : "";
    unsigned pending;
    time_t now = time(NULL);
    FILE *file;
    if (!active)
        return;
    if (force || status_pending == (unsigned)-1 || frames % 60 == 0)
        status_pending = ra ? KhRaPendingCount(ra) : 0;
    pending = status_pending;
    if (user[0])
        snprintf(account, sizeof account, "%s: %s%s", say("Account", "Cuenta"), user,
                 ra && KhRaShared(ra) ? say(" (the sign-in of ReKHCoM on this card)", " (la sesion de ReKHCoM en esta tarjeta)") : "");
    else
        snprintf(account, sizeof account, "%s", say("Account: none linked", "Cuenta: ninguna vinculada"));
    if (rebel_run)
        snprintf(state, sizeof state, "%s", say("Rebel mode (cheats): achievements stay on this console, nothing is sent to RetroAchievements.",
                                                "Modo Rebelde (trucos): los logros quedan en esta consola, nada se envia a RetroAchievements."));
    else if (!user[0])
        snprintf(state, sizeof state, "%s", say("Achievements stay on this console. To send them to RetroAchievements, link this card "
                                                "from a PC with internet (README.md, Achievements; on the GA36: "
                                                "Vincular-RetroAchievements-ShiningSource.bat in this folder).",
                                                "Los logros quedan en esta consola. Para enviarlos a RetroAchievements, vincula esta tarjeta "
                                                "desde un PC con internet (README.md, Logros; en la GA36: "
                                                "Vincular-RetroAchievements-ShiningSource.bat en esta carpeta)."));
    else if (!KhRaEnabled(ra))
        snprintf(state, sizeof state, "%s", say("The account is switched off (retroachievements.ini: enabled=0).",
                                                "La cuenta esta desactivada (retroachievements.ini: enabled=0)."));
    else if (!submitting)
        snprintf(state, sizeof state, "%s", say("Sending is off (SS_RA_SUBMIT=0, or a test run): achievements stay on this console.",
                                                "El envio esta desactivado (SS_RA_SUBMIT=0, o una prueba): los logros quedan en esta consola."));
    else if (account_sync_enabled())
        snprintf(state, sizeof state, "%s", say("Connected: achievements are sent to RetroAchievements (softcore).",
                                                "Conectado: los logros se envian a RetroAchievements (softcore)."));
    else if (KhRaBusy(ra))
        snprintf(state, sizeof state, "%s", say("Connecting to RetroAchievements...", "Conectando con RetroAchievements..."));
    else if (KhRaSslMissing())
        snprintf(state, sizeof state, "%s", say("No OpenSSL (libssl.so.3 / libssl.so.1.1) on this system: achievements are kept and wait.",
                                                "Sin OpenSSL (libssl.so.3 / libssl.so.1.1) en este sistema: los logros se guardan y esperan."));
    else
        snprintf(state, sizeof state, KhRaStatus(ra)[0] ? "%s (%s)" : "%s", say("Not connected: achievements earned here are sent the next time the game starts "
                                                     "with internet.",
                                                     "Sin conexion: los logros se enviaran la proxima vez que el juego arranque con internet."),
                 KhRaStatus(ra));
    at += snprintf(at, sizeof text, "%s\n%s\n", account, state);
    at += snprintf(at, sizeof text - (size_t)(at - text), "%s: %u\n%s: %u/%u\n",
                   say("Waiting to be sent", "Pendientes de enviar"), pending,
                   say(rebel_run ? "Rebel achievements on this console" : "Achievements on this console",
                       rebel_run ? "Logros rebeldes en esta consola" : "Logros en esta consola"), local_earned, local_total);
    if (!force && !strcmp(text, status_last))
        return;
    memcpy(status_last, text, sizeof text);
    if ((file = open_in_folder("retroachievements_estado.txt", "w")) != NULL) {
        fprintf(file, "%s%s: %s", text, say("Console clock", "Hora de la consola"), ctime(&now));
        fclose(file);
    }
}
#else
static void status_update(int force) { (void)force; }
#endif

/* ---- the calls of the host ---- */

void ss_ach_start(const uint8_t *rom, size_t size)
{
    const char *text = getenv("SS_LANG"), *cheats = getenv("SS_CHEATS");
    rom_bytes = rom;
    rom_size = size;
    spanish = text && text[0] == 'e' && text[1] == 's';
    rebel_run = cheats && cheats[0];            /* the rule of core/rebel.c */
    active = path_from_env("SS_RA_DIR", folder_utf8, sizeof folder_utf8);
    if (active) {
        KhRaMigrateBadges(folder_utf8);
        local_start();
        if (!rebel_run)
            account_start();
        status_update(1);
    }
    rom_bytes = NULL;                           /* the ROM is hashed: the caller may let it go */
    rom_size = 0;
}

int ss_ach_start_file(const char *path)
{
    FILE *file = path ? open_path(path, "rb") : NULL;
    uint8_t *rom = NULL;
    size_t size = 0;
    if (file) {
        if ((rom = malloc(32u << 20)) != NULL)
            size = fread(rom, 1, 32u << 20, file);
        fclose(file);
    }
    if (!rom || size < 0x100000) {
        fprintf(stderr, "achievements: the ROM could not be read for its hash; none are evaluated\n");
        free(rom);
        return 0;
    }
    ss_ach_start(rom, size);
    free(rom);
    return 1;
}

/* The start notice is shown the first time achievements run here, and afterwards only when the account is synced
 * again (online) after a start that was not: achievements/mode-seen keeps the mode last announced ("online" or
 * "other"). */
static int mode_notice_due(int online)
{
    char path[2200], last[16] = "";
    FILE *marker;
    int seen;
    if (!active || getenv("SS_FRAMES")) return 0;   /* a headless run has no notice to show */
#ifdef _WIN32
    {
        wchar_t wide[1200];
        DWORD attributes;
        snprintf(path, sizeof path, "%s\\achievements", folder_utf8);
        if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1200)) return 0;
        attributes = GetFileAttributesW(wide);
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
        snprintf(path, sizeof path, "%s\\achievements\\mode-seen", folder_utf8);
        if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1200)) return 0;
        marker = _wfopen(wide, L"rb");
        seen = marker != NULL;
        if (marker) { if (!fgets(last, sizeof last, marker)) last[0] = 0; fclose(marker); }
        if (seen && !(online && strncmp(last, "online", 6) != 0) && !(!online && strncmp(last, "online", 6) == 0))
            return 0;
        marker = _wfopen(wide, L"wb");
    }
#else
    {
        struct stat info;
        snprintf(path, sizeof path, "%s/achievements", folder_utf8);
        if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode)) return 0;
        snprintf(path, sizeof path, "%s/achievements/mode-seen", folder_utf8);
        marker = fopen(path, "rb");
        seen = marker != NULL;
        if (marker) { if (!fgets(last, sizeof last, marker)) last[0] = 0; fclose(marker); }
        if (seen && !(online && strncmp(last, "online", 6) != 0) && !(!online && strncmp(last, "online", 6) == 0))
            return 0;
        marker = fopen(path, "wb");
    }
#endif
    if (marker) {
        fputs(online ? "online\n" : "other\n", marker);
        fclose(marker);
    }
    /* first time: show; online again after a start that was not: show; dropping back to offline: only remembered */
    return !seen || online;
}

/* Show the mode the first time and when the account is synced again (mode_notice_due). Keep a diagnostic for each game's start. */
static void announce(void)
{
    char line[200];
    const char *heading;
    if (announced || (ra ? KhRaBusy(ra) : frames < 120))
        return;
    announced = 1;
    if (rebel_run) {
        heading = say("REBEL MODE ACTIVATED", "MODO REBELDE ACTIVADO");
        snprintf(line, sizeof line, say("Rebel achievements %u/%u", "Logros rebeldes %u/%u"), local_earned, local_total);
    } else if (account_sync_enabled()) {
        rc_client_user_game_summary_t summary;
        heading = say("ONLINE MODE ACTIVATED", "MODO EN LINEA ACTIVADO");
        rc_client_get_user_game_summary(KhRaClient(ra), &summary);
        snprintf(line, sizeof line, "RetroAchievements: %s  %u/%u", rc_client_get_user_info(KhRaClient(ra))->display_name,
                 (unsigned)summary.num_unlocked_achievements, (unsigned)summary.num_core_achievements);
    } else {
        /* This includes signed-in spectator sessions: the game keeps progress locally. */
        heading = say("LOCAL MODE ACTIVATED", "MODO LOCAL ACTIVADO");
        if (ra && KhRaError(ra) && KhRaStatus(ra)[0])
            fprintf(stderr, "achievements: %s\n", KhRaStatus(ra));
#ifdef SS_RA_HANDHELD
        if (ra && KhRaUser(ra)[0] && KhRaEnabled(ra) && submitting) {
            /* linked, no connection now: unlocks are queued and sent at the next start with internet */
            heading = say("OFFLINE MODE", "MODO SIN CONEXION");
            snprintf(line, sizeof line, "%s", say("Sent when online", "Se envian al tener internet"));
        } else {
            snprintf(line, sizeof line, "%s", say("Achievements stay on this console", "Los logros se guardan en esta consola"));
        }
#else
        snprintf(line, sizeof line, "%s", say("Achievements stay on this PC", "Los logros se guardan en este PC"));
#endif
    }
    if (mode_notice_due(!rebel_run && account_sync_enabled()))
        ss_plat_notice(heading, line, rebel_run, NULL, 0);
    fprintf(stderr, "achievements: %s - %s\n", heading, line);
    status_update(1);
}

/* tests: the bytes the evaluation reads at a few addresses (SS_RA_TEST_PEEK) */
static void test_peek(const char *list)
{
    char line[512];
    int length = snprintf(line, sizeof line, "achievements: peek frame %lu", frames);
    while (list && *list && length < (int)sizeof line - 16) {
        char *end;
        uint32_t address = (uint32_t)strtoul(list, &end, 16);
        uint8_t byte = 0;
        if (end == list)
            break;
        length += snprintf(line + length, sizeof line - (size_t)length, " %06x=%s%02x", (unsigned)address,
                           read_memory(address, &byte, 1, NULL) ? "" : "?", byte);
        list = *end == ',' ? end + 1 : end;
    }
    fprintf(stderr, "%s\n", line);
}

void ss_ach_frame(int run_flags)
{
    static const char *test_notice, *peek;
    frames++;
    if (frames == 1) {
        test_notice = getenv("SS_RA_TEST_NOTICE");
        peek = getenv("SS_RA_TEST_PEEK");
    }
    if (test_notice && frames == 120) {
        const char *png = getenv("SS_RA_TEST_ICON");
        ss_plat_notice((run_flags & SS_RUN_REBEL) ? say("REBEL ACHIEVEMENT", "LOGRO REBELDE") : say("ACHIEVEMENT UNLOCKED", "LOGRO DESBLOQUEADO"),
                       test_notice, run_flags & SS_RUN_REBEL, icon_pixels,
                       png ? icon_from_surface(SDL_LoadPNG(png)) : 0);
    }
    if (peek && frames % 300 == 0)
        test_peek(peek);
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
            KhRaPump(ra, 1);
        if (!have_set && frames % 60 == 0) {
            char name[64];
            FILE *file;
            snprintf(name, sizeof name, "ra_set_%s.json", local_hash);
            if ((file = open_in_folder(name, "rb")) != NULL) {
                fclose(file);
                rc_runtime_destroy(&local_runtime);
                free(local_list);
                local_list = NULL;
                local_total = local_earned = 0;
                local_start();
            }
        }
        if (have_set)
            rc_runtime_do_frame(&local_runtime, local_event, local_peek, NULL, NULL);
        if (ra)
            account_notices();
        if (frames % 60 == 0)
            status_update(0);
    }
    if (active || rebel_run)
        announce();
}
