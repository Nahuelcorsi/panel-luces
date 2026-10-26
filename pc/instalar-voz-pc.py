"""Deja la voz del amigo andando en esta PC y que arranque sola con Windows.

Doble clic (o: python instalar-voz-pc.py). La primera vez instala Piper en pc/.venv-voz (tarda unos minutos) y, si
pc/voces/ está vacía, baja una voz provisoria de Piper (es_AR-daniela-high, unos 110 MB) que voz_pc.py pone aguda
como de dibujito. Después crea un acceso en Inicio de Windows que levanta voz_pc.py sin ventana, lo arranca ya mismo
y muestra la dirección que va en "voz_url" del amigo.json del panel.
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
VOCES = AQUI / "voces"
INICIO = Path(os.environ["APPDATA"]) / "Microsoft/Windows/Start Menu/Programs/Startup/amigo-voz.cmd"
PROVISORIA = "https://huggingface.co/rhasspy/piper-voices/resolve/main/es/es_AR/daniela/high/es_AR-daniela-high.onnx"


def ip_local():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.1", 80))
        return s.getsockname()[0]
    except OSError:
        return "IP-DE-ESTA-PC"
    finally:
        s.close()


def bajar_voz_provisoria():
    if any(VOCES.glob("*.onnx")):
        return
    VOCES.mkdir(exist_ok=True)
    print("Bajando la voz provisoria (unos 110 MB, solo la primera vez)...")
    for url in (PROVISORIA + ".json", PROVISORIA):
        destino = VOCES / url.rsplit("/", 1)[1]
        urllib.request.urlretrieve(url, destino.with_suffix(destino.suffix + ".parcial"))
        destino.with_suffix(destino.suffix + ".parcial").replace(destino)


def main():
    if not PY.exists():
        print("Instalando Piper (unos minutos, solo la primera vez)...")
        subprocess.run([sys.executable, "-m", "venv", str(VENV)], check=True)
        subprocess.run([str(PY), "-m", "pip", "install", "--upgrade", "piper-tts"], check=True)
    bajar_voz_provisoria()

    INICIO.write_text(f'@start "" "{PYW}" "{AQUI / "voz_pc.py"}"\r\n', encoding="utf-8")
    print("Listo el arranque automático con Windows.")
    viejo = INICIO.with_name("rickuel-voz.cmd")   # el arranque de la voz anterior
    if viejo.exists():
        viejo.unlink()
        print("Saqué el arranque de la voz vieja: reiniciá la PC para que quede solo la nueva.")

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

    print(f"\nVoz del amigo andando. En el amigo.json del panel va:\n  \"voz_url\": \"http://{ip_local()}:8090\"")


if __name__ == "__main__":
    try:
        main()
    finally:
        input("\nEnter para cerrar")
