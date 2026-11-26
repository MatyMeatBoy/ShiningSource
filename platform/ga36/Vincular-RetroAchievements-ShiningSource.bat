@echo off
rem Vincula ShiningSource GA36 de esta tarjeta con RetroAchievements desde este PC.
rem Si ReKHCoM ya esta vinculado en la tarjeta, usa esa misma sesion.
python "%~dp0ra_vincular_cuenta.py" "%~d0/"
if errorlevel 9009 echo No se encontro Python. Instala Python 3 desde python.org y vuelve a abrir este archivo.
pause
