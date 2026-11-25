# ShiningSource para GA36 / ShiningSource for GA36

Paquete manual de Ports para la consola GA36-MB V1.2 (Allwinner A33, Linux ARMv7 hard-float, pantalla 640x480 en `/dev/fb0`, kernel sunxi 3.4.39 de fábrica). Pensado para los firmwares goodluckOS y ClonDiClon.

Manual Ports package for the GA36-MB V1.2 handheld (Allwinner A33, ARMv7 hard-float Linux, 640x480 screen on `/dev/fb0`, stock sunxi 3.4.39 kernel). Targets the goodluckOS and ClonDiClon firmwares.

## Español

### Instalación

1. Apaga la consola y conecta la tarjeta al ordenador. Abre el volumen FAT32 `HOME` y entra en su carpeta `ports`, donde ya están los otros ports (`ports/<Nombre>.sh` junto a `ports/<carpeta>/`).
2. Extrae el ZIP ahí. Debe quedar `ports/ShiningSource GA36.sh` junto a la carpeta `ports/shiningsourcega36/`.
3. Copia tu ROM a `ports/shiningsourcega36/roms/rom.gba` (ver abajo).
4. Expulsa la tarjeta con seguridad, arranca la consola y abre **ShiningSource GA36** en **Ports** de EmulationStation. Si la entrada no aparece, reinicia EmulationStation o la consola para que vuelva a leer la carpeta.

### ROM

Necesitas tu propio volcado sin modificar del cartucho europeo de Shining Soul II (código `AU2P`, SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`). El paquete no incluye ningún contenido del juego.

El launcher busca, en este orden: `shiningsourcega36/roms/rom.gba`; el primer `*.gba` de `shiningsourcega36/roms/`; un archivo cuyo nombre contenga `Shining`, `Soul` y `II` o `2` en `roms/gba/` o `gba/` de la raíz de la tarjeta. La ROM sólo se lee en su sitio: nunca se copia ni se modifica.

### Controles

Los controles de la consola se usan como un mando. **Start + Select** abre las opciones del juego; desde ahí se elige **Salir** para volver a EmulationStation. Si tu firmware necesita un mapeo SDL propio, coloca `gamecontrollerdb.txt` en `shiningsourcega36/` o define `SDL_GAMECONTROLLERCONFIG_FILE`.

### Imagen 4:3

La pantalla de 640x480 se usa entera: un lienzo de 320×240 píxeles del juego a 2× exactos, sin filtrado. En los mapas el campo se ve más allá del encuadre de la GBA por los cuatro lados. Los menús, la pantalla de título, las tiendas y las cinemáticas conservan su encuadre propio de 240×160 (480×320 en la pantalla), centrado y rodeado por el decorado del port: el campo «Soul», las ilustraciones laterales de cada escena extendidas arriba y abajo, o los bordes de la imagen prolongados. Las opciones (**Start + Select**), el aviso para saltar escenas y los fundidos ocupan el lienzo completo y se leen a 2×.

`SS_CANVAS` elige el lienzo: `320x240` por defecto; `240x160` muestra sólo la imagen original de la GBA a 480×320 en el centro; vacía (`SS_CANVAS=`) vuelve a la imagen panorámica de 320×160 de RC1. En **Opciones**, «Lados: Negro» quita el decorado y «Wide Pro: No» deja sólo el campo «Soul».

### Partidas y registros

Las partidas, la configuración y los registros se guardan en `shiningsourcega36/user/`. Cada arranque escribe `last-run.log` y conserva el anterior como `last-run.1.log`. Si el juego no arranca, ese archivo explica el motivo (por ejemplo, ROM ausente). Adjunta los dos al informar de un problema, junto con el nombre del firmware.

### Estado de RC1

Esta versión candidata se ha verificado sólo en emulación y con una auditoría del binario (ELF ARMv7 hard-float, glibc incluida que declara ABI de kernel 3.2.0, inferior a 3.4.39). El vídeo, el audio, los controles y el rendimiento en la consola física **todavía no se han comprobado**.

El juego lleva su propio cargador, glibc, SDL3 y libasound en `shiningsourcega36/libarmhf/`, y la configuración de ALSA en `shiningsourcega36/alsa/` para firmwares sin `/usr/share/alsa`. Variables de diagnóstico: `SS_FBDEV` (vacía desactiva el framebuffer), `SS_CANVAS` (ver «Imagen 4:3»), `SDL_VIDEO_DRIVER` y `SDL_AUDIO_DRIVER`. `GA36-release.json` lista la versión, el commit y el SHA-256 de cada archivo.

## English

### Installation

1. Power the handheld off and put the card in your computer. Open the FAT32 `HOME` volume and its `ports` folder, where the other ports already live (`ports/<Name>.sh` beside `ports/<folder>/`).
2. Extract the ZIP there. You should get `ports/ShiningSource GA36.sh` beside the `ports/shiningsourcega36/` folder.
3. Copy your ROM to `ports/shiningsourcega36/roms/rom.gba` (see below).
4. Eject the card safely, boot the handheld and open **ShiningSource GA36** from EmulationStation's **Ports**. If the entry is missing, restart EmulationStation or the handheld so it rescans the folder.

### ROM

You need your own unmodified dump of the European Shining Soul II cartridge (code `AU2P`, SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`). The package contains no game content.

The launcher looks, in order, for: `shiningsourcega36/roms/rom.gba`; the first `*.gba` in `shiningsourcega36/roms/`; a file whose name contains `Shining`, `Soul` and `II` or `2` in `roms/gba/` or `gba/` at the root of the card. The ROM is only read in place: it is never copied or modified.

### Controls

The handheld's buttons act as a gamepad. **Start + Select** opens the game's options; choose **Exit** there to return to EmulationStation. If your firmware needs its own SDL mapping, put `gamecontrollerdb.txt` in `shiningsourcega36/` or set `SDL_GAMECONTROLLERCONFIG_FILE`.

### 4:3 picture

The whole 640x480 screen is used: a canvas of 320×240 game pixels at an exact 2×, unfiltered. On the maps the field is shown beyond the GBA's frame on all four sides. Menus, the title screen, shops and cut-scenes keep their own 240×160 framing (480×320 on the screen), centred and surrounded by the port's decor: the "Soul" field, each scene's side artwork extended above and below, or the picture's edges drawn out. The options (**Start + Select**), the skip-scene prompt and fades cover the whole canvas and read at 2×.

`SS_CANVAS` chooses the canvas: `320x240` by default; `240x160` shows only the original GBA picture at 480×320 in the middle; empty (`SS_CANVAS=`) brings back RC1's 320×160 wide picture. In **Options**, "Sides: Black" removes the decor and "Wide Pro: Off" leaves only the "Soul" field.

### Saves and logs

Saves, settings and logs are written to `shiningsourcega36/user/`. Each start writes `last-run.log` and keeps the previous one as `last-run.1.log`. If the game does not start, that file says why (for example, a missing ROM). Attach both when reporting a problem, together with the firmware name.

### RC1 status

This release candidate was verified in emulation only, plus an audit of the binary (ARMv7 hard-float ELF, bundled glibc declaring Linux kernel ABI 3.2.0, below 3.4.39). Video, audio, input and performance on the physical handheld are **not verified yet**.

The game ships its own loader, glibc, SDL3 and libasound in `shiningsourcega36/libarmhf/`, plus the ALSA configuration in `shiningsourcega36/alsa/` for firmwares without `/usr/share/alsa`. Diagnostic variables: `SS_FBDEV` (empty disables the framebuffer), `SS_CANVAS` (see "4:3 picture"), `SDL_VIDEO_DRIVER` and `SDL_AUDIO_DRIVER`. `GA36-release.json` lists the version, the commit and the SHA-256 of every file.
