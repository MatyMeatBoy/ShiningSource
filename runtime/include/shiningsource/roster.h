#ifndef SHININGSOURCE_ROSTER_H
#define SHININGSOURCE_ROSTER_H

enum ss_net_roster_stage {
    SS_ROSTER_EMPTY,
    SS_ROSTER_CONNECTED,
    SS_ROSTER_SELECTING_SAVE,
    SS_ROSTER_CREATING_CHARACTER,
    SS_ROSTER_READY,
    SS_ROSTER_IN_GAME,
    SS_ROSTER_RECONNECTING
};

struct ss_net_roster_entry {
    enum ss_net_roster_stage stage;
    int class_id, palette_id;       /* -1 until known */
    int local;
};

/* Read the occupied seats by index. An empty row has SS_ROSTER_EMPTY. */
int ss_net_roster(struct ss_net_roster_entry entries[4]);
int ss_net_roster_lobby(void);
int ss_net_roster_can_start(void);

/* Game menu hooks. Only the transport publishes these observations. */
void ss_net_roster_set_stage(enum ss_net_roster_stage stage);
void ss_net_roster_set_character(int class_id, int palette_id);
void ss_net_roster_set_host_start(int enabled);

#endif
