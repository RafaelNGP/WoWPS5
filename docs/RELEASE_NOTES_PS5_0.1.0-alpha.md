# WoWPS5 PS5 0.1.0-alpha

The first public development version of the PS5 port of WoWPS 2.12,
**released as source code only**. **This is an alpha:** expect bugs, missing
features and save-format changes between builds. Back up your saves.

Build: [BUILD_PS5.md](BUILD_PS5.md). Installation: [INSTALL_PS5.md](INSTALL_PS5.md).
You need your own WotLK 3.3.5a (12340) client data; no Blizzard files are
included.

## What is in it

**Platform**
- Native PS5 homebrew application (title ID PPSA99809), started from the Home
  screen through ShadowMountPlus; no PC needed at runtime.
- Vulkan through Mesa RADV on the console GPU. 4K output with the world
  rendered at 1080p by default; the login screen and the test scenes run at
  60 FPS.
- Saves, settings and logs in `/data/wow_ps/` (sandbox elevation via elfldr).

**Standalone world** (single player and LAN, beyond WoWPS 2.12)
- Class abilities, from AzerothCore's rules, verified on the console by an
  automated self-test of 160+ abilities. Highlights:
  - Warrior: stances, Sunder Armor, Bloodrage, Slam, shouts, Retaliation,
    Recklessness, Spell Reflection.
  - Paladin: seals and judgements, blessings, Lay on Hands, Hand of Freedom,
    Hand of Salvation, Holy Wrath, Righteous Fury and Righteous Defense.
  - Hunter: ranged attack power, Steady Shot, Hunter's Mark, Feign Death,
    Scorpid Sting.
  - Rogue: Vanish, Feint, Fan of Knives, Dismantle, Cloak of Shadows.
  - Priest: Inner Fire, Psychic Scream, Fade, Shadowfiend, Prayer of Healing,
    Shadow Protection.
  - Shaman: totems, Bloodlust/Heroism, the four weapon imbues.
  - Mage: Frost/Ice/Mage Armor, Frost Nova, Cone of Cold.
  - Warlock: curses, Fear, Howl of Terror, Banish.
  - Druid: Faerie Fire, Innervate, Cyclone, Pounce, Maim, Tiger's Fury.
  - Death Knight: diseases, Death Grip, Raise Dead, Death Pact.
- Creature spells can now be partly resisted by the player's resistances.
- Bags and bank bags, mining, smelting, blacksmithing, cooking and first aid.
- Quests started, progressed and turned in at game objects (chests, posters):
  1341 of 1345 catalog quests reachable from a fresh character.
- Gamepad navigation of tabbed panels (L1/R1).

## Compatibility

- Save48 (reads Save1-48). LAN113: every LAN console needs this same build.
- WoWPS 2.12 (PS4) saves load, but this build's saves do not go back to 2.12.

## Known issues and limits

- The system on-screen keyboard (IME) is not available to homebrew; the game
  uses its own on-screen keyboard.
- Performance has not been profiled zone by zone; long sessions are not
  certified.
- The standalone world is partial: some class mechanics, talents and procs,
  dungeon and raid scripts, PvP and guilds are missing.
- External-realm play is inherited from WoWPS 2.12 and has not been re-verified
  on the PS5.

## Licensing

WoWPS5 is under the project's [LICENSE](../LICENSE) (MIT with an additional
non-commercial restriction). A PS5 binary built from it also contains
third-party runtime components with their own licenses:
- GPL-3.0-or-later: the application start-up/allocation runtime and sandbox
  elevation client from ps5-native-app-boilerplate (BlackBearReloaded), and
  the `libc.prx` module;
- MIT: Mesa RADV and its PS5 winsys.

GPL-3.0 does not allow the extra non-commercial restriction on a combined
work, so **no prebuilt binary is distributed** until that is resolved: either
those components are replaced with permissively licensed code or their authors
grant a linking exception. Building the app for your own console is not
affected.

The local-world data are conversions of AzerothCore tables (GPL-2.0; see
`assets/local_realm/NOTICE.txt`).
