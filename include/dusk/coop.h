#pragma once

namespace dusk::coop {

// Architecture supports up to 4 players (engine pad channels / camera slots
// are natively 4-wide); v1 activates at most 2.
constexpr int kMaxPlayers = 4;

enum class State { Disabled, Solo, Active, Stashed };

State getState();
// True when more than one window is being rendered (Active state, not stashed).
bool isSplitActive();
// Number of players currently in the world (1 when Solo/Disabled).
int playerCount();
// Player index (1..3) if the given fpc process ID belongs to a pending/live
// guest, else -1. P1 is never a guest.
int guestPlayerNo(unsigned int procID);

}  // namespace dusk::coop
