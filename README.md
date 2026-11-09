# Lingmo Store

The app store of the ArchLingmo desktop ("Loja de aplicativos"): one place to find,
install and remove applications without a terminal. Flathub first, the Arch
repositories second, the AUR not at all.

- **Catalog** — AppStream metadata read with libappstream: the Arch repositories'
  data (`archlinux-appstream-data`, `/usr/share/swcatalog`) and Flathub's
  (`/var/lib/flatpak/appstream`, downloaded by the store when missing or older than a
  day). An app found in both is shown once, with both sources.
- **Home** — rotating banner, popular apps and categories. The curated lists live in
  [`data/featured.json`](data/featured.json).
- **Search** — instant, accent-insensitive, understands everyday Portuguese words
  ("editor de fotos", "planilha", "jogos").
- **App page** — screenshots, description, developer (with Flathub's *verified*
  badge), license, size and version, and the choice between *Versão do Arch* and
  *Flathub* when both exist. The Arch package is recommended when it's in the official
  repositories and not older than Flathub's; otherwise Flathub.
- **Permissions before installing** — for Flathub apps, the sandbox permissions of the
  app's metadata (`flatpak info --show-metadata` when installed, `flatpak remote-info
  --show-metadata flathub <ref>` otherwise, Flathub's `/api/v2/summary/<id>` when the
  remote isn't set up) in plain words with a risk level: running commands outside the
  sandbox (`--talk-name=org.freedesktop.Flatpak`), the whole D-Bus session, all files,
  camera and devices, microphone, SSH keys, saved passwords, network…; a summary sits
  under the Install button. Arch packages say clearly that they run without a sandbox
  and which repository (official or not) they come from. Cached for three days.
- **Ratings and reviews** — from [ODRS](https://odrs.gnome.org), the service GNOME
  Software and KDE Discover use: average and count on every app card and page (one
  ~2 MB download a day, matched by AppStream id, `.desktop` id, Flathub id and the ids
  an app provides), the newest reviews on the app page (in the user's language, else
  English; cached for six hours) and, for installed apps, *Escrever avaliação*. The
  reviewer is identified, like in GNOME Software, by a salted SHA-1 of the login and
  `/etc/machine-id`. Offline, the last answers are shown; nothing waits on the network.
- **Curated apps and collections** — apps whose repository has no AppStream data
  (`curated` in `featured.json`: name, texts in English and pt-BR, icon, screenshots,
  categories, the file types they open and what they replace) are listed once pacman's
  sync db has their package, or when their `repo` (a separate ArchLingmo repository)
  isn't in `pacman.conf` yet: then the app page offers *Ativar e instalar*, asks
  *Os apps Craft vêm de um repositório separado do ArchLingmo. Ativar?* and runs
  `enable-repo` with the packages in one authorization. The first are the storytold
  Craft apps (WordCraft, GridCraft, PhotoCraft, CADCraft…), the developers' own Arch
  packages mirrored into `[archlingmo-craft]` (GitHub release assets; new ISOs list it
  already) after checking checksums and contents
  (`packaging/thirdparty/craft-apps` and `scripts/publish-craft.sh` in the workspace). `experimental` apps carry an
  *Experimental* badge and a *pre-alpha: work on copies of your files* banner; the
  file types they open are shown as information, never made default handlers.
  `collections` adds a featured banner to the home page with a one-click `bundle`
  (*Instalar suíte de escritório*: WordCraft, GridCraft and DeckCraft in one pacman
  transaction).
- **Installed** — what was installed from the store, with Open and Remove. Updates are
  left to Lingmo Updater (linked from the sidebar).
- **Games** — *Preparar para jogos* enables `[multilib]`, installs Steam, the 32-bit
  Vulkan driver for the GPUs `lspci` reports, GameMode and optionally MangoHud, in one
  transaction, and adds the user to the `gamemode` group (GameMode's polkit rule needs
  it); *Proton para todos os jogos* sets Proton Experimental for "all other titles"
  (`CompatToolMapping` → `0` in Steam's `config/config.vdf`, only while Steam is closed,
  with a backup); *Modo jogo* explains `gamemoderun %command%`; *Seu jogo funciona?*
  combines ProtonDB and Are We Anti-Cheat Yet into a verdict, and popular online games
  whose anti-cheat blocks Linux (`antiCheatWatch` in `featured.json`, checked against
  Are We Anti-Cheat Yet) are listed as a warning.
- **Native games** — classic games rebuilt as native programs by decompilation and
  recompilation projects (`nativeGames` in `featured.json`). The store never ships or
  links to game data: entries that need the user's own copy say so and explain how.

- **Windows games (Lutris)** — search results get a section *Jogos de Windows (Lutris)*:
  Windows games from [lutris.net](https://lutris.net) that have an installer
  (`/api/games?search=<text>&with-installers=1`, ranked by the store since the API
  answers in alphabetical order; asked 600 ms after the user stops typing, at most one
  request a second, backing off after `429`). A game's page
  (`/api/games/<slug>`: genres, description and installers) offers the published
  *wine* installers (the one for the user's region preselected, e.g. *Standard (BR)*),
  a *Roda com Wine* badge, Are We Anti-Cheat Yet's verdict when it blocks Linux, and a
  warning for online games (MMORPG, MOBA…) that may refuse Wine. Answers and covers are
  cached in `$XDG_CACHE_HOME/lingmo-store/lutris` (searches and pages a day, covers 30
  days; stale ones are shown offline). Covers only from `https://lutris.net`.
  Installing uses the **Flathub build of Lutris** (`net.lutris.Lutris`), never a
  native `lutris` or its data (`~/.local/share/lutris`, `~/.config/lutris`): when it's
  missing it is installed first through the normal Flathub path (same consent), its
  sandbox permissions shown on the game's page. Then `flatpak run net.lutris.Lutris
  lutris:install/<installer-slug>` opens Lutris' own installer window; the store shows
  *Instalando pelo Lutris…* and watches that Lutris' library
  (`~/.var/app/net.lutris.Lutris/data/lutris/pga.db`, read only) until the game is
  installed (or Lutris is closed), then offers *Jogar*: `flatpak run net.lutris.Lutris
  lutris:rungame/<slug>`. The first time, the flatpak's own `system.yml` gets
  `game_path: ~/Games/Lutris`, so its games never land on a native Lutris'
  `~/Games/<game>`.
  As published on Flathub, Lutris may read and change the whole home folder and start
  programs outside its sandbox (`--talk-name=org.freedesktop.Flatpak`), besides
  `devices=all` and `/media`. *Isolamento extra para o Lutris* (on by default) applies
  `flatpak override --user --nofilesystem=home --nofilesystem=~/Games
  --nofilesystem=/media --nofilesystem=/run/media
  --nofilesystem=~/.var/app/com.valvesoftware.Steam --filesystem=~/Games/Lutris:create
  --filesystem=xdg-download:ro --no-talk-name=org.freedesktop.Flatpak net.lutris.Lutris`
  before every install and launch; turning it off runs `flatpak override --user --reset
  net.lutris.Lutris` (only if the store had applied it).

## How software is installed

- **Flathub apps**: libflatpak transactions on the **system** installation. The apps
  are shared by every user and appear in the launcher, and flatpak's own polkit rules
  let administrators (group `wheel`) install without typing a password. The `flathub`
  remote is added (`flatpak remote-add --system`, with the `.flatpakrepo` embedded in
  the binary) only after the user agrees. `LINGMO_STORE_FLATPAK_INSTALLATION=user`
  switches to the per-user installation.
- **Arch packages**: `pkexec /usr/lib/lingmo-store/lingmo-store-helper`, a small
  shell script and the only thing that runs as root (polkit action
  `com.lingmo.store.manage-packages`, `auth_admin_keep`):

  | command                  | runs                                                |
  |--------------------------|-----------------------------------------------------|
  | `install <pkg>…`         | `pacman -S --needed --noconfirm`                    |
  | `remove <pkg>…`          | `pacman -Rns --noconfirm` (refuses essential ones)  |
  | `enable-multilib`        | uncomments `[multilib]` in `/etc/pacman.conf` after a timestamped backup, validated with `pacman-conf` |
  | `enable-repo archlingmo-craft [<pkg>…]` | adds the fixed `[archlingmo-craft]` section (GitHub release assets of `devalexandre/archlingmo-craft`) to `/etc/pacman.conf` after a timestamped backup, validated with `pacman-conf` (nothing is done when it's there), syncs that repository alone (`pacman -Sy` with a throwaway config: the system's `[options]` plus that section, so the next `-S` is no partial upgrade), then `pacman -S --needed --noconfirm archlingmo-craft/<pkg>…`. Any other repository name is refused: the sections are hard-coded |
  | `prepare-gaming <pkg>…`  | `enable-multilib` if needed, then `pacman -Syu --needed --noconfirm` (a sync plus install in one transaction, never a partial upgrade); with `gamemode`, `gpasswd -a <user of PKEXEC_UID> gamemode` |

  pacman's output is turned into a progress bar and, on failure, into a sentence
  ("Sem internet…", "Outro programa está instalando, tente de novo em instantes", …).

## Command line and D-Bus

```
lingmo-store                          open the store
lingmo-store --app <id>               open an app's page
lingmo-store --search "<text>"        open search results
lingmo-store --games                  open the games page
lingmo-store appstream://<id>         same as --app
lingmo-store <file>.flatpakref        page of the app in the flatpakref
```

`<id>` may be an AppStream id (with or without `.desktop`, any case), a Flathub id,
a flatpak ref (`app/<id>/<arch>/<branch>`) or an Arch package name
(`--app com.google.Chrome`, `--app org.libreoffice.LibreOffice`, `--app firefox`).
Unknown ids fall back to a search.

Only one instance runs per session. It owns `com.lingmo.Store` on the session bus and
exports `/Store` with interface `com.lingmo.Store`:

| method                        | effect                                         |
|-------------------------------|------------------------------------------------|
| `activate(as arguments)`      | same as the command line (a second instance calls this with its own arguments) |
| `openApp(s id)`               | `--app id`                                     |
| `search(s text)`              | `--search text`                                |
| `openGames()`                 | `--games`                                      |
| `show()`                      | raise the window                               |

## Build

```sh
cmake -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr
ninja -C build
DESTDIR=/tmp/stage ninja -C build install
```

Build dependencies: `qt6-base qt6-declarative qt6-tools qt6-5compat kwindowsystem
appstream flatpak pacman libjxl cmake ninja` (libflatpak, libalpm and libjxl are
optional at build time; without them Flathub, Arch packages or Arch's JPEG XL icons
are unavailable).

Runtime: `lingmoui`, `appstream`, `archlinux-appstream-data`, `flatpak`, `pacman`,
`polkit` with an agent, `pciutils` (GPU detection); optional `kimageformats`.

## Testing without touching the system

`LINGMO_STORE_FAKE` makes every change a simulation: the helper runs from the
resources in its fake mode (never as root) and flatpak transactions are simulated.
Values: `1`/`ok`, `fail`, `offline`, `lock`, `cancel`.

| variable                               | effect                                          |
|----------------------------------------|-------------------------------------------------|
| `LINGMO_STORE_PACMAN_CONF=<copy>`      | fake mode: read and edit this copy of pacman.conf |
| `LINGMO_STORE_FLATPAK_INSTALLATION=user` | use `$XDG_DATA_HOME/flatpak`                  |
| `LINGMO_STORE_FAKE_GPU=nvidia\|amd\|intel` | skip lspci                                  |
| `LINGMO_STORE_FAKE_NATIVE=soh,zelda3`  | fake mode: pretend `[archlingmo]` has these native games |
| `LINGMO_STORE_FAKE_ARCHLINGMO=all\|<pkg>,…` | fake mode: pretend their repository has these curated apps (`all`: every one); one with a `repo` only once the `LINGMO_STORE_PACMAN_CONF` copy lists it (fake `enable-repo` adds it there) |
| `LINGMO_STORE_FAKE_DELAY`              | seconds between fake pacman steps               |
| `LINGMO_STORE_ODRS_URL=<url>`          | another ODRS server (e.g. a local stub); in fake mode without it, submitting a review is only logged |
| `LINGMO_STORE_FLATHUB_API=<url>`       | another Flathub API (permissions fallback)      |
| `LINGMO_STORE_STEAM_DIR=<dir>`         | the Steam folder whose `config/config.vdf` is edited |
| `LINGMO_STORE_FAKE_STEAM_RUNNING=1\|0` | skip looking for a running Steam                |
| `LINGMO_STORE_LUTRIS_API=<url>`        | another lutris.net (e.g. a local stub)          |

```sh
cp /etc/pacman.conf /tmp/pacman.conf
LINGMO_STORE_FAKE=1 LINGMO_STORE_PACMAN_CONF=/tmp/pacman.conf lingmo-store --games
```

In fake mode the Lutris commands are only logged (`[fake] would run flatpak …`) and the
game counts as installed a few seconds later.

Unit tests (recorded lutris.net answers in `tests/fixtures/lutris`, a fake command
runner, a scratch HOME; no network, no flatpak):

```sh
cmake -B build -DBUILD_TESTING=ON && ninja -C build && (cd build && ctest)
```

## License

GPL-3.0-or-later.
