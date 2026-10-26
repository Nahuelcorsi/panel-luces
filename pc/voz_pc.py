"""Voz del amigo para el panel: servidor chico que corre en la PC y convierte texto en audio con Piper.

Una sola voz para todos lados: el panel (celu, PC, notebook) le pide el audio por POST /hablar {"texto": "..."}
(ver "voz_url" en amigo.json) y devuelve un WAV.
- Usa el primer modelo .onnx (con su .onnx.json al lado) que encuentre en pc/voces/. Cuando entrenes una voz propia,
  la pegás ahí (y sacás la otra) y alcanza con reiniciar esto. También se puede pasar --modelo.
- Voz de dibujito: --tono sube el tono sin cambiar la velocidad (1 = la voz tal cual, 1.3 = aguda, por defecto).
  Truco: Piper habla más lento en la misma proporción y el WAV se marca con una frecuencia más alta, así se
  reproduce más rápido y más agudo, y queda a velocidad normal.
- No lee lo que va entre asteriscos ni las etiquetas de emoción.
Se corre con el Python que tenga Piper instalado (piper-tts):
  python voz_pc.py [--modelo RUTA/voz.onnx] [--tono 1.3] [--puerto 8090]
"""
import argparse
import io
import json
import re
import shutil
import tempfile
import threading
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from piper import PiperVoice, SynthesisConfig

MAX_TEXTO = 600
VOCES = Path(__file__).resolve().parent / "voces"
voz = None
TONO = 1.3
RUIDO, RUIDO_W = 0.45, 0.6
candado = threading.Lock()   # Piper no es para usarlo desde varios hilos a la vez


def buscar_modelo():
    return next((m for m in sorted(VOCES.glob("*.onnx")) if m.with_suffix(".onnx.json").exists()), None)


def carpeta_espeak():
    """espeak-ng no abre rutas con letras como la ñ en Windows: si hace falta, copia sus datos de pronunciación a la
    carpeta temporal, que tiene una ruta sin acentos."""
    from piper.phonemize_espeak import ESPEAK_DATA_DIR
    if str(ESPEAK_DATA_DIR).isascii():
        return ESPEAK_DATA_DIR
    copia = Path(tempfile.gettempdir()) / "amigo-espeak-ng-data"
    if not (copia / "phontab").exists():
        shutil.copytree(ESPEAK_DATA_DIR, copia, dirs_exist_ok=True)
    return copia


def limpiar_para_hablar(texto):
    texto = re.sub(r"\[\w+\]", "", texto)            # etiquetas de emoción como [feliz]
    texto = re.sub(r"\*[^*]{0,40}\*", " ", texto)     # acotaciones entre asteriscos (*suspira*)
    return " ".join(texto.replace("*", "").split())


def sintetizar(texto):
    texto = limpiar_para_hablar(texto)
    sr = voz.config.sample_rate
    audio = b""
    if re.search(r"\w", texto):
        buf = io.BytesIO()
        with candado, wave.open(buf, "wb") as w:
            voz.synthesize_wav(texto, w, SynthesisConfig(length_scale=TONO, noise_scale=RUIDO, noise_w_scale=RUIDO_W))
        buf.seek(0)
        with wave.open(buf) as w:
            sr, audio = w.getframerate(), w.readframes(w.getnframes())
    salida = io.BytesIO()
    with wave.open(salida, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(int(sr * TONO))
        w.writeframes(audio or b"\0\0" * int(sr * 0.08))
    return salida.getvalue()


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
    p.add_argument("--modelo", help="si no se pasa, usa el primero que haya en pc/voces/")
    p.add_argument("--tono", type=float, default=TONO, help="1 = voz tal cual; más alto = más aguda")
    p.add_argument("--puerto", type=int, default=8090)
    p.add_argument("--ruido", type=float, default=RUIDO)
    p.add_argument("--ruido-w", type=float, default=RUIDO_W)
    a = p.parse_args()
    TONO, RUIDO, RUIDO_W = max(0.5, min(2.0, a.tono)), a.ruido, a.ruido_w
    modelo = Path(a.modelo) if a.modelo else buscar_modelo()
    if not modelo or not modelo.exists():
        raise SystemExit(f"No encuentro el modelo de voz: poné un .onnx con su .onnx.json en {VOCES} o pasalo con --modelo")
    voz = PiperVoice.load(modelo, espeak_data_dir=carpeta_espeak())
    print(f"Voz del amigo lista en el puerto {a.puerto} ({modelo.name}, tono {TONO})")
    ThreadingHTTPServer(("0.0.0.0", a.puerto), Manejador).serve_forever()
