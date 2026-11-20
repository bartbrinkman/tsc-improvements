# tsc-improvements

A free mod for Train Simulator Classic. It makes the game run faster and removes some picture faults.

The mod is one file, `version.dll`. You put it in the game folder. It does not change any game file. To remove it, you delete the file again.

This is a fan project. It is not made by, or supported by, the makers of the game.

## Install

You need Train Simulator Classic on Windows, 64-bit, from Steam. The mod was tested with the DirectX 9 version of the game (`RailWorks64.exe`), November 2026.

1. Go to the [Releases](../../releases) page. Download the newest `tsc-improvements-<version>.zip`.
2. Open the game folder. In Steam, right-click **Train Sim Classic**. Select **Manage**, then **Browse local files**. This opens the folder that has `RailWorks64.exe` in it.
3. Unzip the two files `version.dll` and `tscfix.ini` into that folder.
4. Start the game as usual.

When the mod runs, it writes the file `tscfix.log` into the game folder.

Do you already have a `version.dll` from a different mod in the game folder? Then do not overwrite it. The two mods cannot be used together.

## Remove

1. Close the game.
2. In the game folder, delete `version.dll`, `tscfix.ini` and `tscfix.log`.

Steam's "Verify integrity of game files" does not remove the mod. You must delete the files yourself.

## What the mod does

- **More frames per second.** The game draws the picture on more processor cores. On the test PC, a standing view at Haarlem took 26.7 ms per frame with the mod and 39.0 ms without it.
- **Fewer stutters.** Some slow steps of the game are made faster. Examples are scenery that unloads and script calls that change nothing.
- **Fewer crashes from memory limits.** Two fixed memory areas of the game get more room.
- **Steadier picture.**
  - Numbers on signs no longer flicker.
  - The cab view is fixed to the cab. It no longer sways slowly when the train stands still. The head movement while driving is also gone.
  - A braked train that stands still now stays fully still.
  - Far signal lenses no longer flicker while the train stands.
  - Parked trains no longer lose their car bodies when you drive past them.
- **Picture changes.**
  - With supersampling on, the picture is made smaller with a sharper filter, and the game's FXAA blur is off.
  - The game's bloom and colour step is replaced by the mod's own.
  - Lamp light gets weaker in daylight. It no longer changes brightness from moment to moment.
- **Clearer crash messages.** When the game stops, the message tells you what happened. The full record is in `tscfix.log`.

## Settings

The settings are in `tscfix.ini` in the game folder. Open it with Notepad. Each setting has a short description. The game reads the settings when it starts.

To switch off a part of the mod, set its value to `0`. For example, to keep the game's own picture filter:

```ini
[graphics]
better_resolve=0
```

## When the game updates

The mod finds the parts of the game that it changes when the game starts. If a game update changes such a part, the mod does not change it. It writes a line about this in `tscfix.log`. The game then works as without that part of the mod.

## Problems

If you have a problem, open an issue on this page. Attach `tscfix.log` from the game folder. Tell us the route, the train and what you saw.

## For developers

How the mod works, how to build it and all settings are in [docs/TECHNICAL.md](docs/TECHNICAL.md).

No part of the game is stored in this project. See [docs/IP-POLICY.md](docs/IP-POLICY.md).

MinHook (BSD 2-clause license) is in `third_party/minhook`.
