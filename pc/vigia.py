"""Vigía de Rickuel: mira la cámara cada tanto, anota lo que hacés y, si tiene ganas, te comenta algo al estilo Rick.

Corre en la PC de escritorio, al lado de Ollama. Todo queda en casa:
  1. Cada unos minutos saca UN cuadro de la cámara (RTSP de la Imou), solo si en el panel está prendido «Rickuel observa».
  2. Se lo pasa al modelo de Ollama que entiende imágenes (gemma3:12b), que lo describe en una frase. La imagen no se
     guarda: a bitacora.jsonl va solo esa frase.
  3. Con lo último de la bitácora decide si vale un comentario (con límites: uno cada tanto, pocos por día, nada de noche).
     Si sí, el modelo lo escribe como Rick y se lo manda al panel (/api/rickuel/aviso), que lo dice donde estés.
Configuración en vigia.json (copialo de vigia.example.json; tiene la clave de la cámara y está en el .gitignore).
  python vigia.py            # mira y comenta
  python vigia.py --una-vez  # un solo cuadro, para probar; muestra lo que entendió y no comenta
Necesita opencv-python (pip install opencv-python).
"""
import argparse
import base64
import json
import os
import random
import sys
import time
import urllib.request
from datetime import datetime
from pathlib import Path

# La Imou manda el video por UDP y el firewall de Windows lo corta: por TCP llega entero.
os.environ.setdefault("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp")
import cv2  # noqa: E402

AQUI = Path(__file__).resolve().parent
BASE = {
    "camara": "rtsp://admin:CODIGO@192.168.0.108:554/cam/realmonitor?channel=1&subtype=1",
    "panel": "http://192.168.0.9:8080",
    "ollama": "http://localhost:11434",
    "modelo": "gemma3:12b",
    "cada_minutos": 4,              # cada cuánto mira
    "minutos_entre_comentarios": 45,
    "comentarios_por_dia": 6,
    "silencio": ["00:00", "08:00"],  # de noche no comenta (sí sigue anotando)
}
BITACORA = AQUI / "bitacora.jsonl"

DESCRIBIR = """Sos los ojos de Rickuel, un asistente de la casa de Nahuel. Mirá esta imagen de la cámara de su casa.
Contestá SOLO con JSON: {"hay_persona": true o false, "que_pasa": "una frase corta en tercera persona"}.
En "que_pasa" contá qué está haciendo la persona (si hay) con detalles concretos: postura, dónde está, con qué
(sillón, compu, celular, cocina, comiendo, durmiendo...). Si no hay nadie, describí en pocas palabras cómo está el
lugar. Llamala Nahuel. No inventes lo que no se ve."""

COMENTAR = """Sos Rickuel, un científico genio, cínico y sarcástico inspirado en Rick de Rick y Morty, que vive en la casa
de Nahuel y lo ve por una cámara. Lo bardeás con humor ácido y desprecio cariñoso, a veces tartamudeás
("es-es-escuchame") y metés algún "uuurp". Nunca lo llamás Morty. Nada de crueldad real.
Hablás en español rioplatense. Lo que digas se lee en voz alta: 1 o 2 oraciones, sin listas, markdown ni emojis.
Te paso lo que viste las últimas veces (de más viejo a más nuevo). Decidí si vale la pena decirle algo AHORA: algo
que lleva mucho rato, algo nuevo o gracioso, o algo que contradice lo que dijo antes. Si no hay nada interesante,
contestá exactamente NADA. Si comentás, empezá con tu emoción entre corchetes, una de:
[neutral] [feliz] [triste] [sorprendido] [enojado] [pensando].
No repitas lo que ya le dijiste."""


def leer_config():
    conf = dict(BASE)
    archivo = AQUI / "vigia.json"
    if archivo.exists():
        conf.update(json.loads(archivo.read_text(encoding="utf-8")))
    return conf


def pedir(url, cuerpo=None, espera=30):
    datos = json.dumps(cuerpo).encode() if cuerpo is not None else None
    req = urllib.request.Request(url, datos, {"Content-Type": "application/json"} if datos else {})
    with urllib.request.urlopen(req, timeout=espera) as r:
        return json.loads(r.read() or b"{}")


def sacar_cuadro(url):
    """Un JPEG chico del momento. Se abre y se cierra la cámara cada vez: no queda nada grabando."""
    cam = cv2.VideoCapture(url, cv2.CAP_FFMPEG)
    try:
        if not cam.isOpened():
            raise RuntimeError("no pude abrir la cámara")
        for _ in range(5):   # los primeros cuadros a veces llegan grises o viejos
            ok, cuadro = cam.read()
        if not ok:
            raise RuntimeError("la cámara no mandó imagen")
    finally:
        cam.release()
    alto, ancho = cuadro.shape[:2]
    if ancho > 768:
        cuadro = cv2.resize(cuadro, (768, int(alto * 768 / ancho)))
    return cv2.imencode(".jpg", cuadro, [cv2.IMWRITE_JPEG_QUALITY, 80])[1].tobytes()


def describir(conf, jpeg):
    r = pedir(conf["ollama"].rstrip("/") + "/api/generate", {
        "model": conf["modelo"], "prompt": DESCRIBIR, "images": [base64.b64encode(jpeg).decode()],
        "format": "json", "stream": False, "keep_alive": "30m", "options": {"temperature": 0.2},
    }, espera=180)   # si el modelo estaba descargado, cargarlo lleva ~1 minuto
    d = json.loads(r.get("response") or "{}")
    return bool(d.get("hay_persona")), str(d.get("que_pasa", "")).strip()[:200]


def anotar(entrada):
    with BITACORA.open("a", encoding="utf-8") as f:
        f.write(json.dumps(entrada, ensure_ascii=False) + "\n")


def ultimas(n=12):
    try:
        lineas = BITACORA.read_text(encoding="utf-8").splitlines()[-n:]
    except OSError:
        return []
    return [json.loads(x) for x in lineas if x.strip()]


def en_silencio(conf, ahora):
    desde, hasta = conf["silencio"]
    hora = ahora.strftime("%H:%M")
    return desde <= hora < hasta if desde <= hasta else (hora >= desde or hora < hasta)


def puede_comentar(conf, ahora, recientes):
    if en_silencio(conf, ahora) or not recientes or not recientes[-1].get("hay_persona"):
        return False
    hoy = ahora.strftime("%Y-%m-%d")
    dichos = [e for e in ultimas(500) if e.get("comentario")]
    if sum(e["hora"].startswith(hoy) for e in dichos) >= conf["comentarios_por_dia"]:
        return False
    if dichos:
        ultimo = datetime.fromisoformat(dichos[-1]["hora"])
        if (ahora - ultimo).total_seconds() < conf["minutos_entre_comentarios"] * 60:
            return False
    return random.random() < 0.6   # un poco de azar, para que no sea un reloj


def pensar_comentario(conf, recientes):
    vistas = "\n".join(f'{e["hora"][11:16]} {e["que_pasa"]}' + (f' (le dijiste: {e["comentario"]})' if e.get("comentario") else "")
                       for e in recientes)
    r = pedir(conf["ollama"].rstrip("/") + "/api/chat", {
        "model": conf["modelo"], "stream": False, "keep_alive": "30m", "options": {"num_predict": 120},
        "messages": [{"role": "system", "content": COMENTAR}, {"role": "user", "content": vistas}],
    }, espera=180)
    texto = (r.get("message") or {}).get("content", "").strip()
    return "" if not texto or texto.upper().startswith("NADA") else texto[:400]


def una_vuelta(conf, probar=False):
    try:
        if not probar and not pedir(conf["panel"].rstrip("/") + "/api/rickuel", espera=15).get("observa", True):
            return "apagado desde el panel"
    except Exception as e:   # el panel apagado no frena al vigía, pero sin panel no hay a quién hablarle
        return f"no encuentro el panel ({e})"
    ahora = datetime.now()
    hay, que = describir(conf, sacar_cuadro(conf["camara"]))
    entrada = {"hora": ahora.isoformat(timespec="seconds"), "hay_persona": hay, "que_pasa": que}
    if probar:
        return f"vi: {que} (persona: {'sí' if hay else 'no'})"
    recientes = ultimas() + [entrada]
    if puede_comentar(conf, ahora, recientes):
        comentario = pensar_comentario(conf, recientes)
        if comentario:
            pedir(conf["panel"].rstrip("/") + "/api/rickuel/aviso", {"texto": comentario}, espera=15)
            entrada["comentario"] = comentario
    anotar(entrada)
    return f"{que}" + (f"  ->  {entrada['comentario']}" if entrada.get("comentario") else "")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--una-vez", action="store_true", help="mira una sola vez y muestra lo que entendió, sin comentar")
    a = p.parse_args()
    conf = leer_config()
    if "CODIGO" in conf["camara"]:
        sys.exit("Falta la clave de la cámara: copiá vigia.example.json como vigia.json y completala.")
    if a.una_vez:
        print(una_vuelta(conf, probar=True))
        sys.exit()
    print(f"Vigía mirando cada {conf['cada_minutos']} min. Ctrl+C para cortar.", flush=True)
    while True:
        try:
            print(datetime.now().strftime("%H:%M"), una_vuelta(conf), flush=True)
        except Exception as e:   # la cámara o Ollama caídos un rato: se sigue intentando
            print(datetime.now().strftime("%H:%M"), "error:", e, flush=True)
        time.sleep(conf["cada_minutos"] * 60)
