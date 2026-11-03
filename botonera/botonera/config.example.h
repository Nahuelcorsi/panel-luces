// Copiá este archivo como config.h y completalo. config.h no se sube a GitHub.
#pragma once

#define WIFI_RED   "nombre-de-tu-wifi"
#define WIFI_CLAVE "clave-de-tu-wifi"

// IP fija del celular que corre server.py (reservala en el router para que no cambie)
#define PANEL "http://192.168.0.50:8080"

// Algunas placas "CYD" traen otro chip de pantalla (las que tienen 2 conectores USB).
// Si la pantalla queda en blanco, probá con 1. Si los colores salen invertidos, cambiá INVERTIR_COLORES.
#define PANTALLA_ST7789  0
#define INVERTIR_COLORES 0

// Si al girar las perillas el brillo baja en vez de subir, poné 1.
#define PERILLA_AL_REVES 0
// La mayoría de las perillas KY-040 dan 4 pasos por "clic"; si salta de a dos, poné 2.
#define PASOS_POR_CLIC   4

// 1 si la botonera va con pila 18650: desenchufada se duerme a los 30 s y se despierta con las perillas o la pantalla.
// Enchufada (o con 0) queda siempre prendida. OJO: hoy no hay pin que vea el USB (el P6 es el botón para hablar)
// y el IO35 es del micrófono, así que nunca se duerme del todo, ni con 1.
// Volumen de los ruiditos (1 a 128, 20 si no está):
// #define VOLUMEN 20
#define BATERIA 1

// Qué maneja cada perilla (1 a 4): el nombre de una luz o de un grupo, como aparece en el panel.
// "" = esa perilla no hace nada. Si borrás esta línea, van en el orden de devices.json.
#define PERILLAS {"Living", "Cocina", "Habitacion", ""}
