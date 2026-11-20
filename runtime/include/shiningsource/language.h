#ifndef SHININGSOURCE_LANGUAGE_H
#define SHININGSOURCE_LANGUAGE_H

#include <stdint.h>

/* The European cartridge's language IDs, in resource-table order. */
enum ss_game_language {
    SS_LANG_UNCHOSEN = -1,
    SS_LANG_EN, SS_LANG_FR, SS_LANG_DE, SS_LANG_IT, SS_LANG_ES, SS_LANG_COUNT
};

#define SS_GAME_LANGUAGE_FILE "shiningsource-language.txt"

static inline enum ss_game_language ss_game_language_from_id(unsigned id)
{
    return id < SS_LANG_COUNT ? (enum ss_game_language)id : SS_LANG_EN;
}

/* Read the cartridge's current selection from guest code only. */
enum ss_game_language ss_game_language_current(void);

static inline const char *ss_language_text(enum ss_game_language language, const char *en, const char *fr,
                                           const char *de, const char *it, const char *es)
{
    const char *const text[SS_LANG_COUNT] = { en, fr, de, it, es };
    return text[ss_game_language_from_id((unsigned)language)];
}

static inline const char *ss_game_text(const char *en, const char *fr, const char *de,
                                       const char *it, const char *es)
{
    return ss_language_text(ss_game_language_current(), en, fr, de, it, es);
}

int ss_game_language_start_menu(void);
void ss_game_language_confirm(unsigned id);

#endif
