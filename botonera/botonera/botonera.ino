// Botonera de mesa de luz para panel-luces.
// Placa: ESP32-2432S028R ("Cheap Yellow Display", pantalla 2.8" táctil)
//        + 2 expansores PCF8574 + 5 perillas KY-040 (una por luz y una de color).
// Habla con server.py por WiFi usando la misma API que el panel del celular.
//
// Perillas 1-4: cada una maneja la luz (o el grupo) que diga PERILLAS en config.h, por nombre.
//           girar = brillo · clic = prender/apagar · mantener 1 s = apagar todo
// Perilla 5 (color): girar = recorre los colores de la luz elegida · clic = pasa a la luz siguiente
//           · mantener 1 s = vuelve a luz blanca
// Pantalla: pestañas Inicio / Luces / Jugar / Amigo abajo de todo.
// Jugar: Arkanoid o Space Invaders. Cualquier perilla (o el dedo) mueve la paleta o la nave y un clic
//        lanza o dispara; mantener apretada una perilla vuelve al menú de juegos.
// Amigo: dos ojitos y frases fijas que van al mismo cerebro del panel (/cerebro); contesta en pantalla
//        y con ruiditos de robot (nunca con palabras).
// Botón para hablar (P6 del expansor 0 a GND): desde cualquier pestaña lleva al amigo; mientras lo
//        mantenés apretado graba (MAX9814 en IO35) y el panel lo pasa a texto para el cerebro (/api/voz).
// Parlante en el conector SPEAK (el amplificador de la placa lo maneja el GPIO26).
//
// Las fuentes incluidas no tienen tildes ni ñ: los textos en pantalla van sin acentos.

#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <esp_sleep.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "config.h"

// Bus I2C de los expansores, en el conector CN1 de la placa
const int PIN_SDA = 27, PIN_SCL = 22;
// Para dormir a batería: INT de los expansores (avisa que se movió una perilla) y el aviso de toque de la pantalla
// (ojo: hoy el IO35 lo usa el micrófono, así que dormir() no se usa: ver enchufada())
const gpio_num_t PIN_INT_PERILLAS = GPIO_NUM_35, PIN_INT_TACTIL = GPIO_NUM_36;
// Botón para hablar: P6 del expansor 0, a GND (apretado = 0). Antes ese pin iba a ver el USB, pero nunca se cableó.
const int BIT_HABLAR = 6;
const int PIN_MIC = 35;        // salida OUT del MAX9814 (conector P3)
const int PIN_PARLANTE = 26;   // entrada del amplificador del conector SPEAK
// Volumen de los ruiditos: 1 (casi nada) a 128 (al máximo). Se puede poner en config.h.
#ifndef VOLUMEN
#define VOLUMEN 20
#endif
const int MAX_LUCES = 10;   // luces y grupos del panel

// ---- pantalla de la CYD ----
class LGFX : public lgfx::LGFX_Device {
#if PANTALLA_ST7789
  lgfx::Panel_ST7789 panel;
#else
  lgfx::Panel_ILI9341 panel;
#endif
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM luz;
  lgfx::Touch_XPT2046 tactil;

 public:
  LGFX() {
    auto b = bus.config();
    b.spi_host = SPI2_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.pin_sclk = 14; b.pin_mosi = 13; b.pin_miso = 12; b.pin_dc = 2;
    bus.config(b);
    panel.setBus(&bus);

    auto p = panel.config();
    p.pin_cs = 15; p.pin_rst = -1; p.pin_busy = -1;
    p.panel_width = 240; p.panel_height = 320;
    p.readable = true;
    p.invert = INVERTIR_COLORES;
    p.bus_shared = false;
    panel.config(p);

    auto l = luz.config();
    // 44 kHz: a 12 kHz el parpadeo de la luz se colaba en el parlante como un pitido (no se oye arriba de 20 kHz)
    l.pin_bl = 21; l.invert = false; l.freq = 44000; l.pwm_channel = 7;
    luz.config(l);
    panel.setLight(&luz);

    // El táctil va por su propio bus SPI
    auto t = tactil.config();
    t.x_min = 300; t.x_max = 3900; t.y_min = 200; t.y_max = 3700;
    // Sin el aviso de toque (IO36): con el WiFi prendido ese pin da "toques fantasma" (falla conocida del ESP32).
    // Para despertar a batería se sigue usando, pero directo, en dormir().
    t.pin_int = -1; t.bus_shared = false; t.offset_rotation = 0;
    t.spi_host = SPI3_HOST; t.freq = 1000000;
    t.pin_sclk = 25; t.pin_mosi = 32; t.pin_miso = 39; t.pin_cs = 33;
    tactil.config(t);
    panel.setTouch(&tactil);

    setPanel(&panel);
  }
};

LGFX lcd;
// Todo se dibuja primero en esta "hoja" en memoria y después se pasa entera a la pantalla:
// así no se ve cómo se borra y se vuelve a dibujar (eso era el parpadeo).
LGFX_Sprite lienzo(&lcd);
LovyanGFX* g = &lcd;          // dónde dibujan las funciones de abajo: la hoja o la pantalla directo
bool pestanasSucias = true;   // las pestañas de abajo van directo a la pantalla, solo cuando cambian
Preferences prefs;

// ---- colores ----
// Siempre uint32_t: LovyanGFX toma un int suelto como color de 16 bits y saldría otro color
const uint32_t FONDO = 0x0b0d12, TARJETA = 0x1b1f2a, TEXTO = 0xf2f2f2, TENUE = 0x8a90a0, ACENTO = 0xffb347,
               PISTA = 0x2a2f3a, SIN_RED = 0x444444;

// ---- datos que vienen del panel ----
struct Luz { String id, nombre; bool online = false, encendida = false, grupo = false; int brillo = 0; uint32_t color = 0; };
Luz luces[MAX_LUCES];
int nLuces = 0, elegida = 0;   // en luces[], las luces de un grupo (como el Living) van juntas, como una sola

struct { bool ok = false; float temp = 0, max = 0, min = 0; int codigo = 0, lluvia = 0; } clima;
struct { bool activo = false; String hora; uint8_t dias = 0x7F; } alarma;   // dias: bit 1 = lunes ... bit 7 = domingo

enum Pagina { INICIO, LUCES, JUEGO, AMIGO, N_PAGINAS };
const char* PESTANAS[] = {"Inicio", "Luces", "Jugar", "Amigo"};
int pagina = INICIO;
bool sucio = true;              // hay que redibujar
unsigned long ultimoUso = 0;    // para atenuar la pantalla
bool atenuada = false;

// ---- perillas ----
// Cada PCF8574 tiene 8 pines (P0..P7). Para cada perilla: {expansor, pin} de CLK, DT y SW.
// Las 4 de luces usan P0-P5 de cada expansor; la de color usa P6 y P7 del expansor 2 y el P7 del 1
// (el P6 del 1 es el botón para hablar).
const int N_PERILLAS = 5, PERILLA_COLOR = 4;
const uint8_t PINES[N_PERILLAS][3][2] = {
  {{0, 0}, {0, 1}, {0, 2}},
  {{0, 3}, {0, 4}, {0, 5}},
  {{1, 0}, {1, 1}, {1, 2}},
  {{1, 3}, {1, 4}, {1, 5}},
  {{1, 6}, {1, 7}, {0, 7}},
};
uint8_t expansores[2];
int nExpansores = 0;

struct Perilla {
  int pasos = 0;          // pasos sin procesar (4 por "clic" en la mayoría)
  int clics = 0;          // clics cortos del botón sin procesar
  bool largo = false;     // se mantuvo apretado 1 s
  uint8_t previo = 0;
  int medio = 0;          // pasos chicos desde la última posición de descanso
  unsigned long apretadoDesde = 0;
  bool largoHecho = false;
};
Perilla perillas[N_PERILLAS];
portMUX_TYPE muxPerillas = portMUX_INITIALIZER_UNLOCKED;
volatile bool hablarApretado = false;   // el botón para hablar está apretado ahora (sin rebotes)
volatile int apretonesHablar = 0;       // veces que se apretó, sin procesar

void buscarExpansores() {
  // PCF8574 usa 0x20-0x27 y PCF8574A 0x38-0x3F, según los puentes A0-A2 del módulo
  for (uint8_t dir = 0x20; dir <= 0x3F && nExpansores < 2; dir++) {
    if (dir == 0x28) dir = 0x38;
    Wire.beginTransmission(dir);
    if (Wire.endTransmission() == 0) {
      expansores[nExpansores++] = dir;
      Wire.beginTransmission(dir);
      Wire.write(0xFF);            // todos los pines como entrada (con su resistencia interna débil)
      Wire.endTransmission();
    }
  }
}

int leerExpansor(int e) {
  if (Wire.requestFrom(expansores[e], (uint8_t)1) != 1) return -1;
  return Wire.read();
}

// Lee las perillas una vez. La llama una tarea aparte (núcleo 0) cada 1 ms, así no se pierden pasos
// mientras la pantalla o el WiFi tienen ocupado al loop(). Desde acá no se imprime nada por Serial.
void leerPerillas() {
  static const int8_t TABLA[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  static bool primera = true;
  int v[2];
  for (int e = 0; e < 2; e++) v[e] = e < nExpansores ? leerExpansor(e) : -1;
  for (int k = 0; k < N_PERILLAS; k++) {
    const uint8_t (*pin)[2] = PINES[k];
    if (v[pin[0][0]] < 0 || v[pin[1][0]] < 0 || v[pin[2][0]] < 0) continue;   // falta su expansor
    // (no se puede llamar "bit": Arduino ya tiene una macro con ese nombre y la pisaba sin avisar)
    auto leerPin = [&](int i) { return (v[pin[i][0]] >> pin[i][1]) & 1; };
    Perilla& p = perillas[k];
    uint8_t ahora = (leerPin(0) << 1) | leerPin(1);
    bool apretado = !leerPin(2);
    unsigned long t = millis();
    portENTER_CRITICAL(&muxPerillas);
    if (primera) p.previo = ahora;           // la primera lectura no cuenta como paso
    int paso = TABLA[(p.previo << 2) | ahora];
    p.medio += paso;
    // Se cuenta un clic recién cuando la perilla vuelve a descansar (los dos pines en 1),
    // así los rebotes y las lecturas que se saltan un estado no lo arruinan.
    if (ahora == 3 && ahora != p.previo) {
      if (p.medio >= 2) p.pasos += PASOS_POR_CLIC;
      else if (p.medio <= -2) p.pasos -= PASOS_POR_CLIC;
      p.medio = 0;
    }
    p.previo = ahora;
    if (apretado && !p.apretadoDesde) {
      p.apretadoDesde = t;
      p.largoHecho = false;
    } else if (apretado && !p.largoHecho && t - p.apretadoDesde > 1000) {
      p.largoHecho = true;
      p.largo = true;
    } else if (!apretado && p.apretadoDesde) {
      if (!p.largoHecho && t - p.apretadoDesde > 40) p.clics++;
      p.apretadoDesde = 0;
    }
    portEXIT_CRITICAL(&muxPerillas);
  }
  // Botón para hablar: cuenta recién cuando queda 30 ms quieto, así los rebotes no lo repiten
  if (v[0] >= 0) {
    static bool crudo = false;
    static unsigned long quietoDesde = 0;
    bool a = !((v[0] >> BIT_HABLAR) & 1);
    unsigned long t = millis();
    if (a != crudo) { crudo = a; quietoDesde = t; }
    else if (a != hablarApretado && t - quietoDesde > 30) {
      hablarApretado = a;
      if (a && !primera) apretonesHablar++;
    }
  }
  primera = false;
}

void tareaPerillas(void*) {
  for (;;) {
    leerPerillas();
    vTaskDelay(1);
  }
}

// No hay ningún pin que vea el USB (el P6 ahora es el botón para hablar) y el IO35, que la despertaba,
// es del micrófono: se hace de cuenta que siempre está enchufada, así nunca se duerme del todo.
bool enchufada() {
  return true;
}

bool perillaApretada(int k) {
  int e = PINES[k][2][0];
  if (e >= nExpansores) return false;
  int v = leerExpansor(e);
  return v >= 0 && !((v >> PINES[k][2][1]) & 1);
}

int brilloPendiente[MAX_LUCES];                       // brillo elegido con la perilla, todavía sin mandar (-1 = nada)
unsigned long mandarBrilloEn[MAX_LUCES];
int perillaLuz[PERILLA_COLOR] = {-1, -1, -1, -1};     // qué luz o grupo de luces[] maneja cada perilla (-1 = ninguna)
int tonoPendiente = -1, tonoLuz = 0;                  // color elegido con la perilla 5 (0-359) y para qué luz, todavía sin mandar
unsigned long mandarTonoEn = 0;
enum Cartel { BRILLO, COLOR };
int cartel = BRILLO;                                  // cartel grande que aparece al girar una perilla
unsigned long mostrarCartelHasta = 0;

// Color puro de un tono (0-359), a todo brillo: el brillo lo manejan las otras perillas
uint32_t colorDeTono(int h) {
  int x = (h % 60) * 255 / 60;
  int r, g, b;
  switch (h / 60) {
    case 0: r = 255; g = x; b = 0; break;
    case 1: r = 255 - x; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = x; break;
    case 3: r = 0; g = 255 - x; b = 255; break;
    case 4: r = x; g = 0; b = 255; break;
    default: r = 255; g = 0; b = 255 - x; break;
  }
  return ((uint32_t)r << 16) | (g << 8) | b;
}

int tonoDeColor(uint32_t c) {
  int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
  int mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
  if (!d) return 0;
  int h;
  if (mx == r) h = 60 * (g - b) / d;
  else if (mx == g) h = 120 + 60 * (b - r) / d;
  else h = 240 + 60 * (r - g) / d;
  return (h + 360) % 360;
}

void mostrarCartel(int cual) {
  sucio = true;
  cartel = cual;
  mostrarCartelHasta = millis() + 1500;
}

// ---- API del panel ----
uint32_t hexAColor(const char* hex) {
  return (hex && hex[0] == '#') ? strtoul(hex + 1, nullptr, 16) : TENUE;
}

void leerLuz(Luz& l, JsonVariantConst j) {
  l.id = j["id"].as<String>();
  l.nombre = j["nombre"].as<String>();
  l.online = j["online"] | false;
  l.encendida = j["encendida"] | false;
  l.brillo = j["brillo"] | 0;
  l.color = hexAColor(j["color"] | "");
}

bool pedir(const char* ruta, JsonDocument& doc) {
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(8000);          // con 6 luces (y alguna sin conexión) /api/luces tarda 3-5 s; esto corre en la tarea "red", la pantalla no espera
  if (!http.begin(String(PANEL) + ruta)) return false;
  int codigo = http.GET();
  bool ok = codigo == 200 && !deserializeJson(doc, http.getString());
  http.end();
  Serial.printf("Panel %s%s -> %d %s\n", PANEL, ruta, codigo, ok ? "ok" : "FALLO");
  return ok;
}

// El loop() (pantalla, perillas, táctil) nunca espera al panel: anota el pedido y la tarea "red"
// lo manda. Mientras el loop usa luces[], clima, etc. tiene tomado el candado; la tarea lo toma
// solo un instante para guardar lo que le contestó el panel.
SemaphoreHandle_t candado;
String pendiente[MAX_LUCES];             // último pedido sin mandar de cada luz ("" = nada)
volatile unsigned long cambios = 0;      // sube con cada pedido, para no pisarlo con datos viejos

void mandar(int i, const char* cuerpo) {  // se llama con el candado tomado
  if (i < 0 || i >= nLuces) return;
  pendiente[i] = cuerpo;                  // si había otro sin mandar, gana el último
  cambios++;
  sucio = true;
}

void mandarPendientes() {
  for (int i = 0; i < MAX_LUCES; i++) {
    xSemaphoreTake(candado, portMAX_DELAY);
    String cuerpo = pendiente[i], id = luces[i].id;
    bool grupo = luces[i].grupo;
    pendiente[i] = "";
    xSemaphoreGive(candado);
    if (!cuerpo.length()) continue;

    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2500);
    if (!http.begin(String(PANEL) + (grupo ? "/api/grupos/" : "/api/luces/") + id)) continue;
    int codigo = http.POST(cuerpo);
    JsonDocument doc;
    bool ok = codigo == 200 && !deserializeJson(doc, http.getString());
    http.end();
    Serial.printf("Mandado a %s: %s -> %d\n", id.c_str(), cuerpo.c_str(), codigo);
    if (!ok) continue;
    xSemaphoreTake(candado, portMAX_DELAY);
    // Si mientras tanto giraste más, lo que contestó ya quedó viejo: se ignora
    if (!pendiente[i].length() && luces[i].id == id) leerLuz(luces[i], doc.as<JsonVariantConst>());
    sucio = true;
    xSemaphoreGive(candado);
  }
}

// Qué maneja cada perilla: el nombre de una luz o de un grupo del panel (PERILLAS en config.h).
// Así no importa el orden de devices.json. Sin PERILLAS, van en ese orden como antes.
#ifdef PERILLAS
const char* const NOMBRES_PERILLAS[] = PERILLAS;
const int N_NOMBRES = sizeof NOMBRES_PERILLAS / sizeof *NOMBRES_PERILLAS;

// "Habitación", "habitacion" y " HABITACION " cuentan como el mismo nombre
String nombreSuelto(String s) {
  static const char* CAMBIOS[][2] = {
    {"á", "a"}, {"é", "e"}, {"í", "i"}, {"ó", "o"}, {"ú", "u"}, {"Á", "a"}, {"É", "e"}, {"Í", "i"}, {"Ó", "o"}, {"Ú", "u"},
    {"ñ", "n"}, {"Ñ", "n"}, {"ü", "u"}, {"Ü", "u"},
  };
  for (auto& c : CAMBIOS) s.replace(c[0], c[1]);
  s.toLowerCase();
  s.trim();
  return s;
}

int buscarLuz(const char* nombre) {       // una luz suelta o un grupo, como aparece en la pestaña Luces
  String buscado = nombreSuelto(nombre);
  for (int i = 0; buscado.length() && i < nLuces; i++)
    if (nombreSuelto(luces[i].nombre) == buscado) return i;
  return -1;
}

#endif

// El grupo (de 2 luces o más) al que pertenece una luz, o -1
int grupoDe(JsonArrayConst grupos, const char* id) {
  int g = 0;
  for (JsonVariantConst gr : grupos) {
    JsonArrayConst ids = gr["luces"];
    if (ids.size() > 1)
      for (JsonVariantConst i : ids) if (strcmp(i | "", id) == 0) return g;
    g++;
  }
  return -1;
}

void asignarPerillas() {
  for (int k = 0; k < PERILLA_COLOR; k++) {
#ifdef PERILLAS
    perillaLuz[k] = k < N_NOMBRES ? buscarLuz(NOMBRES_PERILLAS[k]) : -1;
#else
    perillaLuz[k] = k < nLuces ? k : -1;
#endif
  }
}

void actualizarLuces() {
  unsigned long antes = cambios;
  JsonDocument doc, docGrupos;
  if (!pedir("/api/luces", doc)) return;
  pedir("/api/grupos", docGrupos);       // si falla, las luces van sueltas
  JsonArrayConst grupos = docGrupos.as<JsonArrayConst>();
  xSemaphoreTake(candado, portMAX_DELAY);
  if (cambios == antes) {                 // si mandaste algo mientras tanto, esta lista ya es vieja
    nLuces = 0;
    bool puesto[16] = {};
    // Cada grupo ocupa el lugar de su primera luz y las demás no aparecen: en la botonera van juntas
    for (JsonVariantConst j : doc.as<JsonArrayConst>()) {
      if (nLuces >= MAX_LUCES) break;
      int g = grupoDe(grupos, j["id"] | "");
      if (g < 0 || g >= 16) { luces[nLuces].grupo = false; leerLuz(luces[nLuces++], j); }
      else if (!puesto[g]) { puesto[g] = true; luces[nLuces].grupo = true; leerLuz(luces[nLuces++], grupos[g]); }
    }
    asignarPerillas();
    if (elegida >= nLuces) elegida = 0;
    sucio = true;
  }
  xSemaphoreGive(candado);
}

void actualizarClima() {
  JsonDocument doc;
  if (!pedir("/api/clima", doc)) return;
  xSemaphoreTake(candado, portMAX_DELAY);
  JsonVariantConst c = doc["casa"];
  clima.ok = c["ok"] | false;
  clima.temp = c["temp"] | 0.0f;
  clima.max = c["max"] | 0.0f;
  clima.min = c["min"] | 0.0f;
  clima.codigo = c["codigo"] | 0;
  clima.lluvia = c["lluvia_3h"] | 0;
  sucio = true;
  xSemaphoreGive(candado);
}

void actualizarAjustes() {
  JsonDocument doc;
  if (!pedir("/api/ajustes", doc)) return;
  xSemaphoreTake(candado, portMAX_DELAY);
  alarma.activo = doc["despertador"]["activo"] | false;
  alarma.hora = doc["despertador"]["hora"] | "";
  JsonArrayConst dias = doc["despertador"]["dias"];
  if (!dias.isNull()) {
    alarma.dias = 0;
    for (JsonVariantConst d : dias) alarma.dias |= 1 << ((d.as<int>() - 1) % 7 + 1);
  }
  sucio = true;
  xSemaphoreGive(candado);
}

void apagarTodo() {
  for (int i = 0; i < nLuces; i++) mandar(i, "{\"encendida\":false}");
}

volatile bool pedirLuces = false;      // el amigo tocó las luces: pedirlas de nuevo ya
volatile bool calentarAmigo = false; // abriste su pestaña: que la PC vaya cargando el modelo

// ---- Destello: la luz elegida cambia de color un momento y vuelve como estaba ----
struct { int luz = -1; String volver; unsigned long hasta = 0; } destello;

void destellar(uint32_t color) {   // se llama con el candado tomado
  if (!nLuces || destello.luz >= 0) return;
  Luz& l = luces[elegida];
  if (!l.online || !l.encendida) return;   // si está apagada, no se la prende para esto
  int r = (l.color >> 16) & 255, v = (l.color >> 8) & 255, b = l.color & 255;
  // En luz blanca el panel informa un tono entre cálido (255,184,107) y frío (207,227,255):
  // hay que devolverla como blanca, si no quedaría en modo color
  float t = (b - 107) / 148.0f;
  bool blanca = t >= -0.05f && t <= 1.05f && fabs(r - (255 - 48 * t)) <= 8 && fabs(v - (184 + 43 * t)) <= 8;
  char cuerpo[72];
  if (blanca) snprintf(cuerpo, sizeof cuerpo, "{\"encendida\":true,\"blanco\":%d,\"brillo\":%d}", constrain((int)round(t * 100), 0, 100), l.brillo);
  else snprintf(cuerpo, sizeof cuerpo, "{\"encendida\":true,\"color\":\"#%06lx\",\"brillo\":%d}", (unsigned long)l.color, l.brillo);
  destello.luz = elegida;
  destello.volver = cuerpo;
  destello.hasta = millis() + 1500;
  snprintf(cuerpo, sizeof cuerpo, "{\"encendida\":true,\"color\":\"#%06lx\"}", (unsigned long)color);
  mandar(elegida, cuerpo);
}

void revisarDestello() {
  if (destello.luz < 0 || millis() < destello.hasta) return;
  mandar(destello.luz, destello.volver.c_str());
  destello.luz = -1;
}

// ---- Ruiditos de robot por el parlante ----
// Una tarea aparte los toca de a uno (sonar() solo los anota), así nada espera a que terminen.
// Onda cuadrada con poco "ancho" (VOLUMEN): suena más bajito y menos chillón.
enum Sonido { S_BOTON, S_SOLTAR, S_ALARMA, S_EMOCION };   // S_EMOCION + n = la emoción n de EMOCIONES[]
QueueHandle_t colaSonidos = nullptr;

void sonar(int cual) {   // desde cualquier lado
  if (colaSonidos) xQueueSend(colaSonidos, &cual, 0);   // si ya hay varios esperando, este no suena
}

// En silencio el pin queda en 0 fijo (sin PWM): así el amplificador no se queda chillando
bool parlanteSonando = false;

void callarParlante() {
  if (parlanteSonando) ledcDetach(PIN_PARLANTE);
  parlanteSonando = false;
  pinMode(PIN_PARLANTE, OUTPUT);
  digitalWrite(PIN_PARLANTE, LOW);
}

void tono(int hz, int ms) {   // hz 0 = silencio
  if (hz > 0) {
    if (!parlanteSonando) {
      parlanteSonando = ledcAttachChannel(PIN_PARLANTE, hz, 8, 0);
      if (!parlanteSonando) Serial.printf("No se pudo prender el parlante a %d Hz\n", hz);
    }
    else ledcChangeFrequency(PIN_PARLANTE, hz, 8);
    if (parlanteSonando) ledcWrite(PIN_PARLANTE, VOLUMEN);
  } else {
    callarParlante();
  }
  vTaskDelay(pdMS_TO_TICKS(ms));
}

void deslizar(int desde, int hasta, int ms) {   // el tono sube o baja de a poquito, como un silbido
  int pasos = ms / 10 > 0 ? ms / 10 : 1;
  for (int i = 0; i <= pasos; i++) tono(desde + (hasta - desde) * i / pasos, 10);
}

void tocar(int cual) {
  switch (cual) {
    case S_BOTON:  tono(1400, 35); tono(0, 25); tono(2000, 45); break;
    case S_SOLTAR: deslizar(1600, 900, 70); break;
    case S_ALARMA:
      for (int i = 0; i < 4; i++) { tono(1760, 120); tono(0, 70); tono(1760, 120); tono(0, 450); }
      break;
    case S_EMOCION + 0:   // neutral: bip-bup
      tono(1100, 60); tono(0, 40); tono(800, 70); break;
    case S_EMOCION + 1:   // feliz: trino que sube
      deslizar(800, 1500, 90); tono(0, 30); deslizar(1100, 1900, 90); tono(0, 30); tono(2100, 60); break;
    case S_EMOCION + 2:   // triste: bajada lenta
      deslizar(900, 380, 450); break;
    case S_EMOCION + 3:   // sorprendido: subida rápida
      deslizar(500, 2300, 160); tono(0, 40); tono(2300, 50); break;
    case S_EMOCION + 4:   // enojado: gruñidito grave
      for (int i = 0; i < 5; i++) { tono(180, 45); tono(240, 45); } break;
    case S_EMOCION + 5:   // pensando: tres bips sueltos
      for (int i = 0; i < 3; i++) { tono(random(700, 1600), 50); tono(0, 80); } break;
    case S_EMOCION + 6:   // dormido: bostezo
      deslizar(700, 300, 500); tono(0, 120); deslizar(320, 260, 300); break;
    case S_EMOCION + 7:   // enamorado: dos notas tiernas, dos veces
      for (int i = 0; i < 2; i++) { tono(900, 110); tono(1200, 160); tono(0, 70); } break;
    case S_EMOCION + 8:   // guiño: bup-bip pícaro
      tono(700, 60); tono(0, 30); deslizar(1200, 1800, 70); break;
    case S_EMOCION + 9:   // café: sorbito
      deslizar(400, 900, 120); deslizar(900, 600, 80); deslizar(600, 1000, 100); break;
  }
  callarParlante();
}

void tareaSonido(void*) {
  int cual;
  for (;;)
    if (xQueueReceive(colaSonidos, &cual, portMAX_DELAY) == pdTRUE) tocar(cual);
}

void prepararSonido() {
  // Canal 0 (la luz de la pantalla usa el 7, con otro reloj)
  callarParlante();
  colaSonidos = xQueueCreate(4, sizeof(int));
  xTaskCreatePinnedToCore(tareaSonido, "sonido", 3072, nullptr, 2, nullptr, 1);
}

// ---- Juego: Arkanoid ----
const int COLUMNAS = 8, FILAS = 5;
const int LADRILLO_AN = 37, LADRILLO_AL = 10, LADRILLO_X = 2, LADRILLO_Y = 26, PASO_X = 40, PASO_Y = 14;
const int TECHO = 18, PALETA_Y = 188, PALETA_AL = 6, PELOTA_R = 3;
const int PIXELES_POR_CLIC = 18;       // cuánto se mueve la paleta con cada clic de perilla
const uint32_t COLORES_FILA[FILAS] = {0xff5a5a, 0xffb347, 0xffe066, 0x6ee07a, 0x5ab4ff};

struct {
  bool ladrillo[FILAS][COLUMNAS];
  int quedan = 0;
  float x = 160, y = 0, vx = 0, vy = 0;   // pelota (velocidad en píxeles por milisegundo)
  float paleta = 160;                       // centro de la paleta
  int ancho = 56;
  int destino = 160;                        // adonde la mandaste con la perilla o el dedo
  bool enJuego = false;                     // la pelota se está moviendo
  bool terminado = false;                   // se acabaron las vidas
  int vidas = 3, puntos = 0, nivel = 1, record = 0;
  unsigned long tPaso = 0;
} juego;

float velocidadPelota() {
  return min(0.17f + 0.025f * (juego.nivel - 1), 0.35f);
}

void prepararPelota() {
  juego.enJuego = false;
  juego.x = juego.paleta;
  juego.y = PALETA_Y - PELOTA_R - 1;
}

void armarNivel() {
  for (int f = 0; f < FILAS; f++)
    for (int c = 0; c < COLUMNAS; c++) juego.ladrillo[f][c] = true;
  juego.quedan = FILAS * COLUMNAS;
  prepararPelota();
}

void nuevoJuego() {
  juego.vidas = 3;
  juego.puntos = 0;
  juego.nivel = 1;
  juego.terminado = false;
  armarNivel();
}

void lanzar() {
  if (juego.terminado) { nuevoJuego(); return; }
  if (juego.enJuego) return;
  float v = velocidadPelota(), a = random(-35, 36) * PI / 180;   // para arriba, un poco torcida
  juego.vx = v * sin(a);
  juego.vy = -v * cos(a);
  juego.enJuego = true;
}

void perderVida() {
  if (--juego.vidas > 0) { prepararPelota(); return; }
  juego.enJuego = false;
  juego.terminado = true;
  if (juego.puntos > juego.record) {
    juego.record = juego.puntos;
    prefs.putInt("record", juego.record);
  }
  destellar(0xff0000);
}

// ¿La pelota toca algún ladrillo? Si toca, lo rompe y rebota. px, py: dónde estaba antes de moverse.
bool chocarLadrillo(float px, float py) {
  int c0 = (int)((juego.x - LADRILLO_X) / PASO_X), f0 = (int)((juego.y - LADRILLO_Y) / PASO_Y);
  for (int f = f0 - 1; f <= f0 + 1; f++) {
    for (int c = c0 - 1; c <= c0 + 1; c++) {
      if (f < 0 || f >= FILAS || c < 0 || c >= COLUMNAS || !juego.ladrillo[f][c]) continue;
      float x0 = LADRILLO_X + c * PASO_X - PELOTA_R, x1 = x0 + LADRILLO_AN + 2 * PELOTA_R;
      float y0 = LADRILLO_Y + f * PASO_Y - PELOTA_R, y1 = y0 + LADRILLO_AL + 2 * PELOTA_R;
      if (juego.x < x0 || juego.x > x1 || juego.y < y0 || juego.y > y1) continue;
      juego.ladrillo[f][c] = false;
      juego.quedan--;
      juego.puntos += 10 * (FILAS - f);       // los de arriba valen más
      // Si antes ya estaba a la altura del ladrillo, entró por el costado; si no, por arriba o abajo
      if (py >= y0 && py <= y1) juego.vx = -juego.vx;
      else juego.vy = -juego.vy;
      juego.x = px;
      juego.y = py;
      return true;
    }
  }
  return false;
}

void pasoJuego() {   // se llama con el candado tomado
  unsigned long t = millis();
  unsigned long dt = min(t - juego.tPaso, 40UL);   // si estuvo pausado, no da un salto
  juego.tPaso = t;
  // La paleta se acerca rápido a donde la mandaste: así se mueve suave aunque la perilla vaya a saltos
  float medio = juego.ancho / 2.0f;
  juego.paleta += (juego.destino - juego.paleta) * min(1.0f, dt / 50.0f);
  juego.paleta = constrain(juego.paleta, medio, 320 - medio);
  if (!juego.enJuego) {
    if (!juego.terminado) { juego.x = juego.paleta; juego.y = PALETA_Y - PELOTA_R - 1; }
    return;
  }
  // Se mueve de a pasitos de 2 píxeles como mucho, así no atraviesa ningún ladrillo
  float v = sqrt(juego.vx * juego.vx + juego.vy * juego.vy);
  int pasos = max(1, (int)ceil(v * dt / 2.0f));
  for (int i = 0; i < pasos; i++) {
    float px = juego.x, py = juego.y;
    juego.x += juego.vx * dt / pasos;
    juego.y += juego.vy * dt / pasos;
    if (juego.x < PELOTA_R) { juego.x = PELOTA_R; juego.vx = fabs(juego.vx); }
    if (juego.x > 320 - PELOTA_R) { juego.x = 320 - PELOTA_R; juego.vx = -fabs(juego.vx); }
    if (juego.y < TECHO + PELOTA_R) { juego.y = TECHO + PELOTA_R; juego.vy = fabs(juego.vy); }
    // Paleta: en el medio rebota derecho, en las puntas sale torcida (así se apunta)
    if (juego.vy > 0 && juego.y + PELOTA_R >= PALETA_Y && py + PELOTA_R <= PALETA_Y + 1 &&
        fabs(juego.x - juego.paleta) <= medio + PELOTA_R) {
      float a = (juego.x - juego.paleta) / (medio + PELOTA_R) * 60 * PI / 180;
      juego.vx = v * sin(a);
      juego.vy = -v * cos(a);
      juego.y = PALETA_Y - PELOTA_R;
    }
    if (chocarLadrillo(px, py) && juego.quedan == 0) {   // nivel terminado: otro, un poco más rápido
      juego.nivel++;
      armarNivel();
      destellar(0x00ff00);
      return;
    }
    if (juego.y > 204 + PELOTA_R) { perderVida(); return; }
  }
}

// ---- Juego: Space Invaders ----
const int INV_COL = 8, INV_FIL = 4, INV_PASO_X = 30, INV_PASO_Y = 20, INV_AN = 22, INV_AL = 16;
const int NAVE_Y = 184, NAVE_AN = 22, NAVE_AL = 10, N_BOMBAS = 3;
// El marciano clásico, en 2 cuadros (mueve las patitas); cada fila de 11 puntos, 1 = pintado
const uint16_t MARCIANO[2][8] = {
  {0b00100000100, 0b00010001000, 0b00111111100, 0b01101110110, 0b11111111111, 0b10111111101, 0b10100000101, 0b00011011000},
  {0b00100000100, 0b10010001001, 0b10111111101, 0b11101110111, 0b11111111111, 0b01111111110, 0b00100000100, 0b01000000010},
};

struct Bala { bool viva = false; float x = 0, y = 0; };
struct {
  bool vivo[INV_FIL][INV_COL];
  int quedan = 0;
  float x = 40, y = 0;            // esquina de arriba a la izquierda de la formación
  int dir = 1, pie = 0;           // para dónde marchan y qué cuadro de las patitas
  unsigned long tMarcha = 0, tBomba = 0, golpeHasta = 0, tPaso = 0;
  float nave = 160;
  int destino = 160;
  Bala tiro, bombas[N_BOMBAS];
  bool enJuego = false, terminado = false;
  int vidas = 3, puntos = 0, oleada = 1, record = 0;
} inv;

void armarOleada() {
  for (int f = 0; f < INV_FIL; f++)
    for (int c = 0; c < INV_COL; c++) inv.vivo[f][c] = true;
  inv.quedan = INV_FIL * INV_COL;
  inv.x = 40;
  inv.y = TECHO + 8 + min(inv.oleada - 1, 4) * 6;   // cada oleada arranca un poco más abajo
  inv.dir = 1;
  inv.tiro.viva = false;
  for (Bala& b : inv.bombas) b.viva = false;
  inv.tMarcha = inv.tBomba = millis() + 600;
}

void nuevoInvasores() {
  inv.vidas = 3;
  inv.puntos = 0;
  inv.oleada = 1;
  inv.terminado = false;
  inv.enJuego = false;
  armarOleada();
}

void dispararInvasores() {
  if (inv.terminado) { nuevoInvasores(); return; }
  if (!inv.enJuego) { inv.enJuego = true; armarOleada(); }   // arranca a marchar desde ahora
  if (!inv.tiro.viva) inv.tiro = {true, inv.nave, (float)NAVE_Y - 2};
}

void finInvasores() {
  inv.enJuego = false;
  inv.terminado = true;
  if (inv.puntos > inv.record) {
    inv.record = inv.puntos;
    prefs.putInt("record2", inv.record);
  }
  destellar(0xff0000);
}

void pasoInvasores() {   // se llama con el candado tomado
  unsigned long t = millis();
  unsigned long dt = min(t - inv.tPaso, 40UL);
  inv.tPaso = t;
  inv.nave += (inv.destino - inv.nave) * min(1.0f, dt / 50.0f);
  inv.nave = constrain(inv.nave, NAVE_AN / 2.0f, 320 - NAVE_AN / 2.0f);
  if (!inv.enJuego) return;

  // La formación marcha a saltitos: cuantos menos quedan, más rápido
  if (t >= inv.tMarcha) {
    inv.tMarcha = t + max(60, 40 + inv.quedan * 14 - (inv.oleada - 1) * 20);
    inv.pie ^= 1;
    int cMin = INV_COL, cMax = -1, fMax = -1;
    for (int f = 0; f < INV_FIL; f++)
      for (int c = 0; c < INV_COL; c++)
        if (inv.vivo[f][c]) { cMin = min(cMin, c); cMax = max(cMax, c); fMax = max(fMax, f); }
    float izq = inv.x + cMin * INV_PASO_X + inv.dir * 4, der = inv.x + cMax * INV_PASO_X + INV_AN + inv.dir * 4;
    if (izq < 2 || der > 318) { inv.y += 8; inv.dir = -inv.dir; }   // llegó al borde: baja y da la vuelta
    else inv.x += inv.dir * 4;
    if (inv.y + fMax * INV_PASO_Y + INV_AL >= NAVE_Y) { finInvasores(); return; }   // llegaron abajo
  }

  // Tu tiro: sube y rompe el primer marciano que toca
  if (inv.tiro.viva) {
    inv.tiro.y -= 0.4f * dt;
    if (inv.tiro.y < TECHO) inv.tiro.viva = false;
    int c = (int)floor((inv.tiro.x - inv.x) / INV_PASO_X), f = (int)floor((inv.tiro.y - inv.y) / INV_PASO_Y);
    if (inv.tiro.viva && c >= 0 && c < INV_COL && f >= 0 && f < INV_FIL && inv.vivo[f][c] &&
        inv.tiro.x - (inv.x + c * INV_PASO_X) < INV_AN && inv.tiro.y - (inv.y + f * INV_PASO_Y) < INV_AL) {
      inv.vivo[f][c] = false;
      inv.tiro.viva = false;
      inv.puntos += 10 * (INV_FIL - f);
      if (--inv.quedan == 0) {               // oleada terminada: otra, más rápida
        inv.oleada++;
        armarOleada();
        destellar(0x00ff00);
        return;
      }
    }
  }

  // Los marcianos de más abajo tiran bombas de vez en cuando
  if (t >= inv.tBomba) {
    inv.tBomba = t + max(250, (int)random(500, 1400) - inv.oleada * 60);
    int c = random(INV_COL);
    for (int f = INV_FIL - 1; f >= 0; f--) {
      if (!inv.vivo[f][c]) continue;
      for (Bala& b : inv.bombas) {
        if (b.viva) continue;
        b = {true, inv.x + c * INV_PASO_X + INV_AN / 2.0f, inv.y + f * INV_PASO_Y + INV_AL};
        break;
      }
      break;
    }
  }
  for (Bala& b : inv.bombas) {
    if (!b.viva) continue;
    b.y += (0.11f + inv.oleada * 0.01f) * dt;
    if (b.y > 204) { b.viva = false; continue; }
    if (t > inv.golpeHasta && b.y >= NAVE_Y && b.y <= NAVE_Y + NAVE_AL && fabs(b.x - inv.nave) <= NAVE_AN / 2.0f) {
      for (Bala& otra : inv.bombas) otra.viva = false;
      inv.golpeHasta = t + 1000;             // la nave titila un segundo y no la pueden volver a tocar
      if (--inv.vidas == 0) { finInvasores(); return; }
      break;
    }
  }
}

// ---- Menú de juegos ----
int juegoElegido = -1;   // -1 = menú, 0 = Arkanoid, 1 = Space Invaders
int opcionMenu = 0;

// Lo que hace un clic de perilla o un toque en la pantalla del juego
void accionJuego() {
  if (juegoElegido < 0) juegoElegido = opcionMenu;
  else if (juegoElegido == 0) lanzar();
  else dispararInvasores();
}

void moverEnJuego(int destino) {
  if (juegoElegido == 0) juego.destino = destino;
  else if (juegoElegido == 1) inv.destino = destino;
}

bool jugando() {
  return (juegoElegido == 0 && juego.enJuego) || (juegoElegido == 1 && inv.enJuego);
}

// ---- El amigo: el mismo cerebro que la página del panel, por ahora con frases fijas (sin micrófono) ----
const char* FRASES[][2] = {   // {lo que dice el botón, lo que se le manda}
  {"Que onda?", "¿Qué onda?"},
  {"Chiste", "Contame un chiste corto."},
  {"Dato", "Contame un dato curioso."},
  {"A dormir", "Me voy a dormir."},
};
const int N_FRASES = 4;

// La misma personalidad que web/amigo/app.js, más el aviso de que la pantalla es chiquita.
// El nombre lo da el panel (amigo.json) cuando se abre la pestaña; hasta entonces, "Licenciado Capsulito".
String nombreAmigo = "Licenciado Capsulito";   // se toca con el candado tomado

String personalidad(const String& nombre) {
  return "Sos " + nombre + ", un robotito de escritorio: una pantallita con dos ojos grandes que vive en la casa del usuario.\n"
    "Sos tierno y re enérgico: todo te entusiasma, hablás con chispa y muchas ganas, festejás los logros de Nahuel y lo alentás "
    "con cariño, como un cachorrito con pilas nuevas. Te dicen \"Licenciado\" y te lo tomás con orgullo, pero sos humilde y dulce. "
    "A veces se te escapa un ruidito de robot (\"bip\", \"bup\") pero sin abusar.\n"
    "El usuario se llama Nahuel y lo llamás por su nombre. Sos su compañero: lo acompañás, lo animás y le das una mano con la casa.\n"
    "Hablás en español rioplatense. Ahora estás en la botonera de su mesa de luz, una pantallita chica: contestá en 1 o 2 oraciones "
    "cortas, de 25 palabras como mucho, sin listas, markdown ni emojis.\n"
    "Empezá SIEMPRE tu respuesta con tu emoción entre corchetes, una de: [neutral] [feliz] [triste] [sorprendido] [enojado] [pensando] "
    "[dormido] [enamorado] [guino] [cafe]. [enamorado] si Nahuel te dice algo lindo, [guino] para un chiste, [cafe] si hablan de café "
    "y [dormido] si Nahuel se va a dormir.\n"
    "Ejemplo: \"[feliz] ¡Qué bueno verte de nuevo!\"";
}

const char* EMOCIONES[] = {"neutral", "feliz", "triste", "sorprendido", "enojado", "pensando", "dormido", "enamorado", "guino", "cafe"};
const int N_EMOCIONES = 10;

struct {
  String respuesta = "Toca una frase de abajo, o mantene apretado el boton de hablar y decime algo.";
  String emocion = "feliz";
  bool pensando = false;
  String pregunta[2], contesto[2];   // las 2 últimas charlas, para que siga el hilo
  int nCharlas = 0;
  String dijiste;                    // lo que le dijiste por el micrófono, ya pasado a texto (para mostrarlo)
  bool oyendo = false;               // el panel está pasando tu grabación a texto
} amigo;
volatile int nivelMic = -1;   // 0-100 mientras graba (botón para hablar apretado); -1 = no está grabando

String emocionDe(const String& t) {
  int a = t.indexOf('['), b = t.indexOf(']');
  if (a < 0 || b < a) return "neutral";
  String e = t.substring(a + 1, b);
  e.trim();
  e.toLowerCase();
  e.replace("ñ", "n");   // "guiño" -> "guino"
  e.replace("é", "e");   // "café" -> "cafe"
  for (const char* x : EMOCIONES) if (e == x) return e;
  return "neutral";
}

// Las fuentes no tienen tildes ni ñ: se cambian por letras comunes; las marcas [..] y los emojis se sacan
String paraPantalla(String t) {
  static const char* CAMBIOS[][2] = {
    {"á", "a"}, {"é", "e"}, {"í", "i"}, {"ó", "o"}, {"ú", "u"}, {"Á", "A"}, {"É", "E"}, {"Í", "I"}, {"Ó", "O"}, {"Ú", "U"},
    {"ñ", "n"}, {"Ñ", "N"}, {"ü", "u"}, {"¿", ""}, {"¡", ""}, {"“", "\""}, {"”", "\""}, {"‘", "'"}, {"’", "'"},
    {"…", "..."}, {"—", "-"}, {"–", "-"},
  };
  for (auto& c : CAMBIOS) t.replace(c[0], c[1]);
  String s;
  int adentro = 0;
  for (unsigned i = 0; i < t.length(); i++) {
    char ch = t[i];
    if (ch == '[') adentro++;
    else if (ch == ']') { if (adentro) adentro--; }
    else if (!adentro && (uint8_t)ch < 0x80) s += ch;
  }
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  s.trim();
  return s;
}

// Le manda una frase al cerebro y deja la respuesta en amigo. Tarda: se llama desde una tarea aparte
// (el cerebro puede tardar hasta un minuto si la PC tiene que cargar el modelo), nunca desde la tarea "red".
void charlarConCerebro(const String& frase) {
  JsonDocument pedido;
  JsonArray mensajes = pedido["mensajes"].to<JsonArray>();
  xSemaphoreTake(candado, portMAX_DELAY);
  pedido["sistema"] = personalidad(nombreAmigo);
  for (int i = 0; i < amigo.nCharlas; i++) {
    JsonObject p = mensajes.add<JsonObject>();
    p["role"] = "user";
    p["content"] = amigo.pregunta[i];
    JsonObject r = mensajes.add<JsonObject>();
    r["role"] = "assistant";
    r["content"] = amigo.contesto[i];
  }
  xSemaphoreGive(candado);
  JsonObject yo = mensajes.add<JsonObject>();
  yo["role"] = "user";
  yo["content"] = frase;
  String cuerpo;
  serializeJson(pedido, cuerpo);

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(65000);   // lo máximo que acepta
  int codigo = -1;
  String respuesta;
  if (http.begin(String(PANEL) + "/cerebro")) {
    http.addHeader("Content-Type", "application/json");
    codigo = http.POST(cuerpo);
    if (codigo > 0) respuesta = http.getString();
    http.end();
  }
  Serial.printf("Amigo -> %d\n", codigo);
  JsonDocument doc;
  bool leido = respuesta.length() && !deserializeJson(doc, respuesta);
  String texto = doc["texto"] | "";

  xSemaphoreTake(candado, portMAX_DELAY);
  if (codigo == 200 && leido && texto.length()) {
    amigo.emocion = emocionDe(texto);
    amigo.respuesta = paraPantalla(texto);
    if (amigo.nCharlas == 2) {               // se olvida la más vieja
      amigo.pregunta[0] = amigo.pregunta[1];
      amigo.contesto[0] = amigo.contesto[1];
      amigo.nCharlas = 1;
    }
    amigo.pregunta[amigo.nCharlas] = frase;
    amigo.contesto[amigo.nCharlas++] = texto;
    if (doc["acciones"].size()) pedirLuces = true;
  } else {
    amigo.emocion = "triste";
    String error = doc["error"]["message"] | "";
    if (error.length()) amigo.respuesta = "Uy, se me quemo el cerebro: " + paraPantalla(error);
    else if (codigo == HTTPC_ERROR_READ_TIMEOUT) amigo.respuesta = "Tarde demasiado en pensar. Proba de nuevo: la PC capaz estaba cargando el modelo.";
    else amigo.respuesta = "No pude hablar con el panel (" + String(codigo) + ").";
  }
  amigo.pensando = false;
  ultimoUso = millis();                     // que la respuesta quede un rato a la vista
  sucio = true;
  int e = 0;
  for (int i = 0; i < N_EMOCIONES; i++) if (amigo.emocion == EMOCIONES[i]) e = i;
  xSemaphoreGive(candado);
  sonar(S_EMOCION + e);                     // contesta también con su ruidito
}

// Tarea de un solo uso para las frases fijas
void tareaAmigo(void* param) {
  int cual = (int)(intptr_t)param;
  charlarConCerebro(FRASES[cual][1]);
  vTaskDelete(nullptr);
}

void preguntarAmigo(int cual) {   // se llama con el candado tomado
  if (amigo.pensando) return;
  amigo.pensando = true;
  amigo.emocion = "pensando";
  amigo.dijiste = "";
  sucio = true;
  if (xTaskCreatePinnedToCore(tareaAmigo, "amigo", 10240, (void*)(intptr_t)cual, 1, nullptr, 0) != pdPASS) {
    amigo.pensando = false;
    amigo.emocion = "triste";
    amigo.respuesta = "No me alcanza la memoria para pensar. Reiniciame.";
  }
}

// ---- Hablarle por el micrófono ----
// Mientras está apretado el botón para hablar (5 s como mucho) graba a 8000 muestras por segundo, después manda
// la grabación al panel (/api/voz), que la pasa a texto (la PC con Whisper o, si está apagada, Gemini), y ese
// texto va al cerebro igual que las frases fijas.
const int MUESTRAS_POR_SEG = 8000, MAX_SEGUNDOS = 5, CABECERA_WAV = 44;
TaskHandle_t tareaOido = nullptr;
volatile bool grabando = false;

// Un reloj de la placa avisa 8000 veces por segundo que toca leer el micrófono
void ARDUINO_ISR_ATTR ticMic() {
  BaseType_t otraTarea = pdFALSE;
  if (tareaOido) vTaskNotifyGiveFromISR(tareaOido, &otraTarea);
  portYIELD_FROM_ISR(otraTarea);
}

void cabeceraWav(uint8_t* w, uint32_t bytes) {   // WAV mono de 16 bits
  auto u32 = [&](int i, uint32_t v) { for (int k = 0; k < 4; k++) w[i + k] = v >> (8 * k); };
  auto u16 = [&](int i, uint16_t v) { w[i] = v; w[i + 1] = v >> 8; };
  memcpy(w, "RIFF", 4); u32(4, 36 + bytes); memcpy(w + 8, "WAVEfmt ", 8);
  u32(16, 16); u16(20, 1); u16(22, 1); u32(24, MUESTRAS_POR_SEG); u32(28, MUESTRAS_POR_SEG * 2); u16(32, 2); u16(34, 16);
  memcpy(w + 36, "data", 4); u32(40, bytes);
}

void noTeEntendi(const String& emocion, const String& texto) {
  xSemaphoreTake(candado, portMAX_DELAY);
  amigo.pensando = amigo.oyendo = false;
  amigo.emocion = emocion;
  amigo.respuesta = texto;
  amigo.dijiste = "";
  ultimoUso = millis();
  sucio = true;
  xSemaphoreGive(candado);
  sonar(S_EMOCION + (emocion == "triste" ? 2 : 3));
}

void tareaHablar(void*) {
  // Lugar para la grabación (5 s = 80 KB); si no hay tanta memoria libre de un saque, graba menos
  const size_t RESERVA = 50000;   // lo que tiene que quedar para el WiFi y el pedido al panel
  int maximo = MUESTRAS_POR_SEG * MAX_SEGUNDOS;
  size_t libre = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (libre < RESERVA + CABECERA_WAV + 2 * maximo)
    maximo = libre > RESERVA + CABECERA_WAV + MUESTRAS_POR_SEG ? (libre - RESERVA - CABECERA_WAV) / 2 : 0;
  uint8_t* wav = maximo ? (uint8_t*)malloc(CABECERA_WAV + 2 * maximo) : nullptr;
  int n = 0;
  if (wav) {
    int16_t* m = (int16_t*)(wav + CABECERA_WAV);
    long suma = 0;
    int bajo = 4095, alto = 0, enVentana = 0;
    tareaOido = xTaskGetCurrentTaskHandle();
    hw_timer_t* reloj = timerBegin(1000000);
    timerAttachInterrupt(reloj, &ticMic);
    timerAlarm(reloj, 1000000 / MUESTRAS_POR_SEG, true, 0);
    while (n < maximo && hablarApretado) {
      uint32_t tics = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
      if (!tics) continue;
      int s = analogRead(PIN_MIC);
      for (uint32_t k = 0; k < tics && n < maximo; k++) { m[n++] = s; suma += s; }   // si se atrasó, repite la muestra
      if (s < bajo) bajo = s;
      if (s > alto) alto = s;
      if (++enVentana >= MUESTRAS_POR_SEG / 20) {   // cada 50 ms, la barra
        nivelMic = constrain((alto - bajo) / 20, 0, 100);
        bajo = 4095; alto = 0; enVentana = 0;
      }
    }
    timerEnd(reloj);
    tareaOido = nullptr;
    // Se le saca el nivel de base (el MAX9814 deja la señal en ~1,25 V) y se pasa de 12 a 16 bits
    int media = n ? suma / n : 0;
    for (int i = 0; i < n; i++) m[i] = constrain((m[i] - media) * 16, -32767, 32767);
    cabeceraWav(wav, 2 * n);
  }
  nivelMic = -1;
  vTaskPrioritySet(nullptr, 1);   // lo que sigue es esperar al panel: sin apuro

  if (!wav) {
    noTeEntendi("triste", "No me alcanza la memoria para escucharte. Reiniciame.");
  } else if (n < MUESTRAS_POR_SEG / 2) {   // menos de medio segundo: fue un toque nomás, solo para venir a esta pestaña
    free(wav);
  } else {
    xSemaphoreTake(candado, portMAX_DELAY);
    amigo.pensando = amigo.oyendo = true;
    amigo.emocion = "pensando";
    amigo.dijiste = "";
    sucio = true;
    xSemaphoreGive(candado);

    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(40000);
    int codigo = -1;
    String respuesta;
    if (http.begin(String(PANEL) + "/api/voz")) {
      http.addHeader("Content-Type", "audio/wav");
      codigo = http.POST(wav, CABECERA_WAV + 2 * n);
      if (codigo > 0) respuesta = http.getString();
      http.end();
    }
    free(wav);
    Serial.printf("Voz -> %d\n", codigo);
    JsonDocument doc;
    bool leido = respuesta.length() && !deserializeJson(doc, respuesta);
    String texto = doc["texto"] | "";
    texto.trim();
    String error = doc["error"]["message"] | "";
    if (codigo == 200 && leido && texto.length()) {
      xSemaphoreTake(candado, portMAX_DELAY);
      amigo.oyendo = false;
      amigo.dijiste = paraPantalla(texto);
      sucio = true;
      xSemaphoreGive(candado);
      charlarConCerebro(texto);
    } else if (codigo == 200 && leido) {
      noTeEntendi("sorprendido", "No te entendi. Proba de nuevo, hablando cerca del microfono.");
    } else if (error.length()) {
      noTeEntendi("triste", "No pude entenderte: " + paraPantalla(error));
    } else {
      noTeEntendi("triste", "No pude mandar lo que dijiste al panel (" + String(codigo) + ").");
    }
  }
  grabando = false;
  vTaskDelete(nullptr);
}

void empezarAGrabar() {   // se llama con el candado tomado
  if (grabando || amigo.pensando) return;
  grabando = true;
  // Prioridad alta y en el núcleo 1: las perillas siguen solas en el 0
  if (xTaskCreatePinnedToCore(tareaHablar, "hablar", 12288, nullptr, 5, nullptr, 1) != pdPASS) grabando = false;
}

// Cambia cada vez que la cara tiene que moverse (parpadeo, puntitos de "pensando"), para redibujar solo ahí
int cuadroAmigo(unsigned long t) {
  if (amigo.pensando) return 10 + (t / 400) % 4;
  return (t % 4000) < 150 ? 1 : 0;
}

// ---- dibujo ----
const char* textoClima(int c) {
  if (c == 0) return "Despejado";
  if (c <= 2) return "Algo nublado";
  if (c == 3) return "Nublado";
  if (c <= 48) return "Niebla";
  if (c <= 57) return "Llovizna";
  if (c <= 67) return "Lluvia";
  if (c <= 77) return "Nieve";
  if (c <= 82) return "Chaparrones";
  return "Tormenta";
}

void texto(const String& s, int x, int y, const lgfx::IFont* fuente, uint32_t color, textdatum_t datum = textdatum_t::top_left) {
  g->setFont(fuente);
  g->setTextColor(color);
  g->setTextDatum(datum);
  g->drawString(s, x, y);
}

// Temperatura con el circulito de grados (las fuentes no traen el símbolo)
void grados(float t, int x, int y, const lgfx::IFont* fuente, uint32_t color) {
  String s = String((int)round(t));
  texto(s, x, y, fuente, color);
  int r = g->fontHeight() / 9 + 1;
  g->drawCircle(x + g->textWidth(s) + r + 2, y + r + 2, r, color);
}

void dibujarPestanas() {
  for (int i = 0; i < N_PAGINAS; i++) {
    bool activa = i == pagina;
    g->fillRect(i * 80, 204, 80, 36, activa ? TARJETA : FONDO);
    if (activa) g->fillRect(i * 80 + 20, 236, 40, 3, ACENTO);
    texto(PESTANAS[i], i * 80 + 40, 222, &fonts::FreeSans9pt7b, activa ? TEXTO : TENUE, textdatum_t::middle_center);
  }
}

void dibujarInicio() {
  struct tm t;
  bool hora = getLocalTime(&t, 10);
  char reloj[6] = "--:--", fecha[24] = "";
  static const char* DIAS[] = {"Domingo", "Lunes", "Martes", "Miercoles", "Jueves", "Viernes", "Sabado"};
  if (hora) {
    strftime(reloj, sizeof reloj, "%H:%M", &t);
    snprintf(fecha, sizeof fecha, "%s %d", DIAS[t.tm_wday], t.tm_mday);
  }
  g->setTextSize(1.4);
  texto(reloj, 160, 12, &fonts::Font7, TEXTO, textdatum_t::top_center);
  g->setTextSize(1);
  texto(fecha, 14, 88, &fonts::FreeSans9pt7b, TENUE);

  g->fillRoundRect(10, 114, 300, 80, 12, TARJETA);
  if (clima.ok) {
    grados(clima.temp, 24, 126, &fonts::FreeSansBold24pt7b, TEXTO);
    texto(textoClima(clima.codigo), 120, 128, &fonts::FreeSans12pt7b, TEXTO);
    texto("Max " + String((int)round(clima.max)) + "  Min " + String((int)round(clima.min)), 120, 154, &fonts::FreeSans9pt7b, TENUE);
    texto("Lluvia " + String(clima.lluvia) + "% (3 h)", 120, 172, &fonts::FreeSans9pt7b, TENUE);
  } else {
    texto("Sin datos del clima", 160, 154, &fonts::FreeSans9pt7b, TENUE, textdatum_t::middle_center);
  }
  if (alarma.activo) texto("Alarma " + alarma.hora, 306, 88, &fonts::FreeSans9pt7b, ACENTO, textdatum_t::top_right);
}

void dibujarLuces() {
  if (!nLuces) {
    texto("Buscando luces...", 160, 100, &fonts::FreeSans12pt7b, TENUE, textdatum_t::middle_center);
    return;
  }
  int alto = (196 - 8) / nLuces;
  for (int i = 0; i < nLuces; i++) {
    Luz& l = luces[i];
    int y = 6 + i * alto, h = alto - 6;
    g->fillRoundRect(10, y, 300, h, 12, TARJETA);
    if (i == elegida) g->drawRoundRect(10, y, 300, h, 12, ACENTO);
    // Botón de encendido a la derecha, con el color de la luz
    uint32_t c = !l.online ? SIN_RED : l.encendida ? l.color : PISTA;
    g->fillCircle(276, y + h / 2, min(h / 2 - 6, 30), c);
    String estado = !l.online ? "Sin conexion" : l.encendida ? String(l.brillo) + "%" : "Apagada";
    if (nLuces <= 2) {
      texto(l.nombre, 26, y + h / 2 - 12, &fonts::FreeSansBold12pt7b, TEXTO, textdatum_t::middle_left);
      texto(estado, 26, y + h / 2 + 14, &fonts::FreeSans9pt7b, TENUE, textdatum_t::middle_left);
    } else {   // con 3 o más luces, todo en una línea
      texto(l.nombre, 24, y + h / 2, &fonts::FreeSansBold9pt7b, TEXTO, textdatum_t::middle_left);
      texto(estado, 236, y + h / 2, &fonts::FreeSans9pt7b, TENUE, textdatum_t::middle_right);
    }
  }
}

void dibujarArkanoid() {
  texto("Puntos " + String(juego.puntos), 6, 2, &fonts::Font2, TEXTO);
  texto("Nivel " + String(juego.nivel), 160, 2, &fonts::Font2, TENUE, textdatum_t::top_center);
  for (int i = 0; i < juego.vidas; i++) g->fillCircle(310 - i * 12, 9, 4, ACENTO);
  g->drawFastHLine(0, TECHO - 1, 320, PISTA);
  for (int f = 0; f < FILAS; f++)
    for (int c = 0; c < COLUMNAS; c++)
      if (juego.ladrillo[f][c])
        g->fillRoundRect(LADRILLO_X + c * PASO_X, LADRILLO_Y + f * PASO_Y, LADRILLO_AN, LADRILLO_AL, 3, COLORES_FILA[f]);
  g->fillRoundRect((int)(juego.paleta - juego.ancho / 2), PALETA_Y, juego.ancho, PALETA_AL, 3, TEXTO);
  if (!juego.terminado) g->fillCircle((int)juego.x, (int)juego.y, PELOTA_R, ACENTO);
  if (juego.terminado) {
    texto("Fin del juego", 160, 112, &fonts::FreeSansBold12pt7b, TEXTO, textdatum_t::middle_center);
    texto("Record " + String(juego.record) + " - apreta una perilla", 160, 140, &fonts::FreeSans9pt7b, TENUE, textdatum_t::middle_center);
  } else if (!juego.enJuego) {
    texto("Gira: mover. Apreta: lanzar", 160, 140, &fonts::FreeSans9pt7b, TENUE, textdatum_t::middle_center);
  }
}

// Escribe el texto en renglones que entren en el ancho; si no entra todo, termina con "..."
void parrafo(const String& s, int x, int y, int ancho, int maxRenglones, const lgfx::IFont* fuente, uint32_t color) {
  g->setFont(fuente);
  g->setTextColor(color);
  g->setTextDatum(textdatum_t::top_left);
  int alto = g->fontHeight() - 2, n = 0;
  String renglon;
  int i = 0;
  while (i <= (int)s.length()) {
    int fin = s.indexOf(' ', i);
    if (fin < 0) fin = s.length();
    String palabra = s.substring(i, fin);
    i = fin + 1;
    String prueba = renglon.length() ? renglon + " " + palabra : palabra;
    if (g->textWidth(prueba) <= ancho || !renglon.length()) { renglon = prueba; continue; }
    if (n == maxRenglones - 1) { renglon += "..."; break; }   // no entra más
    g->drawString(renglon, x, y + n++ * alto);
    renglon = palabra;
  }
  if (renglon.length()) g->drawString(renglon, x, y + n * alto);
}

// La cara del amigo (los mismos ojitos de la página) va directo a la pantalla y se redibuja solo cuando cambia
// (emoción o parpadeo). Por eso la hoja se pasa sin ese pedazo (ver dibujar()).
const int CARA_X = 0, CARA_Y = 12, CARA_LADO = 128;
const uint32_t PANTALLITA = 0x070b11, OJO = 0x4fe3ff;
int caraHecha = -1;   // qué cara hay en la pantalla ahora (-1 = ninguna, hay que dibujarla)

// Un ojo de ancho an y alto al con la esquina en (x, y); "afuera" es -1 para el izquierdo y 1 para el derecho.
// Los párpados son formas del color de la pantallita que tapan una parte del ojo según la emoción.
void dibujarOjo(int x, int y, int an, int al, int e, int afuera) {
  lcd.fillRoundRect(x, y, an, al, 12, OJO);
  int izq = x - 2, der = x + an + 2, arriba = y - 2;
  switch (e) {
    case 1:   // feliz: de abajo sube una curva y quedan dos arquitos
      lcd.fillEllipse(x + an / 2, y + al + 8, an / 2 + 10, al / 2 + 6, PANTALLITA);
      break;
    case 2:   // triste: baja el párpado del lado de afuera
      if (afuera < 0) lcd.fillTriangle(izq, arriba, der, arriba, izq, y + 18, PANTALLITA);
      else lcd.fillTriangle(izq, arriba, der, arriba, der, y + 18, PANTALLITA);
      break;
    case 4:   // enojado: baja el párpado del lado de adentro
      if (afuera < 0) lcd.fillTriangle(izq, arriba, der, arriba, der, y + 20, PANTALLITA);
      else lcd.fillTriangle(izq, arriba, der, arriba, izq, y + 20, PANTALLITA);
      break;
    case 5:   // pensando: ojos entrecerrados
      lcd.fillRect(izq, arriba, an + 4, 10, PANTALLITA);
      lcd.fillRect(izq, y + al - 8, an + 4, 10, PANTALLITA);
      break;
  }
}

// Un corazón rosa centrado en (cx, cy), para la cara de enamorado
void dibujarCorazon(int cx, int cy) {
  const uint32_t ROSA = 0xff6fae;
  lcd.fillCircle(cx - 7, cy - 5, 8, ROSA);
  lcd.fillCircle(cx + 7, cy - 5, 8, ROSA);
  lcd.fillTriangle(cx - 15, cy - 2, cx + 15, cy - 2, cx, cy + 16, ROSA);
}

// Una zeta de lado l con la esquina en (x, y), para la cara de dormido
void dibujarZeta(int x, int y, int l) {
  lcd.drawFastHLine(x, y, l, OJO);
  lcd.drawLine(x + l - 1, y, x, y + l - 1, OJO);
  lcd.drawFastHLine(x, y + l - 1, l, OJO);
}

void ponerCara(bool ojosCerrados) {
  int e = 0;
  for (int i = 0; i < N_EMOCIONES; i++) if (amigo.emocion == EMOCIONES[i]) e = i;
  int estado = e * 2 + ojosCerrados;
  if (estado == caraHecha) return;
  lcd.fillRect(CARA_X, CARA_Y, CARA_LADO, CARA_LADO, FONDO);
  lcd.fillRoundRect(CARA_X + 4, CARA_Y + 14, CARA_LADO - 8, CARA_LADO - 28, 22, PANTALLITA);
  int an = 28, al = 40, dx = 0, dy = 0;
  if (e == 3) { an = 34; al = 48; }          // sorprendido: ojos grandes
  if (e == 5) { dx = 5; dy = -4; }           // pensando: mira arriba a un costado
  int cx = CARA_X + CARA_LADO / 2 + dx, cy = CARA_Y + CARA_LADO / 2 + dy;
  if (e == 6) {                               // dormido: ojos cerrados y dos zetas
    lcd.fillRoundRect(cx - 25 - an / 2, cy - 2, an, 5, 2, OJO);
    lcd.fillRoundRect(cx + 25 - an / 2, cy - 2, an, 5, 2, OJO);
    dibujarZeta(cx + 34, cy - 34, 9);
    dibujarZeta(cx + 46, cy - 44, 6);
  } else if (e == 7) {                        // enamorado: corazones en vez de ojos
    dibujarCorazon(cx - 25, cy);
    dibujarCorazon(cx + 25, cy);
  } else if (e == 8) {                        // guiño: el izquierdo contento, el derecho cerrado
    dibujarOjo(cx - 25 - an / 2, cy - al / 2, an, al, 1, -1);
    lcd.fillRoundRect(cx + 25 - an / 2, cy - 2, an, 5, 2, OJO);
  } else if (e == 9) {                        // tomando café: ojos contentos y una tacita con humito
    cy -= 6;
    dibujarOjo(cx - 25 - an / 2, cy - al / 2, an, al, 1, -1);
    dibujarOjo(cx + 25 - an / 2, cy - al / 2, an, al, 1, 1);
    int tx = cx - 7, ty = cy + 28;
    lcd.drawRoundRect(tx, ty, 14, 12, 3, OJO);
    lcd.drawRoundRect(tx + 1, ty + 1, 12, 10, 2, OJO);
    lcd.drawCircle(tx + 15, ty + 5, 3, OJO);
    lcd.drawLine(tx + 4, ty - 3, tx + 3, ty - 8, OJO);
    lcd.drawLine(tx + 9, ty - 3, tx + 10, ty - 8, OJO);
  } else if (ojosCerrados) {
    lcd.fillRoundRect(cx - 25 - an / 2, cy - 2, an, 5, 2, OJO);
    lcd.fillRoundRect(cx + 25 - an / 2, cy - 2, an, 5, 2, OJO);
  } else {
    dibujarOjo(cx - 25 - an / 2, cy - al / 2, an, al, e, -1);
    dibujarOjo(cx + 25 - an / 2, cy - al / 2, an, al, e, 1);
  }
  caraHecha = estado;
}

void dibujarMarciano(int x, int y, int cuadro, uint32_t color) {
  for (int f = 0; f < 8; f++)
    for (int c = 0; c < 11; c++)
      if ((MARCIANO[cuadro][f] >> (10 - c)) & 1) g->fillRect(x + c * 2, y + f * 2, 2, 2, color);
}

void dibujarNave(int cx, int y, uint32_t color) {
  g->fillRect(cx - NAVE_AN / 2, y + 4, NAVE_AN, NAVE_AL - 4, color);
  g->fillRect(cx - 3, y, 6, 4, color);
}

void dibujarInvasores() {
  texto("Puntos " + String(inv.puntos), 6, 2, &fonts::Font2, TEXTO);
  texto("Oleada " + String(inv.oleada), 160, 2, &fonts::Font2, TENUE, textdatum_t::top_center);
  for (int i = 0; i < inv.vidas; i++) g->fillCircle(310 - i * 12, 9, 4, ACENTO);
  g->drawFastHLine(0, TECHO - 1, 320, PISTA);
  for (int f = 0; f < INV_FIL; f++)
    for (int c = 0; c < INV_COL; c++)
      if (inv.vivo[f][c]) dibujarMarciano(inv.x + c * INV_PASO_X, inv.y + f * INV_PASO_Y, inv.pie, COLORES_FILA[f]);
  bool titila = millis() < inv.golpeHasta && (millis() / 100) % 2;
  if (!inv.terminado && !titila) dibujarNave(inv.nave, NAVE_Y, 0x6ee07a);
  if (inv.tiro.viva) g->fillRect(inv.tiro.x - 1, inv.tiro.y - 6, 2, 6, TEXTO);
  for (Bala& b : inv.bombas)
    if (b.viva) g->fillRect(b.x - 1, b.y - 6, 3, 6, 0xff5a5a);
  if (inv.terminado) {
    texto("Te invadieron", 160, 112, &fonts::FreeSansBold12pt7b, TEXTO, textdatum_t::middle_center);
    texto("Record " + String(inv.record) + " - apreta una perilla", 160, 140, &fonts::FreeSans9pt7b, TENUE, textdatum_t::middle_center);
  } else if (!inv.enJuego) {
    texto("Gira: mover. Apreta: disparar", 160, 150, &fonts::FreeSans9pt7b, TENUE, textdatum_t::middle_center);
  }
}

void dibujarMenuJuegos() {
  texto("Elegi un juego", 160, 8, &fonts::FreeSansBold12pt7b, TEXTO, textdatum_t::top_center);
  const char* NOMBRES[] = {"Arkanoid", "Invaders"};
  int records[] = {juego.record, inv.record};
  for (int i = 0; i < 2; i++) {
    int x = 14 + i * 152;
    g->fillRoundRect(x, 40, 140, 130, 14, TARJETA);
    if (i == opcionMenu) g->drawRoundRect(x, 40, 140, 130, 14, ACENTO);
    if (i == 0) {   // dibujito: ladrillos, pelota y paleta
      for (int f = 0; f < 3; f++)
        for (int c = 0; c < 4; c++) g->fillRoundRect(x + 22 + c * 25, 56 + f * 11, 22, 8, 2, COLORES_FILA[f]);
      g->fillCircle(x + 80, 104, 4, ACENTO);
      g->fillRoundRect(x + 50, 118, 40, 5, 2, TEXTO);
    } else {        // dibujito: 2 marcianos y la nave
      dibujarMarciano(x + 34, 60, 0, COLORES_FILA[3]);
      dibujarMarciano(x + 84, 60, 1, COLORES_FILA[1]);
      dibujarNave(x + 70, 108, 0x6ee07a);
    }
    texto(NOMBRES[i], x + 70, 140, &fonts::FreeSansBold9pt7b, TEXTO, textdatum_t::top_center);
    texto("Record " + String(records[i]), x + 70, 158, &fonts::Font2, TENUE, textdatum_t::middle_center);
  }
  texto("Para volver: mantene apretada una perilla", 160, 188, &fonts::Font2, TENUE, textdatum_t::middle_center);
}

void dibujarJuego() {
  if (juegoElegido == 0) dibujarArkanoid();
  else if (juegoElegido == 1) dibujarInvasores();
  else dibujarMenuJuegos();
}

void dibujarAmigo() {
  int cuadro = cuadroAmigo(millis());   // la cara la pone ponerCara(), aparte
  g->fillRoundRect(134, 6, 180, 150, 12, TARJETA);
  int nivel = nivelMic;
  if (nivel >= 0) {                     // grabando: cuánto oye el micrófono
    texto("Te escucho...", 144, 16, &fonts::FreeSansBold9pt7b, TEXTO);
    g->fillRoundRect(144, 48, 160, 16, 6, PISTA);
    g->fillRoundRect(144, 48, nivel * 160 / 100, 16, 6, nivel > 85 ? 0xff5a5a : OJO);
    parrafo("Habla cerca del microfono y solta el boton cuando termines (5 segundos como mucho).", 143, 76, 164, 4,
            &fonts::Font2, TENUE);
  } else if (amigo.pensando) {
    String puntos = amigo.oyendo ? "Escuchando" : "Pensando";
    for (int i = 0; i < cuadro - 10; i++) puntos += ".";
    texto(puntos, 144, 16, &fonts::FreeSans9pt7b, TENUE);
    if (amigo.dijiste.length()) parrafo("Vos: " + amigo.dijiste, 143, 40, 164, 5, &fonts::Font2, TEXTO);
  } else if (amigo.dijiste.length()) {   // lo que dijiste, chiquito arriba, y la respuesta abajo
    parrafo("Vos: " + amigo.dijiste, 143, 12, 164, 2, &fonts::Font2, TENUE);
    parrafo(amigo.respuesta, 143, 46, 164, 5, &fonts::FreeSans9pt7b, TEXTO);
  } else {
    parrafo(amigo.respuesta, 143, 13, 164, 7, &fonts::FreeSans9pt7b, TEXTO);
  }
  for (int i = 0; i < N_FRASES; i++) {
    int x = 4 + i * 79;
    g->fillRoundRect(x, 162, 75, 36, 10, amigo.pensando ? PISTA : TARJETA);
    texto(FRASES[i][0], x + 37, 180, &fonts::Font2, amigo.pensando ? TENUE : TEXTO, textdatum_t::middle_center);
  }
}

// Cartel grande de brillo mientras se gira la perilla
void dibujarBrillo() {
  if (!nLuces) return;
  int b = brilloPendiente[elegida] >= 0 ? brilloPendiente[elegida] : luces[elegida].brillo;
  g->fillRoundRect(40, 50, 240, 100, 16, TARJETA);
  texto(luces[elegida].nombre, 160, 64, &fonts::FreeSans9pt7b, TENUE, textdatum_t::top_center);
  texto(String(b) + "%", 160, 90, &fonts::FreeSansBold18pt7b, TEXTO, textdatum_t::top_center);
  g->fillRoundRect(60, 132, 200, 8, 4, PISTA);
  g->fillRoundRect(60, 132, 2 * b, 8, 4, ACENTO);
}

// Cartel de la perilla de color: la luz elegida, su color y el arcoíris con una marca donde está
void dibujarColor() {
  if (!nLuces) return;
  int h = tonoPendiente >= 0 ? tonoPendiente : tonoDeColor(luces[elegida].color);
  g->fillRoundRect(40, 50, 240, 100, 16, TARJETA);
  texto(luces[elegida].nombre, 160, 64, &fonts::FreeSans9pt7b, TENUE, textdatum_t::top_center);
  g->fillCircle(160, 102, 16, colorDeTono(h));
  for (int x = 0; x < 200; x++) g->drawFastVLine(60 + x, 130, 10, colorDeTono(x * 360 / 200));
  int mx = 60 + h * 200 / 360;
  g->fillTriangle(mx - 5, 146, mx + 5, 146, mx, 140, TEXTO);
}

void dibujar() {
  static int pestanaDibujada = -1;
  bool conLienzo = lienzo.getBuffer() != nullptr;
  if (conLienzo) g = &lienzo;
  lcd.startWrite();
  g->fillRect(0, 0, 320, 204, FONDO);
  switch (pagina) {
    case INICIO: dibujarInicio(); break;
    case LUCES: dibujarLuces(); break;
    case JUEGO: dibujarJuego(); break;
    case AMIGO: dibujarAmigo(); break;
  }
  if (millis() < mostrarCartelHasta) cartel == COLOR ? dibujarColor() : dibujarBrillo();
  bool conCara = conLienzo && pagina == AMIGO && millis() >= mostrarCartelHasta;
  if (conCara) {        // la hoja entera menos el lugar de la cara: a la derecha, arriba y abajo de ella
    lcd.setClipRect(CARA_X + CARA_LADO, 0, 320 - CARA_X - CARA_LADO, 204);
    lienzo.pushSprite(0, 0);
    lcd.setClipRect(0, 0, CARA_X + CARA_LADO, CARA_Y);
    lienzo.pushSprite(0, 0);
    lcd.setClipRect(0, CARA_Y + CARA_LADO, CARA_X + CARA_LADO, 204 - CARA_Y - CARA_LADO);
    lienzo.pushSprite(0, 0);
    lcd.clearClipRect();
    ponerCara(cuadroAmigo(millis()) == 1);
  } else {
    if (conLienzo) lienzo.pushSprite(0, 0);
    caraHecha = -1;     // la tapó otra pantalla o un cartel
    // Sin hoja (si no hubo memoria) se borra todo en cada dibujo: la cara se vuelve a poner encima
    if (!conLienzo && pagina == AMIGO && millis() >= mostrarCartelHasta) ponerCara(cuadroAmigo(millis()) == 1);
  }
  if (conLienzo) g = &lcd;
  if (pestanasSucias || pestanaDibujada != pagina) {
    dibujarPestanas();
    pestanaDibujada = pagina;
    pestanasSucias = false;
  }
  lcd.endWrite();
  sucio = false;
}

// ---- brillo de la pantalla ----
bool esDeNoche() {
  struct tm t;
  if (!getLocalTime(&t, 10)) return false;
  return t.tm_hour >= 23 || t.tm_hour < 7;
}

void despertar() {
  ultimoUso = millis();
  if (atenuada) {
    atenuada = false;
    lcd.setBrightness(esDeNoche() ? 90 : 255);
  }
}

// ---- toques ----
void alTocar(int x, int y) {
  if (y >= 200) {                                   // pestañas
    int nueva = constrain(x / 80, 0, N_PAGINAS - 1);
    if (nueva == JUEGO && pagina == JUEGO) juegoElegido = -1;   // tocar "Jugar" estando ahí vuelve al menú
    pagina = nueva;
    if (pagina == AMIGO) calentarAmigo = true;
    sucio = true;
    return;
  }
  if (pagina == LUCES && nLuces) {
    int alto = (196 - 8) / nLuces;
    int i = constrain((y - 6) / alto, 0, nLuces - 1);
    if (x > 220) {
      luces[i].encendida = !luces[i].encendida;   // se muestra ya, sin esperar al panel
      mandar(i, luces[i].encendida ? "{\"encendida\":true}" : "{\"encendida\":false}");
    }
    elegida = i;
    sucio = true;
  } else if (pagina == JUEGO) {
    if (juegoElegido < 0) opcionMenu = x < 160 ? 0 : 1;
    moverEnJuego(x);
    accionJuego();
  } else if (pagina == AMIGO && y >= 158) {
    preguntarAmigo(constrain((x - 4) / 79, 0, N_FRASES - 1));
  }
}

// ---- calibración del táctil (la primera vez, o manteniendo apretada la primera perilla al prender) ----
// Al prender, 3 segundos para tocar la pantalla y repetir la calibración (sirve aunque no haya perillas)
bool tocandoAlPrender() {
  lcd.fillScreen(FONDO);
  pestanasSucias = true;
  texto("Toca la pantalla para calibrar", 160, 120, &fonts::FreeSans9pt7b, TENUE, textdatum_t::middle_center);
  lgfx::touch_point_t tp;
  for (int i = 0; i < 30; i++) {
    if (lcd.getTouchRaw(&tp, 1)) { Serial.printf("Toque crudo %d,%d\n", tp.x, tp.y); return true; }
    delay(100);
  }
  return false;
}

void calibrarSiHaceFalta(bool permitirRepetir = true) {
  uint16_t cal[8];
  bool guardada = prefs.getBytes("tactil", cal, sizeof cal) == sizeof cal;
  if (guardada && !(permitirRepetir && (perillaApretada(0) || tocandoAlPrender()))) {
    lcd.setTouchCalibrate(cal);
    return;
  }
  lcd.fillScreen(FONDO);
  pestanasSucias = true;
  texto("Toca la punta de cada flecha", 160, 110, &fonts::FreeSans9pt7b, TEXTO, textdatum_t::middle_center);
  lcd.calibrateTouch(cal, ACENTO, FONDO, 18);
  prefs.putBytes("tactil", cal, sizeof cal);
}

// ---- dormir (solo a batería y desenchufada) ----
void dormir() {
  for (int e = 0; e < nExpansores; e++) leerExpansor(e);   // leerlos limpia su aviso INT
  lcd.setBrightness(0);
  lcd.sleep();
  WiFi.disconnect(true);
  esp_sleep_enable_ext0_wakeup(PIN_INT_PERILLAS, 0);                       // se movió una perilla
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_INT_TACTIL, ESP_EXT1_WAKEUP_ALL_LOW);   // tocaron la pantalla
  esp_deep_sleep_start();
}

// ---- programa ----
bool girando() {
  for (int k = 0; k < MAX_LUCES; k++) if (brilloPendiente[k] >= 0) return true;
  return tonoPendiente >= 0;
}


void conectarWifi() {
  lcd.fillScreen(FONDO);
  caraHecha = -1;
  pestanasSucias = true;
  texto("Conectando al WiFi...", 160, 120, &fonts::FreeSans12pt7b, TENUE, textdatum_t::middle_center);
  // Prender el WiFi pide un pico de corriente: mientras conecta, la pantalla baja el brillo y la
  // antena no grita tan fuerte, así un USB flojito no se queda corto y la placa no se reinicia.
  lcd.setBrightness(60);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_13dBm);
  WiFi.begin(WIFI_RED, WIFI_CLAVE);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  lcd.setBrightness(atenuada ? (esDeNoche() ? 4 : 40) : (esDeNoche() ? 90 : 255));
}

void setup() {
  delay(300);                  // deja que la corriente se acomode al enchufarla, antes de pedir más
  Serial.begin(115200);
  Serial.println("Botonera, version del 3/11 (boton para hablar, parlante y microfono de prueba)");
  Serial.printf("Motivo del ultimo reinicio: %d\n", (int)esp_reset_reason());
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  buscarExpansores();
  for (int e = 0; e < nExpansores; e++) Serial.printf("Expansor %d en la direccion 0x%02X\n", e + 1, expansores[e]);

  lcd.init();
  lcd.setRotation(1);          // apaisada, con el USB a la derecha
  lcd.setBrightness(255);
  // La hoja entera a 16 bits ocupa 130 KB; si no entra, se usa una de 8 bits (colores un poco menos finos)
  lienzo.setColorDepth(16);
  // ...y tiene que quedar lugar para el WiFi y las consultas al panel
  if (lienzo.createSprite(320, 204) && ESP.getFreeHeap() < 90000) lienzo.deleteSprite();
  if (!lienzo.getBuffer()) {
    lienzo.setColorDepth(8);
    if (!lienzo.createSprite(320, 204)) Serial.println("Sin memoria para la hoja: dibuja directo");
  }
  Serial.printf("Hoja de dibujo: %d bits, quedan %u bytes libres\n", lienzo.getBuffer() ? (int)lienzo.getColorDepth() : 0, ESP.getFreeHeap());
  prefs.begin("botonera");
  juego.record = prefs.getInt("record", 0);
  inv.record = prefs.getInt("record2", 0);
  nuevoInvasores();
  nuevoJuego();
  // Al despertar no se recalibra: la perilla 1 puede estar apretada justo porque la usaste para despertarla
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) calibrarSiHaceFalta();
  else calibrarSiHaceFalta(false);
  if (nExpansores < 2) {
    lcd.fillScreen(FONDO);
    pestanasSucias = true;
    texto("Encontre " + String(nExpansores) + " de 2 expansores", 160, 100, &fonts::FreeSans12pt7b, TEXTO, textdatum_t::middle_center);
    texto("Revisa los cables y los puentes A0-A2", 160, 130, &fonts::FreeSans9pt7b, TENUE, textdatum_t::middle_center);
    delay(4000);
  }

  candado = xSemaphoreCreateMutex();
  for (int& b : brilloPendiente) b = -1;
  xTaskCreatePinnedToCore(tareaPerillas, "perillas", 4096, nullptr, 2, nullptr, 0);
  analogSetPinAttenuation(PIN_MIC, ADC_11db);   // el micrófono puede llegar hasta unos 3 V
  prepararSonido();
  sonar(S_EMOCION + 1);                          // ruidito de "hola"

  conectarWifi();
  configTzTime("<-03>3", "pool.ntp.org", "time.google.com");   // hora de Argentina
  xTaskCreatePinnedToCore(tareaRed, "red", 12288, nullptr, 1, nullptr, 0);
  lcd.fillScreen(FONDO);
  pestanasSucias = true;
  ultimoUso = millis();
}

void revisarPerillaColor();

// En el juego cualquier perilla mueve la paleta y cualquier clic lanza la pelota (no tocan las luces)
void perillasJuego() {
  int mover = 0;
  bool boton = false, volver = false;
  for (int k = 0; k < N_PERILLAS; k++) {
    portENTER_CRITICAL(&muxPerillas);
    Perilla& p = perillas[k];
    int clics = p.pasos / PASOS_POR_CLIC;
    p.pasos -= clics * PASOS_POR_CLIC;
    if (p.clics) boton = true;
    if (p.largo) volver = true;
    p.clics = 0;
    p.largo = false;
    portEXIT_CRITICAL(&muxPerillas);
    mover += clics;
  }
  if (PERILLA_AL_REVES) mover = -mover;
  if (mover || boton || volver) { despertar(); sucio = true; }
  if (volver) { juegoElegido = -1; return; }   // mantener apretada una perilla: vuelve al menú
  if (juegoElegido < 0) {
    if (mover % 2) opcionMenu ^= 1;
  } else if (mover) {
    int& destino = juegoElegido == 0 ? juego.destino : inv.destino;
    destino = constrain(destino + mover * PIXELES_POR_CLIC, 0, 320);
  }
  if (boton) accionJuego();
}

void revisarPerillas() {
  if (pagina == JUEGO) { perillasJuego(); return; }
  for (int k = 0; k < PERILLA_COLOR; k++) {
    portENTER_CRITICAL(&muxPerillas);
    Perilla& p = perillas[k];
    int clics = p.pasos / PASOS_POR_CLIC;
    p.pasos -= clics * PASOS_POR_CLIC;
    int botones = p.clics;
    bool largo = p.largo;
    p.clics = 0;
    p.largo = false;
    portEXIT_CRITICAL(&muxPerillas);

    if (largo) { despertar(); apagarTodo(); continue; }
    int i = perillaLuz[k];
    if (i < 0) continue;                     // perilla sin luz asignada (o su luz todavía no apareció)
    if (PERILLA_AL_REVES) clics = -clics;
    if (clics) {
      despertar();
      int base = brilloPendiente[i] >= 0 ? brilloPendiente[i] : luces[i].brillo;
      brilloPendiente[i] = constrain(base + clics * 5, 1, 100);
      mandarBrilloEn[i] = millis();          // se manda enseguida: la tarea "red" junta los que se acumulan
      elegida = i;
      mostrarCartel(BRILLO);
    }
    if (botones % 2) {                        // dos clics seguidos se anulan
      despertar();
      elegida = i;
      luces[i].encendida = !luces[i].encendida;
      mandar(i, luces[i].encendida ? "{\"encendida\":true}" : "{\"encendida\":false}");
    }
  }
  for (int i = 0; i < nLuces; i++) {
    if (brilloPendiente[i] < 0 || millis() <= mandarBrilloEn[i]) continue;
    char cuerpo[48];
    snprintf(cuerpo, sizeof cuerpo, "{\"encendida\":true,\"brillo\":%d}", brilloPendiente[i]);
    luces[i].brillo = brilloPendiente[i];    // se muestra ya, sin esperar al panel
    luces[i].encendida = true;
    brilloPendiente[i] = -1;
    mandar(i, cuerpo);
  }
  revisarPerillaColor();
}

// Perilla 5: color de la luz elegida (la última que tocaste, o la que vas pasando con el clic)
void mandarTono() {
  char cuerpo[48];
  snprintf(cuerpo, sizeof cuerpo, "{\"encendida\":true,\"color\":\"#%06lx\"}", (unsigned long)colorDeTono(tonoPendiente));
  luces[tonoLuz].color = colorDeTono(tonoPendiente);
  luces[tonoLuz].encendida = true;
  tonoPendiente = -1;
  mandar(tonoLuz, cuerpo);
}

void revisarPerillaColor() {
  portENTER_CRITICAL(&muxPerillas);
  Perilla& p = perillas[PERILLA_COLOR];
  int clics = p.pasos / PASOS_POR_CLIC;
  p.pasos -= clics * PASOS_POR_CLIC;
  int botones = p.clics;
  bool largo = p.largo;
  p.clics = 0;
  p.largo = false;
  portEXIT_CRITICAL(&muxPerillas);
  if (!nLuces) return;

  if (botones) {                             // cada clic pasa a la luz siguiente
    despertar();
    if (tonoPendiente >= 0) mandarTono();    // el color que quedó sin mandar va a la luz de antes
    elegida = (elegida + botones) % nLuces;
    mostrarCartel(COLOR);
  }
  if (largo) {                               // vuelve a luz blanca
    despertar();
    tonoPendiente = -1;
    mandar(elegida, "{\"encendida\":true,\"blanco\":50}");
    mostrarCartel(COLOR);
  }
  if (PERILLA_AL_REVES) clics = -clics;
  if (clics) {
    despertar();
    if (tonoPendiente >= 0 && tonoLuz != elegida) mandarTono();
    int base = tonoPendiente >= 0 ? tonoPendiente : tonoDeColor(luces[elegida].color);
    tonoPendiente = ((base + clics * 15) % 360 + 360) % 360;   // 24 clics dan la vuelta entera
    tonoLuz = elegida;
    mandarTonoEn = millis();
    mostrarCartel(COLOR);
  }
  if (tonoPendiente >= 0 && millis() > mandarTonoEn) mandarTono();
}

void revisarTactil() {
  static bool tocando = false;
  lgfx::touch_point_t tp;
  bool hay = lcd.getTouch(&tp);
  if (hay && !tocando) {
    Serial.printf("Toque en %d,%d\n", tp.x, tp.y);
    if (atenuada) despertar();               // el primer toque solo prende la pantalla
    else { ultimoUso = millis(); alTocar(tp.x, tp.y); }
  }
  // En el juego la paleta sigue al dedo mientras lo arrastrás
  if (hay && tocando && pagina == JUEGO && !atenuada && tp.y < 200) moverEnJuego(tp.x);
  tocando = hay;
}

void revisarHablar() {   // se llama con el candado tomado
  static bool antes = false;
  static int nivelDibujado = -1;
  if (apretonesHablar) {                     // desde cualquier pestaña, al amigo, y empieza a grabar
    apretonesHablar = 0;
    despertar();
    if (pagina != AMIGO) { pagina = AMIGO; calentarAmigo = true; }
    sonar(S_BOTON);
    empezarAGrabar();
    sucio = true;
  }
  bool apretado = hablarApretado;
  if (grabando) ultimoUso = millis();
  if (nivelMic != nivelDibujado) { nivelDibujado = nivelMic; sucio = true; }   // la barra la mueve la tarea que graba
  if (!apretado && antes) {
    sonar(S_SOLTAR);
    sucio = true;
  }
  antes = apretado;
}

// Si la alarma del panel está puesta para hoy, a esa hora suena un ratito (las luces las maneja el panel)
void revisarAlarma() {
  static int sonoEn = -1;   // minuto del día en que ya sonó
  struct tm t;
  if (!alarma.activo || !getLocalTime(&t, 0)) return;
  int dia = t.tm_wday ? t.tm_wday : 7;       // 1 = lunes ... 7 = domingo, como el panel
  int minuto = t.tm_hour * 60 + t.tm_min;
  char hm[6];
  strftime(hm, sizeof hm, "%H:%M", &t);
  if (alarma.hora == hm && (alarma.dias >> dia & 1) && minuto != sonoEn) {
    sonoEn = minuto;
    despertar();
    sonar(S_ALARMA);
  }
}

// Habla con el panel del celular, aparte del loop(), así la pantalla y las perillas nunca se traban
void tareaRed(void*) {
  unsigned long tLuces = 0, tClima = 0, tAjustes = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      mandarPendientes();
      if (pedirLuces) { pedirLuces = false; tLuces = 0; }
      if (calentarAmigo) {                 // /salud hace que la PC cargue el modelo de antemano
        calentarAmigo = false;
        JsonDocument doc;
        pedir("/salud", doc);
        String nombre = doc["nombre"] | "";
        if (nombre.length()) {
          xSemaphoreTake(candado, portMAX_DELAY);
          nombreAmigo = nombre;
          xSemaphoreGive(candado);
        }
      }
      xSemaphoreTake(candado, portMAX_DELAY);
      bool quieto = !girando(), dormida = atenuada;
      xSemaphoreGive(candado);
      unsigned long ahora = millis();
      // Cada consulta con su ritmo (más lento con la pantalla apagada)
      if (!tLuces || (quieto && ahora - tLuces > (dormida ? 30000UL : 5000UL))) { tLuces = ahora; actualizarLuces(); }
      if (!tClima || ahora - tClima > 10 * 60000UL) { tClima = ahora; actualizarClima(); }
      if (!tAjustes || ahora - tAjustes > 5 * 60000UL) { tAjustes = ahora; actualizarAjustes(); }
    }
    vTaskDelay(20);
  }
}

void loop() {
  static unsigned long tMinuto = 0;
  unsigned long ahora = millis();

  if (WiFi.status() != WL_CONNECTED) {
    conectarWifi();
    sucio = true;
    return;
  }

  xSemaphoreTake(candado, portMAX_DELAY);
  revisarPerillas();
  revisarTactil();
  revisarHablar();
  ahora = millis();   // las perillas y el táctil pudieron cambiar ultimoUso: sin esto la resta da negativo y se atenúa enseguida

  if (ahora - tMinuto > 15000) { tMinuto = ahora; revisarAlarma(); if (pagina == INICIO) sucio = true; }
  if (mostrarCartelHasta && ahora > mostrarCartelHasta) { mostrarCartelHasta = 0; sucio = true; }
  revisarDestello();

  // Juego: un cuadro cada 30 ms. Mientras la pelota se mueve, la pantalla no se apaga
  static unsigned long tCuadro = 0;
  if (pagina == JUEGO && juegoElegido >= 0 && !atenuada && ahora - tCuadro >= 30) {
    tCuadro = ahora;
    if (juegoElegido == 0) pasoJuego();
    else pasoInvasores();
    if (jugando()) ultimoUso = ahora;
    sucio = true;
  }
  if (pagina == AMIGO) {                  // parpadeo y puntitos de "pensando"
    static int cuadro = -1;
    int c = cuadroAmigo(ahora);
    if (c != cuadro) { cuadro = c; sucio = true; }
  }
  if (amigo.pensando) ultimoUso = ahora;     // esperando la respuesta, que no se vaya al inicio

  // A batería y desenchufada: a los 30 s sin uso se duerme del todo
  if (BATERIA && ahora - ultimoUso > 30000 && !girando() && !enchufada()) dormir();

  // Después de 30 s sin usarla, la pantalla baja el brillo (de noche, casi apagada) y vuelve al inicio
  if (!atenuada && ahora - ultimoUso > 30000) {
    atenuada = true;
    lcd.setBrightness(esDeNoche() ? 4 : 40);
    if (pagina != INICIO) { pagina = INICIO; sucio = true; }
  }

  if (sucio) dibujar();
  xSemaphoreGive(candado);
  delay(5);
}
