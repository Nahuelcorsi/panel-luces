"""Panel de luces: servidor local para luces Tuya / Smart Life.

Uso:
    python server.py          # usa devices.json (generado por el asistente de TinyTuya)
    python server.py --demo   # luces falsas, para ver el panel sin luces reales
"""
import colorsys
import json
import os
import re
import socket
import ssl
import sys
import threading
import time
import unicodedata
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
# Lo que está haciendo cada rutina, para mostrarlo en el panel y entender qué pasó si algo falla
ESTADO = {"despertador": {}, "trabajo": {}}


def anotar(texto):
    print(datetime.now().strftime("%Y-%m-%d %H:%M:%S"), texto, flush=True)


def aplicar_a(ids, cambios, modo):
    for ident in ids:
        luz = LUCES.get(ident)
        if luz is None or ENVIADO.get((modo, ident)) == cambios:
            continue
        try:
            luz.aplicar(cambios)
            ENVIADO[(modo, ident)] = cambios
            if modo in ESTADO:
                ESTADO[modo].update(ultimo=datetime.now().astimezone().isoformat(timespec="seconds"), error=None)
        except Exception as e:
            anotar(f"{modo}: no pude cambiar {luz.nombre}: {e}")
            if modo in ESTADO:
                ESTADO[modo]["error"] = f"{luz.nombre}: {e}"


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
AVANCE = {}     # (modo, fecha) -> (avance, hora) de la última vuelta, para que el color no vuelva atrás


def vuelta_automatica(ahora):
    a = leer_ajustes()
    d = a["despertador"]
    if d["activo"] and ahora.isoweekday() in d["dias"] and ("despertador", ahora.date()) not in HECHO:
        h, m = (int(x) for x in d["hora"].split(":"))
        hora = ahora.replace(hour=h, minute=m, second=0, microsecond=0)
        inicio = hora - timedelta(minutes=d["minutos"])
        if inicio <= ahora <= hora + timedelta(minutes=2):
            p = min(1, (ahora - inicio) / (hora - inicio))
            ESTADO["despertador"].update(avance=round(p, 3), hasta=hora.isoformat(timespec="seconds"))
            aplicar_a(d["luces"], amanecer(p), "despertador")
            if ahora >= hora:
                HECHO.add(("despertador", ahora.date()))
                ESTADO["despertador"]["avance"] = None
                anotar("despertador: terminado")
    t = a["trabajo"]
    if t["activo"]:
        v = VIAJE.obtener()
        if v.get("ok"):
            salida = datetime.fromisoformat(v["salida"])
            inicio = salida - timedelta(minutes=t["minutos"])
            if ("trabajo", salida.date()) in HECHO:
                return
            if inicio <= ahora < salida:
                # La hora de salida se recalcula con el tráfico cada 5 minutos. Si se atrasa, el color no vuelve
                # hacia el verde: sigue avanzando desde donde estaba hasta la nueva hora.
                clave = ("trabajo", salida.date())
                p = (ahora - inicio) / (salida - inicio)
                if clave in AVANCE:
                    antes, cuando = AVANCE[clave]
                    if p < antes:   # sigue desde el último color y llega al rojo justo a la nueva hora
                        p = antes + (1 - antes) * (ahora - cuando) / (salida - cuando)
                AVANCE[clave] = (p, ahora)
                ESTADO["trabajo"].update(avance=round(p, 3), hasta=salida.isoformat(timespec="seconds"))
                aplicar_a(t["luces"], semaforo(p), "trabajo")
            elif salida <= ahora <= salida + timedelta(minutes=5):
                anotar("trabajo: hora de salir, parpadeo")
                parpadear(t["luces"])
                HECHO.add(("trabajo", salida.date()))
                ESTADO["trabajo"]["avance"] = None


def bucle_automatico():
    while True:
        try:
            vuelta_automatica(datetime.now().astimezone())
        except Exception as e:
            anotar(f"Error en despertador/modo trabajo: {e}")
        time.sleep(10)


def probar(modo, ids):
    """Versión rápida para ver cómo queda: el despertador en 30 s y el modo trabajo en 20 s más el parpadeo."""
    pasos, funcion = (15, amanecer) if modo == "despertador" else (10, semaforo)
    for i in range(pasos + 1):
        aplicar_a(ids, funcion(i / pasos), "prueba")
        time.sleep(2)
    if modo == "trabajo":
        parpadear(ids)
    ENVIADO.clear()


# ---- Rickuel: el cerebro del panel ----
# La página de Rickuel (web/rickuel/) le manda la charla a /cerebro. Acá se arma el contexto de la casa, se elige
# el cerebro que responda (la PC con Ollama si está prendida, si no Gemini) y se ejecutan las acciones que pida.
# rickuel.json (en .gitignore) guarda la IP de la PC y la clave de Gemini; ver rickuel.example.json.

RUTA_RICKUEL = os.path.join(BASE, "rickuel.json")
RUTA_CHARLAS = os.path.join(BASE, "charlas-demo.jsonl" if DEMO else "charlas.jsonl")
LOCK_CHARLAS = threading.Lock()
MAX_ACCIONES = 6
LOCAL_CAIDO = {"hasta": 0}   # si la PC no responde, no se vuelve a probar durante un rato (para no demorar cada charla)

ESCENAS = {
    "apagar": {"encendida": False},
    "prender": {"encendida": True, "blanco": 50, "brillo": 100},
    "relax": {"encendida": True, "blanco": 0, "brillo": 45},
    "tenue": {"encendida": True, "blanco": 0, "brillo": 8},
    "cine": {"encendida": True, "color": "#5b3cff", "brillo": 25},
}
BLANCOS = {"calido": 0, "neutro": 50, "frio": 100}

# Memoria: datos duraderos que Rickuel recuerda de Nahuel y de la casa (memoria.json, en .gitignore).
# Los guarda cuando Nahuel se lo pide o cuenta algo duradero; se ven y se borran desde la pantalla de Rickuel.
RUTA_MEMORIA = os.path.join(BASE, "memoria-demo.json" if DEMO else "memoria.json")
LOCK_MEMORIA = threading.Lock()
MAX_RECUERDOS = 60
MAX_LARGO = 160


def leer_memoria():
    try:
        with open(RUTA_MEMORIA, encoding="utf-8") as f:
            lista = json.load(f)
        return [r for r in lista if isinstance(r, dict) and r.get("texto")]
    except (OSError, ValueError):
        return []


def escribir_memoria(lista):
    with open(RUTA_MEMORIA, "w", encoding="utf-8") as f:
        json.dump(lista, f, ensure_ascii=False, indent=2)


def recordar(texto):
    """Guarda un dato nuevo. Devuelve (ok, detalle)."""
    texto = " ".join(str(texto).split())[:MAX_LARGO]
    if len(texto) < 4:
        return False, "no entendí qué recordar"
    with LOCK_MEMORIA:
        lista = leer_memoria()
        clave = sin_tildes(texto)
        if any(sin_tildes(r["texto"]) == clave for r in lista):
            return True, "ya lo recordaba"
        if len(lista) >= MAX_RECUERDOS:
            return False, "la memoria está llena, borrá algo de la lista"
        lista.append({"id": max([r.get("id", 0) for r in lista] + [0]) + 1, "texto": texto,
                      "fecha": datetime.now().astimezone().isoformat(timespec="seconds")})
        escribir_memoria(lista)
    return True, ""


def olvidar(fragmento):
    """Borra los recuerdos que contengan el fragmento. Devuelve (ok, detalle)."""
    clave = sin_tildes(str(fragmento))
    if len(clave) < 3:
        return False, "no entendí qué olvidar"
    with LOCK_MEMORIA:
        lista = leer_memoria()
        quedan = [r for r in lista if clave not in sin_tildes(r["texto"])]
        if len(quedan) == len(lista):
            return False, f"no recordaba nada de «{fragmento}»"
        escribir_memoria(quedan)
    return True, ""


def borrar_recuerdo(ident):
    with LOCK_MEMORIA:
        escribir_memoria([r for r in leer_memoria() if r.get("id") != ident])


def contexto_memoria():
    lista = leer_memoria()
    if not lista:
        return "Todavía no recordás nada en particular de Nahuel."
    return "Esto es lo que recordás de Nahuel y de la casa (usalo con naturalidad, sin recitarlo):\n" + \
        "\n".join(f"- {r['texto']}" for r in lista)

INSTRUCCIONES_CASA = """
Además de charlar, manejás la casa de Nahuel. Abajo tenés el estado actual.
Para hacer algo, escribí al FINAL de tu respuesta una marca por acción, con este formato exacto:
[accion:luz|NOMBRE|encender]   [accion:luz|NOMBRE|apagar]   [accion:luz|NOMBRE|brillo|50]
[accion:luz|NOMBRE|color|#ff0000]   [accion:luz|NOMBRE|blanco|calido]  (calido, neutro o frio)
[accion:escena|cine]  (apagar, prender, relax, tenue o cine; afecta a todas las luces)
NOMBRE es el nombre de una luz de la lista, o "todas". El brillo va de 1 a 100.
Las marcas solo van cuando Nahuel te ORDENA hacer algo ("apagá", "prendé", "poné", "subí el brillo"). Si charla o pregunta
(por ejemplo "¿cómo está el dormitorio?", "¿está prendida?", "¿qué hora es?"), contestá con el estado de abajo y NO pongas ninguna marca:
una pregunta nunca cambia nada. Ante la duda, no pongas marca.
Tu texto se lee en voz alta y las marcas se quitan solas: no las nombres ni las expliques.
Solo podés hacer lo de esta lista; no inventes otras acciones ni otras luces.

También tenés memoria. Si Nahuel te pide que te acuerdes de algo, o cuenta un dato duradero sobre él o la casa
(gustos, horarios, costumbres), agregá al final [memoria:frase corta y clara en tercera persona].
Ejemplo: [memoria:Nahuel prefiere la luz cálida de noche]
Si te pide que olvides algo: [olvidar:palabra clave]. No guardes charla pasajera, claves, contraseñas ni datos de tarjetas,
ni algo que ya figura en lo que recordás.
Ejemplos:
Nahuel: "apagá todo" -> "[feliz] Dale, a oscuras como un murciélago, uuurp. [accion:luz|todas|apagar]"
Nahuel: "¿está prendida la lámpara?" -> "[neutral] Sí, al 80%, ¿no la ves o qué?"
(Esta segunda respuesta no lleva marca porque Nahuel solo preguntó; no repitas esta nota.)
"""


def sin_tildes(t):
    return "".join(c for c in unicodedata.normalize("NFD", t) if not unicodedata.combining(c)).lower().strip()


def cargar_rickuel():
    if not os.path.exists(RUTA_RICKUEL):
        return {}
    with open(RUTA_RICKUEL, encoding="utf-8") as f:
        return json.load(f)


def resumen_luz(e):
    if not e.get("online"):
        return f"- {e['nombre']}: sin conexión"
    return f"- {e['nombre']}: " + (f"prendida, brillo {e['brillo']}%, color {e['color']}" if e["encendida"] else "apagada")


def contexto_casa():
    """Estado de la casa que se le pasa a Rickuel en cada charla."""
    ahora = datetime.now().astimezone()
    dias = ["lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo"]
    lineas = [f"Ahora es {dias[ahora.weekday()]} {ahora:%d/%m/%Y %H:%M}.", "Luces:"]
    lineas += [resumen_luz(l.estado()) for l in LUCES.values()]
    try:
        v = VIAJE.obtener()
        if v.get("ok"):
            lineas.append(f"Para llegar al trabajo a las {datetime.fromisoformat(v['llegada']):%H:%M} tiene que salir a las "
                          f"{datetime.fromisoformat(v['salida']):%H:%M} (viaje de {v['minutos']} min, {v['demora']} de demora por tráfico).")
        c = CLIMA.obtener().get("casa", {})
        if c.get("ok"):
            lineas.append(f"Clima en casa: {c['temp']:.0f}°, sensación {c['sensacion']:.0f}°, máxima {c['max']:.0f}° y mínima {c['min']:.0f}°, "
                          f"lluvia en las próximas 3 h: {c['lluvia_3h']}%.")
        a = leer_ajustes()
        d = a["despertador"]
        lineas.append(f"Despertador: {'activado a las ' + d['hora'] if d['activo'] else 'apagado'}. "
                      f"Modo trabajo: {'activado' if a['trabajo']['activo'] else 'apagado'}.")
    except Exception as e:
        anotar(f"rickuel: no pude armar todo el contexto: {e}")
    return "\n".join(lineas)


def luces_por_nombre(nombre):
    n = sin_tildes(nombre)
    if n in ("todas", "todo", "todos"):
        return list(LUCES.values())
    return [l for l in LUCES.values() if n in sin_tildes(l.nombre)]


def ejecutar_accion(partes):
    """Ejecuta una acción pedida por Rickuel. Solo se aceptan las de la lista; devuelve (ok, detalle)."""
    tipo = sin_tildes(partes[0]) if partes else ""
    if tipo == "escena" and len(partes) >= 2 and sin_tildes(partes[1]) in ESCENAS:
        cambios = ESCENAS[sin_tildes(partes[1])]
        luces = list(LUCES.values())
    elif tipo == "luz" and len(partes) >= 3:
        luces = luces_por_nombre(partes[1])
        if not luces:
            return False, f"no existe la luz «{partes[1]}»"
        accion = sin_tildes(partes[2])
        valor = partes[3].strip() if len(partes) > 3 else ""
        if accion in ("encender", "prender"):
            cambios = {"encendida": True}
        elif accion == "apagar":
            cambios = {"encendida": False}
        elif accion == "brillo" and valor.isdigit():
            cambios = {"encendida": True, "brillo": max(1, min(100, int(valor)))}
        elif accion == "color" and re.fullmatch(r"#[0-9a-fA-F]{6}", valor):
            cambios = {"encendida": True, "color": valor.lower()}
        elif accion == "blanco" and sin_tildes(valor) in BLANCOS:
            cambios = {"encendida": True, "blanco": BLANCOS[sin_tildes(valor)], "brillo": None}
        else:
            return False, "acción de luz no válida"
    else:
        return False, "acción no válida"
    fallas = []
    for luz in luces:
        try:
            c = dict(cambios)
            if "brillo" in c and c["brillo"] is None:   # blanco: conserva el brillo que tenía
                c["brillo"] = luz.estado().get("brillo") or 100
            luz.aplicar(c)
        except Exception as e:
            fallas.append(f"{luz.nombre} ({e})")
    return (not fallas), ", ".join(fallas)


MARCA = re.compile(r"\[\s*(acci[oó]n|memoria|olvidar)\s*:([^\]]*)\]", re.IGNORECASE)


def sacar_acciones(crudo):
    """Separa el texto hablado de las marcas, ejecuta las marcas y avisa si alguna falló."""
    hechas = []
    for tipo, m in MARCA.findall(crudo)[:MAX_ACCIONES]:
        tipo = sin_tildes(tipo)
        if tipo == "memoria":
            partes = ["memoria", m.strip()]
            ok, detalle = recordar(m)
        elif tipo == "olvidar":
            partes = ["olvidar", m.strip()]
            ok, detalle = olvidar(m)
        else:
            partes = [p.strip() for p in m.split("|")]
            ok, detalle = ejecutar_accion(partes)
        hechas.append({"accion": "|".join(partes), "ok": ok, "detalle": detalle})
        anotar(f"rickuel: {'|'.join(partes)} -> {'ok' if ok else 'falló: ' + detalle}")
    texto = " ".join(MARCA.sub("", crudo).split())
    fallas = [h["detalle"] for h in hechas if not h["ok"] and h["detalle"]]
    if fallas:
        texto += " Ojo, no pude hacer algo: " + "; ".join(fallas) + "."
    return texto, hechas


def pedir_post(url, cuerpo, headers, espera):
    req = urllib.request.Request(url, data=json.dumps(cuerpo).encode(), headers={"content-type": "application/json", **headers})
    try:
        with urllib.request.urlopen(req, timeout=espera, context=SSL) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"{e.code}: {e.read().decode(errors='replace')[:300]}")


def pensar_local(cfg, sistema, mensajes):
    """Ollama en la PC de la casa. Si no está prendida falla enseguida (no espera a que venza el timeout)."""
    url = cfg["local_url"].rstrip("/")
    if time.time() < LOCAL_CAIDO["hasta"]:
        raise RuntimeError("la PC no respondió hace poco")
    host = urllib.parse.urlparse(url)
    try:
        socket.create_connection((host.hostname, host.port or 80), timeout=1.5).close()
    except OSError:
        LOCAL_CAIDO["hasta"] = time.time() + 30
        raise RuntimeError("la PC no está prendida")
    d = pedir_post(f"{url}/api/chat", {
        "model": cfg.get("local_modelo", "gemma3:12b"), "stream": False, "options": {"num_predict": 300},
        "keep_alive": cfg.get("local_mantener", "30m"),
        "messages": [{"role": "system", "content": sistema}, *mensajes]}, {}, cfg.get("local_espera", 120))
    return d.get("message", {}).get("content", "")


CALENTADO = {"hasta": 0}


def calentar_local():
    """Carga el modelo de la PC en memoria de antemano: si estaba descargado, la primera respuesta tarda ~1 minuto."""
    cfg = cargar_rickuel()
    if not cfg.get("local_url") or time.time() < CALENTADO["hasta"]:
        return
    CALENTADO["hasta"] = time.time() + 120

    def trabajo():
        try:
            url = cfg["local_url"].rstrip("/")
            host = urllib.parse.urlparse(url)
            socket.create_connection((host.hostname, host.port or 80), timeout=1.5).close()
            pedir_post(f"{url}/api/generate", {"model": cfg.get("local_modelo", "gemma3:12b"),
                                               "keep_alive": cfg.get("local_mantener", "30m")}, {}, 180)
        except Exception as e:
            anotar(f"rickuel: no pude precargar el modelo: {e}")
    threading.Thread(target=trabajo, daemon=True).start()


def pensar_gemini(cfg, sistema, mensajes):
    url = f"https://generativelanguage.googleapis.com/v1beta/models/{cfg.get('gemini_modelo', 'gemini-flash-lite-latest')}:generateContent"
    cuerpo = {"systemInstruction": {"parts": [{"text": sistema}]},
              "contents": [{"role": "model" if m["role"] == "assistant" else "user", "parts": [{"text": m["content"]}]} for m in mensajes],
              "generationConfig": {"maxOutputTokens": 800}}
    for intento in range(3):
        try:
            d = pedir_post(url, cuerpo, {"x-goog-api-key": cfg["gemini_key"]}, 60)
            break
        except RuntimeError as e:
            # 503 (alta demanda) y 429 (límite por minuto) suelen pasar solos en unos segundos
            if intento == 2 or not str(e).startswith(("503", "429")):
                raise
            time.sleep(2 + intento * 2)
    return "".join(p.get("text", "") for p in d.get("candidates", [{}])[0].get("content", {}).get("parts", []))


def pensar_demo(sistema, mensajes):
    """Cerebro de mentira para probar el panel sin PC ni claves (solo en modo demo)."""
    t = sin_tildes(mensajes[-1]["content"])
    if "apag" in t:
        return "[enojado] Ya va, uuurp, otra vez a oscuras. [accion:luz|todas|apagar]"
    if "prend" in t:
        return "[feliz] Y se hizo la luz, como yo lo dije. [accion:luz|todas|encender]"
    if "cine" in t:
        return "[feliz] Modo cine, che. [accion:escena|cine]"
    if "acordate" in t or "acuerdate" in t:
        return "[feliz] Anotado, aunque tu memoria es un desastre. [memoria:Nahuel prefiere la luz cálida de noche]"
    if "olvida" in t:
        return "[neutral] Ya lo borré. [olvidar:luz cálida]"
    if "inventa" in t:
        return "[pensando] Probemos algo raro. [accion:luz|cocina|apagar]"
    return "[pensando] Estoy en modo demo, Nahuel: sin cerebro de verdad."


def cerebros_disponibles(cfg):
    orden = []
    if cfg.get("local_url"):
        orden.append(("local", lambda s, m: pensar_local(cfg, s, m), cfg.get("local_modelo", "gemma3:12b")))
    if cfg.get("gemini_key"):
        orden.append(("gemini", lambda s, m: pensar_gemini(cfg, s, m), cfg.get("gemini_modelo", "gemini-flash-lite-latest")))
    if DEMO and not orden:
        orden.append(("demo", pensar_demo, "demo"))
    return orden


def pensar(sistema, mensajes):
    """Prueba los cerebros en orden (PC, después Gemini). Devuelve (texto sin marcas, acciones, cerebro, texto crudo)."""
    errores = []
    for nombre, usar, modelo in cerebros_disponibles(cargar_rickuel()):
        try:
            crudo = usar(sistema + "\n" + INSTRUCCIONES_CASA + "\n" + contexto_memoria() +
                         "\n\nEstado de la casa:\n" + contexto_casa(), mensajes)
        except Exception as e:
            errores.append(f"{nombre}: {e}")
            anotar(f"rickuel: falló el cerebro {nombre}: {e}")
            continue
        if not crudo.strip():
            errores.append(f"{nombre}: respuesta vacía")
            continue
        texto, acciones = sacar_acciones(crudo)
        guardar_charla(mensajes[-1]["content"], crudo, nombre, modelo, acciones)
        return texto, acciones, f"{nombre}:{modelo}", crudo
    raise RuntimeError("No tengo cerebro disponible (" + "; ".join(errores) + ")" if errores else
                       "Falta configurar el cerebro en rickuel.json")


def guardar_charla(pregunta, respuesta, cerebro, modelo, acciones):
    """Cada intercambio queda guardado: es el material para entrenar el modelo propio de Rickuel."""
    with LOCK_CHARLAS, open(RUTA_CHARLAS, "a", encoding="utf-8") as f:
        f.write(json.dumps({"fecha": datetime.now().astimezone().isoformat(timespec="seconds"), "cerebro": cerebro,
                            "modelo": modelo, "pregunta": pregunta, "respuesta": respuesta, "acciones": acciones},
                           ensure_ascii=False) + "\n")


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
            return self._json({**leer_ajustes(), "estado": ESTADO})
        if self.path == "/api/memoria":
            return self._json(leer_memoria())
        if self.path == "/salud":   # así la página de Rickuel sabe que está dentro del panel
            calentar_local()        # y de paso se va cargando el modelo mientras Nahuel escribe
            return self._json({"panel": True, "tieneCerebro": bool(cerebros_disponibles(cargar_rickuel()))})
        self._estatico()

    def _estatico(self):
        """Sirve los archivos de web/ (solo de esa carpeta)."""
        ruta = self.path.split("?")[0]
        if ruta.endswith("/"):
            ruta += "index.html"
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
                return self._json({**guardar_ajustes(cuerpo), "estado": ESTADO})
            except Exception as e:
                return self._json({"error": str(e)}, 400)
        if self.path == "/api/memoria/borrar":
            borrar_recuerdo(cuerpo.get("id"))
            return self._json(leer_memoria())
        if self.path == "/cerebro":
            try:
                texto, acciones, cerebro, crudo = pensar(cuerpo["sistema"], cuerpo["mensajes"])
                return self._json({"texto": texto, "acciones": acciones, "cerebro": cerebro, "crudo": crudo})
            except Exception as e:   # la cara muestra el error en vez de quedarse colgada
                return self._json({"error": {"message": str(e)}}, 500)
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
