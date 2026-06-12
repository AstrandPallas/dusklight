#include "dusk/coop/coop_manager.hpp"

#include "SSystem/SComponent/c_malloc.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "dusk/settings.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_graphic.h"

namespace dusk::coop {

static State s_state = State::Disabled;
// Slot [n] belongs to player n+2's guest (P1 has no slot); v1 uses [0].
// Both IDs are recorded at spawn REQUEST time: procID so the actor's create
// phase can self-identify via guestPlayerNo(), actorID so lifecycle checks
// (getGuestActor / isGuestGone) can use SearchByID's creating-vs-gone
// semantics. playerCount() therefore counts a guest from request onward.
static unsigned int s_guestProcID[kMaxPlayers - 1] = {kNoProcID, kNoProcID, kNoProcID};
static unsigned int s_guestActorID[kMaxPlayers - 1] = {kNoProcID, kNoProcID, kNoProcID};

static void restoreSingleWindow() {
    dComIfGp_setWindow(0, 0.0f, 0.0f, FB_WIDTH, FB_HEIGHT, 0.0f, 1.0f, 0, 2);
    dComIfGp_setWindowNum(1);
}

// Applies window rects + camera bindings for `count` players.
// Window-count transitions: shrinking applies immediately; growing is raised
// by camera init_phase2 once the new camera is live — unless forceNum is set
// (stash/restore, where all cameras already exist).
// v1 implements counts 1-2; 3-4 (quadrant/column) are v2 cases added here.
static void applySplitLayout(int count, bool forceNum) {
    if (count <= 1) {
        restoreSingleWindow();
        return;
    }
    // count == 2: horizontal split, P1 top / P2 bottom
    f32 halfH = FB_HEIGHT / 2.0f;
    dComIfGp_setWindow(0, 0.0f, 0.0f, FB_WIDTH, halfH, 0.0f, 1.0f, 0, 2);
    dComIfGp_setWindow(1, 0.0f, halfH, FB_WIDTH, halfH, 0.0f, 1.0f, 1, 2);
    if (forceNum) {
        dComIfGp_setWindowNum(2);
    }
}

// True while camera `cameraIdx`'s process is alive or still in its multi-frame
// create phase. l_fopCamM_id slots are 0 before first use and can hold a dead
// process's ID after a scene change — both count as "not alive" (real process
// IDs start at 1, see fpcBs_MakeOfId).
static bool isCameraAlive(int cameraIdx) {
    fpc_ProcID camID = fopCamM_GetID(cameraIdx);
    if (camID == 0 || camID == kNoProcID) return false;
    return fpcM_SearchByID(camID) != NULL || fpcM_IsCreating(camID);
}

// Deletes camera `cameraIdx` if it is alive and clears the manager-side slot.
// A camera still mid-create can't be deleted (fpcDt_Delete refuses creating
// procs, and fpcM_SearchByID can't see them) — it then parks in init_phase2
// waiting for a player that no longer exists, and the next join adopts it
// (spawnGuest checks isCameraAlive before creating another).
static void teardownCamera(int cameraIdx) {
    fpc_ProcID camID = fopCamM_GetID(cameraIdx);
    if (camID == 0 || camID == kNoProcID) return;
    base_process_class* proc = fpcM_SearchByID(camID);
    if (proc != NULL) {
        fpcM_Delete(proc);
        fopCamM_ClearID(cameraIdx);
    } else if (!fpcM_IsCreating(camID)) {
        // already gone (scene change deletes the stage-layer camera procs) —
        // just drop the stale ID
        fopCamM_ClearID(cameraIdx);
    }
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
    if (p1 == NULL) return;  // no P1 at all
    fopAc_ac_c* p1Done = NULL;
    if (!fopAcM_SearchByID(fopAcM_GetID(p1), &p1Done) || p1Done == NULL) {
        return;  // P1 still mid-create — guest create would corrupt the
                 // shared bgWaitFlg latch in daAlink_c::create
    }

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

        // window 0 = top / camera 0 / P1; window 1 = bottom / camera 1 / P2.
        // windowNum stays 1 until camera 1's init_phase2 raises it (the camera
        // waits for the guest to register into playerInfo[1] first).
        applySplitLayout(2, /*forceNum*/ false);
        dComIfGp_setCameraInfo(playerNo, NULL, playerNo, playerNo, -1);
        if (!isCameraAlive(playerNo)) {
            // mirror dStage_cameraCreate: the framework owns the append and
            // frees it on process delete (fpcBs_DeleteAppend). base.parameters
            // is the camera slot — fopCam_Create copies it into the process
            // parameters, which get_camera_id reads back.
            fopCamM_prm_class* prm =
                (fopCamM_prm_class*)cMl::memalignB(-4, sizeof(fopCamM_prm_class));
            if (prm != NULL) {
                prm->base.position.x = 0.0f;
                prm->base.position.y = 0.0f;
                prm->base.position.z = 0.0f;
                prm->base.parameters = playerNo;
                fopCamM_Create(playerNo, fpcNm_CAMERA2_e, prm);
            }
        }
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
    // tear down this player's camera + restore the single window (v1 only ever
    // creates camera 1; the calls are no-ops for slots that never existed)
    teardownCamera(playerNo);
    dComIfGp_setCameraInfo(playerNo, NULL, playerNo, playerNo, -1);
    if (playerCount() == 1) {
        applySplitLayout(1, /*forceNum*/ true);
        s_state = State::Solo;
    }
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

    static int holdFrames = 0;
    if (s_state == State::Solo && mDoCPd_c::getTrigStart(PAD_2)) {
        holdFrames = 0;
        spawnGuest(1);
    } else if (s_state == State::Active) {
        // leave: hold START on pad 2 (~2s at 30fps logic). Only despawn once
        // the actor fully exists — deleting a mid-create process would leave
        // an orphan Link that self-identifies as P1.
        holdFrames = mDoCPd_c::getHoldStart(PAD_2) ? holdFrames + 1 : 0;
        if (holdFrames > 60 && getGuestActor(1) != NULL) {
            despawnGuest(1);
            holdFrames = 0;
        }
        // scene change deleted the guest from under us → drop to Solo. The
        // camera procs are stage-layer so camera 1 died (or is dying) with the
        // scene — teardownCamera deletes it if somehow still alive and clears
        // the stale l_fopCamM_id slot either way.
        if (isGuestGone(1)) {
            s_guestActorID[0] = kNoProcID;
            s_guestProcID[0] = kNoProcID;
            teardownCamera(1);
            dComIfGp_setCameraInfo(1, NULL, 1, 1, -1);
            applySplitLayout(1, /*forceNum*/ true);
            s_state = State::Solo;
            DuskLog.info("coop: guest Link P2 gone (scene change), back to Solo");
        }
    } else {
        holdFrames = 0;
    }
}

}  // namespace dusk::coop
