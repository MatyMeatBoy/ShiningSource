# ShiningSource para R36S / ShiningSource for R36S

Paquete manual de Ports para la R36S y sus clones (Rockchip RK3326, cuatro Cortex-A35, pantalla 640x480) con un firmware Linux de 64 bits: ArkOS, dArkOS, AmberELEC, ROCKNIX y similares (glibc 2.30 o posterior, DRM/KMS y ALSA).

Manual Ports package for the R36S and its clones (Rockchip RK3326, four Cortex-A35, 640x480 screen) on a 64-bit Linux firmware: ArkOS, dArkOS, AmberELEC, ROCKNIX and the like (glibc 2.30 or later, DRM/KMS and ALSA).

## Español

### Instalación

1. Apaga la consola y conecta la tarjeta del sistema (o la segunda tarjeta, si tu firmware guarda ahí los juegos) al ordenador. Abre la carpeta `ports` de la partición de juegos (`EASYROMS/ports` en ArkOS y dArkOS, `roms/ports` en otros).
2. Extrae el ZIP ahí. Debe quedar `ports/ShiningSource.sh` junto a la carpeta `ports/shiningsource/`.
3. Copia tu ROM a `ports/shiningsource/roms/rom.gba` (ver abajo).
4. Expulsa la tarjeta con seguridad, arranca la consola y abre **ShiningSource** en **Ports**. Si la entrada no aparece, reinicia EmulationStation (Start > Quit > Restart EmulationStation) o la consola.

### ROM

Necesitas tu propio volcado sin modificar del cartucho europeo de Shining Soul II (código `AU2P`, SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`). El paquete no incluye ningún contenido del juego.

El launcher busca, en este orden: `shiningsource/roms/rom.gba`; el primer `*.gba` de `shiningsource/roms/`; un archivo cuyo nombre contenga `Shining`, `Soul` y `II` o `2` en la carpeta `gba` de la tarjeta (`ports/../gba`, `/roms/gba`, `/roms2/gba`). La ROM sólo se lee en su sitio: nunca se copia ni se modifica.

### Controles

Los botones de la consola se usan como un mando: la **A** de la consola es la **A** de la GBA y la **B** la **B**; L1/R1 son L/R. **Start + Select** abre las opciones del juego; desde ahí se elige **Salir** para volver al menú. Si PortMaster está instalado se usa su mapeo SDL del mando; si no, el de SDL o, como último recurso, los botones se leen directamente. Para otro mapeo coloca `gamecontrollerdb.txt` en `shiningsource/` o define `SS_PAD` (ver abajo).

### Imagen

Siempre en **Flat**: un lienzo de 320×240 píxeles del juego a 2× exactos, sin filtrado, llena la pantalla de 640×480. En los mapas el campo se ve más allá del encuadre de la GBA por los cuatro lados; la barra de estado sube al borde superior y la caja de texto con el retrato baja al borde inferior. Idioma, título, partidas, creación de personaje y tiendas llevan alrededor arte hecho para este lienzo; las demás escenas conservan su encuadre de 240×160 con negro alrededor y bordes suaves.

La imagen va a la pantalla por DRM/KMS con tres búferes: nunca se ve un cuadro a medias. Si el panel refresca cerca de los 59,73 Hz de la GBA, el panel marca el ritmo del juego y el sonido lo sigue; si no, se prueba a llevarlo a 59,73 Hz acortando su borrado y sólo se conserva si se mide bien. El dibujo se reparte en los cuatro núcleos.

### Partidas y registros

Las partidas, la configuración y los registros se guardan en `shiningsource/user/`. Cada arranque escribe `last-run.log` (y conserva el anterior como `last-run.1.log`): el modelo, el firmware, la pantalla y su refresco medido y, al salir del juego, los cuadros por segundo, los tiempos de cuadro (media, p95, p99, máximo) y lo que cuesta cada etapa. Si el juego no arranca, ese archivo explica el motivo. Adjúntalo al informar de un problema, junto con el nombre del firmware.

### Variables

`SS_CANVAS` (`320x240` por defecto; `240x160` muestra sólo la imagen de la GBA a 480×320 en el centro), `SS_FBDEV` (vacía: SDL3 con su controlador KMSDRM en lugar de la salida propia), `SS_KMS_HZ=0` (no tocar el modo del panel), `SS_THREADS` (hilos para el dibujo; 1 = uno solo), `SS_PAD` (botones SDL en el orden A B Select Start Derecha Izquierda Arriba Abajo R L), `SS_EVDEV_MAP`, `SDL_AUDIO_DRIVER`. `R36S-release.json` lista la versión, el commit y el SHA-256 de cada archivo.

### Logros (RetroAchievements)

El juego evalúa en la consola los 52 logros oficiales de RetroAchievements (el set va en la carpeta) y muestra el
aviso al desbloquear uno. Para que cuenten en tu cuenta, vincula la tarjeta una vez desde un PC con internet:
copia en `shiningsource/` los archivos `retroachievements.auth` y `retroachievements.ini` de una
tarjeta ya vinculada (ShiningSource o ReKHCoM). Si la tarjeta ya está vinculada para ReKHCoM
(`ports/rekhcomr36s/retroachievements.auth`), ShiningSource usa esa misma sesión y no hace falta nada.

Sin internet, los logros quedan en cola (`retroachievements_pending.txt`) y se envían la próxima vez que el juego
arranque con conexión. `retroachievements_estado.txt` dice la cuenta, si hay conexión, cuántos esperan y la hora de
la consola. Solo softcore. Una partida en Modo Rebelde (trucos) no envía nada. `SS_RA_SUBMIT=0` en
`user/shiningsource.env` deja todos los logros en la consola.

### Estado

Probada en emulación de una R36S (QEMU, cuatro Cortex-A35, raíz de dArkOS y glibc 2.31 de Ubuntu 20.04, pantalla virtio-gpu por DRM): mismo juego cuadro a cuadro e idénticas imágenes que en PC. Aún no se ha probado en una consola física: el vídeo en su panel, el sonido, los controles y la velocidad real están por confirmar.

## English

### Installation

1. Power the handheld off and put the system card (or the second card, if your firmware keeps the games there) in your computer. Open the `ports` folder of the games partition (`EASYROMS/ports` on ArkOS and dArkOS, `roms/ports` elsewhere).
2. Extract the ZIP there. You should get `ports/ShiningSource.sh` beside the `ports/shiningsource/` folder.
3. Copy your ROM to `ports/shiningsource/roms/rom.gba` (see below).
4. Eject the card safely, boot the handheld and open **ShiningSource** from **Ports**. If the entry is missing, restart EmulationStation (Start > Quit > Restart EmulationStation) or the handheld.

### ROM

You need your own unmodified dump of the European Shining Soul II cartridge (code `AU2P`, SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`). The package contains no game content.

The launcher looks, in order, for: `shiningsource/roms/rom.gba`; the first `*.gba` in `shiningsource/roms/`; a file whose name contains `Shining`, `Soul` and `II` or `2` in the card's `gba` folder (`ports/../gba`, `/roms/gba`, `/roms2/gba`). The ROM is only read in place: it is never copied or modified.

### Controls

The handheld's buttons act as a gamepad: the console's **A** is the GBA's **A** and **B** its **B**; L1/R1 are L/R. **Start + Select** opens the game's options; choose **Exit** there to return to the menu. With PortMaster installed its SDL controller mapping is used; otherwise SDL's own, or as a last resort the buttons are read directly. For another mapping put `gamecontrollerdb.txt` in `shiningsource/` or set `SS_PAD` (see below).

### Picture

Always **Flat**: a canvas of 320×240 game pixels at an exact 2×, unfiltered, fills the 640×480 screen. On the maps the field is shown beyond the GBA's frame on all four sides; the status bar goes up to the top edge and the message window with the portrait down to the bottom edge. Language, title, save, character creation and shops are surrounded by art made for this canvas; other scenes keep their 240×160 framing with black and soft edges around it.

The picture goes to the screen through DRM/KMS with three buffers: a half-drawn frame is never seen. When the panel refreshes near the GBA's 59.73 Hz the panel paces the game and the sound follows it; otherwise bringing it to 59.73 Hz by shortening its blanking is tried, and kept only if it measures right. Drawing is spread over the four cores.

### Saves and logs

Saves, settings and logs are written to `shiningsource/user/`. Each start writes `last-run.log` (keeping the previous one as `last-run.1.log`): the model, the firmware, the screen and its measured refresh and, when the game ends, frames per second, frame times (mean, p95, p99, maximum) and what each stage costs. If the game does not start, that file says why. Attach it when reporting a problem, together with the firmware name.

### Variables

`SS_CANVAS` (`320x240` by default; `240x160` shows only the GBA picture at 480×320 in the middle), `SS_FBDEV` (empty: SDL3 with its KMSDRM driver instead of the program's own output), `SS_KMS_HZ=0` (leave the panel's mode alone), `SS_THREADS` (threads for drawing; 1 = one alone), `SS_PAD` (SDL buttons in the order A B Select Start Right Left Up Down R L), `SS_EVDEV_MAP`, `SDL_AUDIO_DRIVER`. `R36S-release.json` lists the version, the commit and the SHA-256 of every file.

### Achievements (RetroAchievements)

The game evaluates the 52 official RetroAchievements on the console (the set ships in the folder) and shows the
notice when one unlocks. To have them count on your account, link the card once from a PC with internet: copy
`retroachievements.auth` and `retroachievements.ini` of an already linked card folder (ShiningSource or ReKHCoM)
into `shiningsource/`. A card already linked for ReKHCoM (`ports/rekhcomr36s/retroachievements.auth`) is used as is.

Without internet the unlocks wait in `retroachievements_pending.txt` and are sent the next time the game starts
online. `retroachievements_estado.txt` shows the account, the connection, how many wait and the console's clock.
Softcore only. A Rebel run (cheats) sends nothing. `SS_RA_SUBMIT=0` in `user/shiningsource.env` keeps every
unlock on the console.

### Status

Tested in an emulated R36S (QEMU, four Cortex-A35, a dArkOS root and Ubuntu 20.04's glibc 2.31, a virtio-gpu screen through DRM): the same game frame by frame and identical pictures as on PC. Not yet tried on a physical handheld: video on its panel, sound, controls and real speed are still to be confirmed.
