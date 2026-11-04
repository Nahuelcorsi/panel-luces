"""El oído del amigo en la PC: pasa a texto lo que le decís a la botonera, con Whisper, sin mandar nada a internet.

La botonera graba mientras apretás su botón para hablar y le manda el WAV al panel del celular (/api/voz); el panel
se lo pasa a este programa y, si la PC está apagada, usa Gemini. Escucha en la red de la casa (el celular tiene que
llegar), en el puerto 8071:
  POST /voz/transcribir  (cuerpo: WAV mono de 8 o 16 bits)  ->  {"texto": "..."}
  GET  /salud                                           ->  {"ok": true}
Usa faster-whisper (el mismo de voz.py). El modelo se carga una sola vez al arrancar y queda en memoria.
  python oido_pc.py [--modelo small] [--puerto 8071]
En el panel, amigo.json puede decir "oido_url": "http://<IP de esta PC>:8071" (si no, prueba el puerto 8071 de la
PC del cerebro, la de "local_url").
"""
import argparse
import io
import json
import threading
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np
from faster_whisper import WhisperModel

MODELO = None
CANDADO = threading.Lock()   # Whisper de a una grabación por vez


def leer_wav(datos):
    """El WAV a 16000 muestras por segundo en float32 (lo que espera Whisper), sin pasar por ffmpeg ni av."""
    with wave.open(io.BytesIO(datos)) as w:
        canales, ancho, tasa = w.getnchannels(), w.getsampwidth(), w.getframerate()
        crudo = w.readframes(w.getnframes())
    if ancho == 1:   # la botonera manda 8 bits sin signo (128 = silencio)
        audio = (np.frombuffer(crudo, dtype=np.uint8).astype(np.float32) - 128) / 128
    elif ancho == 2:
        audio = np.frombuffer(crudo, dtype="<i2").astype(np.float32) / 32768
    else:
        raise ValueError("el WAV tiene que ser de 8 o 16 bits")
    if canales > 1:
        audio = audio.reshape(-1, canales).mean(axis=1)
    if tasa != 16000 and len(audio):
        t = np.arange(0, len(audio) * 16000 // tasa) * tasa / 16000
        audio = np.interp(t, np.arange(len(audio)), audio).astype(np.float32)
    return audio


def transcribir(datos):
    audio = leer_wav(datos)
    with CANDADO:
        partes, _ = MODELO.transcribe(audio, language="es", beam_size=5, vad_filter=True)
        return " ".join(p.text.strip() for p in partes).strip()


class Oido(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, datos, codigo=200):
        cuerpo = json.dumps(datos, ensure_ascii=False).encode()
        self.send_response(codigo)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(cuerpo)))
        self.end_headers()
        self.wfile.write(cuerpo)

    def do_GET(self):
        if self.path == "/salud":
            return self._json({"ok": True})
        self.send_error(404)

    def do_POST(self):
        if self.path != "/voz/transcribir":
            return self.send_error(404)
        try:
            datos = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            t = time.time()
            texto = transcribir(datos)
            print(f"{time.strftime('%H:%M:%S')} ({time.time() - t:.1f} s) {texto}", flush=True)
            self._json({"texto": texto})
        except Exception as e:
            self._json({"error": {"message": str(e)}}, 500)


class Servidor(ThreadingHTTPServer):
    allow_reuse_address = False   # en Windows, reusar el puerto dejaría abrir dos a la vez


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--modelo", default="small")
    p.add_argument("--puerto", type=int, default=8071)
    a = p.parse_args()
    try:
        servidor = Servidor(("0.0.0.0", a.puerto), Oido)   # el celular del panel tiene que poder llegar
    except OSError:
        raise SystemExit(f"El puerto {a.puerto} ya está en uso: el oído ya estaba abierto.")
    print(f"Cargando Whisper ({a.modelo})...", flush=True)
    try:
        MODELO = WhisperModel(a.modelo, device="cuda", compute_type="float16")
    except Exception:   # sin placa de video NVIDIA: en el procesador
        MODELO = WhisperModel(a.modelo, device="cpu", compute_type="int8")
    print(f"El oído del amigo escucha en el puerto {a.puerto}", flush=True)
    servidor.serve_forever()
