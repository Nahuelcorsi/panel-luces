#!/data/data/com.termux/files/usr/bin/sh
# Arranca el panel de luces en Termux y evita que Android lo duerma.
cd "$(dirname "$0")" || exit 1
termux-wake-lock 2>/dev/null
exec python server.py
