"""Voz de Rickuel para el panel: servidor chico que corre en la PC y convierte texto en audio con la voz entrenada.

Una sola voz para todos lados: el panel (celu, PC, notebook) le pide el audio por POST /hablar {"texto": "..."}
(ver "voz_url" en rickuel.json) y devuelve un WAV.
- Busca solo el modelo en la carpeta "compañero de escritorio" de OneDrive (voz-rick/probar/modelo), así cuando
  entrenás una voz nueva y la pegás ahí, alcanza con reiniciar esto. Si no está, lo busca al lado de este archivo
  (es_MX-rick-medium.onnx + .onnx.json). También se puede pasar --modelo.
- Los "uuurp" suenan como eructo (voz-rick/eructo*.wav si hay; si no, uno sintético) y no lee lo que va entre asteriscos.
- Menos ruido de Piper que el que trae el modelo: la voz sale más limpia (--ruido / --ruido-w para ajustar).
Se corre con el Python que tenga Piper instalado (piper-tts):
  python voz_pc.py [--modelo RUTA/es_MX-rick-medium.onnx] [--puerto 8090]
"""
import argparse
import io
import json
import math
import random
import re
import shutil
import tempfile
import threading
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from piper import PiperVoice, SynthesisConfig

MAX_TEXTO = 600
voz = None
VOZ_RICK = None              # carpeta voz-rick (para los eructos)
RUIDO, RUIDO_W = 0.45, 0.6
candado = threading.Lock()   # Piper no es para usarlo desde varios hilos a la vez

ERUCTO = re.compile(r"\*?\b(?:b?u+r+p+|e+r+p+|b?u+rr+p*)\b\*?[.!]?", re.I)
CANDIDATAS = [
    Path.home() / "OneDrive/Desktop/Programacion/Proyectos-personales/compañero de escritorio/voz-rick",
    Path.home() / "Desktop/Programacion/Proyectos-personales/compañero de escritorio/voz-rick",
    Path(__file__).resolve().parent.parent.parent / "compañero de escritorio/voz-rick",
]


def buscar_voz_rick():
    return next((c for c in CANDIDATAS if (c / "probar/modelo/es_MX-rick-medium.onnx").exists()), None)


def carpeta_espeak():
    """espeak-ng no abre rutas con letras como la ñ en Windows: si hace falta, copia sus datos de pronunciación a la
    carpeta temporal, que tiene una ruta sin acentos."""
    from piper.phonemize_espeak import ESPEAK_DATA_DIR
    if str(ESPEAK_DATA_DIR).isascii():
        return ESPEAK_DATA_DIR
    copia = Path(tempfile.gettempdir()) / "rickuel-espeak-ng-data"
    if not (copia / "phontab").exists():
        shutil.copytree(ESPEAK_DATA_DIR, copia, dirs_exist_ok=True)
    return copia


def limpiar_para_hablar(texto):
    texto = re.sub(r"\[\w+\]", "", texto)            # etiquetas de emoción como [feliz]
    texto = re.sub(r"\*[^*]{0,40}\*", " ", texto)     # acotaciones entre asteriscos (*suspira*)
    return " ".join(texto.replace("*", "").split())


def eructo(sr):
    """Un eructo real si hay voz-rick/eructo*.wav (si hay varios, uno al azar); si no, uno sintético."""
    import array
    reales = sorted(VOZ_RICK.glob("eructo*.wav")) if VOZ_RICK else []
    if not reales:
        return eructo_sintetico(sr)
    with wave.open(str(random.choice(reales))) as w:
        sr_orig, canales, ancho = w.getframerate(), w.getnchannels(), w.getsampwidth()
        datos = w.readframes(w.getnframes())
    if ancho != 2:
        return eructo_sintetico(sr)
    m = array.array("h", datos)[::canales]
    if sr_orig != sr:
        n = int(len(m) * sr / sr_orig)
        m = array.array("h", (m[min(int(i * sr_orig / sr), len(m) - 1)] for i in range(n)))
    return m.tobytes()


def eructo_sintetico(sr: int, dur=0.55) -> bytes:
    """Eructo sintético: pulsos graves y rasposos (como fritura vocal) que bajan de tono, pasados por dos
    resonancias de vocal "o/u". Solo biblioteca estándar."""
    import array
    rnd = random.Random(7)
    n = int(sr * dur)

    def resonador(f, q):
        w = 2 * math.pi * f / sr
        al = math.sin(w) / (2 * q)
        b0, a0, a1, a2 = al, 1 + al, -2 * math.cos(w), 1 - al
        return b0 / a0, a1 / a0, a2 / a0

    filtros = [resonador(420, 4), resonador(950, 5)]
    estados = [[0.0, 0.0, 0.0, 0.0] for _ in filtros]
    fase, amp_ciclo, salida = 0.0, 1.0, []
    for i in range(n):
        t = i / n
        f0 = 88 - 38 * t + rnd.uniform(-6, 6)
        fase += f0 / sr
        if fase >= 1:
            fase -= 1
            amp_ciclo = rnd.uniform(0.5, 1.0)        # cada pulso con distinta fuerza: lo vuelve rasposo
        x = math.exp(-fase * 18) * amp_ciclo + rnd.uniform(-0.04, 0.04)
        y = 0.0
        for (b0, a1, a2), e in zip(filtros, estados):
            v = b0 * x - b0 * e[1] - a1 * e[2] - a2 * e[3]   # pasabanda: b0*(x - x[n-2])
            e[1], e[0] = e[0], x
            e[3], e[2] = e[2], v
            y += v
        env = min(1, t * 25) * (1 - t) ** 0.8
        salida.append(y * env)
    # mismo volumen que la voz (RMS ~0.2), con saturación suave para que suene más gordo
    rms = math.sqrt(sum(v * v for v in salida) / n) or 1
    return array.array("h", (int(math.tanh(v / rms * 0.25) * 30000) for v in salida)).tobytes()



def frase(texto):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        voz.synthesize_wav(texto, w, SynthesisConfig(noise_scale=RUIDO, noise_w_scale=RUIDO_W))
    buf.seek(0)
    with wave.open(buf) as w:
        return w.getframerate(), w.readframes(w.getnframes())


def sintetizar(texto):
    sr = voz.config.sample_rate
    silencio = b"\0\0" * int(sr * 0.08)
    audio = b""
    partes = ERUCTO.split(texto)
    with candado:
        for i, parte in enumerate(partes):
            parte = limpiar_para_hablar(parte)
            if re.search(r"\w", parte):
                sr, pcm = frase(parte)
                audio += pcm
            if i < len(partes) - 1:
                audio += silencio + eructo(sr) + silencio
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(audio or silencio)
    return buf.getvalue()


class Manejador(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _enviar(self, codigo, cuerpo, tipo):
        self.send_response(codigo)
        self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(len(cuerpo)))
        self.end_headers()
        self.wfile.write(cuerpo)

    def do_GET(self):
        self._enviar(200, b'{"voz": true}', "application/json")

    def do_POST(self):
        try:
            largo = int(self.headers.get("Content-Length", 0))
            texto = str(json.loads(self.rfile.read(largo) or b"{}").get("texto", "")).strip()[:MAX_TEXTO]
            if not texto:
                raise ValueError("texto vacío")
            self._enviar(200, sintetizar(texto), "audio/wav")
        except Exception as e:
            self._enviar(400, json.dumps({"error": {"message": str(e)}}).encode(), "application/json")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--modelo", help="si no se pasa, lo busca en compañero de escritorio/voz-rick/probar/modelo")
    p.add_argument("--puerto", type=int, default=8090)
    p.add_argument("--ruido", type=float, default=RUIDO)
    p.add_argument("--ruido-w", type=float, default=RUIDO_W)
    a = p.parse_args()
    RUIDO, RUIDO_W = a.ruido, a.ruido_w
    VOZ_RICK = buscar_voz_rick()
    modelo = Path(a.modelo) if a.modelo else (VOZ_RICK / "probar/modelo/es_MX-rick-medium.onnx" if VOZ_RICK else None)
    junto = Path(__file__).resolve().parent / "es_MX-rick-medium.onnx"   # o pegado al lado de este archivo
    if (not modelo or not modelo.exists()) and junto.exists():
        modelo = junto
    if not modelo or not modelo.exists():
        raise SystemExit("No encuentro el modelo de voz. Pasalo con --modelo RUTA/es_MX-rick-medium.onnx")
    if a.modelo and not VOZ_RICK:
        VOZ_RICK = modelo.parent.parent.parent if modelo.parent.name == "modelo" else None
    voz = PiperVoice.load(modelo, espeak_data_dir=carpeta_espeak())
    print(f"Voz de Rickuel lista en el puerto {a.puerto} ({modelo})")
    ThreadingHTTPServer(("0.0.0.0", a.puerto), Manejador).serve_forever()
