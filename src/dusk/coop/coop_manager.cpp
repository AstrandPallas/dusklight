#include "dusk/coop/coop_manager.hpp"

#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "dusk/settings.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_graphic.h"

namespace dusk::coop {

static State s_state = State::Disabled;
// Slot [n] belongs to player n+2's guest (P1 has no slot); v1 uses [0].
// s_guestProcID is set the moment a spawn is REQUESTED (so the actor's create
// phase can identify itself via guestPlayerNo()); s_guestActorID only once the
// actor exists. During spawn-in-progress, guestPlayerNo() resolves but
// playerCount() intentionally does not count the guest yet.
static unsigned int s_guestProcID[kMaxPlayers - 1] = {kNoProcID, kNoProcID, kNoProcID};
static unsigned int s_guestActorID[kMaxPlayers - 1] = {kNoProcID, kNoProcID, kNoProcID};

static void restoreSingleWindow() {
    dComIfGp_setWindow(0, 0.0f, 0.0f, FB_WIDTH, FB_HEIGHT, 0.0f, 1.0f, 0, 2);
    dComIfGp_setWindowNum(1);
}

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

// playerNo 1..3; v1 only uses 1.
// NULL while the guest is despawned OR still in its multi-frame create phase
// (fopAcM_SearchByID reports creating processes as found-but-NULL).
static fopAc_ac_c* getGuestActor(int playerNo) {
    if (s_guestActorID[playerNo - 1] == kNoProcID) return NULL;
    fopAc_ac_c* actor = NULL;
    fopAcM_SearchByID(s_guestActorID[playerNo - 1], &actor);
    return actor;
}

// True once the guest's process no longer exists at all (e.g. a scene change
// deleted every actor). Distinct from "still creating": fopAcM_SearchByID
// returns 1 with a NULL out-pointer while creating, 0 only when gone.
static bool isGuestGone(int playerNo) {
    if (s_guestActorID[playerNo - 1] == kNoProcID) return false;
    fopAc_ac_c* actor = NULL;
    return fopAcM_SearchByID(s_guestActorID[playerNo - 1], &actor) == 0;
}

static void spawnGuest(int playerNo) {
    daPy_py_c* p1 = dComIfGp_getLinkPlayer();
    if (p1 == NULL) return;  // P1 not fully created yet — keeps bgWaitFlg safe

    cXyz pos = p1->current.pos;
    pos.x += 100.0f * playerNo;
    csXyz angle = p1->shape_angle;
    int roomNo = fopAcM_GetRoomNo(p1);

    // s_guestProcID must be set the moment the request succeeds so the new
    // actor's create phase can self-identify via guestPlayerNo(). On failure
    // fopAcM_create returns fpcM_ERROR_PROCESS_ID_e == 0xFFFFFFFF == kNoProcID
    // (see src/f_op/f_op_actor_mng.cpp + f_pc_stdcreate_req.cpp).
    fpc_ProcID id = fopAcM_create(fpcNm_ALINK_e, 0, &pos, roomNo, &angle, NULL, -1);
    if (id != kNoProcID) {
        s_guestProcID[playerNo - 1] = id;
        s_guestActorID[playerNo - 1] = id;
        s_state = State::Active;
        DuskLog.info("coop: guest Link P{} spawning (proc {})", playerNo + 1, id);
    }
}

static void despawnGuest(int playerNo) {
    if (s_guestActorID[playerNo - 1] != kNoProcID) {
        fopAcM_delete(s_guestActorID[playerNo - 1]);
        s_guestActorID[playerNo - 1] = kNoProcID;
        s_guestProcID[playerNo - 1] = kNoProcID;
        DuskLog.info("coop: guest Link P{} despawned", playerNo + 1);
    }
    if (playerCount() == 1) s_state = State::Solo;
}

void tick() {
    if (!getSettings().game.coopEnabled) {
        // restore before despawning: despawnGuest drops s_state to Solo, which
        // would skip the Active check
        if (s_state == State::Active) {
            restoreSingleWindow();
        }
        // despawn any live guest before going Disabled — once Disabled, tick()
        // returns up here every frame and the actor would outlive the manager.
        // A guest still in its multi-frame create phase can't be deleted yet
        // (fopAcM_delete no-ops on creating procs, and clearing s_guestProcID
        // would make the in-flight create re-identify as P1 — same hazard the
        // hold-START despawn path guards against), so hold off Disabled until
        // it finishes and despawn on a later tick.
        bool stillCreating = false;
        for (int i = 0; i < kMaxPlayers - 1; i++) {
            if (s_guestProcID[i] == kNoProcID) continue;
            if (getGuestActor(i + 1) == NULL && !isGuestGone(i + 1)) {
                stillCreating = true;
                continue;
            }
            despawnGuest(i + 1);
        }
        if (!stillCreating) {
            s_state = State::Disabled;
        }
        return;
    }
    if (s_state == State::Disabled) {
        s_state = State::Solo;
        DuskLog.info("coop: enabled, waiting for P2 (START on pad 2)");
    }

    if (s_state == State::Solo && mDoCPd_c::getTrigStart(PAD_2)) {
        spawnGuest(1);
    } else if (s_state == State::Active) {
        // leave: hold START on pad 2 (~2s at 30fps logic). Only despawn once
        // the actor fully exists — deleting a mid-create process would leave
        // an orphan Link that self-identifies as P1.
        static int holdFrames = 0;
        holdFrames = mDoCPd_c::getHoldStart(PAD_2) ? holdFrames + 1 : 0;
        if (holdFrames > 60 && getGuestActor(1) != NULL) {
            despawnGuest(1);
            holdFrames = 0;
        }
        // scene change deleted the guest from under us → drop to Solo
        if (isGuestGone(1)) {
            s_guestActorID[0] = kNoProcID;
            s_guestProcID[0] = kNoProcID;
            s_state = State::Solo;
            DuskLog.info("coop: guest Link P2 gone (scene change), back to Solo");
        }
    }

    // Spike scaffolding: render two stacked views of the SAME camera (no P2 yet).
    if (getSettings().game.coopDebugSplit && dComIfGp_getCamera(0) != NULL) {
        // (re)apply every tick: cameras/cutscenes stomp window rects (ResetView)
        f32 halfH = FB_HEIGHT / 2.0f;
        dComIfGp_setWindow(0, 0.0f, 0.0f, FB_WIDTH, halfH, 0.0f, 1.0f, 0, 2);
        dComIfGp_setWindow(1, 0.0f, halfH, FB_WIDTH, halfH, 0.0f, 1.0f, 0, 2);
        dComIfGp_setWindowNum(2);
        s_state = State::Active;  // drives isSplitActive() → aspect + post-process gates
    } else if (s_state == State::Active && playerCount() == 1 && s_guestProcID[0] == kNoProcID) {
        // keyed on state, not windowNum — can't wedge. Only drop to Solo when
        // no guest exists or is spawning; a live guest keeps state Active even
        // with a single window (real split windows arrive in a later task).
        restoreSingleWindow();
        s_state = State::Solo;
    }
}

}  // namespace dusk::coop
