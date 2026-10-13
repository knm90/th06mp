# th06mp

2–4 player co-op for *Touhou Koumakyou: New Classic* (`th06nc.exe`, ver 1.03, the build linked 2026-09-07), on one PC or over the network.

Extra player characters share the playfield with player 1. Lives, bombs, power and score are one shared pool.

This repository contains no game files. You need your own copy of the game.

## Install

1. Download this repository (**Code → Download ZIP** on GitHub) and unpack it.
2. Copy the three files in `dist\` into the game folder, next to `th06nc.exe`: `th06mp.exe`, `th06mp.dll`, `th06mp.ini`.
3. Start the game with `th06mp.exe` instead of `th06nc.exe`.

The mod is loaded into the running game only; no game file is changed. Running `th06nc.exe` directly gives the unmodified game. To remove the mod, delete the three files and `th06mp.log`.

The files can also stay outside the game folder: pass the path to `th06nc.exe` as an argument to `th06mp.exe`.

If it does not work:

- **"This build of the game is not supported"**: the mod only works with the exact game build named above and refuses to load into any other.
- **Windows SmartScreen or antivirus warning**: `th06mp.exe` is unsigned and loads a DLL into another program, which is what such tools look for. You can build both files from this source instead; see [Build](#build).
- **The game opens but there is no second player**: read `th06mp.log`, written next to `th06mp.dll` on every run. On a Steam install the game may restart itself through Steam, which drops the mod. This is untested; the usual way to start a Steam game directly is a `steam_appid.txt` next to the game's exe holding the game's Steam app id (the number in its store page address), with Steam running.

## Play on one PC

1. Set `players=` in `th06mp.ini` (2–4) and change the keys if you like.
2. Run `th06mp.exe` and start a normal game. The extra players appear when the stage begins.

Default controls:

| Player | Keyboard | Controller |
| --- | --- | --- |
| 1 | arrows, Z shot, X bomb, Shift focus | first controller |
| 2 | W A S D, J shot, K bomb, L focus | next free controller |
| 3, 4 | unbound | next free controller |

The game normally lets player 1 use WASD + J/K/L as well. During co-op, keys bound to other players are taken away from player 1.

Controllers are XInput only: stick or d-pad to move, A shot, B/Y bomb, X/LB/RB/triggers focus. With fewer controllers than players, player 1 moves to the keyboard and the controllers go to the others.

## Play over the network

One machine hosts and is player 1. The others join and become players 2–4 in the order they connect. Every machine runs the full game and only button presses are exchanged, so everyone needs:

- the same game build and the same `th06mp` files;
- the same **lives** and **bombs** in the game's Options (checked when connecting).

Settings in `th06mp.ini`:

| Setting | Host | Guest |
| --- | --- | --- |
| `[net] mode` | `host` | `join` |
| `[net] address` | not used | the host's IP address |
| `[th06mp] players` | total number of players | taken from the host |

The host's `character`, `shot_type`, `target` and `delay` settings also apply to everyone.

1. Make the host reachable on TCP port 17606. Either forward that port on the host's router and give the guests the host's public IP, or put every PC on a virtual LAN (Tailscale, ZeroTier, Radmin VPN) and use the host's address on it. Allow the game through Windows Firewall when it asks.
2. Everyone starts `th06mp.exe`. A small box says who the game is waiting for; it goes away when all players are connected. Esc in that box gives up and plays offline.
3. The host picks everything in the menus and is the only one who can pause. In a stage, each player uses the normal player-1 controls on their own machine.

Button presses take effect `delay` frames late (3 by default). Raise it on the host if the game stutters.

If the games stop matching, the host's `th06mp.log` reports the first tick where they differ (`DESYNC`). To report a problem, set `debug=1` on every machine, play again, and send all the `th06mp.log` files.

## Status

Not done yet:

- Items picked up by players 2–4 are credited through player 1: the item jumps to P1, so the score popup shows there, and when another player crosses the top line the items fly to P1.
- Lives are a single shared pool at the game's normal size, so they drain faster with more players.
- Character and shot type for players 2–4 are set in the ini (`character=`, `shot_type=`), not in a menu. By default player 2 is the other character, player 3 the other shot type, player 4 both.
- A bomb's cut-in portrait always shows player 1's character.
- Replays recorded in co-op will not play back correctly; watching a replay turns the extra players off.
- Network play has one player per machine and no reconnect: after a dropped connection each game carries on alone until restarted.
- Network play has only been tested between two game windows on one PC, with two players, in stage 1.

## Build

Needs LLVM-MinGW (`winget install MartinStorsjo.LLVM-MinGW.UCRT`).

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

The result goes to `dist\`. An existing `dist\th06mp.ini` is kept.

## Layout

- `dist/` — ready-built launcher, mod and default settings.
- `src/launcher/` — `th06mp.exe`: starts the game suspended, loads the DLL, resumes it.
- `src/mod/` — `th06mp.dll`. `game.h` holds every address used; `coop.c` holds the hooks; `net.c` the network layer.
- `tests/patch_test.c` — checks the patch table against the exe without running the game.
- `tests/net_test.c` — runs the network layer between local processes without the game.
- `re/` — Python scripts used to map the exe (`disasm.py`, `index.py`, `xref.py`) and `symbols.txt`, the list of identified functions and globals. They need `pip install capstone`.
