# Changelog

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/) y [Versionado Semántico](https://semver.org/lang/es/).

## [Sin publicar]
### Agregado
- Rickuel habla por la pantalla que estás usando: el panel, la PC o el celular avisan al servidor cuando los tocás y `/api/rickuel/aviso` manda lo que Rickuel diga por su cuenta a la última que usaste (o al panel); si llega algo para el panel, el inicio abre solo la pantalla de Rickuel. En «Cámara y voz» se fija por dónde habla, se prueba y está el interruptor «Rickuel observa» (para el vigía de la cámara, que viene después). Ajustes en `ajustes-rickuel.json`.
- Rickuel en la PC: `pc/rickuel_pc.py` (o `pc/rickuel-pc.bat`) abre su pantalla en el navegador de la PC, con el micrófono y los parlantes de la PC, haciendo de puente en `localhost` hacia el panel.
- La voz entrenada de Rick (Piper) en Rickuel: `pc/voz_pc.py` corre en la PC y el panel le pasa el texto por `/hablar` (`voz_url` en `rickuel.json`); la cara mueve la boca con el audio y, si la PC no responde, habla con la voz del navegador.
- Calificar las respuestas de Rickuel con 👍/👎 (y una corrección opcional en las 👎): cada charla tiene un `id`, las notas se guardan en `calificaciones.json` y `/api/entrenamiento` arma el archivo para entrenar con las buenas y las corregidas.

## [0.2.0] - 2026-10-15
### Agregado
- Látigo en la pantalla de Rickuel: un iconito arriba a la derecha saca un látigo animado que sigue al mouse o al dedo; con un agitón chasquea (sonido), Rickuel se sobresalta y el látigo cae (`web/rickuel/latigo.js`, física adaptada de OpenWhip, licencia MIT).
- Al abrir la pantalla de Rickuel se precarga el modelo de la PC y se le pide que se quede 30 minutos en memoria (`local_mantener` en `rickuel.json`): sin esto, la primera respuesta tras un rato sin uso tardaba más de un minuto.
- Memoria de hechos de Rickuel: guarda y olvida datos (`memoria.json`), se los pasa al modelo en cada charla y se pueden ver y borrar desde su pantalla (`/api/memoria`).
- Rickuel dentro del panel: mosaico en el inicio, pantalla con su cara y chat, y un servidor `/cerebro` que le pasa el estado de la casa, prueba la PC con Ollama y después Gemini, y ejecuta las acciones de luces y escenas que pida. Configuración en `rickuel.json`; las charlas se guardan en `charlas.jsonl`.

## [0.1.0] - 2026-10-15
### Agregado
- Versión inicial del sistema de versionado. Estado actual del proyecto antes de empezar a registrar cambios.
