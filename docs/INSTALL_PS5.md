# Installing WoWPS5 on a PS5 (development builds)

WoWPS5 is a homebrew application. It needs a PS5 that can run homebrew, a
legal copy of the **World of Warcraft: Wrath of the Lich King 3.3.5a (build
12340)** client, and some patience: this is a development version.

## What the console needs

| Component | Why |
|-|-|
| A kernel exploit with **kstuff** | Runs homebrew applications. |
| **ShadowMountPlus** | Registers the WoWPS5 folder as a title on the Home screen. |
| **elfldr** (ELF loader, listening as usual) | Runs `sandbox-elevator.elf`, which lets the app write its saves and logs outside its sandbox. |

No PC, web server or other payload is needed while playing.

## Files

No prebuilt package is published: build the app folder yourself as described in
[BUILD_PS5.md](BUILD_PS5.md). The build produces one folder,
`build-ps5/pkg/PPSA99809/`:

```text
PPSA99809/
  eboot.bin              the game
  sce_sys/               title metadata and icon
  sce_module/libc.prx    runtime library
  sandbox-elevator.elf   elevation helper (run through elfldr)
  assets/ addons/ ...    local world data, shaders, interface add-ons
```

It contains **no Blizzard files**. You supply the client data yourself.

## Steps

1. Copy the `PPSA99809` folder to `/data/homebrew/PPSA99809/` on the console
   (FTP or USB).
2. Copy the `Data` directory of your WotLK 3.3.5a client to
   `/data/homebrew/PPSA99809/Data/`. Keep its layout: the `.MPQ` archives and
   the locale subdirectory (for example `Data/enUS/`). Expect about 17 GB.
3. Let ShadowMountPlus pick up the folder (or rescan), so that **WoWPS5**
   appears on the Home screen.
4. Make sure elfldr is running, then start **WoWPS5** from the Home screen.

The first start builds caches and can take noticeably longer than later ones.

## Saves, settings and logs

Everything the game writes lives under `/data/wow_ps/`:

| Path | Contents |
|-|-|
| `/data/wow_ps/saves/local_realm/` | Single-player / LAN realm saves |
| `/data/wow_ps/wowps/logs/` | `wowps.log` and friends: attach these to bug reports |
| `/data/wow_ps/boot_startup.log` | Start-up log |

**Back up `/data/wow_ps/saves/` before every update.** Development builds change
the save format (this alpha writes Save48 and reads 1-48). A save written by a
newer build may not open in an older one.

## Playing

- **Single Player** starts the standalone local world.
- **Host LAN** / **Join LAN** play together on the local network. Every console
  needs the same WoWPS5 build and content (this alpha speaks LAN113).
- **External Realm** connects to a compatible AzerothCore server; see
  [CONNECTING.md](CONNECTING.md).

Controller bindings are in the main [README](../README.md#controller).

## Updating

Back up `/data/wow_ps/saves/`, build the new version, then replace the contents
of `/data/homebrew/PPSA99809/` with the new app folder, keeping your `Data`
directory.

## Troubleshooting

- **The game does not appear on the Home screen:** check that the folder is
  exactly `/data/homebrew/PPSA99809/` and contains `eboot.bin` and `sce_sys/`.
- **It closes right after starting:** check that `Data/` holds the 3.3.5a MPQs,
  and read `/data/wow_ps/boot_startup.log`.
- **Saves are in the app folder instead of `/data/wow_ps/`:** elfldr was not
  running when the game started, so the sandbox elevation did not happen and the
  game wrote to `/data/homebrew/PPSA99809/` instead. Saves there are lost if
  you replace the folder on update. Start elfldr and restart the game: it
  copies them to `/data/wow_ps/` on the first elevated start.
