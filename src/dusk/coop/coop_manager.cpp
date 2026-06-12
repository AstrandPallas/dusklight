#include "dusk/coop/coop_manager.hpp"

#include "dusk/logging.h"
#include "dusk/settings.h"
#include "m_Do/m_Do_controller_pad.h"

namespace dusk::coop {

static State s_state = State::Disabled;
// Slot [n] belongs to player n+2's guest (P1 has no slot); v1 uses [0].
// s_guestProcID is set the moment a spawn is REQUESTED (so the actor's create
// phase can identify itself via guestPlayerNo()); s_guestActorID only once the
// actor exists. During spawn-in-progress, guestPlayerNo() resolves but
// playerCount() intentionally does not count the guest yet.
static unsigned int s_guestProcID[kMaxPlayers - 1] = {kNoProcID, kNoProcID, kNoProcID};
static unsigned int s_guestActorID[kMaxPlayers - 1] = {kNoProcID, kNoProcID, kNoProcID};

State getState() { return s_state; }

bool isSplitActive() { return s_state == State::Active; }

int playerCount() {
    int n = 1;
    for (int i = 0; i < kMaxPlayers - 1; i++) {
        if (s_guestActorID[i] != kNoProcID) n++;
    }
    return n;
}

int guestPlayerNo(unsigned int procID) {
    if (procID == kNoProcID) return -1;
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

    if (s_state == State::Solo && mDoCPd_c::getTrigStart(PAD_2)) {
        DuskLog.info("coop: P2 join requested");
        // A later task turns this into spawnGuest(); for now it only logs.
    }
}

}  // namespace dusk::coop
