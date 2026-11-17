# ShiningSource 0.8.12 — official local achievements (Windows)

Windows now uses the 52 published RetroAchievements achievements for Shining Soul II (Europe), worth 454 points. The twelve custom port achievements have been removed, including their predicates and generated jewel icons. Existing custom records are ignored without deleting saves or player files.

The release includes the official catalog and its 52 badges, so local achievements work offline immediately. Definitions are evaluated unchanged by rcheevos against the game's GBA memory every frame. Signing in refreshes the catalog. The Local achievements tab lists locked and earned entries, descriptions, points and local dates. Unlock notices display the official badge and title. Local and Rebel records remain separate; neither is imported into an account.

Late catalogs are picked up even without a signed-in account. The original ROM hash is retained after native pointer patching. Account notices are deduplicated by achievement and frame, so a previously earned local achievement does not hide a later account unlock. Multiple account notifications are queued instead of overwriting one another. RA's client warning is not counted as a game achievement.

## Verification

The real official API response identified the supported European ROM as game 7095. All 52 published definitions parse and validate against the host's IWRAM/EWRAM mapping without disabled conditions. A replay of all 252 alternate condition groups triggered the corresponding achievements and loaded their official badge images. Blank RAM did not unlock them. Eleven alternatives already satisfy their unchanged RA definition during the setup snapshot; the replay preserves this behavior rather than rewriting the conditions.

These are synthetic RAM transition checks, not a complete playthrough. No artificial achievements were submitted to a real account. The server identifies this as an unknown client and does not approve hardcore awards. Account submission remains optional, off by default, and softcore only; acceptance of a real earned account award is not claimed by these checks.

This release changes Windows only. Android and Linux remain on 0.8.11. Online rules remain 7 and crossplay compatibility is unchanged. No ROM, save, credentials or private player logs are included.
