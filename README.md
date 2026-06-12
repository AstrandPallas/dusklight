<div align="center">
  <img src="res/logo.png" alt="Logo" width="640">

  <h3>Split-Screen Co-op Fork</h3>

  <p align="center">
    A fork of <a href="https://github.com/TwilitRealm/dusklight">Dusklight</a> adding local split-screen co-op to Twilight Princess.
  </p>
</div>

# Dusklight Co-op

This fork adds **local 2-player split-screen co-op** to Dusklight, the reverse-engineered native PC port of *The Legend of Zelda: Twilight Princess*. Player 2 joins as a second Link with their own camera and half of the screen — explore Hyrule and fight together on one PC.

The architecture is built for up to 4 players; this release activates 2.

## How to play

1. Enable **Split-Screen Co-op** in Settings → Gameplay.
2. Connect a second controller and assign it to **port 2** in the controller settings.
3. In normal gameplay, **press START on controller 2** — a second Link spawns next to Player 1 and the screen splits (P1 top, P2 bottom).
4. **Hold START on controller 2 (~2 seconds)** to leave.

Hearts, rupees, and items are shared — you live and die as a team.

## Status: pre-alpha

This is an early, playable preview. Player 1 is always "the" player for world logic; Player 2 is a guest. Current limitations:

- **Cutscenes, menus, and mounts don't handle P2 yet** — Player 2 is *not* hidden during cutscenes or horseback sequences and may behave oddly during them. Leave (hold START) before story moments for best results.
- Player 2 cannot use Z-targeting/lock-on, X/Y items, or transform; sword, shield, roll, and grab all work.
- Player 2 disappears on area transitions and auto-rejoin is not in yet — press START again after a load.
- Enemies primarily target Player 1 (they'll still hurt P2 on contact).
- Some screen-space effects (bloom, depth of field, motion blur, sun flare) are reduced or anchored to Player 1's view while the screen is split.
- If Player 2 falls somewhere unrecoverable, leave and rejoin to warp back to Player 1.

Bug reports are welcome on the issues page — please include where you were and what both players were doing.

# Setup

> [!IMPORTANT]
> Like upstream Dusklight, this fork does *not* provide any copyrighted assets. **You must dump and supply your own copy of the original game.** Only the GameCube USA and EUR releases are supported.

> [!IMPORTANT]
> A GPU with D3D12, Vulkan, or Metal support is required.

1. Dump your own copy of the game — see [this article](https://wiki.dolphin-emu.org/index.php?title=Ripping_Games) for instructions. `.iso`/`.rvz` and other common formats are supported.
2. Download a release from [this fork's releases page](https://github.com/AstrandPallas/dusklight/releases).
3. Extract, launch Dusklight, select your disc image, and press **Play**.

# Building

Identical to upstream — see the [build instructions](docs/building.md). The co-op work lives on the `coop-splitscreen` branch.

# Credits

All credit for Dusklight itself goes to the [TwilitRealm](https://github.com/TwilitRealm/dusklight) team. Special thanks to the [TP decompilation](https://github.com/zeldaret/tp) team, the GC/Wii decompilation community, the [Aurora](https://github.com/encounter/aurora) developers, and the [TP speedrunning community](https://zsrtp.link).

<br/>
<div align="center">
    <a href="https://github.com/encounter/aurora">
        <img src="assets/aurora-powered.png" alt="Powered by Aurora" width="800">
    </a>
</div>
