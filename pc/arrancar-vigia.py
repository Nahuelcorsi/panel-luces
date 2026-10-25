"""Doble clic: la primera vez instala opencv en .venv-vigia (junto a este archivo) y después arranca el vigía."""
import subprocess, sys, venv
from pathlib import Path

AQUI = Path(__file__).resolve().parent
PY = AQUI / ".venv-vigia" / "Scripts" / "python.exe"
probar = "probar" in Path(__file__).stem
try:
    if not PY.exists():
        print("Primera vez: instalando lo que necesita (tarda unos minutos)...", flush=True)
        venv.create(AQUI / ".venv-vigia", with_pip=True)
        subprocess.run([PY, "-m", "pip", "install", "--quiet", "opencv-python"], check=True)
    subprocess.run([PY, AQUI / "vigia.py"] + (["--una-vez"] if probar else []), cwd=AQUI)
except Exception as e:
    print("Error:", e)
input("\nEnter para cerrar...")
