"""Voz de Rickuel para el panel: servidor chico que corre en la PC y convierte texto en audio con la voz entrenada.

Se corre con el Python que tenga Piper instalado (piper-tts):
  python voz_pc.py --modelo RUTA/es_MX-rick-medium.onnx [--puerto 8090]
El panel le pide el audio por POST /hablar {"texto": "..."} (ver "voz_url" en rickuel.json) y devuelve un WAV.
"""
import argparse
import io
import json
import threading
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from piper import PiperVoice

MAX_TEXTO = 600
voz = None
candado = threading.Lock()   # Piper no es para usarlo desde varios hilos a la vez


def sintetizar(texto):
    buf = io.BytesIO()
    with candado, wave.open(buf, "wb") as wav:
        voz.synthesize_wav(texto, wav)
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
    p.add_argument("--modelo", required=True)
    p.add_argument("--puerto", type=int, default=8090)
    a = p.parse_args()
    voz = PiperVoice.load(a.modelo)
    print(f"Voz de Rickuel lista en el puerto {a.puerto}")
    ThreadingHTTPServer(("0.0.0.0", a.puerto), Manejador).serve_forever()
