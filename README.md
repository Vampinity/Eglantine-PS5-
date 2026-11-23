> [!NOTE]
> This was my first test fork and I don't update it anymore. The old test builds are no longer here.
> Work continues at [Vampinity/GreenHatBoy](https://github.com/Vampinity/GreenHatBoy). That's where to get the alpha.
>
> Everything below is viejunerd's README as it was on 4 Nov.

<p align="center">
  <img src="sce_sys/icon0.png" width="128" alt="EglantinePS5 icon">
</p>

<h1 align="center">EglantinePS5 · NUP build</h1>

<p align="center">
  Run Nintendo Switch 1 and <strong>Switch 2</strong> games on a jailbroken PlayStation 5, from folders or single <code>.nup</code> files.<br>
  Ejecuta juegos de Nintendo Switch 1 y <strong>Switch 2</strong> en una PS5 con jailbreak, desde carpetas o ficheros <code>.nup</code>.
</p>

<p align="center">
  <a href="#english">English</a> · <a href="#español">Español</a> · <a href="../../releases">Releases / Descargas</a>
</p>

> [!WARNING]
> Unofficial, experimental software. It is not affiliated with Sony, Nintendo, the Eglantine team or the Eden team. It contains **no** games, keys or firmware: you must bring your own legally dumped files.
> Software no oficial y experimental. Sin relación con Sony, Nintendo ni los equipos de Eglantine o Eden. **No** incluye juegos, claves ni firmware: tienes que aportar los tuyos, volcados legalmente.

---

# English

## What this is

A PS5 homebrew build that stacks three layers, always updated in this order:

| # | Layer | Source | What we do with it |
|---|---|---|---|
| 1 | **Emulator core** | [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine), branch `nx2` (a Switch 2 capable fork of Eden) | Pinned as the `Eglantine/` submodule at its **latest** `nx2` commit. Never forked: our changes are small patches. |
| 2 | **PS5 frontend** | [heydemoura/EglantinePS5](https://github.com/heydemoura/EglantinePS5) (launcher, Vulkan/RADV port, packaging), built on BlackBearReloaded's ProsperoEden | This repository's full history is that project's; we rebase on its `main` and keep our changes on top. |
| 3 | **NUP support** | `.nup` is Eglantine's single-file package (base game + update + DLC) | The library lists `.nup` files, runs them, and can **pack a game folder into a NUP** on the console. |

Current core pin: **Eglantine `3081c8e`** (`nx2`, `build-219`, 2026-11-04).

## Status (honest version)

| Item | State |
|---|---|
| Console tested | PS5 firmware **13.60**, jailbroken, launched with ShadowMountPlus. |
| Donkey Kong Bananza (Switch 2, `.nup` with update 3.2.0 + DLC) | Runs on the current build (core `3081c8e`) with its update and DLC: **Docked, 1x resolution, 10-25 fps depending on the zone, about 15 in large open areas, 30 fps in cutscenes**. A new zone can show purple the first time it is entered; in one test the game's own "reset terrain" option cleared it. |
| Handheld mode, 0.5x resolution | Not yet confirmed. Earlier failures were caused by a stale shader cache (see below), so they have to be retested. |
| Update and DLC inside a NUP | Both load, and the Library shows them. Checked with Bananza: the game mounts its DLC (log: `NUP add-on 040074A01BF13001 mounted`) and its DLC menu shows 3.2.0 and DLC 1.0.0. The DLC's levels have not been played through yet. |
| Super Mario Bros. Wonder NUP ("Switch 2 Edition + DLC") | The tested file runs as the Switch 1 game with update 1.2.0, and its DLC stays unused: the executables in that file are the Switch 1 build, and the DLC holds Switch 2 data. The Library still lists it. |
| Other Switch 2 titles | Some fail early. Nothing is promised beyond what is listed here. |
| Frame generation (Lossless Scaling) | Compiled in, but pointless at a 20 fps base. |

### Known issue that is fixed here: "stuck at 98% on every launch after the first"

On the console, the Vulkan driver hung a Switch 2 game when it started from the pipeline cache saved by an earlier run. This build neither loads nor saves that global cache (`tools/eglantine-patches/0010-…`); per-game caches still work. If you ever see a hang at 98%, delete `/data/eglantineps5/user/cache/shader/` and tell us (attach `eden_log.txt`).

## Install

1. Download the release ZIP from **[Releases](../../releases)** and extract it. It contains one folder, `PPSA98515`.
2. Copy that folder to `/data/homebrew/PPSA98515` on the PS5 (or another folder ShadowMountPlus scans, e.g. `/mnt/usb0/homebrew/PPSA98515`). Use [ShadowMountPlus 1.7beta4](https://github.com/drakmor/ShadowMountPlus/releases/tag/1.7beta4) or newer.
3. Put your own **keys, firmware and games** in a *game files folder* (layout and options: [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md#install)). Games go in `roms/`: `.nsp`, `.xci`, **`.nup`**, or an extracted folder.
4. Start **EglantinePS5** from the PS5 home screen.

> [!IMPORTANT]
> This build uses the same title ID as the upstream app (`PPSA98515`), so it **replaces** it. From `v1.206.001` on, the in-app updater checks this repository: when a new release is out it asks first, then downloads it, checks its signature and hash, and replaces the app's files in the folder it is installed in. Earlier builds still ask prosperostore.net, so install `v1.206.001` by hand.

## Version numbers

`v1.206.001` is `1`, Eglantine's `build-206`, and this project's own count, which starts again at `001` with each new core build. The console's content version is the same thing as `01.206.001`.

## Using NUP files

- **Play:** drop a `.nup` in `roms/`. It shows in the Library like any game. A NUP holds the base game, its updates and DLC in one file.
- **Make one from a folder:** for a game kept as an extracted folder, open its settings in the Library (Triangle) and use the **NUP** row. Press twice to confirm; packing runs in the background and the `.nup` appears next to the folder when done. Updates and DLC are included.
- The format and the packer are part of the core (`src/core/file_sys/nup_container.*`, `nup_writer.*`).

## What this repository changes

All changes to the emulator core are plain patches in [`tools/eglantine-patches/`](tools/eglantine-patches), applied automatically at build time; if one stops applying, the build says so.

| Patch | Purpose |
|---|---|
| 0001 | Extracted dumps with `base/` + `updates/<version>/` folders |
| 0002 | Ampere U16 float-to-int conversion source modifiers (shader recompiler) |
| 0004 | NUP packer lists folders through `ListDirectory` (native directory reader on the console) |
| 0005 | Log why an NSO or NUP fails to load |
| 0008 | `nx2:vi2` command 201 stub (Switch 2) |
| 0009 | Guest-thread watchdog written to `eden_log.txt` (diagnosis of hangs) |
| 0010 | Do not load/save the global Vulkan static pipeline cache |
| 0011 | The DLC inside a NUP reaches the game through a content provider (`aoc:u`, data storage by ID) |
| 0012 | Donkey Kong Bananza: command lists that are empty when submitted are read when parsed |
| 0014 | Fence downloads: their staging buffers are reused as soon as the fence passes (Bananza ran out of graphics memory) |
| 0015 | Donkey Kong Bananza's upscaler slots (from [Orchid](https://github.com/MythosOfs/Orchid) `48d369310`, MythosOfs) |
| 0016 | Draws that sample their own colour target read a copy (Orchid `a49c81063`, MythosOfs) |
| 0017 | Page checks skipped for buffer ranges with no new CPU writes (Orchid `a3be6e11b`, `5ffe9eb53`, `4476c8671`, `2e0b577bb`; from monteger8/Eglantine-DKB, Claude for adamd) |
| 0018 | GPU split: wakes a back thread that stayed parked (Orchid `1e4e0e82b`, from godpow/Eglantine, godpow) |

Number 0006 is intentionally missing: 32 storage buffers per stage is now in the core itself. Frontend changes (NUP in the library, "pack to NUP", `.gitmodules` pointing at Djihads80's core, Vulkan-port anchors for the newest core, per-line log flushing) are ordinary commits on top of the upstream frontend.

Logs on the console: `/data/eglantineps5/user/log/eden_log.txt` (and `.old.txt`), `/data/eglantineps5/logs/`. Settings: `/data/eglantineps5/config/eglantineps5.json`.

## Updating and building

Update order, every time: **(1)** move the `Eglantine/` submodule to the newest Djihads80 `nx2` commit and re-apply the patches (`tools/update-eglantine.sh`; needs bash ≥ 4 because `apply-eglantine-patches.sh` uses `mapfile`, so on macOS use Homebrew bash or a Linux container); **(2)** merge the newest heydemoura/EglantinePS5 `main`; **(3)** check NUP loading and packing; **(4)** build, test on the console, publish.

Building follows [docs/BUILDING.md](docs/BUILDING.md) (`make package`, Linux x86-64 toolchain). On Apple Silicon we build in an `linux/amd64` Docker container (about 45 minutes for a clean build). Releases are the folder `PPSA98515/` zipped; the release notes list the core commit and the `eboot.bin` SHA-256.

## Credits and license

GPL-3.0-or-later (see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The complete source of every release is this repository plus the pinned `Eglantine/` submodule commit.

Thanks to: **Djihads80 and the Eglantine contributors**; the **Eden** and yuzu projects; **heydemoura** (EglantinePS5 frontend and PS5 port work); **BlackBearReloaded** (ProsperoEden, ps5-opengl, PS5 native app boilerplate); **Mihawk** (PS5 Mesa/RADV and PS5 Vulkan); **mpereiraesaa** (ps5-vulkan); **drakmor** (ShadowMountPlus). The detailed credit list is in [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md).

---

# Español

## Qué es

Una build homebrew para PS5 que apila tres capas, siempre actualizadas en este orden:

| # | Capa | Origen | Qué hacemos |
|---|---|---|---|
| 1 | **Núcleo del emulador** | [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine), rama `nx2` (fork de Eden con soporte Switch 2) | Submódulo `Eglantine/` fijado al **último** commit de `nx2`. No lo forkeamos: nuestros cambios son parches pequeños. |
| 2 | **Frontend de PS5** | [heydemoura/EglantinePS5](https://github.com/heydemoura/EglantinePS5) (lanzador, port de Vulkan/RADV, empaquetado), basado en ProsperoEden de BlackBearReloaded | El historial de este repo es el de ese proyecto; rebaseamos sobre su `main` y mantenemos nuestros cambios encima. |
| 3 | **Soporte NUP** | `.nup` es el paquete de un solo fichero de Eglantine (juego base + update + DLC) | La biblioteca lista y ejecuta `.nup` y puede **empaquetar una carpeta de juego en un NUP** en la propia consola. |

Core actual: **Eglantine `3081c8e`** (`nx2`, `build-219`, 4-nov-2026).

## Estado (versión honesta)

| Punto | Estado |
|---|---|
| Probado en consola | PS5 con firmware **13.60**, con jailbreak y ShadowMountPlus. |
| Donkey Kong Bananza (Switch 2, `.nup` con update 3.2.0 + DLC) | Funciona con la build actual (core `3081c8e`) con su update y su DLC: **modo TV, resolución 1x, 10-25 fps según la zona, unos 15 en zonas abiertas grandes, 30 fps en cinemáticas**. Una zona nueva puede verse morada la primera vez que se entra; en una prueba, la opción del juego «resetear terreno» lo arregló. |
| Modo portátil, resolución 0,5x | Sin confirmar. Los fallos anteriores venían de una caché de shaders antigua (ver abajo), así que hay que repetir la prueba. |
| Update y DLC dentro de un NUP | Se cargan los dos y la biblioteca los muestra. Comprobado con Bananza: el juego monta su DLC (log: `NUP add-on 040074A01BF13001 mounted`) y su menú de DLC muestra 3.2.0 y DLC 1.0.0. Los niveles del DLC aún no se han jugado a fondo. |
| NUP de Super Mario Bros. Wonder («Switch 2 Edition + DLC») | El archivo probado se ejecuta como el juego de Switch 1 con la update 1.2.0, y su DLC queda sin usar: los ejecutables de ese archivo son la versión de Switch 1 y el DLC contiene datos de Switch 2. La biblioteca lo sigue listando. |
| Otros juegos de Switch 2 | Algunos fallan pronto. No prometemos nada más allá de lo que aparece aquí. |
| Generación de fotogramas (Lossless Scaling) | Incluida, pero sin sentido con una base de 20 fps. |

### Problema conocido y arreglado: «se queda al 98% en cada arranque después del primero»

En la consola, el driver de Vulkan colgaba un juego de Switch 2 cuando arrancaba desde la caché de pipelines guardada por una ejecución anterior. Esta build ni carga ni guarda esa caché global (`tools/eglantine-patches/0010-…`); las cachés por juego siguen funcionando. Si alguna vez ves un cuelgue al 98%, borra `/data/eglantineps5/user/cache/shader/` y avísanos (adjunta `eden_log.txt`).

## Instalación

1. Descarga el ZIP desde **[Releases](../../releases)** y descomprímelo. Contiene una carpeta, `PPSA98515`.
2. Copia esa carpeta a `/data/homebrew/PPSA98515` en la PS5 (o a otra carpeta que escanee ShadowMountPlus, p. ej. `/mnt/usb0/homebrew/PPSA98515`). Usa [ShadowMountPlus 1.7beta4](https://github.com/drakmor/ShadowMountPlus/releases/tag/1.7beta4) o posterior.
3. Pon tus **claves, firmware y juegos** en una *carpeta de archivos de juego* (estructura y opciones: [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md#install), en inglés). Los juegos van en `roms/`: `.nsp`, `.xci`, **`.nup`** o una carpeta extraída.
4. Abre **EglantinePS5** desde la pantalla de inicio de la PS5.

> [!IMPORTANT]
> Esta build usa el mismo ID de título que la app original (`PPSA98515`), así que la **sustituye**. Desde `v1.206.001`, el actualizador integrado consulta este repositorio: cuando sale una release nueva pregunta primero, la descarga, comprueba su firma y su hash, y sustituye los archivos de la app en la carpeta donde esté instalada. Las builds anteriores siguen consultando prosperostore.net, así que instala `v1.206.001` a mano.

## Números de versión

`v1.206.001` es `1`, la `build-206` de Eglantine y un contador propio de este proyecto, que vuelve a `001` con cada build nueva del core. En la consola, la versión de contenido es lo mismo: `01.206.001`.

## Usar ficheros NUP

- **Jugar:** deja un `.nup` en `roms/`. Aparece en la biblioteca como cualquier juego. Un NUP contiene el juego base, sus updates y DLC en un fichero.
- **Crear uno desde una carpeta:** para un juego guardado como carpeta extraída, abre sus ajustes en la biblioteca (Triángulo) y usa la fila **NUP**. Pulsa dos veces para confirmar; el empaquetado se hace en segundo plano y el `.nup` aparece junto a la carpeta al terminar. Incluye updates y DLC.
- El formato y el empaquetador forman parte del core (`src/core/file_sys/nup_container.*`, `nup_writer.*`).

## Qué cambia este repositorio

Todos los cambios al core del emulador son parches simples en [`tools/eglantine-patches/`](tools/eglantine-patches), que se aplican solos al compilar; si uno deja de aplicar, la build lo dice. La tabla de parches está en la sección en inglés. Falta el 0006 a propósito: las 32 storage buffers por etapa ya vienen en el core. Los cambios del frontend (NUP en la biblioteca, «empaquetar a NUP», `.gitmodules` apuntando al core de Djihads80, anclajes del port de Vulkan para el core más nuevo, volcado del log línea a línea) son commits normales encima del frontend original.

Logs en la consola: `/data/eglantineps5/user/log/eden_log.txt` (y `.old.txt`), `/data/eglantineps5/logs/`. Ajustes: `/data/eglantineps5/config/eglantineps5.json`.

## Actualizar y compilar

Orden de actualización, siempre: **(1)** subir el submódulo `Eglantine/` al último commit de `nx2` de Djihads80 y reaplicar los parches (`tools/update-eglantine.sh`; necesita bash ≥ 4 porque `apply-eglantine-patches.sh` usa `mapfile`, así que en macOS usa bash de Homebrew o un contenedor Linux); **(2)** fusionar el último `main` de heydemoura/EglantinePS5; **(3)** comprobar la carga y el empaquetado de NUP; **(4)** compilar, probar en consola y publicar.

La compilación sigue [docs/BUILDING.md](docs/BUILDING.md) (`make package`, toolchain Linux x86-64). En Apple Silicon compilamos dentro de un contenedor Docker `linux/amd64` (unos 45 minutos una build limpia). Las releases son la carpeta `PPSA98515/` comprimida; las notas indican el commit del core y el SHA-256 del `eboot.bin`.

## Créditos y licencia

GPL-3.0-or-later (ver [LICENSE](LICENSE) y [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). El código fuente completo de cada release es este repositorio más el commit fijado del submódulo `Eglantine/`.

Gracias a: **Djihads80 y los colaboradores de Eglantine**; los proyectos **Eden** y yuzu; **heydemoura** (frontend EglantinePS5 y port a PS5); **BlackBearReloaded** (ProsperoEden, ps5-opengl, boilerplate nativo de PS5); **Mihawk** (PS5 Mesa/RADV y PS5 Vulkan); **mpereiraesaa** (ps5-vulkan); **drakmor** (ShadowMountPlus). La lista de créditos detallada está en [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md).
