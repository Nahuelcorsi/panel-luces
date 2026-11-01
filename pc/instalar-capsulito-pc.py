"""Deja al Licenciado Capsulito mirando esta PC y que arranque solo con Windows.

Doble clic (o: python instalar-capsulito-pc.py). Crea un acceso en Inicio de Windows que levanta ojo_pc.py sin
ventana (le cuenta al panel qué estás haciendo para que el amigo reaccione), lo arranca ya mismo y saca los arranques
de las voces viejas (la de Rick y la de Piper), porque el amigo ahora habla en idioma robot desde el navegador.
Si el celular del panel cambia de dirección, cambiala en PANEL acá abajo y en amigo-pc.bat, y volvé a correr esto.
"""
import os
import subprocess
import sys
from pathlib import Path

PANEL = "http://192.168.0.12:8080"

AQUI = Path(__file__).resolve().parent
INICIO = Path(os.environ["APPDATA"]) / "Microsoft/Windows/Start Menu/Programs/Startup"
PYW = Path(sys.executable).with_name("pythonw.exe")


def main():
    for viejo in ("rickuel-voz.cmd", "amigo-voz.cmd"):
        if (INICIO / viejo).exists():
            (INICIO / viejo).unlink()
            print(f"Saqué el arranque de la voz vieja ({viejo}).")
    ojo = AQUI / "ojo_pc.py"
    (INICIO / "capsulito-ojo.cmd").write_text(f'@start "" "{PYW}" "{ojo}" --panel {PANEL}\r\n', encoding="utf-8")
    subprocess.Popen([str(PYW), str(ojo), "--panel", PANEL], cwd=AQUI)
    print("\nListo: el Capsulito ya mira esta PC y va a arrancar solo con Windows.")
    print("Reiniciá la PC para que se apaguen las voces viejas.")
    print("Para verlo en esta PC: doble clic en amigo-pc.bat.")
    input("\nApretá Enter para cerrar.")


if __name__ == "__main__":
    main()
