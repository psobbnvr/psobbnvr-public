# PSO:BB:nVR

This is a first-person VR mod for Phantasy Star Online: Blue Burst, played through SteamVR.  
It adds 6DoF head tracking, motion controls, and reprojection for a smooth in-headset experience.  
The mod aims to create a mostly vanilla PSO experience, while adding a few features for VR playability.  These features include:
- First-person view
- Modified character movement (strafing and backpedaling)
- Motion-based melee combat (retains original attack cadence and rhythm)
- Visual swing timing indicator
- Free-aiming of ranged weapons
- Independent mechgun control (Mechguns can be aimed at different targets which halves the normal damage output, or at the same target for full damage)
- Motion casting for techniques
- Technique aiming for targeted techniques
- On-screen keyboard

**Status: beta.** Expect minor visual issues.  The alpha version was in play testing for a few weeks, so most of the major bugs have been worked out.  
If you encounter any issues, please feel free to file a bug report
(see [Reporting a bug](#reporting-a-bug)).

- [Where you can play](#where-you-can-play)
- [Requirements](#requirements)
- [Install](#install)
- [Playing on PSOBB.io](#playing-on-psobbio)
- [Controls](#controls)
- [Settings](#settings)
- [Troubleshooting](#troubleshooting)
- [Reporting a bug](#reporting-a-bug)
- [Uninstall](#uninstall)
- [How it works](#how-it-works)
- [Building from source](#building-from-source)
- [AI Disclaimer](#ai-disclaimer)
- [License](#license)

## Where you can play

This mod was originally developed for the Tethealla v12513 client for use on a local newserv instance.  
However, the folks at PSOBB.io have kindly allowed the use of this mod on their servers for online play.

| Server | Notes |
|---|---|
| Your own local server (e.g. [newserv](https://github.com/fuzziqersoftware/newserv)) | Link to Tethealla client found [here](https://github.com/fuzziqersoftware/newserv#pso-bb) |
| **PSOBB.io** | See [Playing on PSOBB.io](#playing-on-psobbio). |

This mod has been tested with both the Tethealla client and PSOBB.io client.
It has not been tested on any other private-server client and has not been authorized for use on any other private servers.
As this implementation is achieved using memory modifications, it may trip anti-cheat systems or break server rules.
**Due to this, do not attempt to use this on any other server without permission from server owners.  I'm not responsible for you getting your account banned.**

The mod has no network code of its own: every action goes through the game's own routines, so the server sees the same packets a keyboard player sends.

## Requirements

- **Windows 10 or 11.** Linux (Proton) is not supported: the game is a
  32-bit program and Proton currently has no 32-bit OpenXR bridge.
- **A Tethealla-based Blue Burst client**: the English v12513 client
  (PSOBB.io's client is built on it).
- **SteamVR 2.17 or later.** It includes 32-bit OpenXR support.
- **SteamVR set as the OpenXR runtime:** SteamVR Settings → OpenXR →
  "Set SteamVR as OpenXR runtime". The Meta Quest Link app switches this
  back to its own runtime when it updates; the mod checks at every launch
  and tells you if that happened.
- **Quest headsets:** connect through **Steam Link** (the Steam Link app
  on the headset). Link cable, Air Link and Virtual Desktop also work, as
  long as SteamVR is the OpenXR runtime (Virtual Desktop: pick SteamVR,
  not VDXR, in its streaming settings).
- **Laptops with two graphics chips:** before the first launch, tell
  Windows to run the game on the dedicated GPU: Windows Settings →
  System → Display → Graphics → add `psobb.exe` → Options → "High
  performance". Otherwise the headset stays black while the game plays
  sound.

The controls are laid out for Meta Quest Touch, HP Reverb G2 and HTC Vive Wands (Wands are untested). 
Other controllers (Index, Pico) get SteamVR's automatic mapping from the Quest layout.

## Install

1. Download the latest zip from
   [Releases](../../releases). It contains eight files: `d3d8.dll`,
   `dinput8.dll`, `openvr_api.dll`, `openxr_loader.dll`, `psobbvr.ini`,
   `psobbvr_options.exe`, `LICENSE` and `THIRD-PARTY-NOTICES.txt`.
2. Unzip all of them into your game folder, next to `psobb.exe`. If the
   folder already has a `d3d8.dll` or `dinput8.dll` (a graphics or
   widescreen patch), back it up first: the mod's files replace them.
3. Start SteamVR with the headset connected (on Quest, with Steam Link
   running), then launch the game the way you normally do.

The monitor shows a wide side-by-side mirror of both eyes; the headset
shows the game. If VR cannot start, a message box says why and the game
closes when you press OK (see [Troubleshooting](#troubleshooting)).

**Updating:** unzip the new release over the old one. The zip replaces
`psobbvr.ini`, so if you changed your controller bindings, use **Export
bindings...** in the Options program (`psobbvr_options.exe`) first and **Import bindings...**
afterwards.  Make note of your VR Settings tab as well since there is no
import/export function (as parameters here can be added/removed during releases).

## Playing on PSOBB.io

1. Install PSOBB.io and log in once without VR, to be sure the client
   works.
2. In the PSOBB.io folder (by default `C:\Program Files (x86)\PSOBB.IO`),
   **back up `d3d8.dll`**: it is PSOBB.io's widescreen and graphics patch,
   and the mod's `d3d8.dll` replaces it. Then unzip the mod into that
   folder. Windows will ask for administrator permission because the
   folder is under Program Files.
3. In the PSOBB.io launcher's settings, **turn Frame Generation off**.
   With it on, the view flickers in VR.
4. Start SteamVR, then press Play in the launcher as usual.
5. You will need to enter your username and password once more, as the
   mod redirects registry reads and writes so its options don't interfere
   with other installed clients.

Good to know:

- The launcher's resolution, window mode and effect settings (MSAA, SSAO,
  HDR and so on) belong to PSOBB.io's `d3d8.dll`, so they have no effect
  while the mod is installed. The mod sets its own resolution.
- Set **graphics detail and sound** in the PSOBB.io launcher. Each launch
  through the launcher copies those over the Options program's **Game
  options** page.

## Controls

The names below are the OpenXR names; the physical buttons are listed
per controller [further down](#physical-buttons). "Menu open" means any
game menu or dialog is showing.

| Control | Normal play | Menu open |
|---|---|---|
| Left stick | walk (light push walks, full push runs) | walk |
| Right stick left / right | smooth turn | left / right |
| Right stick up / down | — | up / down |
| Right trigger | attack / bottom palette slot (with a melee weapon it arms the attack and your swing performs it) | confirm |
| Left trigger | left palette slot (heavy attack / item / technique) | back |
| Both triggers | right palette slot (special attack) | — |
| Right grip (hold) | the game's Ctrl: the three trigger slots switch to their alternate set (set those up in Customize with right grip held) | — |
| Right grip + A | Quick Menu | press again to cycle Quick Menu pages |
| A | quick chat | — |
| Right grip + B | open the game menu | close it |
| Y | item details / chat context (Tab) | — |
| Right grip + X | open the chat line (the on-screen keyboard comes up with it) | — |
| Right stick click | Enter | confirm |
| Left stick click | Esc | Esc |
| Right grip + left stick click | **recenter**: face forward, stand normally, press | same |
| Left grip (hold) + A / B / X / Y | hotkeys 1–4 | — |
| Left grip + flick right stick up / right / down / left | hotkeys 5–8 | — |
| Left grip + right stick click / left stick click | hotkeys 9 / 0 | — |

Chords are decided the moment the button goes down: hold the grip
**first**, then press the button. Hotkeys do not fire while the right
grip is held.

**Fighting.** Guns are aimed with your right hand. With a melee weapon, the
trigger arms the attack and swinging your hand performs it; with
daggers, knuckles and twin swords either hand's swing counts, so a combo
can go hand over hand. 

**Swing indicator.** The hexagon at the top of the view helps time your
swings: **green** = a swing attacks now, **blue** = a swing lands the
next strike of the attack in progress (for multi-hit weapons like daggers, 
twin swords, and double sabers), **grey** = wait. A ring shrinks
onto it as your next swing comes due.  You can turn it off in the
Options program.

**Techniques.** Your left hand aims (Foie, Barta, Rafoie, Gibarta, Gizonde and Megid)
and a swing with the right hand casts. Techniques you don't aim (Resta, Shifta,
Deband, Zonde, Grants and the like) cast on a swing of either hand.

Swinging is optional: untick "Swing to attack" or "Swing to cast" in the
Options program and that action fires on the button press instead.

**Walking.** A forward push walks the way your character faces; turn
with the right stick. Tick "Head-based locomotion" in the Options
program to walk where you look instead. If you do, set "HUD position" to
"Follows your head" too: the HUD's position only updates 30 times a
second, so when it moves with your body it judders.

**Typing.** Whenever the game opens a text field (chat, login, character
name), a keyboard panel appears in front of you with a beam from each
controller. Point at a key and pull the trigger. A trigger pull with the
beam off the panel is Enter (right hand) or Backspace (left hand). A real
keyboard works too.

### Physical buttons

| Name above | Meta Quest Touch | HP Reverb G2 | HTC Vive Wands |
|---|---|---|---|
| Sticks | thumbsticks | thumbsticks | trackpads |
| Stick click | press the thumbstick | press the thumbstick | click the trackpad |
| Trigger | index trigger | index trigger | trigger |
| Grip | side grip | side grip | side grip buttons |
| X / Y | X / Y on the left controller | X / Y on the left | — |
| A / B | A / B on the right controller | A / B on the right | — |

The controllers' menu buttons are left free: on a Quest they belong to
the Steam and Meta overlays, which is why the game menu is a grip chord.

**HTC Vive Wands** have no A / B / X / Y, so quick chat, the game menu, Tab,
the chat line and hotkeys 1–4 have no button by default. Bind them to
other chords in the Options program, for example `menu = left_menu`,
`quick_chat = right_menu` and `tab = right_grip+right_menu`.

## Settings

`psobbvr_options.exe` in the game folder is the Options program. It has
three pages:

- **Controller bindings:** one entry per action; type or pick a chord
  (`left_grip+x`, `menu`, `right_stick_click`, ...). **Check** explains
  anything it would reject; **Save** writes `psobbvr.ini`, and a running
  game picks it up within a second. **Export** / **Import** save and
  load your layout.
- **Game options:** what the original game's `option.exe` set: graphics
  detail, fog, music and sound effects, "save ID and password". The mod
  keeps these in its own registry key (`HKEY_CURRENT_USER\Software\psobbvr\PSOBB`),
  so other PSO installs on your PC are untouched. Resolution and window
  mode are not offered because the mod controls them. Shadow detail is
  always low and Advanced Effect always off: the mod forces both, because
  they display incorrectly in VR.
- **VR settings:** resolution per eye (lower it if the headset
  stutters), eye height, HUD position, HUD and menu distance and size,
  terrain draw distance, turn speed, stick deadzone, hand and weapon
  size, the swing and casting options, vibration. Click a setting to
  read what it does.

Game options and VR settings take effect the next time the game starts.
Everything else, with a comment explaining each setting, is in
`psobbvr.ini`.

Which physical button counts as "A" or "grip" is decided by SteamVR's
own binding page: SteamVR → Settings → Controllers → Manage controller
bindings → this game.

## Troubleshooting

1. **A message box says VR did not start, and the game closes.** Read
   the box. "SteamVR not running": start SteamVR, wait for its status
   window, then launch the game (the game never starts SteamVR itself).
   Otherwise it is almost always one of: SteamVR is not the OpenXR
   runtime (the Meta app changed it back), SteamVR is older than 2.17,
   or the headset was not connected / Steam Link was not running.
2. **The headset shows the game but the controllers do nothing.** Open
   SteamVR's controller bindings for this game and check they are the
   game's own (Quest: "Oculus Touch").
3. **The camera is at the wrong height.** Stand normally, look forward
   and press right grip + left stick click to recenter.
4. **The view judders.** Check the OpenXR runtime (item 1); over Steam
   Link, try a lower streaming resolution, or lower the resolution per
   eye in the VR settings.
5. **Headset black, sound playing, and the monitor shows a small plain
   window.** On a laptop with two graphics chips the game is running on
   the integrated one; see [Requirements](#requirements).
6. **A box says "the game could not start".** The game's own graphics
   setup failed. Send the logs.
7. **Sound, graphics detail and "save ID and password"** are not in the
   in-game menus. Use the Game options page of the Options program. The
   original `option.exe` writes to a registry key the mod does not read,
   and it usually cannot save on modern PCs anyway.

## Reporting a bug

Open an [issue](../../issues) and attach the logs from the game folder:
`psobbvr-mod.log` and `psobbvr-vr.log` (the two launches before are
kept as `.1.log` and `.2.log`). Say what you did, your character's class,
the weapon, your headset and how it is connected. The logs start with
the build and the launch settings. `psobbvr-vr.log` is only written while
"Diagnostic logging" is ticked in the Options program (the default).

For menu problems, a screenshot of the monitor mirror helps most.

## Uninstall

Delete the mod's files from the game folder (`d3d8.dll`, `dinput8.dll`,
`openvr_api.dll`, `openxr_loader.dll`, `psobbvr.ini`,
`psobbvr_options.exe`, and the `psobbvr-*.log` files) and put back any
`d3d8.dll` / `dinput8.dll` you backed up (on PSOBB.io: its own
`d3d8.dll`). To remove the mod's settings too, delete the registry key
`HKEY_CURRENT_USER\Software\psobbvr`.

## How it works

The game is a 32-bit Direct3D 8 program that runs its logic at 30 frames
per second. The mod never speeds that up; SteamVR's reprojection keeps
head movement smooth between game frames.

- **`dinput8.dll`** loads first. It gives the mod its own copy of the
  game's registry settings, so other PSO installs are untouched, and
  forces the few settings VR needs.
- **`d3d8.dll`** is built on [d3d8to9](https://github.com/crosire/d3d8to9),
  which translates the game's Direct3D 8 calls to Direct3D 9. On top of
  that it renders the scene once per eye from your head's position, lays
  the HUD and menus out as panels in front of you, and hands each frame
  to SteamVR through Direct3D 11 and OpenXR (OpenVR is a fallback). It
  also places the camera at your character's eyes, turns controller
  buttons into the game's own key presses, drives walking and turning
  through the game's own movement code, and hooks game
  routines (with [MinHook](https://github.com/TsudaKageyu/minhook)) for
  swinging, aiming and hand placement.

Game addresses are hard-coded for the v12513 client; every one is named
in the source together with the game routine it comes from.

## Building from source

You need Visual Studio 2022 with the "Desktop development with C++"
workload, and CMake 3.21 or later. Everything else is in
`mod/third_party/`.

```
cmake -S mod -B mod/build -G "Visual Studio 17 2022" -A Win32
cmake --build mod/build --config Release
```

The build **must be 32-bit** (`-A Win32`): the game is 32-bit, and the
configure step stops on anything else. The results are
`mod/build/Release/dinput8.dll`, `mod/build/d3d8to9/Release/d3d8.dll`
and `mod/build/Release/psobbvr_options.exe`. Copy them into the game
folder together with `mod/third_party/openvr/bin/win32/openvr_api.dll`,
`mod/third_party/openxr_sdk/bin/win32/openxr_loader.dll` and
`client/psobbvr.ini`. The two DLLs always go in as a pair.

The logs open with the git revision the build was configured from, so
re-run the configure step after pulling.

## AI Disclaimer

AI was heavily leveraged during the creation of this mod.  I have a technical
background, albeit not one in software development, so while I provided the guidance,
direction, and testing, AI was used for all of the coding and reverse
engineering tasks.

## License

MIT; see [LICENSE](LICENSE). The bundled libraries (d3d8to9, MinHook,
OpenVR, the OpenXR SDK) keep their own licenses, reproduced in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

Phantasy Star Online is a trademark of SEGA. This project is not
affiliated with or endorsed by SEGA, and it includes no game files.
