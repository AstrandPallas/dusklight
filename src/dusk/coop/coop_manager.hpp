#pragma once
#include "dusk/coop.h"

class dAttention_c;
class daAlink_c;
class fopAc_ac_c;

namespace dusk::coop {

// Called once per game-logic frame from duskExecute().
void tick();

// Per-player projectile ownership. X/Y item projectiles (arrow/sling stone,
// boomerang, player bomb) are normal world actors that historically read the
// P1 singleton (daAlink_getAlinkActorClass()) for spawn position, aim, facing,
// catch position and owner-count bookkeeping. In co-op a guest-fired
// projectile must thread back to the guest instead. The firing daAlink_c
// stamps its player slot onto the projectile actor's ID at creation; readers
// resolve it through getOwnerAlink(), which falls back to P1 when no owner is
// recorded (solo, or any un-threaded creation path) so non-co-op stays
// byte-for-byte vanilla.
//
// ownerPlayerNo is the firing player's slot (0 = P1). Stamping slot 0 records
// nothing — P1-owned projectiles share the vanilla singleton fallback.
void setProjectileOwner(unsigned int projectileActorID, int ownerPlayerNo);
// -1 when the projectile has no recorded (non-P1) owner.
int getProjectileOwnerPlayerNo(unsigned int projectileActorID);

// Owner latch for the create phase. fopAcM_fastCreate runs the new actor's
// create() synchronously and returns only afterwards, so the ID-keyed stamp
// at the call site lands AFTER create() has already read its owner (boomerang
// keep-matrix, insect-bomb facing, sling-stone fly data). The firing player
// brackets the fastCreate call with begin/endPendingProjectileOwner so
// getOwnerAlink() resolves to the firer even mid-create; the ID stamp then
// pins it for the actor's remaining life. Nested/forgotten begins are safe:
// end clears unconditionally and the latch is only consulted as a fallback.
void beginPendingProjectileOwner(int ownerPlayerNo);
void endPendingProjectileOwner();

// Resolves the owning player's daAlink_c for a projectile actor. Falls back to
// the P1 singleton (daAlink_getAlinkActorClass()) when no owner is recorded or
// the recorded owner's actor can no longer be resolved. Never returns the
// recorded owner if that player slot is empty — always a valid live player or
// P1. This is the single funnel every rerouted in-projectile singleton read
// goes through.
daAlink_c* getOwnerAlink(fopAc_ac_c* projectile);

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
