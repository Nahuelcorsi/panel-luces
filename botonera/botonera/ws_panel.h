// Cliente WebSocket hecho a mano para hablar con server.py en /ws (el mismo protocolo que tiempo_real.py, del
// lado del cliente). Sin librerías extra: solo WiFiClient de la placa.
//
// Diferencias con el lado del servidor:
// - El cliente SIEMPRE manda sus frames enmascarados (4 bytes al azar con los que se hace XOR a los datos).
// - Los frames del servidor llegan sin máscara.
// - El servidor manda un ping cada 20 s; acá se contesta con un pong (enmascarado) y, si en 60 s no llegó nada,
//   la conexión se da por muerta y se vuelve a conectar.
//
// Lo usa una sola tarea (la tarea "red"): no es seguro llamarlo desde dos tareas a la vez.
#pragma once
#include <WiFi.h>
#include <esp_random.h>

class WsPanel {
  WiFiClient cli;
  bool abierto = false;
  unsigned long ultimoDato = 0;

  static String claveAlAzar() {   // 16 bytes al azar en base64 (24 letras): Sec-WebSocket-Key
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t b[18];
    esp_fill_random(b, sizeof b);
    String s;
    for (int i = 0; i < 18; i += 3) {
      uint32_t n = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
      for (int k = 3; k >= 0; k--) s += B64[(n >> (6 * k)) & 63];
    }
    return s.substring(0, 22) + "==";
  }

  bool mandarFrame(uint8_t opcode, const uint8_t* datos, size_t largo) {
    uint8_t cab[14];
    size_t n = 0;
    cab[n++] = 0x80 | opcode;                       // FIN + tipo
    if (largo < 126) cab[n++] = 0x80 | largo;       // 0x80 = enmascarado
    else if (largo < 65536) { cab[n++] = 0x80 | 126; cab[n++] = largo >> 8; cab[n++] = largo & 0xFF; }
    else return false;                              // la botonera nunca manda tanto
    uint32_t r = esp_random();
    uint8_t mascara[4] = {(uint8_t)(r >> 24), (uint8_t)(r >> 16), (uint8_t)(r >> 8), (uint8_t)r};
    memcpy(cab + n, mascara, 4);
    n += 4;
    if (cli.write(cab, n) != n) return cerrarMal();
    uint8_t trozo[256];
    for (size_t i = 0; i < largo; i += sizeof trozo) {
      size_t m = min(sizeof trozo, largo - i);
      for (size_t k = 0; k < m; k++) trozo[k] = datos[i + k] ^ mascara[(i + k) & 3];
      if (cli.write(trozo, m) != m) return cerrarMal();
    }
    return true;
  }

  bool cerrarMal() {
    cli.stop();
    abierto = false;
    return false;
  }

 public:
  // url como en config.h: "http://192.168.0.50:8080"; ruta: "/ws?rol=botonera"
  bool conectar(const String& url, const char* ruta) {
    cli.stop();
    abierto = false;
    char host[40] = "";
    int puerto = 80;
    if (sscanf(url.c_str(), "http://%39[^:/]:%d", host, &puerto) < 1) return false;
    if (!cli.connect(host, puerto, 1500)) return false;
    cli.setNoDelay(true);   // los pedidos chiquitos salen sin esperar a juntar más
    cli.setTimeout(3);      // segundos, para leer el saludo y cada frame
    cli.printf("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
               ruta, host, puerto, claveAlAzar().c_str());
    String estado = cli.readStringUntil('\n');
    if (estado.indexOf(" 101") < 0) return cerrarMal();
    for (int i = 0; i < 30; i++) {                  // los encabezados hasta la línea vacía
      String linea = cli.readStringUntil('\n');
      if (linea == "\r" || linea.length() == 0) break;
    }
    abierto = true;
    ultimoDato = millis();
    return true;
  }

  bool conectado() {
    if (abierto && (!cli.connected() || millis() - ultimoDato > 60000UL)) cerrarMal();
    return abierto;
  }

  void cerrar() {
    if (abierto) { uint8_t chau[2] = {0x03, 0xE8}; mandarFrame(0x8, chau, 2); }   // 1000 = cierre normal
    cerrarMal();
  }

  bool mandar(const String& texto) {
    return abierto && mandarFrame(0x1, (const uint8_t*)texto.c_str(), texto.length());
  }

  // Si llegó un mensaje de texto completo lo deja en `msg` y devuelve true. No espera si no hay nada.
  // Contesta los pings solo. Los mensajes partidos en varios frames (el servidor no los manda) se descartan.
  bool recibir(String& msg) {
    while (abierto && cli.available() >= 2) {
      uint8_t b[2];
      if (cli.readBytes(b, 2) != 2) return cerrarMal();
      uint8_t opcode = b[0] & 0x0F;
      bool fin = b[0] & 0x80;
      size_t largo = b[1] & 0x7F;
      if (largo == 126) {
        uint8_t l[2];
        if (cli.readBytes(l, 2) != 2) return cerrarMal();
        largo = (l[0] << 8) | l[1];
      } else if (largo == 127) {
        return cerrarMal();                          // más de 64 KB: el panel nunca manda eso
      }
      if (largo > 16384) return cerrarMal();        // lo más largo que manda el panel son las luces (~1 KB)
      String datos;
      datos.reserve(largo);
      uint8_t trozo[128];
      for (size_t leido = 0; leido < largo;) {
        size_t m = cli.readBytes(trozo, min(sizeof trozo, largo - leido));
        if (m == 0) return cerrarMal();
        datos.concat((const char*)trozo, m);
        leido += m;
      }
      ultimoDato = millis();
      if (opcode == 0x9) { mandarFrame(0xA, (const uint8_t*)datos.c_str(), datos.length()); continue; }   // ping -> pong
      if (opcode == 0x8) return cerrarMal();       // el servidor cierra
      if (opcode == 0x1 && fin) { msg = datos; return true; }
    }
    return false;
  }
};
