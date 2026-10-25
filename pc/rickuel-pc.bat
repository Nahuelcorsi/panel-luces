@echo off
rem Abre a Rickuel en esta PC (micrófono y parlantes de la PC). Cambiá la IP por la del celular del panel.
set PANEL=http://192.168.0.XX:8080
cd /d "%~dp0"
python rickuel_pc.py --panel %PANEL%
