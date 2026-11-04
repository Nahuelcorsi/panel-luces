# Botonera de mesa de luz

Una cajita con pantalla táctil de 2.8", **4 perillas, una por luz**, y una quinta perilla para el color, que muestra lo mismo que el panel del celular y controla las luces. Habla con `server.py` por WiFi, usando la misma API (`/api/luces`, `/api/clima`, `/api/ajustes`, `/cerebro`), así que no hay que tocar el servidor.

## Cómo se usa

- **Las perillas 1 a 4 manejan una luz cada una**: la que diga `PERILLAS` en `config.h`, por nombre (por ejemplo `{"Habitacion", "Lampara", "", ""}`). También puede ser un grupo del panel (como `"Living"`), y entonces maneja todas sus luces juntas. Una perilla con `""` no hace nada. Sin `PERILLAS`, van en el orden de `devices.json`.
  - **Girar**: sube o baja el brillo de esa luz (aparece un cartel con el nombre y el %).
  - **Clic**: la prende o la apaga.
  - **Mantener 1 segundo** (cualquiera): apaga todo, para cuando ya estás acostado.
- **La quinta perilla** (la de la derecha) cambia el color de la luz elegida, que es la última que tocaste.
  - **Girar**: recorre los colores; aparece un cartel con el arcoíris y dónde estás. Da la vuelta entera en 24 clics.
  - **Clic**: pasa a la luz siguiente.
  - **Mantener 1 segundo**: vuelve a luz blanca.
  - Solo hace algo en las lámparas RGB.
- **Pantalla**, cuatro pestañas abajo:
  - **Inicio**: hora grande, fecha, clima de casa (temperatura, máx/mín, lluvia en las próximas 3 h) y la alarma si está activa.
  - **Luces**: una tarjeta por luz. Tocar el círculo de la derecha la prende o apaga.
  - **Jugar**: Arkanoid. Cualquier perilla (o el dedo) mueve la paleta y un clic lanza la pelota. Al perder, la luz elegida se pone roja un momento; al pasar de nivel, verde. Guarda el récord.
  - **Amigo**: los mismos dos ojitos de la página (dibujados con formas, cambian con la emoción y parpadean) y 4 frases fijas que van al mismo cerebro del panel (`/cerebro`); contesta en pantalla y puede manejar las luces. El nombre lo toma del panel (`amigo.json`). Sin micrófono por ahora.
- A los 30 segundos sin uso la pantalla baja el brillo y vuelve al Inicio. De 23 a 7 queda casi apagada para no molestar. El primer toque solo la despierta; las perillas funcionan siempre, aunque esté apagada.

## Piezas (MercadoLibre)

| Pieza | Cant. | Qué buscar | Nota |
|---|---|---|---|
| Placa con pantalla | 1 | `ESP32-2432S028R` o "Cheap Yellow Display 2.8" | ESP32 + pantalla táctil 320×240 en una sola placa. Suele venir con un cable de 4 pines. |
| Expansor I2C | 2 | `PCF8574 módulo` (ej. Nubbeo CJMCU-8574) | La placa tiene pocos pines libres; cada expansor suma 8 pines; entre los dos alcanzan para las 5 perillas. Los dos se enganchan uno atrás del otro. |
| Perilla | 5 | `encoder rotativo KY-040` | Ya trae la perilla de plástico y los pines. |
| Cables | 1 bolsa | `cables dupont hembra hembra` | Si la placa no trajo el cable de 4 pines, buscá también `cable JST 1.25mm 4 pines`. |
| Mini protoboard | 1 | `mini protoboard 170 puntos` | Para repartir 3.3V y GND a las 5 perillas sin soldar. |
| Alimentación | 1 | Cargador de celular 5V 1A + cable micro USB **de datos** | El mismo cable sirve para cargarle el programa desde la PC. |
| Caja | 1 | `caja plástica para proyectos electrónicos` (unos 15×10×5 cm) | O impresa en 3D / de madera. Un agujero rectangular para la pantalla y cinco de 7 mm para las perillas. |

No hace falta soldar ni resistencias: los expansores ya traen las suyas.

## Cableado

La placa tiene un conector chico llamado **CN1** con 4 pines: `GND`, `IO22`, `IO27`, `3.3V`. Los nombres están impresos al lado; revisalos porque hay versiones que cambian el orden.

**1. Placa → primer expansor** (pines `GND VCC SDA SCL` de un costado del módulo):

| CN1 | Expansor 1 |
|---|---|
| `GND` | `GND` |
| `3.3V` | `VCC` (**no** 5V) |
| `IO27` | `SDA` |
| `IO22` | `SCL` |

**2. Expansor 1 → expansor 2**: el módulo tiene la misma fila de pines del otro lado; el segundo se enchufa ahí (o con 4 cables, `GND-GND`, `VCC-VCC`, `SDA-SDA`, `SCL-SCL`).

**3. Direcciones**: los dos expansores no pueden tener la misma dirección. Cada módulo tiene 3 puentes amarillos (`A0`, `A1`, `A2`). Dejá el primero como viene y en el segundo **cambiá de lugar solo el puente `A0`**. El programa los encuentra solo y, si no ve los dos, lo avisa en pantalla al prender.

**4. Perillas**: cada expansor tiene 8 pines de datos, `P0` a `P7`.

| Perilla | Expansor | `CLK` | `DT` | `SW` |
|---|---|---|---|---|
| 1 | 1 | `P0` | `P1` | `P2` |
| 2 | 1 | `P3` | `P4` | `P5` |
| 3 | 2 | `P0` | `P1` | `P2` |
| 4 | 2 | `P3` | `P4` | `P5` |
| 5 (color) | 2 y 1 | expansor 2 `P6` | expansor 2 `P7` | **expansor 1** `P7` |

La perilla de color reparte sus cables entre los dos expansores: `CLK` y `DT` van al segundo y `SW` al primero, porque el `P6` del primero queda para la batería.

Y en las 5 perillas: `+` a 3.3V y `GND` a GND. Para no hacer un enredo, llevá un cable `VCC` y uno `GND` del expansor a una fila de la mini protoboard y de ahí sacá uno para cada perilla. Con esto se usan todos los pines de los dos expansores; para sumar más botones haría falta un tercero.

## Opcional: a batería

Con `BATERIA 1` en `config.h`, cuando está desenchufada se duerme del todo a los 30 s sin uso y se despierta al girar o apretar una perilla o al tocar la pantalla. El primer gesto solo la despierta, y tarda 1-2 s en reconectarse. Enchufada, queda siempre prendida. Con una 18650 de 3000 mAh debería durar alrededor de una semana por carga (es una estimación; hay que medirla).

**Piezas extra:** pila 18650 de marca (Samsung, LG o Sony, unos 3000 mAh; nada de "9800 mAh"), portapila 18650, módulo cargador **TP4056 USB-C con protección**, elevador **MT3608**, interruptor deslizante, un **multímetro** y un **soldador** (40-60 W) con estaño: el TP4056 y el MT3608 tienen contactos para soldar.

**⚠️ Antes de conectar la placa:** con el multímetro, girá el tornillito del MT3608 hasta que la salida marque **5.0 V**. De fábrica puede salir más alto y quemar la CYD.

| Desde | Hacia |
|---|---|
| Portapila `+` / `−` | TP4056 `B+` / `B−` |
| TP4056 `OUT+` | interruptor → MT3608 `VIN+` |
| TP4056 `OUT−` | MT3608 `VIN−` |
| MT3608 `VOUT+` / `VOUT−` | Conector de alimentación de la CYD (`P1`): `VIN` / `GND` |
| Pin `INT` de los dos expansores (unidos) | P3 `IO35`, y una resistencia de 10k entre `IO35` y 3.3V |
| TP4056 `IN+` (5V del USB) | resistencia 10k → expansor 1 `P6` |
| Expansor 1 `P6` | dos resistencias de 10k en serie (20k) → GND |

La carga se hace enchufando un cable USB-C al TP4056: la luz roja indica que está cargando y la azul (o verde), que terminó. Si tu módulo PCF8574 no tiene pin `INT`, se despierta solo con la pantalla.

## Antes de cargar el programa

1. **IP del celular.** La botonera arranca buscando el panel en la IP de `config.h`. Si el celu cambia de IP, la botonera lo busca sola en la red de la casa y se acuerda de la nueva, así que no hace falta recompilar. Igual conviene reservar la IP del celular en el router (suele estar en "DHCP" → "Reserva de direcciones"). La IP la ves en Termux con `ifconfig` o en Ajustes → WiFi.
2. Probá desde la PC que `http://IP-DEL-CELULAR:8080` abra el panel. Si abre, la botonera también va a poder.

## Cargar el programa (Arduino IDE)

1. Instalar [Arduino IDE 2](https://www.arduino.cc/en/software).
2. Archivo → Preferencias → "URLs adicionales de gestor de placas": `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Herramientas → Placa → Gestor de placas: instalar **esp32** (de Espressif).
4. Herramientas → Gestionar bibliotecas: instalar **LovyanGFX** y **ArduinoJson** (versión 7).
5. Abrir `botonera/botonera.ino`. Copiar `config.example.h` como `config.h` (en la misma carpeta) y completar WiFi e IP del panel.
6. Placa: **ESP32 Dev Module**. Herramientas → Partition Scheme: **Minimal SPIFFS (1.9MB APP with OTA)** (hace falta para poder cargarla por WiFi). Puerto: el COM que aparece al enchufarla. Si no aparece ningún COM, falta el driver **CH340** (buscalo como "CH340 driver Windows").
7. Subir. La primera vez pide **calibrar el táctil**: tocá la punta de cada flecha con la uña o un palito. Queda guardado. Para repetirlo, tocá la pantalla cuando al prender diga "Toca la pantalla para calibrar" (o prendela con la perilla 1 apretada).

## Cargar el programa por WiFi (sin cable)

Después de cargarla una vez por USB con el esquema **Minimal SPIFFS (1.9MB APP with OTA)**, las próximas veces se puede cargar por WiFi:

1. La botonera prendida y conectada al WiFi, y la PC en la misma red.
2. En el Arduino IDE: Herramientas → Puerto. Abajo, en "Puertos de red", aparece **botonera at 192.168.0.x**. Elegila.
3. Subir, como siempre. La botonera muestra "Cargando programa nuevo..." con una barrita, y al terminar se reinicia sola.

Si no aparece en "Puertos de red": esperá un minuto con el IDE abierto, revisá que la PC esté en el mismo WiFi y que el firewall de Windows deje pasar al Arduino IDE. Con `#define OTA_CLAVE "algo"` en `config.h` el IDE te pide esa clave antes de cargar.

## Si algo no anda

- **Pantalla en blanco**: en `config.h` poné `PANTALLA_ST7789 1` (hay placas con otro chip de pantalla).
- **Colores raros** (negro se ve blanco): cambiá `INVERTIR_COLORES`.
- **Las perillas van al revés**: `PERILLA_AL_REVES 1`. **Saltan de a dos pasos**: `PASOS_POR_CLIC 2`.
- **"Encontre 1 de 2 expansores"**: revisá los 4 cables entre módulos y que el segundo tenga el puente `A0` cambiado.
- **Una perilla maneja la luz equivocada o no hace nada**: revisá que el nombre en `PERILLAS` (en `config.h`) sea igual al del panel.
- **"Uy! No encuentro al celu del panel"**: el celular está apagado, sin WiFi o el server no está corriendo. La botonera lo sigue buscando sola, también si cambió de IP.
- **El brillo no acompaña la luz del cuarto**: en el monitor serie (115200) aparece "Sensor de luz N" cada 10 s. Anotá el número con el cuarto iluminado y a oscuras y ponelos en `LUZ_CLARO` y `LUZ_OSCURO` en `botonera.ino`.
- **"Sin datos del clima"**: el servidor todavía no tiene `lugares.json` configurado.

## Ideas para después

- Un parlantito en el conector `SPEAK` para que la alarma también suene.
