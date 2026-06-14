#include "dusk/coop/coop_manager.hpp"

#include "SSystem/SComponent/c_malloc.h"
#include "dusk/coop_game.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_player.h"
#include "d/d_attention.h"
#include "d/d_com_inf_game.h"
#include "d/d_demo.h"
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
// True once a guest camera's init_phase2 has fully completed (body constructed,
// view set up). Lets tick() safely re-assert the split — a guest camera that
// inits mid scene-fade raises windowNum to 2, but the fade lowers it back to 1
// each frame and init_phase2 won't re-raise, so P2's half stays black until this
// re-assert kicks in. Cleared on teardown so we never force a window onto a
// camera whose body isn't built yet (that crashes).
static bool s_guestCamInitDone[kMaxPlayers - 1] = {false, false, false};

// Set when a scene change deletes the guest out from under us — the manager
// respawns it automatically once P1 fully exists again and no stash context
// is active. Cleared by deliberate despawns (hold-START leave, coop disable)
// and on spawn success.
static bool s_wantRejoin = false;

// Recovery thresholds (Active, guest alive): teleport the guest back to P1
// when it strays this far in XZ or falls this far below P1.
static const f32 kRecoverDistXZ = 8000.0f;
static const f32 kRecoverDrop = 4000.0f;

// X offset from P1 for guest spawn/snap positions, scaled by playerNo so
// multiple guests don't stack on the same spot.
static constexpr f32 kGuestSpawnOffsetX = 100.0f;

// A pending auto-rejoin that hasn't succeeded within this many Solo logic
// frames (~60s at 30fps) is abandoned — otherwise s_wantRejoin would survive
// quit-to-title and auto-spawn P2 into the next loaded save.
static constexpr int kRejoinTimeoutFrames = 1800;
static int s_rejoinWaitFrames = 0;

// coop: story-event settle gate. A second Link present while the single-track
// event system runs hangs it, so P2 is EVICTED (despawned) when a story event
// starts — not suspended. Reviving a suspended guest into the immediate
// post-event frame (esp. the first-twilight transform) hangs, but a FRESH
// create after the event is the proven-safe path (it's how form-reconcile
// already rebuilds P2). The event flickers event_runCheck on/off for many
// frames as it settles, so the rejoin waits for the coast to stay clear this
// many consecutive frames before recreating P2. This was sized large (45)
// defensively, back when recreating P2 mid-flicker could crash; with the real
// crashes fixed (aurora vertex-buffer cap + the guarded startup-event block) it
// only needs to debounce the event flicker, so it's short now — P2 snaps back
// promptly once the event ends.
static constexpr int kEventSettleFrames = 12;
static int s_eventSettleFrames = 0;

// coop: grace window (frames) that keeps P2 from being evicted in the tail of an
// NPC conversation, after P1 leaves PROC_TALK but while the talk event lingers.
static constexpr int kTalkGraceFrames = 30;

// Per-player Z-targeting: slot [n] is player n+1's dAttention_c (P1's lives
// embedded in dComIfG_play_c). Created at join REQUEST time — before the
// guest actor's create phase caches dComIfGp_getAttention(mPlayerNo) and
// before camera n's init_phase2 binds it. The instance is NOT freed at
// despawn: the guest actor (and its cached mAttention) can outlive the
// despawn call by a frame while the fpc delete request drains, so the
// instance is reaped only once the actor is fully gone (s_attnReapID holds
// the actor that must die first). A rejoin before the reap fires simply
// reuses the live instance and cancels the reap.
static dAttention_c* s_attention[kMaxPlayers - 1];
static unsigned int s_attnReapID[kMaxPlayers - 1] = {kNoProcID, kNoProcID, kNoProcID};

// Per-player projectile ownership table. Keyed by the projectile actor's ID,
// value is the firing player's slot (1..kMaxPlayers-1; P1/slot 0 is never
// recorded — it shares the vanilla singleton fallback). Bounded in practice:
// each player has at most one live arrow/sling stone and one boomerang, plus a
// few player bombs (mActiveBombNum caps at 3), so a small fixed table covers
// every guest's in-flight projectiles with room to spare. Stale entries (a
// freed projectile's ID) are harmless — getOwnerAlink re-checks the owner
// player is live and falls back to P1 otherwise, and a slot is reclaimed the
// next time an equal/colliding ID is stamped. kNoProcID marks a free slot.
static const int kMaxProjectileOwners = 32;
static unsigned int s_projOwnerID[kMaxProjectileOwners];
static u8 s_projOwnerNo[kMaxProjectileOwners];
static bool s_projOwnerInit = false;

// Create-phase owner latch (see header). -1 = inactive.
static int s_pendingOwnerNo = -1;

static void initProjectileOwners() {
    if (s_projOwnerInit) return;
    for (int i = 0; i < kMaxProjectileOwners; i++) {
        s_projOwnerID[i] = kNoProcID;
        s_projOwnerNo[i] = 0;
    }
    s_projOwnerInit = true;
}

// Hide+freeze bit for stashed guests. f_op_actor.cpp skips BOTH execute and
// draw while this status is set, unconditionally — the usual
// fopAcStts_NOEXEC_e route does not work on a player actor because its
// NOPAUSE profile bit makes dEvt_control_c::moveApproval() return 2
// (force-approve), which bypasses the NOEXEC check entirely. A suspended
// actor never runs its execute, so it registers no collision (cc_set happens
// in execute) and can't take damage or input. daSus_c::check is the only
// other writer of this bit and skips group fopAc_PLAYER_e, so on a guest
// Link the bit is ours alone: it persists until we clear it.
static const u32 kSuspendStatus = fopAcStts_UNK_0x20000000_e;

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

// True once camera `cameraIdx` finished its create phases — only then is its
// dCamera_c constructed and registered in cameraInfo, i.e. safe to point a
// window at (a mid-create camera raises windowNum itself in init_phase2).
static bool isCameraReady(int cameraIdx) {
    fpc_ProcID camID = fopCamM_GetID(cameraIdx);
    if (camID == 0 || camID == kNoProcID) return false;
    return fpcM_SearchByID(camID) != NULL;
}

// One-player-only contexts: pause/menus, a cutscene camera, or P1 riding
// anything (horse/boar/spinner/canoe/board — daPy_py_c::checkRide). This
// predicate IS the co-op feel — keep it conservative. dDemo_c::m_object only
// exists inside the play scene (d_s_play creates/removes it) and getCamera()
// asserts on it, so the guard is load-bearing: tick() also runs on the
// boot/menu scenes.
static bool shouldStash() {
    if (dComIfGp_isPauseFlag()) return true;
    if (dDemo_c::m_object != NULL && dDemo_c::getCamera() != NULL) return true;
    daPy_py_c* p1 = daPy_getPlayerActorClass();
    if (p1 != NULL && p1->checkRide()) return true;
    return false;
}

// coop: P1 is in (or just finishing) an NPC conversation. Talk trips
// event_runCheck() the same as a heavy demo, but it is harmless with a guest
// present — the NPC's look-at and the dialogue both resolve to P1 (the
// searchPlayerTarget split keeps NPCs off the guest), so there is no reason to
// despawn P2. The story-event eviction skips this case and leaves P2 standing
// there, as it did before co-op touched it.
//
// The conversation's event lingers a few frames past PROC_TALK (the NPC's
// wind-down), so a bare PROC_TALK check despawned + respawned P2 in that tail.
// A short grace, refreshed every frame P1 is actually in PROC_TALK and counted
// down only while we're polling (event running + guest present), rides out the
// tail without a flicker. If a genuinely heavy event follows the talk, the grace
// just delays P2's eviction by a fraction of a second — harmless now that the
// real crashes are fixed.
static int s_talkGrace = 0;
static bool p1IsTalking() {
    daAlink_c* p1 = (daAlink_c*)daPy_getPlayerActorClass();
    if (p1 != NULL && p1->mProcID == daAlink_c::PROC_TALK) {
        s_talkGrace = kTalkGraceFrames;
        return true;
    }
    if (s_talkGrace > 0) {
        s_talkGrace--;
        return true;
    }
    return false;
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

fopAc_ac_c* nearestPlayer(const cXyz& i_pos) {
    fopAc_ac_c* best = dComIfGp_getPlayer(0);
    // Solo/Disabled have no guests, and Stashed guests are frozen/invisible
    // and must not draw aggro — all three pin targeting to P1.
    if (s_state != State::Active || best == NULL) return best;
    f32 bestDist = best->current.pos.abs2(i_pos);
    for (int i = 1; i < kMaxPlayers; i++) {
        fopAc_ac_c* guest = dComIfGp_getPlayer(i);
        if (guest == NULL) continue;  // empty slot / guest mid-create
        // belt-and-suspenders for the Active->Stashed transition frame: a
        // suspended guest never runs execute and can't be a target
        if (fopAcM_CheckStatus(guest, kSuspendStatus)) continue;
        f32 dist = guest->current.pos.abs2(i_pos);
        if (dist < bestDist) {
            bestDist = dist;
            best = guest;
        }
    }
    return best;
}

// Called from camera init_phase2 once a guest camera (index 1..3) is fully built.
void markGuestCameraReady(int cameraIdx) {
    if (cameraIdx >= 1 && cameraIdx < kMaxPlayers) {
        s_guestCamInitDone[cameraIdx - 1] = true;
    }
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

// Drops the guest at P1's side and kills inherited momentum (vertical speed
// and forward run speed). Used by the stash restore and by the distance/fall
// recovery.
static void snapGuestToP1(fopAc_ac_c* guest, int playerNo) {
    daPy_py_c* p1 = daPy_getPlayerActorClass();
    if (p1 == NULL) return;
    guest->current.pos = p1->current.pos;
    guest->current.pos.x += kGuestSpawnOffsetX * playerNo;
    guest->old.pos = guest->current.pos;
    guest->speed.y = 0.0f;
    guest->speedF = 0.0f;
}

// Creates (or revives) player playerNo's attention instance. Storage comes
// from cMl (persistent), the instance's own 0x9000 solid heap from the game
// heap — same lifetime class as the camera prm append. On alloc failure the
// slot stays NULL and every reader falls back to P1's instance (the pre-
// per-player behavior); the camera-side Init guards against that fallback.
static void createAttention(int playerNo) {
    s_attnReapID[playerNo - 1] = kNoProcID;  // rejoin reuses a live instance
    if (s_attention[playerNo - 1] != NULL) return;
    void* mem = cMl::memalignB(-4, sizeof(dAttention_c));
    if (mem == NULL) {
        DuskLog.info("coop: P{} attention alloc failed, sharing P1 lock-on", playerNo + 1);
        return;
    }
    // the guest actor doesn't exist yet — runGuestAttention() and camera
    // init_phase2 bind the real player pointer before the first Run()
    s_attention[playerNo - 1] =
        JKR_NEW_ARGS (mem) dAttention_c((fopAc_ac_c*)NULL, PAD_1 + playerNo);
}

// Arms the deferred reap: the instance is destroyed once actorID (the guest
// that may still hold a cached mAttention pointer) no longer exists.
static void scheduleAttnReap(int playerNo, unsigned int actorID) {
    if (s_attention[playerNo - 1] != NULL) {
        s_attnReapID[playerNo - 1] = actorID;
    }
}

// Runs at the top of every tick, in every state — a quit-to-title can leave
// an armed reap behind that must still fire on the menu scenes.
static void reapAttention() {
    for (int i = 0; i < kMaxPlayers - 1; i++) {
        if (s_attnReapID[i] == kNoProcID) continue;
        fopAc_ac_c* actor = NULL;
        if (fopAcM_SearchByID(s_attnReapID[i], &actor) != 0) continue;  // still dying
        if (s_attention[i] != NULL) {
            s_attention[i]->~dAttention_c();
            cMl::free(s_attention[i]);
            s_attention[i] = NULL;
        }
        s_attnReapID[i] = kNoProcID;
        DuskLog.info("coop: P{} attention reaped", i + 2);
    }
}

dAttention_c* getAttention(int playerNo) {
    if (playerNo < 1 || playerNo >= kMaxPlayers) return NULL;
    return s_attention[playerNo - 1];
}

void runGuestAttention() {
    if (s_state != State::Active) return;
    for (int i = 1; i < kMaxPlayers; i++) {
        dAttention_c* attn = s_attention[i - 1];
        if (attn == NULL) continue;
        fopAc_ac_c* guest = getGuestActor(i);
        if (guest == NULL) continue;  // despawned or still mid-create
        // belt-and-suspenders for the Active->Stashed transition frame — a
        // suspended guest takes no input, so its lock-on must not advance
        if (fopAcM_CheckStatus(guest, kSuspendStatus)) continue;
        // re-assert the binding every frame: camera init_phase2 also binds,
        // but a rejoin reuses this instance through a camera that may have
        // been adopted without re-running its bind, and mpPlayer must never
        // be left pointing at a freed actor
        attn->Init(guest, PAD_1 + i);
        attn->Run();
    }
}

void drawGuestAttention() {
    if (s_state != State::Active) return;
    for (int i = 1; i < kMaxPlayers; i++) {
        dAttention_c* attn = s_attention[i - 1];
        if (attn == NULL) continue;
        fopAc_ac_c* guest = getGuestActor(i);
        if (guest == NULL || fopAcM_CheckStatus(guest, kSuspendStatus)) continue;
        attn->Draw();
    }
}

static void spawnGuest(int playerNo) {
    if (shouldStash() || dComIfGp_event_runCheck()) {
        // no joining during cutscenes/menus/rides/story-events — also closes
        // the deferred horse-start hazard (the guest's create would read latched
        // horse-start globals while P1 rides). event_runCheck covers forced
        // story events that aren't a dDemo-with-camera (first-twilight demo38);
        // a guest created mid-event hangs the single-track event system, so a
        // mistimed START press during a cutscene is refused, not honored.
        DuskLog.info("coop: P{} join refused (cutscene/menu/ride active)", playerNo + 1);
        return;
    }
    daPy_py_c* p1 = dComIfGp_getLinkPlayer();
    if (p1 == NULL) return;  // no P1 at all
    fopAc_ac_c* p1Done = NULL;
    if (!fopAcM_SearchByID(fopAcM_GetID(p1), &p1Done) || p1Done == NULL) {
        return;  // P1 still mid-create — guest create would corrupt the
                 // shared bgWaitFlg latch in daAlink_c::create
    }

    cXyz pos = p1->current.pos;
    pos.x += kGuestSpawnOffsetX * playerNo;
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
        s_wantRejoin = false;
        s_rejoinWaitFrames = 0;

        // per-player Z-targeting — must exist before the guest's create phase
        // caches it and before camera init_phase2 binds it
        createAttention(playerNo);

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
    // deliberate removal (hold-START leave, coop disable) cancels any pending
    // auto-rejoin
    s_wantRejoin = false;
    if (playerNo >= 1 && playerNo < kMaxPlayers) {
        s_guestCamInitDone[playerNo - 1] = false;
    }
    if (s_guestActorID[playerNo - 1] != kNoProcID) {
        // free the attention instance only once the actor (whose cached
        // mAttention points at it) is truly gone
        scheduleAttnReap(playerNo, s_guestActorID[playerNo - 1]);
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
    reapAttention();
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
        s_wantRejoin = false;
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

    // Scene change deleted the guest from under us (possible both mid-gameplay
    // and mid-cutscene, so check Active AND Stashed) → drop to Solo and queue
    // an auto-rejoin. The camera procs are stage-layer so camera 1 died (or is
    // dying) with the scene — teardownCamera deletes it if somehow still alive
    // and clears the stale l_fopCamM_id slot either way.
    if ((s_state == State::Active || s_state == State::Stashed) && isGuestGone(1)) {
        scheduleAttnReap(1, s_guestActorID[0]);  // actor already gone -> fires next tick
        s_guestActorID[0] = kNoProcID;
        s_guestProcID[0] = kNoProcID;
        s_guestCamInitDone[0] = false;
        teardownCamera(1);
        dComIfGp_setCameraInfo(1, NULL, 1, 1, -1);
        applySplitLayout(1, /*forceNum*/ true);
        s_state = State::Solo;
        s_wantRejoin = true;
        s_rejoinWaitFrames = 0;
        DuskLog.info("coop: guest Link P2 gone (scene change), back to Solo");
    }

    static int holdFrames = 0;
    if (s_state == State::Solo) {
        holdFrames = 0;
        // settle counter: count consecutive frames with no story event and no
        // other stash context. Any event/stash frame resets it. The rejoin only
        // fires once it crosses the threshold, so a flickering forced event
        // (demo38) can't bait a fresh P2 into the volatile transition window.
        if (!shouldStash() && !dComIfGp_event_runCheck()) {
            if (s_eventSettleFrames < kEventSettleFrames) s_eventSettleFrames++;
        } else {
            s_eventSettleFrames = 0;
        }
        if (mDoCPd_c::getTrigStart(PAD_2)) {
            spawnGuest(1);  // refuses by itself during stash contexts
        } else if (s_wantRejoin && !shouldStash() &&
                   s_eventSettleFrames >= kEventSettleFrames) {
            // auto-rejoin after a scene change or a story-event eviction took
            // the guest. spawnGuest clears the flag on success; a transient
            // refusal (P1 still mid-create, alloc failure) just retries.
            //
            // coop: gated on the settle counter — a scene change can drop
            // straight into a forced story event (first-twilight demo38), and
            // that event isn't a dDemo-with-camera so shouldStash() misses it.
            // Recreating a guest INTO a running (or just-ended, still-settling)
            // event hangs the single-track event system. Wait out the event AND
            // a stability window, then bring a fresh P2 back.
            spawnGuest(1);
        }
        // a rejoin that never lands (quit-to-title tore the play session
        // down) must not survive into the next loaded save — time it out. Don't
        // count down while an event is running, or a long forced cutscene would
        // abandon a legitimately-pending rejoin.
        if (s_wantRejoin && !dComIfGp_event_runCheck() && ++s_rejoinWaitFrames > kRejoinTimeoutFrames) {
            s_wantRejoin = false;
            s_rejoinWaitFrames = 0;
            DuskLog.info("coop: pending P2 rejoin timed out, abandoned");
        }
    } else if (s_state == State::Active) {
        // leave: hold START on pad 2 (~2s at 30fps logic). Only honored while
        // Active — a stashed (invisible) P2 can't leave. Only despawn once
        // the actor fully exists — deleting a mid-create process would leave
        // an orphan Link that self-identifies as P1.
        holdFrames = mDoCPd_c::getHoldStart(PAD_2) ? holdFrames + 1 : 0;
        if (holdFrames > 60 && getGuestActor(1) != NULL) {
            despawnGuest(1);
            holdFrames = 0;
        } else if (dComIfGp_event_runCheck() && getGuestActor(1) != NULL && !p1IsTalking()) {
            // coop: story-event eviction. A forced cutscene (first-twilight
            // demo38) isn't a dDemo-with-camera, so shouldStash() misses it, and
            // a second Link present while the single-track event system runs
            // hangs it. Despawn now — suspend-and-revive across it died, and so
            // did auto-recreating a fresh P2 into the gaps of the post-transform
            // cutscene CHAIN (transform -> wolf -> cell -> Midna): each gap
            // respawns P2, the next cutscene evicts it, and a camera create
            // landing in that churn is fatal.
            //
            // Auto-rejoin once the event ends AND the world settles: despawn
            // now, re-arm the rejoin, and let the settle-gated auto-rejoin in
            // the Solo branch bring a fresh P2 back (it waits kEventSettleFrames
            // of no-event frames, so it can't recreate P2 into the flickering
            // transition window). Manual START still works too.
            despawnGuest(1);   // clears s_wantRejoin
            s_wantRejoin = true;       // re-arm: auto-rejoin after the event settles
            s_rejoinWaitFrames = 0;
            s_eventSettleFrames = 0;
            DuskLog.info("coop: P2 evicted for story event (auto-rejoin after it ends)");
        } else if (shouldStash()) {
            // Single P1 window while stashed. isSplitActive() goes false, which
            // releases the split-only renderer gates; the guest's camera is
            // suspended separately in d_camera.cpp (camera_execute/camera_draw
            // early-return during Stashed) so it can't stomp shared view state.
            // A guest still mid-create has no actor to flag yet; the Stashed
            // branch below picks it up once it exists.
            if (fopAc_ac_c* guest = getGuestActor(1)) {
                fopAcM_OnStatus(guest, kSuspendStatus);
            }
            applySplitLayout(1, /*forceNum*/ true);
            s_state = State::Stashed;
            DuskLog.info("coop: P2 stashed");
        } else {
            // coop: re-engage the split if the scene-fade clobbered it. A guest
            // camera that finished init_phase2 mid-fade raised windowNum to 2,
            // but the fade lowered it back and init_phase2 won't re-raise — so
            // P2's half stays black (single window) until forced. Only do this
            // once the camera's body is actually built (s_guestCamInitDone),
            // never on a process that's still mid-init (that crashes), and only
            // here in normal play — never during a stash/cutscene/menu.
            if (s_guestCamInitDone[0] && getGuestActor(1) != NULL &&
                dComIfGp_getWindowNum() < 2) {
                applySplitLayout(2, /*forceNum*/ true);
            }

            // distance/fall recovery: a guest that fell behind a loading gap
            // or off a cliff P1 already crossed gets teleported back.
            fopAc_ac_c* guest = getGuestActor(1);
            daPy_py_c* p1 = daPy_getPlayerActorClass();
            if (guest != NULL && p1 != NULL) {
                // coop: P2 follows P1's form. If P1 changed form while P2 was
                // active in the same scene (quick-transform, or a stash/restore
                // spanning a story transform), rebuild the guest in the new form
                // via the proven create path — despawn + auto-rejoin, where
                // spawnGuest re-reads P1's current form — rather than a live
                // mid-game model/heap swap (changeWolf allocates a full wolf +
                // wolf-Midna model set into the instance heap).
                bool p1Wolf = p1->checkWolf() != 0;
                bool guestWolf = ((daPy_py_c*)guest)->checkWolf() != 0;
                if (p1Wolf != guestWolf) {
                    despawnGuest(1);
                    s_wantRejoin = true;  // despawnGuest cleared it; re-arm
                    s_rejoinWaitFrames = 0;
                    DuskLog.info("coop: P2 re-forming to match P1");
                } else {
                    f32 distXZSq = guest->current.pos.abs2XZ(p1->current.pos);
                    f32 yDrop = p1->current.pos.y - guest->current.pos.y;
                    if (distXZSq > kRecoverDistXZ * kRecoverDistXZ || yDrop > kRecoverDrop) {
                        snapGuestToP1(guest, 1);
                        DuskLog.info("coop: P2 recovered to P1");
                    }
                }
            }
        }
    } else if (s_state == State::Stashed) {
        holdFrames = 0;
        fopAc_ac_c* guest = getGuestActor(1);
        if (!shouldStash() && daPy_getPlayerActorClass() != NULL) {
            // restore: unfreeze next to P1 (the stash context likely moved
            // P1 — a dropped-off horse ride, a cutscene warp). If P1 is gone
            // (scene teardown edge) stay Stashed this tick — gone-detection
            // will clean up the guest shortly. Stash is now only entered for
            // pause/ride/demo-with-camera, all of which revive cleanly — story
            // events take the evict path instead, so no restore hysteresis here.
            if (guest != NULL) {
                snapGuestToP1(guest, 1);
                fopAcM_OffStatus(guest, kSuspendStatus);
            }
            // cameras already exist → force windowNum back to 2; if camera 1
            // is somehow still mid-create (stash hit during the join frames),
            // don't force — its init_phase2 raises windowNum once it's safe.
            applySplitLayout(2, /*forceNum*/ isCameraReady(1));
            s_state = State::Active;
            DuskLog.info("coop: P2 restored");
        } else {
            // re-assert every frame: covers a guest that finished its create
            // after the stash transition, and stomps a late camera-1
            // init_phase2 raising windowNum mid-stash.
            if (guest != NULL) {
                fopAcM_OnStatus(guest, kSuspendStatus);
            }
            applySplitLayout(1, /*forceNum*/ true);
        }
    } else {
        holdFrames = 0;
    }
}

void setProjectileOwner(unsigned int projectileActorID, int ownerPlayerNo) {
    initProjectileOwners();
    // slot 0 (P1) records nothing — vanilla singleton already resolves to it.
    if (ownerPlayerNo <= 0 || ownerPlayerNo >= kMaxPlayers) return;
    if (projectileActorID == kNoProcID) return;
    int freeIdx = -1;
    for (int i = 0; i < kMaxProjectileOwners; i++) {
        // overwrite any existing entry for this ID (a re-fired pooled arrow
        // keeps the same actor ID across shots)
        if (s_projOwnerID[i] == projectileActorID) {
            s_projOwnerNo[i] = (u8)ownerPlayerNo;
            return;
        }
        if (freeIdx < 0 && s_projOwnerID[i] == kNoProcID) freeIdx = i;
    }
    if (freeIdx < 0) {
        // table full (shouldn't happen at v1 player counts) — reclaim the slot
        // of any owner whose projectile actor is already gone
        for (int i = 0; i < kMaxProjectileOwners; i++) {
            fopAc_ac_c* dead = NULL;
            if (fopAcM_SearchByID(s_projOwnerID[i], &dead) == 0) {
                freeIdx = i;
                break;
            }
        }
    }
    if (freeIdx < 0) return;  // give up gracefully → projectile falls back to P1
    s_projOwnerID[freeIdx] = projectileActorID;
    s_projOwnerNo[freeIdx] = (u8)ownerPlayerNo;
}

int getProjectileOwnerPlayerNo(unsigned int projectileActorID) {
    if (!s_projOwnerInit || projectileActorID == kNoProcID) return -1;
    for (int i = 0; i < kMaxProjectileOwners; i++) {
        if (s_projOwnerID[i] == projectileActorID) return s_projOwnerNo[i];
    }
    return -1;
}

void beginPendingProjectileOwner(int ownerPlayerNo) {
    s_pendingOwnerNo = (ownerPlayerNo > 0 && ownerPlayerNo < kMaxPlayers) ? ownerPlayerNo : -1;
}

void endPendingProjectileOwner() {
    s_pendingOwnerNo = -1;
}

daAlink_c* getOwnerAlink(fopAc_ac_c* projectile) {
    int ownerNo = -1;
    if (projectile != NULL) {
        ownerNo = getProjectileOwnerPlayerNo(fopAcM_GetID(projectile));
    }
    // create-phase fallback: the ID stamp hasn't landed yet, so honor the
    // latch the firing player set around the fastCreate call
    if (ownerNo <= 0 && s_pendingOwnerNo > 0) {
        ownerNo = s_pendingOwnerNo;
    }
    if (ownerNo > 0 && ownerNo < kMaxPlayers) {
        daAlink_c* owner = (daAlink_c*)dComIfGp_getPlayer(ownerNo);
        if (owner != NULL) {
            return owner;
        }
    }
    // solo, P1-owned, unrecorded, or the owner actor is gone → vanilla P1
    return daAlink_getAlinkActorClass();
}

}  // namespace dusk::coop

// Per-player attention accessor declared in d_com_inf_game.h / d_camera.h.
// playerNo 0 (and any slot without an instance) resolves to the embedded P1
// instance, so existing no-arg call sites and per-player reads share one
// fallback-safe funnel.
dAttention_c* dComIfGp_getAttention(int i_playerNo) {
    if (i_playerNo > 0) {
        dAttention_c* attn = dusk::coop::getAttention(i_playerNo);
        if (attn != NULL) {
            return attn;
        }
    }
    return dComIfGp_getAttention();
}
