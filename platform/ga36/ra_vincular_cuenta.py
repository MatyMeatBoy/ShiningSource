"""Sign a handheld's card in to RetroAchievements for ShiningSource, from a PC with internet.

usage: python ra_vincular_cuenta.py [SD root, default F:/]

The game reads its session from <SD>/ports/shiningsourcega36/retroachievements.auth (128-byte user name,
128-byte token, both zero-padded) and retroachievements.ini (enabled=1), the same files ReKHCoM GA36 uses.

1. If ReKHCoM GA36 on the same card is already signed in (ports/rekhcomga36/retroachievements.auth), its
   session is copied: a RetroAchievements token belongs to the account, not to one game.
2. Otherwise <SD>/ports/shiningsourcega36/retroachievements_cuenta.txt (usuario= / contrasena=) is used to sign
   in once (the request rcheevos sends, r=login2); the password is then removed from that file.

Neither the password nor the token is ever printed.
"""
import json
import sys
import urllib.parse
import urllib.request
from pathlib import Path

root = Path(sys.argv[1] if len(sys.argv) > 1 else 'F:/')
game = root / 'ports' / 'shiningsourcega36'
if not game.is_dir():
    raise SystemExit(f'No encuentro {game}. Pon la SD con ShiningSource GA36 instalado.')
auth_file = game / 'retroachievements.auth'
ini_file = game / 'retroachievements.ini'
account = game / 'retroachievements_cuenta.txt'


def valid_auth(data):
    return len(data) == 256 and data[0] != 0 and data[128] != 0


def finish(auth, how):
    name = auth[:128].rstrip(b'\0').decode('utf-8', errors='replace')
    auth_file.write_bytes(auth)
    ini_file.write_text('enabled=1\n', newline='\n')
    print(f'Listo: ShiningSource queda vinculado a la cuenta {name} ({how}).')
    print('Ya puedes poner la SD en la consola.')
    raise SystemExit(0)


rekhcom = root / 'ports' / 'rekhcomga36' / 'retroachievements.auth'
if not account.is_file() and rekhcom.is_file():
    shared = rekhcom.read_bytes()
    if valid_auth(shared):
        finish(shared, 'sesion copiada de ReKHCoM')

if not account.is_file():
    account.write_text('usuario=\ncontrasena=\n', encoding='utf-8', newline='\n')
    raise SystemExit(f'Escribe tu usuario y contrasena de RetroAchievements en {account} y vuelve a abrir este archivo.')

user = password = ''
for line in account.read_text(encoding='utf-8', errors='replace').splitlines():
    if line.startswith('#') or '=' not in line:
        continue
    key, value = line.split('=', 1)
    key = key.strip().lower()
    if key in ('usuario', 'user', 'username'):
        user = value.strip()
    elif key in ('contrasena', 'contraseña', 'password'):
        password = value.rstrip('\r\n')
if not user or not password:
    raise SystemExit('El archivo necesita una linea usuario= y otra contrasena=.')

body = urllib.parse.urlencode({'r': 'login2', 'u': user, 'p': password}).encode()
password = None
request = urllib.request.Request('https://retroachievements.org/dorequest.php', data=body, method='POST',
                                 headers={'Content-Type': 'application/x-www-form-urlencoded',
                                          'User-Agent': 'ShiningSource/0.9.5 rcheevos'})
body = None
try:
    with urllib.request.urlopen(request, timeout=20) as response:
        reply = json.loads(response.read().decode('utf-8', errors='replace'))
except Exception as error:  # network or server: nothing on the card is changed
    raise SystemExit(f'No se pudo conectar con RetroAchievements ({type(error).__name__}). La tarjeta no se modifico.')
request = None
if not reply.get('Success') or not reply.get('Token'):
    raise SystemExit('RetroAchievements rechazo el inicio de sesion: ' + str(reply.get('Error', 'sin detalle')) +
                     '. La tarjeta no se modifico.')
name = str(reply.get('User') or user)
token = str(reply['Token'])
if len(name.encode()) >= 128 or len(token.encode()) >= 128:
    raise SystemExit('Respuesta inesperada del servidor. La tarjeta no se modifico.')
auth = name.encode().ljust(128, b'\0') + token.encode().ljust(128, b'\0')
token = None
account.write_text(f'usuario={name}\n# Sesion iniciada desde el PC: la contrasena se borro de este archivo.\n',
                   encoding='utf-8', newline='\n')
finish(auth, 'sesion nueva')
