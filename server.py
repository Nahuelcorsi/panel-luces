"""Panel de luces: servidor local para luces Tuya / Smart Life.

Uso:
    python server.py          # usa devices.json (generado por el asistente de TinyTuya)
    python server.py --demo   # luces falsas, para ver el panel sin luces reales
"""
import colorsys
import json
import os
import ssl
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BASE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(BASE, "web")
PUERTO = 8080
DEMO = "--demo" in sys.argv
TIPOS = {".html": "text/html; charset=utf-8", ".svg": "image/svg+xml",
         ".webmanifest": "application/manifest+json", ".js": "text/javascript", ".css": "text/css"}

if not DEMO:
    import tinytuya


class LuzFalsa:
    """Luz simulada para el modo demo."""

    def __init__(self, nombre, ident):
        self.nombre, self.id = nombre, ident
        self.encendida, self.brillo, self.color = True, 80, "#ffb347"

    def estado(self):
        return {"id": self.id, "nombre": self.nombre, "online": True,
                "encendida": self.encendida, "brillo": self.brillo, "color": self.color}

    def aplicar(self, cambios):
        if "encendida" in cambios:
            self.encendida = bool(cambios["encendida"])
        if "brillo" in cambios:
            self.brillo = int(cambios["brillo"])
        if "color" in cambios:
            self.color = cambios["color"]
        if "blanco" in cambios:
            self.color = ("#ffb86b", "#fff1dc", "#cfe3ff")[min(2, int(cambios["blanco"]) // 34)]


def color_hex(estado):
    """Color a mostrar. En modo color viene como HSV hexadecimal (HHHHSSSSVVVV); en blanco, un tono cálido."""
    c = estado.get("colour")
    if estado.get("mode") != "colour" or not isinstance(c, str) or len(c) != 12:
        t = min(1, max(0, (estado.get("colourtemp") or 0) / 1000))
        cal, fri = (255, 184, 107), (207, 227, 255)
        return "#%02x%02x%02x" % tuple(round(a + (b - a) * t) for a, b in zip(cal, fri))
    h, s = int(c[0:4], 16) / 360, int(c[4:8], 16) / 1000
    r, g, b = colorsys.hsv_to_rgb(h, s, 1)
    return "#%02x%02x%02x" % (round(r * 255), round(g * 255), round(b * 255))


class LuzTuya:
    """Luz real controlada por la red local con TinyTuya."""

    def __init__(self, datos):
        self.datos = datos
        self.nombre = datos.get("name") or datos["id"]
        self.id = datos["id"]
        self.lock = threading.Lock()
        self.dev = None
        self.ultimo_fallo = 0

    def _conectar(self):
        """Crea el dispositivo la primera vez que se lo necesita; reintenta si no lo encuentra."""
        if self.dev is None:
            if time.time() - self.ultimo_fallo < 60:
                raise RuntimeError("luz no encontrada, reintenta en un rato")
            d = self.datos
            try:
                dev = tinytuya.BulbDevice(
                    d["id"], d.get("ip") or "Auto", d["key"],
                    version=float(d.get("version") or 3.3))
            except Exception:
                self.ultimo_fallo = time.time()
                raise
            dev.set_socketTimeout(3)
            dev.set_socketRetryLimit(1)
            self.dev = dev
        return self.dev

    def estado(self):
        base = {"id": self.id, "nombre": self.nombre}
        try:
            with self.lock:
                e = self._conectar().state()
            if "Error" in e:
                return {**base, "online": False}
            return {**base, "online": True, "encendida": bool(e.get("is_on")),
                    "brillo": max(1, round(e.get("brightness", 1000) / 10)),
                    "color": color_hex(e)}
        except Exception:
            return {**base, "online": False}

    def aplicar(self, cambios):
        with self.lock:
            dev = self._conectar()
            if "color" in cambios:
                c = cambios["color"].lstrip("#")
                dev.set_colour(int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16))
            if "blanco" in cambios:
                brillo = max(1, min(100, int(cambios.get("brillo") or 100)))
                dev.set_white_percentage(brillo, max(0, min(100, int(cambios["blanco"]))))
            elif "brillo" in cambios:
                dev.set_brightness_percentage(max(1, min(100, int(cambios["brillo"]))))
            if "encendida" in cambios:
                (dev.turn_on if cambios["encendida"] else dev.turn_off)()


def cargar_luces():
    if DEMO:
        return [LuzFalsa("Dormitorio", "demo1"), LuzFalsa("Lámpara", "demo2")]
    ruta = os.path.join(BASE, "devices.json")
    if not os.path.exists(ruta):
        sys.exit("Falta devices.json. Generalo con: python -m tinytuya wizard")
    with open(ruta, encoding="utf-8") as f:
        return [LuzTuya(d) for d in json.load(f) if d.get("key")]


LUCES = {}


# ---- Viaje al trabajo y clima ----
# lugares.json: direcciones de casa y trabajo, hora de llegada y clave de TomTom (ver lugares.example.json).

def cargar_lugares():
    ruta = os.path.join(BASE, "lugares.json")
    if not os.path.exists(ruta):
        return {}
    with open(ruta, encoding="utf-8") as f:
        return json.load(f)


try:
    # En Windows al almacén de certificados de Python le pueden faltar raíces (como la de TomTom); certifi las trae todas.
    import certifi
    SSL = ssl.create_default_context(cafile=certifi.where())
except ImportError:
    SSL = ssl.create_default_context()


def pedir_json(url, intentos=2):
    req = urllib.request.Request(url, headers={"User-Agent": "panel-luces/1.0 (panel casero)"})
    for n in range(intentos):
        try:
            with urllib.request.urlopen(req, timeout=10, context=SSL) as r:
                return json.load(r)
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            if n == intentos - 1:
                raise
            time.sleep(1)


class Cache:
    """Guarda el último resultado de una función durante unos segundos."""

    def __init__(self, segundos, funcion):
        self.segundos, self.funcion = segundos, funcion
        self.lock = threading.Lock()
        self.valor, self.cuando = None, 0

    def obtener(self):
        with self.lock:
            vigencia = 30 if isinstance(self.valor, dict) and self.valor.get("ok") is False else self.segundos
            if self.valor is None or time.time() - self.cuando > vigencia:
                self.valor, self.cuando = self.funcion(), time.time()
            return self.valor


COORDENADAS = {}


def coordenadas(lugar):
    """Lat/lon del lugar: las del archivo si están, si no busca la dirección en OpenStreetMap (una sola vez)."""
    if lugar.get("lat") is not None and lugar.get("lon") is not None:
        return float(lugar["lat"]), float(lugar["lon"])
    direccion = (lugar.get("direccion") or "").strip()
    if not direccion:
        raise ValueError("falta la dirección")
    if direccion not in COORDENADAS:
        r = pedir_json("https://nominatim.openstreetmap.org/search?format=json&limit=1&q="
                       + urllib.parse.quote(direccion))
        if not r:
            raise ValueError(f"no encontré la dirección «{direccion}»")
        COORDENADAS[direccion] = (float(r[0]["lat"]), float(r[0]["lon"]))
    return COORDENADAS[direccion]


def proxima_llegada(cfg, ahora):
    """Próximo día laboral (lunes a sábado por defecto) a la hora de llegada que todavía no pasó."""
    h, m = (int(x) for x in cfg.get("llegada", "08:00").split(":"))
    dias = cfg.get("dias_laborales", [1, 2, 3, 4, 5, 6])  # 1 = lunes … 7 = domingo
    for n in range(8):
        dia = (ahora + timedelta(days=n)).replace(hour=h, minute=m, second=0, microsecond=0)
        if dia.isoweekday() in dias and dia > ahora:
            return dia
    raise ValueError("no hay días laborales configurados")


def calcular_viaje():
    cfg = cargar_lugares()
    ahora = datetime.now().astimezone()
    if DEMO:
        llegada = proxima_llegada(cfg, ahora)
        return {"ok": True, "llegada": llegada.isoformat(), "salida": (llegada - timedelta(minutes=47)).isoformat(),
                "minutos": 47, "demora": 9, "km": 31.4}
    clave = (cfg.get("tomtom_key") or "").strip()
    if not cfg.get("casa") or not cfg.get("trabajo"):
        return {"ok": False, "error": "Falta configurar las direcciones en lugares.json"}
    if not clave:
        return {"ok": False, "error": "Falta la clave de TomTom en lugares.json"}
    try:
        (la1, lo1), (la2, lo2) = coordenadas(cfg["casa"]), coordenadas(cfg["trabajo"])
        llegada = proxima_llegada(cfg, ahora)
        # Con arriveAt, TomTom calcula a qué hora salir; cerca de la salida usa el tráfico en vivo.
        url = (f"https://api.tomtom.com/routing/1/calculateRoute/{la1},{lo1}:{la2},{lo2}/json"
               f"?key={urllib.parse.quote(clave)}&traffic=true&travelMode=car"
               f"&arriveAt={urllib.parse.quote(llegada.isoformat(timespec='seconds'))}")
        resumen = pedir_json(url)["routes"][0]["summary"]
        return {"ok": True, "llegada": llegada.isoformat(), "salida": resumen["departureTime"],
                "minutos": round(resumen["travelTimeInSeconds"] / 60),
                "demora": round(resumen.get("trafficDelayInSeconds", 0) / 60),
                "km": round(resumen["lengthInMeters"] / 1000, 1)}
    except Exception as e:
        return {"ok": False, "error": f"No pude calcular el viaje: {e}"}


def clima_de(lugar):
    la, lo = coordenadas(lugar)
    d = pedir_json(
        f"https://api.open-meteo.com/v1/forecast?latitude={la}&longitude={lo}&timezone=auto&forecast_days=2"
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
        "&hourly=precipitation_probability&daily=temperature_2m_max,temperature_2m_min")
    c, dia, h = d["current"], d["daily"], d["hourly"]
    # Probabilidad de lluvia de las próximas horas (no la del día entero, que incluye horas que ya pasaron)
    i = h["time"].index(c["time"][:13] + ":00")
    prox = [x or 0 for x in h["precipitation_probability"][i:]]
    return {"temp": c["temperature_2m"], "sensacion": c["apparent_temperature"], "humedad": c["relative_humidity_2m"],
            "codigo": c["weather_code"], "viento": c["wind_speed_10m"], "dia": bool(c["is_day"]),
            "max": dia["temperature_2m_max"][0], "min": dia["temperature_2m_min"][0],
            "lluvia_3h": max(prox[:3]), "lluvia_12h": max(prox[:12])}


def calcular_clima():
    cfg = cargar_lugares()
    salida = {}
    for clave, nombre in (("casa", "Casa"), ("trabajo", "Trabajo")):
        if DEMO:
            salida[clave] = {"nombre": nombre, "ok": True, "temp": 18.4 if clave == "casa" else 20.1, "sensacion": 17.2,
                             "humedad": 64, "codigo": 2 if clave == "casa" else 61, "viento": 12, "dia": True,
                             "max": 22, "min": 11, "lluvia_3h": 10 if clave == "casa" else 70,
                             "lluvia_12h": 30 if clave == "casa" else 80}
            continue
        try:
            salida[clave] = {"nombre": nombre, "ok": True, **clima_de(cfg.get(clave) or {})}
        except Exception as e:
            salida[clave] = {"nombre": nombre, "ok": False, "error": str(e)}
    salida["ok"] = all(v["ok"] for v in salida.values())   # si algo falló, la caché lo reintenta antes
    return salida


VIAJE = Cache(5 * 60, calcular_viaje)
CLIMA = Cache(10 * 60, calcular_clima)


# ---- Despertador y modo trabajo ----
# Corren en el servidor, así funcionan aunque la pantalla del panel esté apagada.
# ajustes.json guarda lo que se elige en el panel (hora, días, luces…).

RUTA_AJUSTES = os.path.join(BASE, "ajustes-demo.json" if DEMO else "ajustes.json")
AJUSTES_BASE = {
    "despertador": {"activo": False, "hora": "06:30", "dias": [1, 2, 3, 4, 5, 6], "luces": None, "minutos": 20},
    "trabajo": {"activo": False, "luces": None, "minutos": 30},
}
LOCK_AJUSTES = threading.Lock()


def leer_ajustes():
    ajustes = json.loads(json.dumps(AJUSTES_BASE))
    if os.path.exists(RUTA_AJUSTES):
        with open(RUTA_AJUSTES, encoding="utf-8") as f:
            for modo, valores in json.load(f).items():
                if modo in ajustes:
                    ajustes[modo].update(valores)
    for modo in ajustes.values():
        if modo["luces"] is None:          # sin elegir todavía: todas las luces
            modo["luces"] = list(LUCES)
    return ajustes


def guardar_ajustes(nuevos):
    with LOCK_AJUSTES:
        ajustes = leer_ajustes()
        for modo, valores in nuevos.items():
            if modo in ajustes and isinstance(valores, dict):
                ajustes[modo].update({k: v for k, v in valores.items() if k in AJUSTES_BASE[modo]})
        d = ajustes["despertador"]
        h, m = (int(x) for x in d["hora"].split(":"))
        d["hora"] = f"{h % 24:02d}:{m % 60:02d}"
        for modo in ajustes.values():
            modo["minutos"] = max(1, min(120, int(modo["minutos"])))
        with open(RUTA_AJUSTES, "w", encoding="utf-8") as f:
            json.dump(ajustes, f, ensure_ascii=False, indent=2)
        return ajustes


def mezclar(a, b, t):
    a, b = a.lstrip("#"), b.lstrip("#")
    return "#" + "".join("%02x" % round(int(a[i:i + 2], 16) + (int(b[i:i + 2], 16) - int(a[i:i + 2], 16)) * t)
                         for i in (0, 2, 4))


def amanecer(p):
    """Paso del despertador (p de 0 a 1): rojo anaranjado muy tenue, después naranja y al final blanco cálido fuerte."""
    if p < .6:
        q = p / .6
        return {"encendida": True, "color": mezclar("#ff3000", "#ffa040", q), "brillo": round(1 + q * 34)}
    q = (p - .6) / .4
    return {"encendida": True, "blanco": round(q * 35), "brillo": round(35 + q * 65)}


def semaforo(p):
    """Paso del modo trabajo (p de 0 a 1): de verde a rojo pasando por amarillo."""
    r, g, b = colorsys.hsv_to_rgb((1 - p) / 3, 1, 1)
    return {"encendida": True, "color": "#%02x%02x%02x" % (round(r * 255), round(g * 255), round(b * 255)), "brillo": 100}


ENVIADO = {}   # (modo, id de luz) -> último cambio mandado, para no repetir lo mismo cada vuelta


def aplicar_a(ids, cambios, modo):
    for ident in ids:
        luz = LUCES.get(ident)
        if luz is None or ENVIADO.get((modo, ident)) == cambios:
            continue
        try:
            luz.aplicar(cambios)
            ENVIADO[(modo, ident)] = cambios
        except Exception as e:
            print(f"{modo}: no pude cambiar {luz.nombre}: {e}")


def parpadear(ids, veces=4):
    """Aviso de que es la hora de salir: parpadea en rojo y queda roja."""
    rojo = {"encendida": True, "color": "#ff0000", "brillo": 100}
    for _ in range(veces):
        aplicar_a(ids, {"encendida": False}, "aviso")
        time.sleep(.7)
        aplicar_a(ids, rojo, "aviso")
        time.sleep(.7)
    ENVIADO.clear()


HECHO = set()   # (modo, fecha) que ya terminaron, para no repetirlos


def vuelta_automatica(ahora):
    a = leer_ajustes()
    d = a["despertador"]
    if d["activo"] and ahora.isoweekday() in d["dias"] and ("despertador", ahora.date()) not in HECHO:
        h, m = (int(x) for x in d["hora"].split(":"))
        hora = ahora.replace(hour=h, minute=m, second=0, microsecond=0)
        inicio = hora - timedelta(minutes=d["minutos"])
        if inicio <= ahora <= hora + timedelta(minutes=2):
            aplicar_a(d["luces"], amanecer(min(1, (ahora - inicio) / (hora - inicio))), "despertador")
            if ahora >= hora:
                HECHO.add(("despertador", ahora.date()))
    t = a["trabajo"]
    if t["activo"]:
        v = VIAJE.obtener()
        if v.get("ok"):
            salida = datetime.fromisoformat(v["salida"])
            inicio = salida - timedelta(minutes=t["minutos"])
            if ("trabajo", salida.date()) in HECHO:
                return
            if inicio <= ahora < salida:
                aplicar_a(t["luces"], semaforo((ahora - inicio) / (salida - inicio)), "trabajo")
            elif salida <= ahora <= salida + timedelta(minutes=5):
                parpadear(t["luces"])
                HECHO.add(("trabajo", salida.date()))


def bucle_automatico():
    while True:
        try:
            vuelta_automatica(datetime.now().astimezone())
        except Exception as e:
            print("Error en despertador/modo trabajo:", e)
        time.sleep(15)


def probar(modo, ids):
    """Versión rápida para ver cómo queda: el despertador en 30 s y el modo trabajo en 20 s más el parpadeo."""
    pasos, funcion = (15, amanecer) if modo == "despertador" else (10, semaforo)
    for i in range(pasos + 1):
        aplicar_a(ids, funcion(i / pasos), "prueba")
        time.sleep(2)
    if modo == "trabajo":
        parpadear(ids)
    ENVIADO.clear()


class Handler(BaseHTTPRequestHandler):
    def _json(self, datos, codigo=200):
        cuerpo = json.dumps(datos).encode()
        self.send_response(codigo)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(cuerpo)))
        self.end_headers()
        self.wfile.write(cuerpo)

    def do_GET(self):
        if self.path == "/api/luces":
            return self._json([l.estado() for l in LUCES.values()])
        if self.path == "/api/viaje":
            return self._json(VIAJE.obtener())
        if self.path == "/api/clima":
            return self._json(CLIMA.obtener())
        if self.path == "/api/ajustes":
            return self._json(leer_ajustes())
        self._estatico()

    def _estatico(self):
        """Sirve los archivos de web/ (solo de esa carpeta)."""
        ruta = self.path.split("?")[0]
        destino = os.path.realpath(os.path.join(WEB, ("index.html" if ruta == "/" else ruta.lstrip("/"))))
        if not destino.startswith(os.path.realpath(WEB) + os.sep) or not os.path.isfile(destino):
            return self.send_error(404)
        with open(destino, "rb") as f:
            cuerpo = f.read()
        self.send_response(200)
        self.send_header("Content-Type", TIPOS.get(os.path.splitext(destino)[1], "application/octet-stream"))
        self.send_header("Content-Length", str(len(cuerpo)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(cuerpo)

    def do_POST(self):
        try:
            largo = int(self.headers.get("Content-Length", 0))
            cuerpo = json.loads(self.rfile.read(largo) or b"{}")
        except Exception as e:
            return self._json({"error": str(e)}, 400)
        if self.path == "/api/ajustes":
            try:
                return self._json(guardar_ajustes(cuerpo))
            except Exception as e:
                return self._json({"error": str(e)}, 400)
        if self.path in ("/api/probar/despertador", "/api/probar/trabajo"):
            modo = self.path.rsplit("/", 1)[1]
            threading.Thread(target=probar, args=(modo, leer_ajustes()[modo]["luces"]), daemon=True).start()
            return self._json({"ok": True})
        if not self.path.startswith("/api/luces/"):
            return self.send_error(404)
        luz = LUCES.get(self.path.rsplit("/", 1)[1])
        if luz is None:
            return self._json({"error": "luz desconocida"}, 404)
        try:
            luz.aplicar(cuerpo)
        except Exception as e:
            return self._json({"error": str(e)}, 400)
        self._json(luz.estado())

    def log_message(self, *args):
        pass


class Servidor(ThreadingHTTPServer):
    # En Windows, reusar el puerto deja que dos servidores escuchen a la vez y las consultas se reparten entre ambos.
    allow_reuse_address = os.name != "nt"


if __name__ == "__main__":
    LUCES = {l.id: l for l in cargar_luces()}
    try:
        servidor = Servidor(("0.0.0.0", PUERTO), Handler)
    except OSError:
        sys.exit(f"El puerto {PUERTO} ya está en uso: cerrá el otro servidor del panel antes de abrir este.")
    threading.Thread(target=bucle_automatico, daemon=True).start()
    print(datetime.now().strftime("%Y-%m-%d %H:%M:%S"), f"Panel de luces en http://localhost:{PUERTO}" + (" (modo demo)" if DEMO else ""), flush=True)
    servidor.serve_forever()
