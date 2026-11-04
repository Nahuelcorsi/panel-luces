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
#pragma once
#include <driver/dac_continuous.h>

#ifndef PIN_PARLANTE_SONIDOS
#define PIN_PARLANTE_SONIDOS 26   // el único que llega al amplificador de la placa (canal 1 del DAC)
#endif
// Volumen de los ruiditos: 1 (casi nada) a 128 (al máximo). Se puede poner en config.h.
#ifndef VOLUMEN
#define VOLUMEN 20
#endif

// S_EMOCION + n = la emoción n de EMOCIONES[] del .ino:
// neutral, feliz, triste, sorprendido, enojado, pensando, dormido, enamorado, guino, cafe
enum Sonido { S_BOTON, S_SOLTAR, S_ALARMA, S_HOLA, S_EMOCION };

namespace sonidos {

const int TASA = 22050;            // muestras por segundo (el DAC por DMA no baja de 19,6 kHz en el ESP32)
const int BLOQUE = 512;            // muestras que se arman de una vez antes de pasarlas al DAC
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
      armar(S_EMOCION, f);
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
  float amp = VOLUMEN * 2.0f;                       // con VOLUMEN 20, 40 de los 127 que hay para cada lado
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

void tarea(void*) {
  int cual;
  for (;;) {
    if (xQueueReceive(cola, &cual, portMAX_DELAY) != pdTRUE) continue;
    if (!prenderDac()) { Serial.println("No se pudo prender el parlante (DAC)"); continue; }
    lleno = 0;
    rampa(0, 128, 60);                                  // de 0 al medio, despacito
    do {                                                // si llegaron más mientras tanto, van seguidos
      frase.cuantas = 0;
      armar(cual, frase);
      for (int i = 0; i < frase.cuantas; i++) tocarNota(frase.n[i]);
      for (int i = 0; i < TASA / 25; i++) mandar(128);  // 40 ms de aire entre frase y frase
    } while (xQueueReceive(cola, &cual, 0) == pdTRUE);
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

// Una vez, en setup()
inline void prepararSonido() {
  sonidos::soltarPin();
  sonidos::cola = xQueueCreate(4, sizeof(int));
  // Núcleo 1 y prioridad 2: más que el loop() (así no se entrecorta) pero menos que la grabación del micrófono
  xTaskCreatePinnedToCore(sonidos::tarea, "sonido", 4096, nullptr, 2, nullptr, 1);
}
