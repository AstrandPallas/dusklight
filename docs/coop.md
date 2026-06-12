# Split-Screen Co-op Guide

Local 2-player split-screen co-op for Twilight Princess. Player 2 joins as a
second Link with their own camera and half of the screen.

## Getting started

1. **Enable co-op:** Settings → Gameplay → **Split-Screen Co-op**.
2. **Connect a second controller** and assign it to **port 2** in the
   controller settings.
3. **Join:** during normal gameplay, press **START on controller 2**. A second
   Link spawns next to Player 1 and the screen splits.
4. **Leave:** hold **START on controller 2** for about 2 seconds.

## Screen layout

The screen splits horizontally: **Player 1 plays on the top half, Player 2 on
the bottom half**. A small **P1**/**P2** tag appears in the top-left corner of
each half whenever the split starts, then fades out after a few seconds.

## How Player 2 behaves

- **Cutscenes, menus, and rides:** Player 2 is hidden automatically while a
  cutscene plays, a menu is open, or Player 1 is riding anything (horse, boar,
  spinner, canoe, snowboard). The screen returns to full size for Player 1,
  and Player 2 reappears at Player 1's side once the moment is over.
- **Area transitions:** loading into a new area briefly removes Player 2;
  they rejoin automatically once the new area is ready.
- **Recovery:** if Player 2 strays too far from Player 1 or falls somewhere
  they can't get back from, they are warped back to Player 1's side.

## Shared resources

Hearts, rupees, and items are shared between both players — you live and die
as a team. If the shared hearts run out, it's game over for both players.

## Current limitations

- Player 2 cannot use Z-targeting/lock-on, X/Y items, or transform; sword,
  shield, roll, and grab all work.
- Enemies mostly target Player 1 (they can still hurt Player 2 on contact).
- Some screen-space effects (bloom, depth of field, motion blur, sun flare)
  are reduced or anchored to Player 1's view while the screen is split.

The architecture supports up to 4 players; this release activates 2.
