"""Deja la voz de Rickuel andando en esta PC y que arranque sola con Windows.

Doble clic (o: python instalar-voz-pc.py). La primera vez instala Piper en pc/.venv-voz (tarda unos minutos).
Después crea un acceso en Inicio de Windows que levanta voz_pc.py sin ventana, lo arranca ya mismo y muestra la
dirección que va en "voz_url" del rickuel.json del panel.
"""
import os
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

AQUI = Path(__file__).resolve().parent
VENV = AQUI / ".venv-voz"
PY = VENV / "Scripts" / "python.exe"
PYW = VENV / "Scripts" / "pythonw.exe"
INICIO = Path(os.environ["APPDATA"]) / "Microsoft/Windows/Start Menu/Programs/Startup/rickuel-voz.cmd"


def ip_local():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.1", 80))
        return s.getsockname()[0]
    except OSError:
        return "IP-DE-ESTA-PC"
    finally:
        s.close()


def main():
    if not PY.exists():
        print("Instalando Piper (unos minutos, solo la primera vez)...")
        subprocess.run([sys.executable, "-m", "venv", str(VENV)], check=True)
        subprocess.run([str(PY), "-m", "pip", "install", "--upgrade", "piper-tts"], check=True)

    INICIO.write_text(f'@start "" "{PYW}" "{AQUI / "voz_pc.py"}"\r\n', encoding="utf-8")
    print("Listo el arranque automático con Windows.")

    try:
        urllib.request.urlopen("http://localhost:8090", timeout=2)
        print("La voz ya estaba corriendo: si cambiaste el modelo, reiniciá la PC (o cerrá pythonw en el Administrador de tareas).")
    except OSError:
        subprocess.Popen([str(PYW), str(AQUI / "voz_pc.py")], cwd=AQUI)
        for _ in range(60):
            time.sleep(1)
            try:
                urllib.request.urlopen("http://localhost:8090", timeout=2)
                break
            except OSError:
                pass
        else:
            print("No arrancó. Probá en una terminal: " + f'"{PY}" "{AQUI / "voz_pc.py"}"' + " y mandale el error a Claude.")
            return

    print(f"\nVoz de Rickuel andando. En el rickuel.json del panel va:\n  \"voz_url\": \"http://{ip_local()}:8090\"")


if __name__ == "__main__":
    try:
        main()
    finally:
        input("\nEnter para cerrar")
