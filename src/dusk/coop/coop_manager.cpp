#include "dusk/coop/coop_manager.hpp"

#include "dusk/logging.h"
#include "dusk/settings.h"
#include "m_Do/m_Do_controller_pad.h"

namespace dusk::coop {

static State s_state = State::Disabled;
// Slot [n] belongs to player n+1 (guests only; P1 has no slot). v1 uses [0].
static unsigned int s_guestProcID[kMaxPlayers - 1] = {};   // 0 = none
static unsigned int s_guestActorID[kMaxPlayers - 1] = {};

State getState() { return s_state; }

bool isSplitActive() { return s_state == State::Active; }

int playerCount() {
    int n = 1;
    for (int i = 0; i < kMaxPlayers - 1; i++) {
        if (s_guestActorID[i] != 0) n++;
    }
    return n;
}

int guestPlayerNo(unsigned int procID) {
    if (procID == 0) return -1;
    for (int i = 0; i < kMaxPlayers - 1; i++) {
        if (s_guestProcID[i] == procID) return i + 1;
    }
    return -1;
}

void tick() {
    if (!dusk::getSettings().game.coopEnabled) {
        s_state = State::Disabled;
        return;
    }
    if (s_state == State::Disabled) {
        s_state = State::Solo;
        DuskLog.info("coop: enabled, waiting for P2 (START on pad 2)");
    }

    if (s_state == State::Solo && (mDoCPd_c::getTrig(PAD_2) & PAD_BUTTON_START)) {
        DuskLog.info("coop: P2 join requested");
        // A later task turns this into spawnGuest(); for now it only logs.
    }
}

}  // namespace dusk::coop
