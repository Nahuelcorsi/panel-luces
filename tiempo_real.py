"""WebSocket hecho a mano, solo con la librería estándar (RFC 6455).

Por qué existe: antes cada pantalla le preguntaba al servidor cada 4 segundos "¿hay algo nuevo?". Con un WebSocket
la conexión queda abierta y el servidor avisa en el momento (luces, avisos del amigo, lo que pasa en la PC), y la
respuesta del cerebro llega de a pedacitos mientras se genera.

Cómo funciona el protocolo, en corto:
1. El cliente hace un GET normal con "Upgrade: websocket" y una clave al azar (Sec-WebSocket-Key).
2. El servidor contesta 101 con Sec-WebSocket-Accept = base64(sha1(clave + GUID fijo)). Desde ahí el mismo
   socket deja de hablar HTTP y pasa a mandar "frames".
3. Cada frame: 1 byte con FIN + tipo (texto, binario, cierre, ping, pong), 1 byte con "enmascarado" + largo
   (si el largo es 126 siguen 2 bytes más, si es 127 siguen 8), 4 bytes de máscara si viene del cliente, y los datos.
   Lo que manda el cliente siempre viene enmascarado (XOR con la máscara); lo que manda el servidor, nunca.
"""
import base64
import hashlib
import json
import socket
import struct
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"   # fijo, lo define el protocolo
TEXTO, BINARIO, CIERRE, PING, PONG, CONTINUA = 0x1, 0x2, 0x8, 0x9, 0xA, 0x0
MAX_MENSAJE = 1 << 20      # 1 MB: los mensajes de las pantallas son JSON chiquitos
PING_CADA = 20             # segundos: mantiene viva la conexión y detecta pantallas que se fueron sin avisar
SIN_RESPUESTA = 60         # si en este tiempo no llegó nada (ni un pong), la conexión se da por muerta


def clave_aceptada(clave):
    return base64.b64encode(hashlib.sha1((clave + GUID).encode()).digest()).decode()


def armar_frame(opcode, datos=b""):
    """Frame del servidor: sin máscara, siempre completo (FIN = 1)."""
    largo = len(datos)
    if largo < 126:
        cabecera = struct.pack("!BB", 0x80 | opcode, largo)
    elif largo < 1 << 16:
        cabecera = struct.pack("!BBH", 0x80 | opcode, 126, largo)
    else:
        cabecera = struct.pack("!BBQ", 0x80 | opcode, 127, largo)
    return cabecera + datos


def leer_exacto(archivo, n):
    datos = archivo.read(n)
    if datos is None or len(datos) < n:
        raise ConnectionError("se cortó la conexión")
    return datos


def leer_frame(archivo):
    """Devuelve (fin, opcode, datos ya desenmascarados)."""
    b1, b2 = leer_exacto(archivo, 2)
    fin, opcode = bool(b1 & 0x80), b1 & 0x0F
    enmascarado, largo = bool(b2 & 0x80), b2 & 0x7F
    if largo == 126:
        largo = struct.unpack("!H", leer_exacto(archivo, 2))[0]
    elif largo == 127:
        largo = struct.unpack("!Q", leer_exacto(archivo, 8))[0]
    if largo > MAX_MENSAJE:
        raise ConnectionError("mensaje demasiado grande")
    if not enmascarado:
        raise ConnectionError("el cliente tiene que mandar los frames enmascarados")
    mascara = leer_exacto(archivo, 4)
    datos = leer_exacto(archivo, largo)
    # XOR byte a byte con la máscara, de a 4: se hace con enteros grandes porque byte a byte en Python es lento
    entero = int.from_bytes(datos, "big") ^ int.from_bytes((mascara * (largo // 4 + 1))[:largo], "big")
    return fin, opcode, entero.to_bytes(largo, "big")


class Conexion:
    """Una pantalla conectada. `pantalla` es dónde está (panel, pc, celu, botonera) y `rol` qué es: "inicio" (el panel
    de luces), "amigo" (la cara del Capsulito, que es la que habla) o "botonera"."""

    def __init__(self, sock, archivo_lectura, pantalla, rol):
        self.sock, self.lectura, self.pantalla, self.rol = sock, archivo_lectura, pantalla, rol
        self.lock = threading.Lock()   # varios hilos pueden mandarle cosas a la vez (luces, avisos, cerebro)
        self.abierta = True
        self.ultimo_dato = time.time()

    def _mandar(self, opcode, datos):
        with self.lock:
            if not self.abierta:
                return False
            try:
                self.sock.sendall(armar_frame(opcode, datos))
                return True
            except OSError:
                self.abierta = False
                return False

    def mandar(self, tipo, **datos):
        return self._mandar(TEXTO, json.dumps({"tipo": tipo, **datos}, ensure_ascii=False).encode())

    def cerrar(self):
        if self.abierta:
            self._mandar(CIERRE, struct.pack("!H", 1000))
        self.abierta = False
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def mensajes(self):
        """Generador de mensajes de texto ya decodificados como JSON. Contesta pings y arma mensajes partidos."""
        partes, tipo_partido = [], None
        while self.abierta:
            try:
                fin, opcode, datos = leer_frame(self.lectura)
            except (ConnectionError, OSError, struct.error):
                return
            self.ultimo_dato = time.time()
            if opcode == CIERRE:
                return
            if opcode == PING:
                self._mandar(PONG, datos)
                continue
            if opcode == PONG:
                continue
            if opcode in (TEXTO, BINARIO):
                partes, tipo_partido = [datos], opcode
            elif opcode == CONTINUA and tipo_partido:
                partes.append(datos)
            else:
                continue
            if sum(map(len, partes)) > MAX_MENSAJE:
                return
            if fin:
                completo, partes, tipo = b"".join(partes), [], tipo_partido
                tipo_partido = None
                if tipo != TEXTO:
                    continue
                try:
                    yield json.loads(completo.decode())
                except (ValueError, UnicodeDecodeError):
                    continue


class Canal:
    """Todas las conexiones abiertas. Cualquier parte del servidor puede avisar algo con avisar()/mandar_a()."""

    def __init__(self):
        self.conexiones = set()
        self.lock = threading.Lock()
        threading.Thread(target=self._latidos, daemon=True).start()

    def agregar(self, conexion):
        with self.lock:
            self.conexiones.add(conexion)

    def sacar(self, conexion):
        with self.lock:
            self.conexiones.discard(conexion)

    def lista(self, pantalla=None, rol=None):
        with self.lock:
            return [c for c in self.conexiones if c.abierta and (pantalla is None or c.pantalla == pantalla)
                    and (rol is None or c.rol == rol)]

    def pantallas(self):
        return {c.pantalla for c in self.lista()}

    def avisar(self, tipo, solo_rol=None, **datos):
        """A todas las pantallas conectadas (o solo a las de un rol)."""
        for c in self.lista(rol=solo_rol):
            c.mandar(tipo, **datos)

    def mandar_a(self, pantalla, tipo, solo_rol=None, **datos):
        """Solo a una pantalla. Devuelve a cuántas conexiones les llegó (0 = no hay ninguna abierta)."""
        return sum(c.mandar(tipo, **datos) for c in self.lista(pantalla, solo_rol))

    def _latidos(self):
        while True:
            time.sleep(PING_CADA)
            ahora = time.time()
            for c in self.lista():
                if ahora - c.ultimo_dato > SIN_RESPUESTA:
                    c.cerrar()   # su hilo sale del bucle de mensajes y se saca solo
                else:
                    c._mandar(PING, b"")


def aceptar(handler, canal, pantalla, rol):
    """Hace el saludo HTTP -> WebSocket sobre un BaseHTTPRequestHandler. Devuelve la Conexion o None si no era
    un pedido de WebSocket válido (en ese caso ya contestó el error)."""
    clave = handler.headers.get("Sec-WebSocket-Key")
    if "websocket" not in handler.headers.get("Upgrade", "").lower() or not clave:
        handler.send_error(400, "Esto es para WebSocket")
        return None
    handler.send_response(101, "Switching Protocols")
    handler.send_header("Upgrade", "websocket")
    handler.send_header("Connection", "Upgrade")
    handler.send_header("Sec-WebSocket-Accept", clave_aceptada(clave))
    handler.end_headers()
    handler.wfile.flush()
    handler.close_connection = True   # cuando termine el WebSocket, el hilo no intenta leer otro pedido HTTP
    sock = handler.connection
    sock.settimeout(None)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # los pedacitos del cerebro salen sin demora
    conexion = Conexion(sock, handler.rfile, pantalla, rol)
    canal.agregar(conexion)
    return conexion
