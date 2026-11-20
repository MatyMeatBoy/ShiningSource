#ifndef SHININGSOURCE_LANGUAGE_FILE_H
#define SHININGSOURCE_LANGUAGE_FILE_H

#include "language.h"
#include <stdio.h>
#include <string.h>

static inline const char *ss_game_language_code(enum ss_game_language language)
{
    const char *const codes[] = { "en", "fr", "de", "it", "es" };
    return language >= SS_LANG_EN && language < SS_LANG_COUNT ? codes[language] : "";
}

static inline enum ss_game_language ss_game_language_preference_at(const char *path)
{
    FILE *file = fopen(path, "rb");
    char code[5];
    size_t size;
    if (!file)
        return SS_LANG_UNCHOSEN;
    size = fread(code, 1, sizeof code, file);
    fclose(file);
    if (!(size == 2 || (size == 3 && code[2] == '\n') ||
          (size == 4 && code[2] == '\r' && code[3] == '\n')))
        return SS_LANG_UNCHOSEN;
    for (int i = SS_LANG_EN; i < SS_LANG_COUNT; ++i)
        if (!memcmp(code, ss_game_language_code((enum ss_game_language)i), 2))
            return (enum ss_game_language)i;
    return SS_LANG_UNCHOSEN;
}

static inline enum ss_game_language ss_game_language_preference(void)
{
    enum ss_game_language current = ss_game_language_preference_at(SS_GAME_LANGUAGE_FILE);
    return current != SS_LANG_UNCHOSEN ? current : ss_game_language_preference_at(SS_GAME_LANGUAGE_FILE ".bak");
}

static inline int ss_game_language_save(enum ss_game_language language)
{
    const char *path = SS_GAME_LANGUAGE_FILE;
    const char *fresh = SS_GAME_LANGUAGE_FILE ".tmp";
    const char *backup = SS_GAME_LANGUAGE_FILE ".bak";
    const char *code = ss_game_language_code(language);
    if (!*code)
        return 0;
    FILE *file = fopen(fresh, "wb");
    if (!file)
        return 0;
    int good = fwrite(code, 1, 2, file) == 2;
    if (fclose(file) || !good) {
        remove(fresh);
        return 0;
    }
    if (rename(fresh, path) == 0) {
        remove(backup);
        return 1;
    }
    /* C runtimes that cannot replace an existing file leave its backup
     * readable until the new selection is in place. */
    remove(backup);
    if (rename(path, backup) == 0) {
        if (rename(fresh, path) == 0) {
            remove(backup);
            return 1;
        }
        rename(backup, path);
    }
    remove(fresh);
    return 0;
}

#endif
