# panel-luces

Panel web táctil para controlar luces **Tuya / Smart Life** por la red local, sin la app oficial y sin pasar por la nube. Pensado para correr en un celular Android viejo (con Termux) y dejarlo como panel fijo en la pared.

## Qué hace
- **Una luz por pantalla**: se desliza el dedo para cambiar de luz.
- Botón de encendido grande que brilla con el color de la luz.
- Brillo, color (arcoíris) y tres blancos (cálido, neutro, frío).
- Se instala como app a pantalla completa y pide que la pantalla no se apague.
- Si una luz no responde, la muestra "Sin conexión" y sigue reintentando.
- Modo `--demo` con luces simuladas, para probarlo sin hardware.

## Cómo funciona
- `server.py`: servidor HTTP en Python (solo librería estándar) que habla con las luces por LAN con [TinyTuya](https://github.com/jasonacox/tinytuya) y expone una API chica:
  - `GET /api/luces`: estado de todas las luces.
  - `POST /api/luces/<id>`: cambia `encendida`, `brillo` (1-100), `color` (`#rrggbb`) o `blanco` (0 cálido - 100 frío).
- `web/`: la página del panel (HTML, CSS y JS sin dependencias ni paso de compilación).

## Probarlo sin luces
```bash
python server.py --demo
```
Abrir `http://localhost:8080`.

## Usarlo con luces reales
1. Tener las luces emparejadas en Smart Life y una cuenta gratis en [iot.tuya.com](https://iot.tuya.com) con un proyecto Cloud vinculado a esa cuenta de Smart Life.
2. Instalar TinyTuya y generar las claves locales (se necesita una sola vez):
   ```bash
   pip install tinytuya
   python -m tinytuya wizard
   ```
   Esto crea `devices.json` con el nombre, ID y clave local de cada luz.
3. Estando en el mismo WiFi que las luces, buscar su IP y versión de protocolo:
   ```bash
   python -m tinytuya scan
   ```
   y agregar `"ip"` y `"version"` a cada entrada de `devices.json` (ver `devices.example.json`).
4. Arrancar:
   ```bash
   python server.py
   ```

## En un celular Android (Termux)
1. Instalar Termux desde F-Droid o los releases de GitHub (no la versión de Play Store).
2. `pkg install python python-cryptography` y `pip install tinytuya`.
3. Copiar esta carpeta (con tu `devices.json`) al celular y correr `sh iniciar.sh`.
4. Abrir `http://localhost:8080` en Chrome e instalarlo como app (menú → Instalar app).
5. Para que arranque solo al prender el celular, usar [Termux:Boot](https://github.com/termux/termux-boot) con un script en `~/.termux/boot/` que ejecute `iniciar.sh`.

## Seguridad
`devices.json` contiene las **claves locales** de tus luces y `tinytuya.json` las credenciales de tu cuenta de desarrollador de Tuya. Están en el `.gitignore`: no los subas ni los compartas. El servidor no tiene autenticación, así que usalo solo dentro de tu red de confianza.

## Aclaración
Proyecto personal, hecho a medida para unas luces concretas. No está afiliado a Tuya ni a Smart Life.
