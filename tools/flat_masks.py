#!/usr/bin/env python3
"""PixelBuddy inputs for the Flat (4:3) art of the framed scenes.

  flat_masks.py --scene NAME --centre SHOT.png --art assets/launcher/<name>-native-sides.png [--out DIR]

Flat shows the console's 240 x 160 picture in the middle of a 320 x 240 canvas. Beside it the scene keeps the
approved 16:9 art (22 columns a side, at one art pixel per game pixel); everything else of the canvas is new art,
which PixelBuddy generates only there. This writes, in DIR/NAME/ (default assets/launcher/43-masks):

  centro.png     the console's picture, 240 x 160 (from a capture at a whole-number scale, read back exactly)
  base.png       320 x 240: the picture and the approved 16:9 columns where they go, magenta where art is missing
  mascara.png    320 x 240: white where PixelBuddy must paint, black where pixels are kept
  LEEME.txt      what to ask for and how the result is used

The generated result is saved as assets/launcher/<scene>-flat.png at exactly 320 x 240 (shown at 2x it is the
GA36's 640 x 480 panel; nothing bigger is kept). The port uses only what lies outside the picture.
"""
import argparse
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
W, H, CW, CH = 240, 160, 320, 240
LEFT, TOP = (CW - W) // 2, (CH - H) // 2
MAGENTA = (255, 0, 255)


def native(path):
    image = Image.open(path).convert("RGB")
    if image.size == (W, H):
        return image
    scale = image.width // W
    if image.size != (W * scale, H * scale):
        raise SystemExit("%s is not the console's picture at a whole-number scale (%dx%d)" % (path, *image.size))
    small = image.resize((W, H), Image.NEAREST)
    if small.resize(image.size, Image.NEAREST).tobytes() != image.tobytes():
        raise SystemExit("%s does not read back exactly at 1/%d: not a clean %dx capture" % (path, scale, scale))
    return small


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", required=True)
    ap.add_argument("--centre", required=True, type=Path)
    ap.add_argument("--art", required=True, type=Path)
    ap.add_argument("--out", type=Path, default=ROOT / "assets/launcher/43-masks")
    args = ap.parse_args()
    centre = native(args.centre)
    art = Image.open(args.art).convert("RGB")
    if art.size != (44, H):
        raise SystemExit("%s: the approved 16:9 art is 44 x 160 (22 columns a side)" % args.art)
    half = art.width // 2
    base = Image.new("RGB", (CW, CH), MAGENTA)
    mask = Image.new("L", (CW, CH), 255)
    base.paste(centre, (LEFT, TOP))
    base.paste(art.crop((0, 0, half, H)), (LEFT - half, TOP))
    base.paste(art.crop((half, 0, art.width, H)), (LEFT + W, TOP))
    mask.paste(0, (LEFT - half, TOP, LEFT + W + half, TOP + H))
    folder = args.out / args.scene
    folder.mkdir(parents=True, exist_ok=True)
    centre.save(folder / "centro.png")
    base.save(folder / "base.png")
    mask.save(folder / "mascara.png")
    (folder / "LEEME.txt").write_text(
        "Escena %s, arte Flat 4:3 (PixelBuddy).\n\n"
        "base.png (320x240) tiene el cuadro original de la consola en el centro (240x160 en 40,40) y, a cada lado,\n"
        "las 22 columnas del arte 16:9 aprobado (%s). El magenta es lo que falta: 18 columnas por lado junto a\n"
        "ellas y las franjas de 40 filas arriba y abajo. mascara.png: blanco = pintar, negro = conservar.\n\n"
        "Pedir: continuar el fondo de la escena en las zonas magenta, siguiendo el arte aprobado de los lados y el\n"
        "fondo original (misma paleta, grano, luz y escala de pixel nativa), sin repetir ni prolongar personajes,\n"
        "texto ni interfaz, y sin tocar un pixel fuera del magenta.\n\n"
        "Resultado: assets/launcher/%s-flat.png de 320x240 exactos (en la GA36 se ve a 2x = 640x480). El port\n"
        "usa solo lo que queda fuera del cuadro de la consola. Revisar a escala entera antes de aprobar.\n"
        % (args.scene, args.art.name, args.scene), encoding="utf-8")
    print(folder)


if __name__ == "__main__":
    main()
