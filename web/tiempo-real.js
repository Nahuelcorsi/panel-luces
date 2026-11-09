// Conexión en vivo con el servidor del panel (WebSocket en /ws, ver tiempo_real.py).
// Uso:
//   TR.conectar({ pantalla: 'panel', rol: 'inicio' });   // rol: 'inicio' (panel de luces) o 'amigo' (la cara)
//   TR.on('luces', d => ...);                             // el servidor avisa: luces, aviso, hay_aviso, pc
//   const r = await TR.pensar(sistema, mensajes, trozo => ..., () => ...);   // el cerebro de a pedacitos
// Si se corta, se reconecta solo (esperando cada vez un poco más, hasta 15 s). Mientras TR.conectado es false, las
// pantallas siguen consultando como antes, así nada deja de andar si el WebSocket falla.
const TR = (() => {
  let ws = null, opciones = null, espera = 1000, siguiente = 1, conectado = false;
  const oyentes = {}, pendientes = new Map();
  const ESPERA_CEREBRO = 150000;   // un poco más que lo que el servidor le da al cerebro más lento

  function emitir(tipo, datos) {
    for (const fn of oyentes[tipo] || []) { try { fn(datos); } catch (e) { console.error(e); } }
  }

  function abrir() {
    const protocolo = location.protocol === 'https:' ? 'wss' : 'ws';
    try {
      ws = new WebSocket(`${protocolo}://${location.host}/ws?pantalla=${opciones.pantalla}&rol=${opciones.rol}`);
    } catch (e) { return reintentar(); }
    ws.onopen = () => { conectado = true; espera = 1000; emitir('conectado'); };
    ws.onmessage = (e) => {
      let d;
      try { d = JSON.parse(e.data); } catch { return; }
      if (typeof d.id === 'number' && pendientes.has(d.id)) return respuestaCerebro(d);
      emitir(d.tipo, d);
    };
    ws.onclose = () => {
      const estaba = conectado;
      conectado = false; ws = null;
      for (const p of pendientes.values()) { clearTimeout(p.timer); p.mal(new Error('se cortó la conexión con el panel')); }
      pendientes.clear();
      if (estaba) emitir('desconectado');
      reintentar();
    };
  }
  function reintentar() {
    setTimeout(abrir, espera);
    espera = Math.min(espera * 2, 15000);
  }

  function respuestaCerebro(d) {
    const p = pendientes.get(d.id);
    if (d.tipo === 'trozo') return p.alTrozo?.(d.texto);
    if (d.tipo === 'reinicio') return p.alReiniciar?.();
    clearTimeout(p.timer);
    pendientes.delete(d.id);
    if (d.tipo === 'respuesta') p.ok(d);
    else p.mal(new Error(d.mensaje || 'el cerebro no contestó'));
  }

  return {
    get conectado() { return conectado; },
    conectar(op) { if (!opciones) { opciones = op; abrir(); } },
    on(tipo, fn) { (oyentes[tipo] ||= []).push(fn); },
    mandar(obj) { if (conectado) ws.send(JSON.stringify(obj)); },
    pensar(sistema, mensajes, alTrozo, alReiniciar) {
      if (!conectado) return Promise.reject(new Error('sin conexión en vivo'));
      return new Promise((ok, mal) => {
        const id = siguiente++;
        const timer = setTimeout(() => { pendientes.delete(id); mal(new Error('el cerebro tardó demasiado')); }, ESPERA_CEREBRO);
        pendientes.set(id, { ok, mal, alTrozo, alReiniciar, timer });
        ws.send(JSON.stringify({ tipo: 'pensar', id, sistema, mensajes }));
      });
    },
  };
})();
