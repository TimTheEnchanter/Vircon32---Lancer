# Changelog

## 1.0 (2026-11-06)

First release.

### Game
- A first-person trench-run shooter drawn in glowing vector lines. Each wave attacks an enemy dreadnought in three phases:
  - **Interceptors:** swooping fighters that throw shootable fireballs; holding the crosshair off-centre turns the view after them.
  - **The hull:** a low pass shooting gun towers, with a 5,000 bonus for clearing every one.
  - **The trench:** steer around its cross-section past wall turrets, catwalks and laser gates, and hit the reactor vent at the end. Miss and you go round again.
- Bombers (from wave 2) fire spreads of three fireballs and take three hits. Homing mines arrive from wave 3.
- Shield generators on the hull: each one destroyed removes two turrets from the trench.
- Three dreadnought classes in turn, each with its own silhouette, colour and mix of defences: Lance, Carrier and Bastion.
- Each wave is faster and fiercer than the last, with every speed capped.
- Six shields (eight on Easy, four on Hard); one comes back with each dreadnought destroyed.

### Feel
- A cockpit that sways against your aim, with gun housings whose muzzles flare as you fire.
- The canopy cracks where you're hit and seals again as shields return.
- You drop out of a jump at each wave, and the view dives onto the hull and into the trench.
- The dreadnought's end: you pull up out of the trench, watch it burn and break up, then a blast flings wreckage and a shockwave across the screen.

### Modes, records and secrets
- A Pilot Record of eight feats, two of them hidden until found.
- Three modes unlocked by feats, each keeping a best score with initials:
  - Trench Run: trench after trench;
  - Time Attack: three minutes, where hits cost time and vents add it;
  - Last Pilot: one shield and double points.
- Four display colours unlocked by feats: Amber, Green, White and Spectrum.
- A secret stage inside the dreadnought (see the spoiler section of the README).

### Sound
- Seven chiptune tracks: title, interceptors, hull, trench, the core, a victory fanfare and a game over sting.
- Thirteen sound effects.
- The music ducks while paused, and music and effects volumes are separate.

### Menus and saving
- Pause menu (Resume, Options, Quit to title), with quitting needing a second press.
- Options:
  - music and effects volume;
  - Easy/Normal/Hard;
  - screen shake and flashes;
  - display colour;
  - reset high scores.
- A top-5 high score table with initials, tagged E or H for Easy or Hard.
- An attract mode cycling the logo, high scores and Pilot Record, then a replay of the best recorded game.
- The memory card keeps:
  - the high scores and your options;
  - the Pilot Record and each mode's best;
  - a replay of your best game.

  Cards written by every earlier version load, keeping their scores and options.

### Release testing
- No dropped frames and no halts across:
  - immortal autopilot runs: the Standard game to wave 10, Trench Run to wave 25, and the secret stage found every wave to wave 7;
  - 50 minutes of random input (with everything unlocked and a fresh card, with no card, and with a card), which wandered through every menu and mode;
  - every screen and title page.
- Audio peaks at 55% with no glitches.
- The built-in demo replays the game it was recorded from frame for frame.

## Development versions

- **0.6:** the Pilot Record, modes, displays and the secret core stage.
- **0.5:** music.
- **0.4:** game feel: cockpit, cracked glass, transitions, the dreadnought's end.
- **0.3:** bombers, mines, shield generators, laser gates and the three dreadnought classes.
- **0.2:** pause menu, options, difficulty, high score table, attract demo and memory card.
- **0.1:** the playable core: interceptors, hull and trench.
