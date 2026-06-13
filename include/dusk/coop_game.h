#pragma once

// Co-op helpers that need engine types. include/dusk/coop.h stays include-free;
// anything taking or returning engine objects lives here instead.

#include "f_op/f_op_actor.h"

namespace dusk::coop {

// Nearest live, unstashed player actor to i_pos: P1 plus any active guests.
// Never NULL while P1 exists — empty guest slots, mid-create guests and
// stashed (suspended) guests are skipped, and outside Active split co-op it
// returns P1 immediately (solo cost: one slot read).
fopAc_ac_c* nearestPlayer(const cXyz& i_pos);

// Called from camera init_phase2 when a guest camera (index 1..3) is fully
// built; lets the manager safely re-assert the split if a scene-fade clobbered
// the window count after the camera came up.
void markGuestCameraReady(int cameraIdx);

}  // namespace dusk::coop
