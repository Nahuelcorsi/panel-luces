"""El amigo mira lo que hacés en la PC (Windows) y se lo cuenta al panel, que lo hace reaccionar en todas sus pantallas.

Cada 2 segundos mira qué programa está en primer plano y lo pasa a una categoría (programando, video, música,
juego, charla, oficina, navegando); también si no tocás nada hace rato (inactivo), si volviste (vuelve) y si
llevás horas sin parar (descanso). Al panel solo le manda la categoría y el nombre de la app ("VS Code",
"YouTube"): nunca el título de la ventana, lo que escribís ni lo que hay en pantalla. El título se mira acá,
solo para reconocer páginas como YouTube o Netflix dentro del navegador.

Sin instalar nada: usa el Python común. Se corre con pythonw para que no tenga ventana:
  pythonw ojo_pc.py --panel http://192.168.0.12:8080
"""
import argparse
import ctypes
import json
import os
import time
import urllib.request
from ctypes import wintypes

user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32

INACTIVO_SEG = 5 * 60          # sin tocar mouse ni teclado este rato: "inactivo"
DESCANSO_SEG = 2 * 60 * 60     # activo este rato sin pausas largas: le sugiere un descanso
REPETIR_SEG = 3 * 60           # no avisa la misma categoría dos veces seguidas antes de esto
FIRME_SEG = 4                  # una ventana tiene que quedar este rato para contar (así no salta con cada Alt+Tab)

# Programa (.exe en minúsculas) -> (categoría, nombre que se muestra)
PROGRAMAS = {
    "code.exe": ("programando", "VS Code"), "devenv.exe": ("programando", "Visual Studio"),
    "arduino ide.exe": ("programando", "Arduino IDE"), "pycharm64.exe": ("programando", "PyCharm"),
    "windowsterminal.exe": ("programando", "Terminal"), "cmd.exe": ("programando", "Terminal"),
    "powershell.exe": ("programando", "Terminal"), "claude.exe": ("programando", "Claude"),
    "spotify.exe": ("musica", "Spotify"), "rekordbox.exe": ("musica", "rekordbox"), "mixxx.exe": ("musica", "Mixxx"),
    "vlc.exe": ("video", "VLC"), "mpc-hc64.exe": ("video", "MPC"), "potplayermini64.exe": ("video", "PotPlayer"),
    "whatsapp.exe": ("charla", "WhatsApp"), "discord.exe": ("charla", "Discord"), "telegram.exe": ("charla", "Telegram"),
    "winword.exe": ("oficina", "Word"), "excel.exe": ("oficina", "Excel"), "powerpnt.exe": ("oficina", "PowerPoint"),
    "obsidian.exe": ("oficina", "Obsidian"),
}
NAVEGADORES = {"chrome.exe", "msedge.exe", "firefox.exe", "brave.exe", "opera.exe", "opera_gx.exe"}
# Dentro del navegador: palabra en el título -> (categoría, nombre)
PAGINAS = [
    ("youtube music", "musica", "YouTube Music"), ("spotify", "musica", "Spotify"), ("youtube", "video", "YouTube"),
    ("netflix", "video", "Netflix"), ("disney+", "video", "Disney+"), ("prime video", "video", "Prime Video"),
    ("twitch", "video", "Twitch"), ("whatsapp", "charla", "WhatsApp"),
    ("github", "programando", "GitHub"), ("stack overflow", "programando", "Stack Overflow"),
    ("colab", "programando", "Colab"), ("claude", "programando", "Claude"),
]
JUEGOS = ("\\steamapps\\common\\", "\\epic games\\", "\\riot games\\", "\\xboxgames\\")
PROPIAS = ("licenciado capsulito",)   # la pantalla del amigo en la PC: mirarlo a él no cuenta


class LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.UINT), ("dwTime", wintypes.DWORD)]


def segundos_quieto():
    info = LASTINPUTINFO(ctypes.sizeof(LASTINPUTINFO))
    user32.GetLastInputInfo(ctypes.byref(info))
    return (kernel32.GetTickCount() - info.dwTime) / 1000


def ventana_activa():
    """(ruta del .exe, título) de la ventana en primer plano."""
    hwnd = user32.GetForegroundWindow()
    largo = user32.GetWindowTextLengthW(hwnd)
    titulo = ctypes.create_unicode_buffer(largo + 1)
    user32.GetWindowTextW(hwnd, titulo, largo + 1)
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    ruta = ""
    proceso = kernel32.OpenProcess(0x1000, False, pid.value)   # PROCESS_QUERY_LIMITED_INFORMATION
    if proceso:
        buf, tam = ctypes.create_unicode_buffer(1024), wintypes.DWORD(1024)
        if kernel32.QueryFullProcessImageNameW(proceso, 0, buf, ctypes.byref(tam)):
            ruta = buf.value
        kernel32.CloseHandle(proceso)
    return ruta, titulo.value


def clasificar(ruta, titulo):
    """(categoría, app) o None si no hay nada que contar."""
    exe, t, r = os.path.basename(ruta).lower(), titulo.lower(), ruta.lower()
    if not exe or any(p in t for p in PROPIAS):
        return None
    if any(j in r for j in JUEGOS):
        return "juego", os.path.splitext(os.path.basename(ruta))[0][:30]
    if exe in NAVEGADORES:
        for palabra, categoria, nombre in PAGINAS:
            if palabra in t:
                return categoria, nombre
        return "navegando", "el navegador"
    return PROGRAMAS.get(exe)


def avisar(panel, categoria, app=""):
    req = urllib.request.Request(panel.rstrip("/") + "/api/amigo/pc", json.dumps({"categoria": categoria, "app": app}).encode(),
                                 {"Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req, timeout=4).close()
    except OSError:
        pass   # el panel apagado o sin red: se vuelve a intentar con el próximo cambio


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--panel", default="http://192.168.0.12:8080")
    p.add_argument("--ver", action="store_true", help="muestra en la consola lo que va detectando, para probar")
    a = p.parse_args()

    actual, candidata, desde = None, None, 0.0
    avisado = {}                       # categoría -> última vez que se avisó
    quieto_antes, activo_desde, descanso_avisado = False, time.time(), 0.0
    while True:
        ahora = time.time()
        quieto = segundos_quieto() > INACTIVO_SEG
        if quieto != quieto_antes:
            quieto_antes = quieto
            avisar(a.panel, "inactivo" if quieto else "vuelve")
            if a.ver:
                print("inactivo" if quieto else "vuelve")
            if not quieto:
                activo_desde = ahora
        if not quieto:
            if ahora - activo_desde > DESCANSO_SEG and ahora - descanso_avisado > DESCANSO_SEG:
                descanso_avisado = ahora
                avisar(a.panel, "descanso")
            try:
                cosa = clasificar(*ventana_activa())
            except OSError:
                cosa = None
            if cosa != candidata:
                candidata, desde = cosa, ahora
            elif cosa and cosa != actual and ahora - desde >= FIRME_SEG:
                actual = cosa
                if ahora - avisado.get(cosa[0], 0) > REPETIR_SEG:
                    avisado[cosa[0]] = ahora
                    avisar(a.panel, *cosa)
                    if a.ver:
                        print(cosa)
        time.sleep(2)


if __name__ == "__main__":
    main()
