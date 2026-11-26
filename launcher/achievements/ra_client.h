#ifndef KH_RA_CLIENT_H
#define KH_RA_CLIENT_H

#include "rc_client.h"
#include <SDL3/SDL_surface.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KhRa KhRa;
typedef uint32_t (*KhRaRead)(uint32_t address, uint8_t* out, uint32_t size, void* userdata);
/* Optional transport for deterministic integration tests. NULL uses HTTPS. */
typedef void (*KhRaTransport)(const rc_api_request_t*, rc_client_server_callback_t, void*);
typedef void (*KhRaImageCallback)(const uint8_t* bytes, size_t size, int status, void* userdata);
/* Public badge images use the same bounded HTTPS queue; no account token. */
int KhRaFetchImage(KhRa* ra, const char* url, KhRaImageCallback callback, void* userdata);
/* Old desktop installs cached images alongside the executable. */
void KhRaMigrateBadges(const char* directory);
SDL_Surface* KhRaLoadBadge(const char* directory, const char* badge_name);

KhRa* KhRaCreate(const char* directory, KhRaRead read, void* userdata, KhRaTransport transport);
void KhRaDestroy(KhRa* ra);
void KhRaPump(KhRa* ra, int evaluate);
void KhRaSetEnabled(KhRa* ra, int enabled);
int KhRaEnabled(const KhRa* ra);
void KhRaLogin(KhRa* ra, const char* username, const char* password, int remember);
void KhRaLogout(KhRa* ra);
void KhRaSetRom(KhRa* ra, const uint8_t* rom, size_t size);
rc_client_t* KhRaClient(KhRa* ra);
const char* KhRaStatus(const KhRa* ra);
const char* KhRaHash(const KhRa* ra);
int KhRaBusy(const KhRa* ra);
int KhRaError(const KhRa* ra);
int KhRaAccountUnlocked(const KhRa* ra, uint32_t achievement_id);
int KhRaAccountProgressAvailable(const KhRa* ra);
/* Validation mode never submits unlocks, scores, or rich presence. */
int KhRaValidationMode(void);
void KhRaInvalidate(KhRa* ra, const char* reason);
const char* KhRaTakeNotice(KhRa* ra);
uint32_t KhRaNoticeId(const KhRa* ra); /* id of the achievement behind the last notice */
/* Game process: evaluate and submit (softcore) only these achievement ids. */
void KhRaStartOfficial(KhRa* ra, const uint32_t* ids, uint32_t count);
/* An achievement the local evaluation earned, for the account set up here: sent at the next start online. 1: queued. */
int KhRaQueueLocal(KhRa* ra, uint32_t id, long long when);
unsigned KhRaPendingCount(KhRa* ra); /* unlocks of this account waiting in retroachievements_pending.txt */
const char* KhRaUser(const KhRa* ra); /* the account's name ("" without one); never the token */
int KhRaShared(const KhRa* ra); /* the token is another game's (SS_RA_SHARED_AUTH) */
int KhRaSslMissing(void); /* Linux: OpenSSL was looked for and is not there */

#ifdef __cplusplus
}
#endif
#endif
