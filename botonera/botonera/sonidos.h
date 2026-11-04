// Ruiditos del Capsulito en la botonera: el mismo "idioma robot" que la página del amigo (web/amigo/bips.js),
// fabricado en el momento con el DAC del GPIO26 (conector SPEAK). Nunca palabras: trinos, silbidos que suben y
// bajan, temblorcitos, zumbidos y soplidos, cada vez un poco distintos.
//
// Cómo funciona: una tarea aparte arma cada frase como una lista de notas (cada nota se desliza de un tono a
// otro, con su forma de onda, su volumen, su temblor y su ataque), la "dibuja" muestra por muestra a 22050 por
// segundo y se la pasa al DAC por DMA. El resto del programa solo llama a sonar(): nada espera a que termine.
// Entre sonidos el pin queda en 0 fijo, como antes (así el amplificador no chilla), y al empezar y terminar
// el nivel sube y baja despacito, para que no haga "tuc".
//
// Uso: prepararSonido() una vez en setup(); después sonar(S_HOLA), sonar(S_BOTON), sonar(S_EMOCION + n)...
// y charlar(texto, n) para que "diga" una frase en idioma robot (un bip por pedacito); bipAhora() dice cuál de
// esos bips se está oyendo en este momento (para mover los ojitos al ritmo).
#pragma once
#include <driver/dac_continuous.h>

#ifndef PIN_PARLANTE_SONIDOS
#define PIN_PARLANTE_SONIDOS 26   // el único que llega al amplificador de la placa (canal 1 del DAC)
#endif
// Volumen de los ruiditos: 1 (casi nada) a 128 (al máximo). Se puede poner en config.h.
#ifndef VOLUMEN
#define VOLUMEN 28
#endif

// S_EMOCION + n = la emoción n de EMOCIONES[] del .ino:
// neutral, feliz, triste, sorprendido, enojado, pensando, dormido, enamorado, guino, cafe
// S_CHARLA + n = la última frase de charlar(), con el timbre de la emoción n
// S_TIC_SUBE + % y S_TIC_BAJA + % (0-100): tiquito al girar una perilla de brillo, más agudo cuanto más brillo
// S_TIC_COLOR + tono/4 (0-89): tiquito suave de la perilla de color
enum Sonido { S_BOTON, S_SOLTAR, S_ALARMA, S_HOLA, S_PRENDER, S_APAGAR, S_EMOCION, S_CHARLA = S_EMOCION + 10,
              S_TIC_SUBE = S_CHARLA + 10, S_TIC_BAJA = S_TIC_SUBE + 101, S_TIC_COLOR = S_TIC_BAJA + 101,
              S_FIN = S_TIC_COLOR + 90 };

namespace sonidos {

const int TASA = 22050;            // muestras por segundo (el DAC por DMA no baja de 19,6 kHz en el ESP32)
const int BLOQUE = 256;            // muestras que se arman de una vez antes de pasarlas al DAC (con 4, unos 46 ms de demora)
const int MAX_NOTAS = 24;
const float PI2 = 6.2831853f;

enum Forma : uint8_t { SENO, TRIANGULO, CUADRADA, SIERRA, SOPLIDO };

struct Nota {
  float f0, f1;        // tono al empezar y al terminar (Hz); en SOPLIDO es el centro del filtro
  uint16_t ms;         // cuánto dura
  Forma forma;
  float vol;           // 0-1
  float tiembla;       // temblor (vibrato), en Hz de más y de menos
  uint16_t ataque;     // ms en llegar al volumen
  uint16_t despues;    // silencio después, en ms
};

struct Frase {
  Nota n[MAX_NOTAS];
  int cuantas = 0;
  void agregar(float f0, float f1, int ms, Forma forma = SENO, float vol = 0.8f, float tiembla = 0, int ataque = 8,
               int despues = 0) {
    if (cuantas < MAX_NOTAS) n[cuantas++] = {f0, f1, (uint16_t)ms, forma, vol, tiembla, (uint16_t)ataque, (uint16_t)despues};
  }
};

inline float azar(float a, float b) { return a + (b - a) * (esp_random() / 4294967295.0f); }
inline int azarEntero(int a, int b) { return a + (int)(esp_random() % (uint32_t)(b - a + 1)); }

// ---- charla en idioma robot (como bips.charla de la página) ----
// Un bip por cada 2 o 3 vocales, con el timbre de la emoción; una pregunta termina subiendo y una
// exclamación, más fuerte. El texto lo deja charlar() acá y lo lee la tarea.
char textoCharla[256];
portMUX_TYPE muxCharla = portMUX_INITIALIZER_UNLOCKED;
const int MAX_BIPS = 16;
const int MS_POR_LETRA_CHARLA = 80;   // igual que MS_POR_LETRA del .ino
// Cuándo se oye cada bip (en millis), para los ojitos: lo llena la tarea al armar la charla
volatile unsigned long bipDesde[MAX_BIPS], bipHasta[MAX_BIPS];
volatile int nBips = 0;
volatile int charlasHechas = 0;   // sube cada vez que se arma una charla (para saber que ya empezó)

void armarCharla(int emocion, Frase& f) {
  // {tono base, forma}: neutral, feliz, triste, sorprendido, enojado, pensando, dormido, enamorado, guiño, café
  static const float BASE[] = {850, 1000, 450, 1200, 220, 700, 350, 1100, 1000, 800};
  static const Forma FORMA[] = {SENO, TRIANGULO, SENO, TRIANGULO, SIERRA, SENO, SENO, SENO, CUADRADA, SENO};
  char t[sizeof textoCharla];
  portENTER_CRITICAL(&muxCharla);
  memcpy(t, textoCharla, sizeof t);
  portEXIT_CRITICAL(&muxCharla);
  int e = emocion >= 0 && emocion < 10 ? emocion : 0, vocales = 0, largo = strlen(t);
  bool enVocal = false;
  for (int i = 0; i < largo; i++) {
    bool v = strchr("aeiouAEIOU", t[i]) != nullptr;
    if (v && !enVocal) vocales++;
    enVocal = v;
  }
  int fin = largo;
  while (fin > 0 && (t[fin - 1] == ' ' || t[fin - 1] == '.')) fin--;
  bool pregunta = fin > 0 && t[fin - 1] == '?', grita = fin > 0 && t[fin - 1] == '!';
  int n = constrain((int)roundf(vocales / 2.5f), 2, MAX_BIPS);
  float vol = FORMA[e] == SIERRA || FORMA[e] == CUADRADA ? 0.55f : 0.75f;
  // Los bips se reparten a lo largo del tiempo que tarda en escribirse el texto en la pantalla (una letra cada
  // MS_POR_LETRA_CHARLA), así van juntos: se estira la pausa después de cada uno (hasta 600 ms)
  int total = largo * MS_POR_LETRA_CHARLA, propio = 0;
  for (int i = 0; i < n; i++) {
    bool ultimo = i == n - 1;
    float f0 = BASE[e] * azar(0.75f, 1.35f), f1 = ultimo && pregunta ? f0 * 1.8f : f0 * azar(0.85f, 1.25f);
    int dur = azarEntero(50, 110) * (ultimo && pregunta ? 2 : 1);
    int pausa = azar(0, 1) < 0.2f ? azarEntero(80, 140) : azarEntero(15, 40);   // de vez en cuando, como una coma
    f.agregar(f0, f1, dur, FORMA[e], ultimo && grita ? fminf(1, vol * 1.4f) : vol, 0, 6, pausa);
    propio += dur + pausa;
  }
  int extra = n > 1 && total > propio ? (total - propio) / (n - 1) : 0;
  if (extra > 600) extra = 600;
  for (int i = f.cuantas - n; i < f.cuantas - 1; i++) f.n[i].despues += extra;
}

// El "idioma" de un número de sonido: S_CHARLA + n es una charla, todo lo demás una frase fija
void armar(int cual, Frase& f);
bool esCharla(int cual) { return cual >= S_CHARLA && cual < S_CHARLA + 10; }

// ---- las frases de cada sonido (las mismas ideas que bips.js, con un poco más de vida) ----
void armar(int cual, Frase& f) {
  switch (cual) {
    case S_HOLA: {   // "uí-du-dí-dííí": se despierta contento
      float b = azar(700, 850);
      f.agregar(b, b * 1.6f, 70, TRIANGULO, 0.8f, 0, 6, 25);
      f.agregar(b * 1.3f, b * 1.1f, 60, SENO, 0.7f, 0, 6, 20);
      f.agregar(b * 1.5f, b * 2.0f, 80, TRIANGULO, 0.8f, 0, 6, 30);
      f.agregar(b * 2.0f, b * 2.6f, 220, SENO, 0.8f, 60, 15);
      break;
    }
    case S_BOTON:    // blip corto que sube: "te escucho"
      f.agregar(azar(1200, 1400), 2200, 45, TRIANGULO, 0.7f, 0, 4, 15);
      f.agregar(2300, 2500, 35, SENO, 0.6f, 0, 3);
      break;
    case S_SOLTAR:   // "bup" que baja: "listo"
      f.agregar(azar(1700, 1900), 850, 80, SENO, 0.7f, 0, 4);
      break;
    case S_PRENDER:  // "bu-dí!": dos notas que suben, la luz se prendió
      f.agregar(660, 880, 60, TRIANGULO, 0.7f, 0, 4, 20);
      f.agregar(1320, 1760, 90, SENO, 0.75f, 20, 4);
      break;
    case S_APAGAR:   // "dí-bu": dos notas que bajan, se apagó
      f.agregar(1320, 1100, 60, SENO, 0.7f, 0, 4, 20);
      f.agregar(700, 440, 110, TRIANGULO, 0.6f, 0, 4);
      break;
    case S_ALARMA:   // sirenita amable: sube y baja, cuatro veces, cada vez más arriba
      for (int i = 0; i < 4; i++) {
        float b = 900 + i * 120;
        f.agregar(b, b * 1.8f, 160, TRIANGULO, 0.9f, 0, 10);
        f.agregar(b * 1.8f, b * 1.2f, 140, TRIANGULO, 0.9f, 30, 5, 260);
      }
      break;
    case S_EMOCION + 0: {   // neutral: "bi-bup", como quien asiente
      float b = azar(850, 1050);
      f.agregar(b, b * azar(1.15f, 1.35f), 70, SENO, 0.75f, 0, 6, 40);
      f.agregar(b * 0.8f, b * 0.65f, 90, SENO, 0.7f, 0, 6);
      break;
    }
    case S_EMOCION + 1: {   // feliz: trino que sube a saltitos y un remate con temblorcito
      float b = azar(800, 1000);
      int n = azarEntero(4, 6);
      for (int i = 0; i < n; i++) {
        f.agregar(b, b * azar(1.25f, 1.5f), azarEntero(45, 70), TRIANGULO, 0.8f, 0, 4, azarEntero(10, 25));
        b *= azar(1.08f, 1.2f);
      }
      f.agregar(b, b * 1.3f, 180, SENO, 0.8f, 70, 10);
      break;
    }
    case S_EMOCION + 2: {   // triste: "buuuu" que se cae despacio, temblando, y un suspirito
      float b = azar(620, 720);
      f.agregar(b, b * 0.5f, 600, SENO, 0.75f, 14, 60, 60);
      f.agregar(330, 240, 350, SENO, 0.45f, 8, 50, 40);
      f.agregar(1200, 700, 260, SOPLIDO, 0.35f, 0, 80);
      break;
    }
    case S_EMOCION + 3: {   // sorprendido: subida de golpe y un "¿¡bip!?" arriba
      f.agregar(azar(300, 420), azar(2100, 2500), 170, TRIANGULO, 0.85f, 0, 5, 50);
      f.agregar(2400, 2700, 50, SENO, 0.7f, 0, 3, 40);
      f.agregar(2600, 3000, 45, SENO, 0.6f, 0, 3);
      break;
    }
    case S_EMOCION + 4: {   // enojado: zumbido grave y cortado, que se tuerce para abajo, y un bufido
      for (int i = 0; i < 4; i++) {
        float b = azar(140, 210);
        f.agregar(b, b * 0.8f, azarEntero(65, 95), SIERRA, 0.75f, 12, 5, azarEntero(25, 45));
      }
      f.agregar(900, 300, 220, SOPLIDO, 0.5f, 0, 15);
      break;
    }
    case S_EMOCION + 5: {   // pensando: burbujeo "bip-bup-bip" con ritmo irregular
      int n = azarEntero(5, 7);
      for (int i = 0; i < n; i++) {
        float b = azar(450, 1300);
        f.agregar(b, b * azar(1.3f, 1.9f), azarEntero(35, 55), SENO, 0.6f, 0, 4, azarEntero(30, 110));
      }
      break;
    }
    case S_EMOCION + 6:     // dormido: ronquido de robot, soplido y silbidito que se apaga
      f.agregar(85, 115, 600, SIERRA, 0.45f, 3, 250, 40);
      f.agregar(400, 700, 380, SOPLIDO, 0.4f, 0, 120, 60);
      f.agregar(1300, 850, 380, SENO, 0.4f, 10, 60);
      break;
    case S_EMOCION + 7: {   // enamorado: "uiiii-uuuu" con temblor, dos veces
      float b = azar(1000, 1150);
      f.agregar(b, b * 1.55f, 230, SENO, 0.8f, 30, 25, 20);
      f.agregar(b * 1.5f, b * 0.8f, 380, SENO, 0.7f, 30, 25, 90);
      f.agregar(b * 1.1f, b * 1.6f, 180, SENO, 0.6f, 35, 20);
      break;
    }
    case S_EMOCION + 8:     // guiño: dos blips pícaros y un deslizón
      f.agregar(1500, 1500, 50, CUADRADA, 0.55f, 0, 3, 60);
      f.agregar(2100, 2400, 60, CUADRADA, 0.55f, 0, 3, 30);
      f.agregar(1800, 1300, 90, TRIANGULO, 0.6f, 0, 5);
      break;
    case S_EMOCION + 9:     // café: sorbito (soplido que sube) y un "aaahh" satisfecho
      f.agregar(700, 2400, 360, SOPLIDO, 0.55f, 0, 90, 70);
      f.agregar(620, 470, 480, SENO, 0.7f, 9, 60);
      break;
    default:
      if (esCharla(cual)) armarCharla(cual - S_CHARLA, f);
      else if (cual >= S_TIC_SUBE && cual < S_TIC_BAJA) {      // brillo para arriba: "tic" que sube
        float b = 600 + (cual - S_TIC_SUBE) * 12;
        f.agregar(b, b * 1.3f, 28, TRIANGULO, 0.5f, 0, 2);
      } else if (cual >= S_TIC_BAJA && cual < S_TIC_COLOR) {   // brillo para abajo: "tuc" que baja
        float b = 600 + (cual - S_TIC_BAJA) * 12;
        f.agregar(b * 1.2f, b * 0.85f, 28, TRIANGULO, 0.5f, 0, 2);
      } else if (cual >= S_TIC_COLOR && cual < S_FIN) {        // color: gotita suave, el tono sigue al color
        float b = 500 + (cual - S_TIC_COLOR) * 12;
        f.agregar(b, b * 1.1f, 22, SENO, 0.35f, 0, 2);
      } else armar(S_EMOCION, f);
  }
}

// ---- el que dibuja las muestras ----
QueueHandle_t cola = nullptr;
dac_continuous_handle_t dac = nullptr;
uint8_t bloque[BLOQUE];
int lleno = 0;

void mandar(uint8_t v) {
  bloque[lleno++] = v;
  if (lleno == BLOQUE) {
    dac_continuous_write(dac, bloque, BLOQUE, nullptr, -1);   // espera (solo esta tarea) a que el DMA lo tome
    lleno = 0;
  }
}

// Del nivel "de" al "a" (0-255) en ms milisegundos, despacito: así el parlante no hace "tuc"
void rampa(int de, int a, int ms) {
  int n = TASA * ms / 1000;
  for (int i = 0; i < n; i++) mandar(de + (a - de) * i / n);
}

void tocarNota(const Nota& nt) {
  int n = TASA * nt.ms / 1000;
  if (n <= 0) return;
  float amp = VOLUMEN * 2.0f;                       // con VOLUMEN 28, 56 de los 127 que hay para cada lado
  if (amp > 127) amp = 127;
  // Las ondas con puntas (cuadrada, sierra) suenan más fuerte que el seno con el mismo volumen
  if (nt.forma == CUADRADA || nt.forma == SIERRA) amp *= 0.6f;
  amp *= nt.vol;
  float f = nt.f0, paso = powf(nt.f1 / nt.f0, 1.0f / n);   // se desliza "en escalones iguales al oído"
  float fase = 0, faseTemblor = 0, temblorHz = azar(9, 14);
  int ataque = TASA * nt.ataque / 1000 + 1, suelta = n / 3 + 1;
  if (suelta > TASA / 25) suelta = TASA / 25;       // se apaga en 40 ms como mucho
  float bajo = 0, banda = 0;                        // filtro del soplido
  for (int i = 0; i < n; i++) {
    float fi = f + nt.tiembla * sinf(faseTemblor);
    faseTemblor += PI2 * temblorHz / TASA;
    float s;
    switch (nt.forma) {
      case SENO: s = sinf(PI2 * fase); break;
      case TRIANGULO: s = fase < 0.5f ? 4 * fase - 1 : 3 - 4 * fase; break;
      case CUADRADA: s = fase < 0.5f ? 1 : -1; break;
      case SIERRA: s = 2 * fase - 1; break;
      default: {   // SOPLIDO: ruido que pasa por un filtro centrado en fi (filtro de estado variable)
        float ruido = azar(-1, 1), k = 2 * sinf(3.14159f * fi / TASA);
        bajo += k * banda;
        float alto = ruido - bajo - 0.4f * banda;
        banda += k * alto;
        s = banda * 1.6f;
        if (s > 1) s = 1;
        if (s < -1) s = -1;
      }
    }
    fase += fi / TASA;
    if (fase >= 1) fase -= 1;
    f *= paso;
    float env = 1;
    if (i < ataque) env = (float)i / ataque;
    if (n - i < suelta) env *= (float)(n - i) / suelta;
    mandar(128 + (int)(s * amp * env));
  }
  for (int i = 0, m = TASA * nt.despues / 1000; i < m; i++) mandar(128);
}

void soltarPin() {   // el pin en 0 fijo, sin el DAC
  pinMode(PIN_PARLANTE_SONIDOS, OUTPUT);
  digitalWrite(PIN_PARLANTE_SONIDOS, LOW);
}

bool prenderDac() {
  dac_continuous_config_t cfg = {};
  cfg.chan_mask = DAC_CHANNEL_MASK_CH1;
  cfg.desc_num = 4;
  cfg.buf_size = BLOQUE;
  cfg.freq_hz = TASA;
  cfg.offset = 0;
  cfg.clk_src = DAC_DIGI_CLK_SRC_DEFAULT;
  cfg.chan_mode = DAC_CHANNEL_MODE_SIMUL;
  if (dac_continuous_new_channels(&cfg, &dac) != ESP_OK) { dac = nullptr; return false; }
  if (dac_continuous_enable(dac) != ESP_OK) { dac_continuous_del_channels(dac); dac = nullptr; return false; }
  return true;
}

void apagarDac() {
  dac_continuous_disable(dac);
  dac_continuous_del_channels(dac);
  dac = nullptr;
  soltarPin();
}

Frase frase;   // una sola a la vez (solo la usa la tarea)

// ¿Viene otro sonido? Espera hasta 400 ms con el parlante prendido en silencio: así, si girás una perilla,
// los tiquitos salen seguidos sin que el parlante se apague y se prenda entre uno y otro
bool otroSonido(int& cual, unsigned long& escritas) {
  const int PEDAZO = TASA / 100;   // de a 10 ms
  for (int t = 0; t < 400; t += 10) {
    if (xQueueReceive(cola, &cual, 0) == pdTRUE) return true;
    for (int i = 0; i < PEDAZO; i++) mandar(128);
    escritas += PEDAZO;
  }
  return xQueueReceive(cola, &cual, 0) == pdTRUE;
}

void tarea(void*) {
  int cual;
  for (;;) {
    if (xQueueReceive(cola, &cual, portMAX_DELAY) != pdTRUE) continue;
    if (!prenderDac()) { Serial.println("No se pudo prender el parlante (DAC)"); continue; }
    lleno = 0;
    rampa(0, 128, 60);                                  // de 0 al medio, despacito
    unsigned long escritas = TASA * 60 / 1000;          // muestras ya mandadas (la rampa), para saber cuándo se oye cada cosa
    unsigned long inicio = millis();
    do {                                                // si llegaron más mientras tanto, van seguidos
      frase.cuantas = 0;
      armar(cual, frase);
      if (esCharla(cual)) {                             // anota cuándo se va a oír cada bip (para los ojitos)
        unsigned long t = inicio + escritas * 1000 / TASA;
        int n = frase.cuantas < MAX_BIPS ? frase.cuantas : MAX_BIPS;
        for (int i = 0; i < n; i++) {
          bipDesde[i] = t;
          bipHasta[i] = t + frase.n[i].ms;
          t += frase.n[i].ms + frase.n[i].despues;
        }
        nBips = n;
        charlasHechas++;
      }
      for (int i = 0; i < frase.cuantas; i++) {
        tocarNota(frase.n[i]);
        escritas += TASA * (frase.n[i].ms + frase.n[i].despues) / 1000;
      }
      bool tic = cual >= S_TIC_SUBE && cual < S_FIN;
      int aire = tic ? TASA / 200 : TASA / 25;          // 5 ms detrás de un tiquito, 40 ms entre frase y frase
      escritas += aire;
      for (int i = 0; i < aire; i++) mandar(128);
    } while (otroSonido(cual, escritas));
    rampa(128, 0, 60);                                  // y de vuelta a 0
    for (int i = 0; i < 4 * BLOQUE; i++) mandar(0);     // que el DMA termine de sacar todo antes de apagarlo
    apagarDac();
  }
}

}  // namespace sonidos

// Desde cualquier lado: anota el sonido y sigue (si ya hay varios esperando, este no suena)
inline void sonar(int cual) {
  if (sonidos::cola) xQueueSend(sonidos::cola, &cual, 0);
}

// Que "diga" el texto en idioma robot, con el timbre de la emoción n (0-9). El texto no se pronuncia: solo
// cuenta su largo y si termina en pregunta o exclamación.
inline void charlar(const String& texto, int emocion) {
  portENTER_CRITICAL(&sonidos::muxCharla);
  strncpy(sonidos::textoCharla, texto.c_str(), sizeof sonidos::textoCharla - 1);
  sonidos::textoCharla[sizeof sonidos::textoCharla - 1] = 0;
  portEXIT_CRITICAL(&sonidos::muxCharla);
  sonar(S_CHARLA + emocion);
}

// Qué bip de la última charla se está oyendo ahora (0, 1, 2...), o -1 si ninguno
inline int bipAhora() {
  unsigned long t = millis();
  for (int i = 0; i < sonidos::nBips; i++)
    if (t >= sonidos::bipDesde[i] && t < sonidos::bipHasta[i]) return i;
  return -1;
}

// Cuánto de la última charla ya se oyó, de 0 a 1 (para escribir el texto al mismo ritmo que los bips)
inline float avanceCharla() {
  int n = sonidos::nBips;
  if (n <= 0) return 1;
  unsigned long t = millis();
  if (t < sonidos::bipDesde[0]) return 0;
  if (t >= sonidos::bipHasta[n - 1]) return 1;
  for (int i = n - 1; i >= 0; i--) {
    if (t < sonidos::bipDesde[i]) continue;
    float dur = sonidos::bipHasta[i] - sonidos::bipDesde[i];
    float parte = dur > 0 ? (t - sonidos::bipDesde[i]) / dur : 1;
    return (i + (parte > 1 ? 1 : parte)) / n;   // en la pausa después de un bip, queda quieto
  }
  return 0;
}

// Una vez, en setup()
inline void prepararSonido() {
  sonidos::soltarPin();
  sonidos::cola = xQueueCreate(4, sizeof(int));
  // Núcleo 1 y prioridad 2: más que el loop() (así no se entrecorta) pero menos que la grabación del micrófono
  xTaskCreatePinnedToCore(sonidos::tarea, "sonido", 4096, nullptr, 2, nullptr, 1);
}
