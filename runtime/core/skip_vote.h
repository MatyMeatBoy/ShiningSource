#ifndef SHININGSOURCE_SKIP_VOTE_H
#define SHININGSOURCE_SKIP_VOTE_H

#include <stdint.h>

enum { SKIP_START = 1, SKIP_BACK, SKIP_VOTE_MS = 30000 };

struct skip_ballot {
    uint32_t scene, expires;
    unsigned owner, spent, votes, members; /* owner: seat plus one, zero when idle */
    int kind, open;
};

static inline void skip_ballot_clear(struct skip_ballot *vote)
{
    vote->owner = vote->votes = 0;
}

static inline void skip_ballot_update(struct skip_ballot *vote, int kind, int open, unsigned members, uint32_t now)
{
    if (kind != vote->kind) {
        if (!++vote->scene)
            ++vote->scene;
        vote->kind = kind;
        vote->spent = 0;
        skip_ballot_clear(vote);
    }
    /* A room change cannot turn an incomplete ballot into a unanimous one. */
    if (members != vote->members)
        skip_ballot_clear(vote);
    vote->members = members;
    vote->open = open && (members & (members - 1));
    if (vote->owner && (!vote->open || (int32_t)(now - vote->expires) >= 0))
        skip_ballot_clear(vote);
}

static inline void skip_ballot_press(struct skip_ballot *vote, uint32_t scene, unsigned owner,
                                     unsigned seat, int button, uint32_t now)
{
    unsigned bit = 1u << seat;
    if (!vote->open || scene != vote->scene || owner != vote->owner || !(vote->members & bit))
        return;
    if (!owner) {
        if (button == SKIP_START && !(vote->spent & bit)) {
            vote->owner = seat + 1;
            vote->spent |= bit;
            vote->votes = bit;
            vote->expires = now + SKIP_VOTE_MS;
        }
    } else if (owner == seat + 1) {
        skip_ballot_clear(vote);
    } else if (button == SKIP_START) {
        vote->votes |= bit;
    } else {
        vote->votes &= ~bit;
    }
}

static inline int skip_ballot_ready(const struct skip_ballot *vote)
{
    return vote->open && vote->owner && vote->votes == vote->members;
}

#endif
