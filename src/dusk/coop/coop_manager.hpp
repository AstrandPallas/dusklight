#pragma once
#include "dusk/coop.h"

class dAttention_c;

namespace dusk::coop {

// Called once per game-logic frame from duskExecute().
void tick();

// Guest players' Z-targeting (attention) instances. Slot 0 (P1) is the
// engine-owned instance embedded in dComIfG_play_c; instances for players
// 1..3 are created here at join request and reaped once the guest actor is
// fully gone. NULL when player n has no instance — most readers should go
// through dComIfGp_getAttention(int), which falls back to slot 0.
dAttention_c* getAttention(int playerNo);

// Per-frame tick/draw for guest attention instances, called from d_s_play
// right after slot 0's Run()/Draw(). No-ops unless coop is Active.
void runGuestAttention();
void drawGuestAttention();

}  // namespace dusk::coop
