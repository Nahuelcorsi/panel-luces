# Changelog

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/) y [Versionado Semántico](https://semver.org/lang/es/).

## [Sin publicar]
### Cambiado
- **Rickuel se reemplaza por un amigo nuevo, de cero.** Su cara ahora son dos ojitos celestes en una pantallita (parpadean, siguen el dedo, cambian con la emoción y rebotan al hablar), con personalidad nueva (simpático, curioso y juguetón) y nombre configurable en `amigo.json` («Compañero» por defecto). La página pasa a `web/amigo/`, la API a `/api/amigo`, y la configuración, la memoria, las charlas y las calificaciones a archivos nuevos (`amigo.json`, `amigo-memoria.json`, `amigo-charlas.jsonl`, `amigo-calificaciones.json`, `amigo-ajustes.json`): arranca sin recuerdos.
- Voz de dibujito: en el navegador sale aguda y rápida; en la PC, `pc/voz_pc.py` usa la voz de Piper que haya en `pc/voces/` y la sube de tono con `--tono`. `pc/instalar-voz-pc.py` baja una voz provisoria (`es_AR-daniela-high`) hasta que haya una entrenada.
- Con la PC apagada, `/hablar` se da cuenta en 2 segundos (antes esperaba 30) y la pantalla pasa enseguida a la voz del navegador.
- Botonera: la pestaña «Amigo» dibuja los mismos ojitos (sin imágenes) y usa la personalidad y el nombre nuevos.

### Agregado
- El amigo habla por la pantalla que estás usando: el panel, la PC o el celular avisan al servidor cuando los tocás y `/api/amigo/aviso` manda lo que diga por su cuenta a la última que usaste (o al panel). En «Por dónde habla» se fija la salida y se prueba.
- El amigo en la PC: `pc/amigo_pc.py` (o `pc/amigo-pc.bat`, con la IP del celular del panel) abre su pantalla en el navegador de la PC, con el micrófono y los parlantes de la PC, haciendo de puente en `localhost` hacia el panel.
- Calificar las respuestas con 👍/👎 (y una corrección opcional en las 👎): cada charla tiene un `id` y `/api/entrenamiento` arma el archivo para entrenar con las buenas y las corregidas.

### Quitado
- La cámara: el vigía que miraba la cámara Imou y comentaba lo que hacías (`pc/vigia.py` y sus arrancadores) y el interruptor «observa».

## [0.2.0] - 2026-10-15
### Agregado
- Látigo en la pantalla de Rickuel: un iconito arriba a la derecha saca un látigo animado que sigue al mouse o al dedo; con un agitón chasquea (sonido), Rickuel se sobresalta y el látigo cae (`web/rickuel/latigo.js`, física adaptada de OpenWhip, licencia MIT).
- Al abrir la pantalla de Rickuel se precarga el modelo de la PC y se le pide que se quede 30 minutos en memoria (`local_mantener` en `rickuel.json`): sin esto, la primera respuesta tras un rato sin uso tardaba más de un minuto.
- Memoria de hechos de Rickuel: guarda y olvida datos (`memoria.json`), se los pasa al modelo en cada charla y se pueden ver y borrar desde su pantalla (`/api/memoria`).
- Rickuel dentro del panel: mosaico en el inicio, pantalla con su cara y chat, y un servidor `/cerebro` que le pasa el estado de la casa, prueba la PC con Ollama y después Gemini, y ejecuta las acciones de luces y escenas que pida. Configuración en `rickuel.json`; las charlas se guardan en `charlas.jsonl`.

## [0.1.0] - 2026-10-15
### Agregado
- Versión inicial del sistema de versionado. Estado actual del proyecto antes de empezar a registrar cambios.
