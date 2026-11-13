/* The port's own achievements: a short list that needs no account and no download, so a player who never signed
 * in to RetroAchievements has something to earn. Titles and descriptions are the port's (English and Spanish);
 * the conditions are C predicates in ach_host.c over fields the decompilation documents; the icon is a jewel
 * drawn by the program in the colour given here. Shared by the game host and the launcher (C and C++).
 *
 * Ids start at SS_PORT_ID so they can sit in the same record files as RetroAchievements ids. Never renumber:
 * the records on players' disks hold these numbers. */
#ifndef SS_PORT_LIST_H
#define SS_PORT_LIST_H

#define SS_PORT_ID 2130706432u          /* 0x7F000000 */

enum {
    SS_PORT_FIRST_STEPS, SS_PORT_LEVEL_5, SS_PORT_LEVEL_10, SS_PORT_LEVEL_25, SS_PORT_LEVEL_50,
    SS_PORT_GOLD_1K, SS_PORT_GOLD_10K, SS_PORT_GOLD_100K, SS_PORT_FULL_PACK, SS_PORT_CLOSE_CALL,
    SS_PORT_STURDY, SS_PORT_ONLINE, SS_PORT_COUNT
};

struct ss_port_achievement {
    const char *title_en, *title_es, *text_en, *text_es;
    unsigned char red, green, blue;
};

static const struct ss_port_achievement ss_port_list[SS_PORT_COUNT] = {
    { "First Steps", "Primeros pasos", "Enter the world with a character.", "Entra al mundo con un personaje.", 92, 214, 226 },
    { "Apprentice", "Aprendiz", "Reach level 5.", "Llega al nivel 5.", 120, 220, 120 },
    { "Adventurer", "Aventurero", "Reach level 10.", "Llega al nivel 10.", 90, 200, 90 },
    { "Veteran", "Veterano", "Reach level 25.", "Llega al nivel 25.", 60, 170, 70 },
    { "Champion", "Campeón", "Reach level 50.", "Llega al nivel 50.", 40, 140, 60 },
    { "Pocket Money", "Calderilla", "Hold 1,000 gold between your bag and the storage.", "Reúne 1.000 de oro entre la bolsa y el almacén.", 255, 226, 138 },
    { "Savings", "Ahorros", "Hold 10,000 gold between your bag and the storage.", "Reúne 10.000 de oro entre la bolsa y el almacén.", 232, 168, 56 },
    { "Treasure", "Tesoro", "Hold 100,000 gold between your bag and the storage.", "Reúne 100.000 de oro entre la bolsa y el almacén.", 200, 120, 30 },
    { "Pack Rat", "Bolsa llena", "Fill all 25 places of your bag.", "Llena los 25 huecos de la bolsa.", 176, 140, 100 },
    { "Close Call", "Por los pelos", "Stay alive with a tenth of your health or less.", "Sigue con vida con una décima parte de tu vida o menos.", 240, 90, 80 },
    { "Sturdy", "Resistente", "Reach a maximum health of 200.", "Llega a 200 de vida máxima.", 230, 120, 160 },
    { "Better Together", "Mejor en compañía", "Play five minutes of an online session.", "Juega cinco minutos de una sesión en línea.", 150, 130, 240 },
};

#endif
