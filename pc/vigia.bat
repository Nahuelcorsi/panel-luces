@echo off
rem Vigia de Rickuel: la primera vez instala lo que necesita (opencv) en pc\.venv-vigia y después lo arranca.
cd /d "%~dp0"
if not exist .venv-vigia\Scripts\python.exe (
  python -m venv .venv-vigia || exit /b 1
  .venv-vigia\Scripts\python -m pip install --quiet opencv-python
)
.venv-vigia\Scripts\python vigia.py %*
