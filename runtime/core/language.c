#include "shiningsource/language_file.h"

enum ss_game_language ss_game_language_current(void)
{
    return ss_game_language_from_id(*(const uint32_t *)(uintptr_t)0x03003470u);
}

int ss_game_language_start_menu(void)
{
    /* The selector orders English, German, French, Italian and Spanish. */
    static const int menu_positions[] = { 0, 2, 1, 3, 4 };
    enum ss_game_language language = ss_game_language_preference();
    return language == SS_LANG_UNCHOSEN ? -1 : menu_positions[language];
}

void ss_game_language_confirm(unsigned id)
{
    if (id < SS_LANG_COUNT && ss_game_language_preference() != (enum ss_game_language)id)
        ss_game_language_save((enum ss_game_language)id);
}
