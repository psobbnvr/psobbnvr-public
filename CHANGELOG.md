# Changelog

Tester-facing notes per release. Newest first.

## 0.1.0-beta-pub (2026-09-24)

- Built from the public source code. Nothing changes in play: the same mod as 0.1.0-beta.

## 0.1.0-beta (2026-09-23)

- psobbvr_options.exe is simpler: fewer, clearer VR settings, and the window now fits your screen at any Windows scaling and can be resized. VR settings take effect the next time the game starts.
- Logs are now psobbvr-vr.log and psobbvr-mod.log in the game folder, and the last three launches of each are kept (.1.log, .2.log). Send them with bug reports. "Diagnostic logging" in psobbvr_options.exe turns the VR log off.
- The debug panel on the monitor window (Insert) and the numpad teleport and tuning menus are no longer in player builds.

## 0.0.18-alpha (2026-09-22)

- Fixed an issue with the head and arm culling that was causing the player's mini-map icon to disappear.
- Added left hand casting for all non-hand targeted techs

## 0.0.17-alpha (2026-09-22)

- Other players and party NPCs no longer turn to face wherever you are facing; they keep their own facing while you strafe, backpedal or turn your head.
- Other players' mags no longer swing about as you turn your head; every mag now hovers where the game intends (the game aimed mags with a camera one frame newer than the pose, which a headset's turning made visible).
- Yes/No prompts (a guild card offered to you, "return to Pioneer 2?" after dying) now take the stick: up and down move between the answers.
- Other players' name, level and mode labels now sit centred over their heads, upright and facing you, and stay there while you look around or open the menu (they used to float nearby and drift with your head).
- New option "Head-based locomotion" in psobbvr_options.exe: a forward push walks wherever you are looking instead of the way your character faces. Your character turns to follow as you walk, the view holds still (only the right stick and recenter turn it), and the HUD comes round with you. Off by default.

## 0.0.16-alpha (2026-09-20)

- New binding "chat": right grip + X opens the chat line (it is Space in this game, not Enter - Enter only sends). Rebind it in psobbvr_options.exe.
- On-screen keyboard: whenever the game opens a text field (chat, login, character name, party name) a keyboard panel appears in front of you below eye level, with a beam from each controller. Point at a key and pull the trigger; a pull off the panel is Enter (right hand) or Backspace (left hand). On by default; turn it off in psobbvr_options.exe ("On-screen keyboard").
- The HUD now stays put while you look around: it sits in front of your character, turns with your stick turns (not your head, and not the turn an attack makes toward an enemy) and comes along as you walk, so its edges can be looked at and it shows which way you are facing. Options in psobbvr_options.exe ("HUD placement"): in front of your character (default), fixed in the room, or head-locked as before.
- Swing-timing indicator simplified: green = your next swing attacks (a fresh attack, the combo's next step, or a charged heavy/special once it's ready), blue = it lands the next strike of the attack in progress (daggers and the like), grey = not yet. The yellow charging state is gone, and the grey hexagon is now the same size as the others. Now ON by default; turn it off in psobbvr_options.exe ("Swing-timing indicator").
- Swing-timing indicator: the ring now lands on the hexagon on the frame it lights, runs on through the last combo step's recovery to the idle green, and no longer jumps on the first swing; the hexagon no longer lights early after a hit (sabers: the swing for the third combo step was accepted early and its hit landed late). It now sits level with the HP/TP bars.
- Daggers, double sabers and twin swords: a strike you do not swing for within a second now cancels the attack instead of landing on its own.
- Heavy and special attacks now land on the swing that releases them, like normal attacks, and are paced the same way afterwards; with daggers, double sabers and twin swords each strike of a heavy/special is a stab of your own.
- A charged heavy/special that is never swung is now cancelled after 3 seconds instead of firing on its own; release the chord to charge again.
- Swing-timing indicator: after a technique cast the ring no longer pops to full size and hangs there; it now shrinks through the cast to when your next swing counts.
- Swing-timing indicator: hidden whenever the game hides its command palette (menus, dialogs, the lobby, cutscenes), so it only shows while you can attack. Toggle in psobbvr_options.exe ("Swing-timing indicator only while the command palette shows").

## 0.0.15-alpha (2026-09-19)

- Melee hits now come at the game's own pace: the swing still lands the hit right away, but the time the skipped wind-up would have taken is waited out afterwards (your character holds still for it), so combos and bare-handed punches can't be swung faster than the original game. Slowed characters swing slower, as they should. Toggle in psobbvr_options.exe ("Melee hits paced like the original game").
- Daggers, double sabers and twin swords: every strike is a swing of your own. Each extra strike of a combo step waits for your next swing (up to a second, then lands on its own), and a swing after the step's last strike chains the next step.
- Daggers, knuckles and twin swords (one in each hand): a swing of either hand now strikes, so a combo can be stabbed hand over hand. The right trigger still arms the attack. Toggle in psobbvr_options.exe ("Twin weapons: the left hand swings too").
- New swing-timing indicator at the top of the view, built from the game's own hexagon art: the centre hexagon says when your next swing will do something (green = starts an attack, blue = chains the combo or lands the waiting strike, yellow = charging, a red flash = you swung too early and that combo can't chain), and a spinning ring shrinks onto it as the swing comes due. Toggle in psobbvr_options.exe ("Swing-timing indicator").

## 0.0.14-alpha (2026-09-15)

- Telepipe beam FX no longer move with head tilt.
- Removed camera swing from melee attacks; character moves in the direction of the attack but your view stays facing forward.
- Removed unnecessary keybind defaults.

## 0.0.13-alpha (2026-09-14)

- Resolved a few minor visual bugs that have been annoying me for a while...
--Grass FX in Forest and environmental fog in Central Control Area render correctly in world-space
--Photon bullet contrails now follow bullets correctly
--Enhanced photon bullet contrails by duplicating the contrail to create a cross-quad, making the contrail visible from all angles

## 0.0.12-alpha (2026-09-13)

- Implemented in-game weapon table classification to fix some melee weapons being detected as ranged, including unarmed combat. *
- Improved two-handed weapon detection as some weapons were incorrectly being classified as two-handed weapons which caused components to be split between both hands, such as claws and Slicer of Assassin.
- Fixed claw-class weapon hand positioning (still needs per-class adjustment).

\* Due to motion control, unarmed combat has a very fast hit cadence. This will be adjusted later on.

## 0.0.11-alpha (2026-09-13)

- Twin mechguns: the left gun now targets exactly what the right gun can, boxes included, through the game's own targeting. The reticle no longer turns blue near a hotbar technique or hops between grouped enemies.
- Twin mechguns: the left gun's aim cone now matches the right gun's.
- Twin mechguns: with only the left gun locked on, the right gun no longer dealt hidden full damage to that enemy.
- Twin mechguns: when both guns hit the same enemy, one damage number shows the total instead of two half numbers drawn on top of each other.
- Gun targeting measures its cone from the aiming hand instead of the character's centre, which fixes a sideways bias at close range.
- Standing on an item you can't pick up no longer paints the item cursor on the left gun's target.
- A controller that kept vibrating after a shot should now stop on its own.

## 0.0.10-alpha (2026-09-13)

- Updated technique targeting method per the game's code: Zonde, Grants, Gifoie, Rabarta, Razonde and the support techniques have omnidirectional targeting; Foie, Barta, Rafoie, Gibarta, Gizonde and Megid are aimed by the left hand, each keeping its vanilla targeting behavior. Also fixes issue where support techniques weren't hitting friendlies consistently.
- Fixed an issue with object culling where boxes that aren't in view can be targeted with techs but not destroyed.
- Added settings to psobbvr_options.exe to enable/disable swing to cast and swing to melee.

## 0.0.9-alpha (2026-09-13)

- Partisans, the parasol weapons and Glide Divine now sit on the controller at the right angle (they were turned 90 or 180 degrees).
- Setup: the SteamVR beta is no longer required. The current stable release (2.17.7) has the 32-bit OpenXR runtime, so you can switch back to the stable channel.

## 0.0.8-alpha (2026-09-13)

- Fixed bullet pathing to resolve an issue where untargeted bullets were shooting downward on Quest controllers.
- PSOBB.exe window no longer needs to be in focus for all controller inputs to work.

## 0.0.7-alpha (2026-09-12)

- Shot-type weapons fired without a lock now spread their five pellets around where the controller points instead of stacking them on one line

## 0.0.6-alpha (2026-09-12)

- Fixed the VR hands sometimes floating about a metre away from the controllers and swinging in arcs after arriving in Pioneer 2 (the hand capture used a frozen frame from the holstered weapon)
- Fixed a pulsing ghost of the menu frame appearing in the world with a menu open (a fourth menu drawer the 0.0.5 fix did not cover)

## 0.0.5-alpha (2026-09-11)

- Options program: Export and Import buttons on the Controller bindings page, so custom bindings survive an update (export before unzipping a new build, import afterwards, then Save)
- Fixed shot-type weapons (Shot, Spread, Cannon, Launcher, Arms, Crush Bullet, Meteor Smash, Final Impact, Belra Cannon, Baranz Launcher, Dark Meteor, S-rank Shot/Punch, TypeSH/SHOT, Rocket Punch) being handled as melee weapons instead of guns
- Fixed the menu selection bar and the equipped-item marker disappearing on Pioneer 2 after returning from Ragol

## 0.0.4-alpha (2026-09-11)

- Fixed geometry vanishing when standing on specific spots in Pioneer 2 (e.g. inside the Medical Center)
- Options program: added a "Capture menu diagnostics" button on the VR settings page for reporting menu bugs
- The launch log now records the game's graphics options

## 0.0.3-alpha (2026-09-10)

- Quick chat moved to A (right grip + A opens the Quick Menu); game menu moved to right grip + B, since the Quest's menu buttons are taken by the Steam and Meta overlays
- Both grips + a button is now always the bank-2 hotkey, regardless of other bindings
- Options program: a note pops up when setting the optional quick_menu binding, explaining it creates a second Quick Menu button

## 0.0.2-alpha (2026-09-10)

- Fixed an issue where CTRLBUF registry key was not created on launch if missing

## 0.0.1-alpha (2026-09-09)

- First closed-alpha build
