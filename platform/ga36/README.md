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

Los controles de la consola se usan como un mando: el botón de abajo es la **A** de la GBA y el de la derecha la **B**. **Mantener Select 2 segundos** abre (y cierra) las opciones del juego; desde ahí se elige **Salir** para volver a EmulationStation. Si tu firmware necesita un mapeo SDL propio, coloca `gamecontrollerdb.txt` en `shiningsourcega36/` o define `SDL_GAMECONTROLLERCONFIG_FILE`.

### Imagen 4:3

La pantalla de 640x480 se usa entera: un lienzo de 320×240 píxeles del juego a 2× exactos, sin filtrado. En los mapas el campo se ve más allá del encuadre de la GBA por los cuatro lados. La caja de texto, el retrato de quien habla y lo demás que la GBA pone abajo bajan al borde inferior; la barra de estado sube al borde superior. Idioma, título, partidas, creación de personaje y tiendas tienen alrededor arte hecho para este lienzo a partir del arte aprobado de la vista panorámica (borradores pendientes de aprobación); las demás escenas conservan su encuadre de 240×160 con negro alrededor. Las opciones (**mantener Select 2 s**), el aviso para saltar escenas y los fundidos ocupan el lienzo completo y se leen a 2×.

`SS_CANVAS` elige el lienzo: `320x240` por defecto; `240x160` muestra sólo la imagen original de la GBA a 480×320 en el centro; vacía (`SS_CANVAS=`) vuelve a la imagen panorámica de 320×160 de RC1.

### Partidas y registros

Las partidas, la configuración y los registros se guardan en `shiningsourcega36/user/`. Cada arranque escribe `last-run.log` y conserva el anterior como `last-run.1.log`. Si el juego no arranca, ese archivo explica el motivo (por ejemplo, ROM ausente). Adjunta los dos al informar de un problema, junto con el nombre del firmware.

### Logros (RetroAchievements)

El juego evalúa en la consola los 52 logros oficiales de RetroAchievements (el set va en la carpeta) y muestra el
aviso al desbloquear uno. Para que cuenten en tu cuenta, vincula la tarjeta una vez desde un PC con internet:
escribe `usuario=` y `contrasena=` en `shiningsourcega36/retroachievements_cuenta.txt` y abre
`shiningsourcega36/Vincular-RetroAchievements-ShiningSource.bat` (necesita Python 3). La contraseña se borra del archivo al
terminar; la consola solo guarda la sesión (`retroachievements.auth`). Si la tarjeta ya está vinculada para ReKHCoM,
ShiningSource usa esa misma sesión y no hace falta nada.

Sin internet, los logros quedan en cola (`retroachievements_pending.txt`) y se envían la próxima vez que el juego
arranque con conexión. `retroachievements_estado.txt` dice la cuenta, si hay conexión, cuántos esperan y la hora de
la consola. Solo softcore. Una partida en Modo Rebelde (trucos) no envía nada. `SS_RA_SUBMIT=0` en
`user/shiningsource.env` deja todos los logros en la consola.

### Estado de v0.1

Probada en la consola (goodluckOS, kernel 7.2.9) hasta la RC3; la v0.1 añade el dibujo repartido en los cuatro núcleos del A33 y se ha verificado en emulación ARMv7 de 4 núcleos (mismas imágenes y mismo juego que en PC, fotograma a fotograma). La velocidad real de la v0.1 en la consola está por medir: la entrada **ShiningSource - Medir FPS** (unos 6 minutos, automática, no toca las partidas) la mide y guarda el resultado en `shiningsourcega36/diagnostico-NNN/`. Un cierre deja `user/shiningsource_crash.log`.

El juego lleva su propio cargador, glibc, SDL3 y libasound en `shiningsourcega36/libarmhf/`, y la configuración de ALSA en `shiningsourcega36/alsa/` para firmwares sin `/usr/share/alsa`. Variables de diagnóstico: `SS_FBDEV` (vacía desactiva el framebuffer), `SS_CANVAS` (ver «Imagen 4:3»), `SS_THREADS` (hilos para el dibujo; 1 = uno solo), `SS_PAD` (botones), `SDL_VIDEO_DRIVER` y `SDL_AUDIO_DRIVER`. `GA36-release.json` lista la versión, el commit y el SHA-256 de cada archivo.

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

The handheld's buttons act as a gamepad: the bottom face button is the GBA's **A** and the right one its **B**. **Holding Select for 2 seconds** opens (and closes) the game's options; choose **Exit** there to return to EmulationStation. If your firmware needs its own SDL mapping, put `gamecontrollerdb.txt` in `shiningsourcega36/` or set `SDL_GAMECONTROLLERCONFIG_FILE`.

### 4:3 picture

The whole 640x480 screen is used: a canvas of 320×240 game pixels at an exact 2×, unfiltered. On the maps the field is shown beyond the GBA's frame on all four sides. The message window, the speaker's portrait and whatever else the GBA puts at the bottom go down to the bottom edge; the status bar goes up to the top edge. Language, title, save, character creation and shops are surrounded by art made for this canvas from the approved wide-view art (drafts pending approval); other scenes keep their 240×160 framing with black around it. The options (**hold Select 2 s**), the skip-scene prompt and fades cover the whole canvas and read at 2×.

`SS_CANVAS` chooses the canvas: `320x240` by default; `240x160` shows only the original GBA picture at 480×320 in the middle; empty (`SS_CANVAS=`) brings back RC1's 320×160 wide picture.

### Saves and logs

Saves, settings and logs are written to `shiningsourcega36/user/`. Each start writes `last-run.log` and keeps the previous one as `last-run.1.log`. If the game does not start, that file says why (for example, a missing ROM). Attach both when reporting a problem, together with the firmware name.

### Achievements (RetroAchievements)

The game evaluates the 52 official RetroAchievements on the console (the set ships in the folder) and shows the
notice when one unlocks. To have them count on your account, link the card once from a PC with internet: write
`usuario=` and `contrasena=` in `shiningsourcega36/retroachievements_cuenta.txt` and open
`shiningsourcega36/Vincular-RetroAchievements-ShiningSource.bat` (needs Python 3). The password is removed from the file afterwards;
the console only keeps the session (`retroachievements.auth`). A card already linked for ReKHCoM is used as is.

Without internet the unlocks wait in `retroachievements_pending.txt` and are sent the next time the game starts
online. `retroachievements_estado.txt` shows the account, the connection, how many wait and the console's clock.
Softcore only. A Rebel run (cheats) sends nothing. `SS_RA_SUBMIT=0` in `user/shiningsource.env` keeps every
unlock on the console.

### v0.1 status

Tried on the handheld (goodluckOS, kernel 7.2.9) up to RC3; v0.1 adds drawing spread over the A33's four cores and was verified in 4-core ARMv7 emulation (the same pictures and the same game as on PC, frame by frame). v0.1's real speed on the handheld is still to be measured: the **ShiningSource - Medir FPS** entry (about 6 minutes, automatic, saves untouched) measures it into `shiningsourcega36/diagnostico-NNN/`. A crash leaves `user/shiningsource_crash.log`.

The game ships its own loader, glibc, SDL3 and libasound in `shiningsourcega36/libarmhf/`, plus the ALSA configuration in `shiningsourcega36/alsa/` for firmwares without `/usr/share/alsa`. Diagnostic variables: `SS_FBDEV` (empty disables the framebuffer), `SS_CANVAS` (see "4:3 picture"), `SS_THREADS` (threads for drawing; 1 = one alone), `SS_PAD` (buttons), `SDL_VIDEO_DRIVER` and `SDL_AUDIO_DRIVER`. `GA36-release.json` lists the version, the commit and the SHA-256 of every file.
