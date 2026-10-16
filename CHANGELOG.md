# Changelog

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/) y [Versionado Semántico](https://semver.org/lang/es/).

## [Sin publicar]
### Agregado
- Látigo en la pantalla de Rickuel: un iconito arriba a la derecha saca un látigo animado que sigue al mouse o al dedo; con un agitón chasquea (sonido), Rickuel se sobresalta y el látigo cae (`web/rickuel/latigo.js`, física adaptada de OpenWhip, licencia MIT).
- Al abrir la pantalla de Rickuel se precarga el modelo de la PC y se le pide que se quede 30 minutos en memoria (`local_mantener` en `rickuel.json`): sin esto, la primera respuesta tras un rato sin uso tardaba más de un minuto.
- Memoria de hechos de Rickuel: guarda y olvida datos (`memoria.json`), se los pasa al modelo en cada charla y se pueden ver y borrar desde su pantalla (`/api/memoria`).
- Rickuel dentro del panel: mosaico en el inicio, pantalla con su cara y chat, y un servidor `/cerebro` que le pasa el estado de la casa, prueba la PC con Ollama y después Gemini, y ejecuta las acciones de luces y escenas que pida. Configuración en `rickuel.json`; las charlas se guardan en `charlas.jsonl`.

## [0.1.0] - 2026-10-15
### Agregado
- Versión inicial del sistema de versionado. Estado actual del proyecto antes de empezar a registrar cambios.
