@echo off
rem Abre al amigo en esta PC (micrófono y parlantes de la PC). El celular del panel está en 192.168.0.9 (si cambia, cambiala acá).
set PANEL=http://192.168.0.9:8080
cd /d "%~dp0"
python amigo_pc.py --panel %PANEL%
