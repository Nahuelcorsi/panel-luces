"""Rickuel en la PC: abre la pantalla de Rickuel del panel en el navegador de la PC, con su micrófono y sus parlantes.

El navegador solo deja usar el micrófono en páginas seguras (https o localhost), y el panel es http://<celular>:8080.
Por eso este programa hace de puente: sirve en http://localhost:8070 todo lo que le pide al panel, y la página
se abre con ?pantalla=pc para que el panel sepa que es la PC (y Rickuel hable por acá cuando la estés usando).
Solo usa la biblioteca estándar:
  python rickuel_pc.py --panel http://192.168.0.20:8080 [--puerto 8070] [--sin-abrir]
"""
import argparse
import urllib.error
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PANEL = ""
ESPERA = 120   # el cerebro puede tardar más de un minuto si el modelo se estaba cargando


class Puente(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _pasar(self, cuerpo=None):
        req = urllib.request.Request(PANEL + self.path, data=cuerpo, method=self.command)
        if self.headers.get("Content-Type"):
            req.add_header("Content-Type", self.headers["Content-Type"])
        try:
            with urllib.request.urlopen(req, timeout=ESPERA) as r:
                codigo, tipo, datos = r.status, r.headers.get("Content-Type", ""), r.read()
        except urllib.error.HTTPError as e:
            codigo, tipo, datos = e.code, e.headers.get("Content-Type", ""), e.read()
        except Exception as e:   # el celular apagado o fuera de la red
            codigo, tipo = 502, "application/json"
            datos = ('{"error": {"message": "No encuentro el panel en %s (%s)"}}' % (PANEL, e)).encode()
        self.send_response(codigo)
        if tipo:
            self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(len(datos)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(datos)

    def do_GET(self):
        self._pasar()

    def do_POST(self):
        self._pasar(self.rfile.read(int(self.headers.get("Content-Length", 0))))


class Servidor(ThreadingHTTPServer):
    allow_reuse_address = False   # en Windows, reusar el puerto dejaría abrir dos puentes a la vez


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--panel", required=True, help="dirección del panel, por ejemplo http://192.168.0.20:8080")
    p.add_argument("--puerto", type=int, default=8070)
    p.add_argument("--sin-abrir", action="store_true", help="no abrir el navegador")
    a = p.parse_args()
    PANEL = a.panel.rstrip("/")
    url = f"http://localhost:{a.puerto}/rickuel/?panel&pantalla=pc"
    try:
        servidor = Servidor(("127.0.0.1", a.puerto), Puente)   # solo para esta PC
    except OSError:   # ya estaba abierto: alcanza con abrir la página
        webbrowser.open(url)
        raise SystemExit
    print(f"Rickuel en la PC: {url} (panel: {PANEL})")
    if not a.sin_abrir:
        webbrowser.open(url)
    servidor.serve_forever()
