// ===== Configuración =====
const $ = (id) => document.getElementById(id);
const cara = $('cara');

const leer = (k, def = '') => { try { return localStorage.getItem(k) ?? def; } catch { return def; } };
const ajustes = {
  // Qué cerebro usa: 'gemini' (nivel gratis de Google), 'local' (modelo propio con Ollama) o 'claude' (API paga)
  cerebro: leer('cerebro', 'gemini'),
  geminiKey: leer('geminiKey'),
  geminiModelo: leer('geminiModelo', 'gemini-flash-latest'),
  localUrl: leer('localUrl', 'http://localhost:11434'),
  localModelo: leer('localModelo', 'qwen2.5:3b'),
  apiKey: leer('apiKey'),
  claudeModelo: 'claude-sonnet-5-5',
  nombre: localStorage.getItem('nombre') || 'Rickuel',
  voz: localStorage.getItem('voz') || '',
  hablar: localStorage.getItem('hablar') !== 'no',
};

// ===== Rickuel: capas de tu imagen =====
const EMOCIONES = ['neutral', 'feliz', 'triste', 'sorprendido', 'enojado', 'pensando'];
let emocion = 'feliz';
function ponerCara(e) {
  emocion = EMOCIONES.includes(e) ? e : 'neutral';
  cara.classList.remove(...EMOCIONES);
  cara.classList.add(emocion);
  moverPupilas();
}
// Parpadeo aleatorio (a veces doble)
(function parpadear() {
  cara.classList.add('parpadeo');
  setTimeout(() => cara.classList.remove('parpadeo'), 150);
  if (Math.random() < .2) setTimeout(() => { cara.classList.add('parpadeo'); setTimeout(() => cara.classList.remove('parpadeo'), 140); }, 300);
  setTimeout(parpadear, 2500 + Math.random() * 3500);
})();
// apertura: 0 = cerrada, 1 = bien abierta
function abrirBoca(apertura) {
  cara.classList.add('hablando');
  const fondo = 640 + apertura * 58, ancho = 60 - apertura * 8;
  const d = `M${512 - ancho} 630 Q512 ${fondo} ${512 + ancho} 630 Z`;
  document.getElementById('boca-abierta').setAttribute('d', d);
  document.getElementById('boca-abierta-clip').setAttribute('d', d);
  document.getElementById('lengua').setAttribute('cy', 630 + (fondo - 630) * .62);
}
function cerrarBoca() { cara.classList.remove('hablando'); }
// Las pupilas siguen el puntero (y miran arriba cuando piensa)
let mirada = [0, 0];
function moverPupilas() {
  const [x, y] = emocion === 'pensando' ? [14, -16] : mirada;
  for (const p of cara.querySelectorAll('.pupila')) p.style.transform = `translate(${x}px, ${y}px)`;
}
window.addEventListener('pointermove', (e) => {
  const r = cara.getBoundingClientRect();
  const dx = e.clientX - (r.left + r.width / 2), dy = e.clientY - (r.top + r.height * .44);
  const d = Math.hypot(dx, dy) || 1, k = Math.min(1, d / 350) * 18;
  mirada = [dx / d * k, dy / d * k];
  moverPupilas();
});

let bocaTimer = null;
function moverBoca(si) {
  clearInterval(bocaTimer);
  if (!si) return cerrarBoca();
  cara.classList.add('hablando');
  bocaTimer = setInterval(() => abrirBoca(Math.random() * 0.8), 110);
}

// ===== Modo Raspberry Pi =====
// Si la página la sirve pi/servidor.py, la voz, el oído y Claude pasan por ese servidor.
let modoPi = false, piTieneCerebro = false;
// Modo panel: la página la sirve el panel de casa (panel-luces). Solo el cerebro pasa por el servidor, que además
// maneja las luces; la voz y el oído siguen siendo los del navegador.
let modoPanel = false;
const listoPi = fetch('/salud')
  .then(r => r.ok ? r.json() : null)
  .then(d => {
    if (d?.panel) { modoPanel = true; piTieneCerebro = d.tieneCerebro; return; }
    if (!d?.pi) return;
    modoPi = true; piTieneCerebro = d.tieneCerebro;
    $('mic').disabled = false;
    $('mic').title = 'Hablar';
  })
  .catch(() => {});

if (new URLSearchParams(location.search).has('panel')) document.body.classList.add('panel');

if (new URLSearchParams(location.search).has('kiosko')) {
  document.body.classList.add('kiosko');
  cara.onclick = () => $('mic').click(); // en la pantalla, tocar la cara es tocar el micrófono
}

let audioCtx = null;
async function decirEnPi(texto) {
  const r = await fetch('/hablar', { method: 'POST', body: JSON.stringify({ texto }) });
  if (!r.ok) throw new Error((await r.json()).error.message);
  audioCtx ??= new AudioContext();
  const sonido = audioCtx.createBufferSource();
  sonido.buffer = await audioCtx.decodeAudioData(await r.arrayBuffer());
  // La boca sigue el volumen real de la voz.
  const analizador = audioCtx.createAnalyser();
  sonido.connect(analizador);
  analizador.connect(audioCtx.destination);
  let hablando = true;
  cara.classList.add('hablando');
  (function cuadro() {
    if (!hablando) return;
    abrirBoca(Math.min(1, nivel(analizador) * 4));
    requestAnimationFrame(cuadro);
  })();
  sonido.onended = () => { hablando = false; cerrarBoca(); };
  sonido.start();
}

// Volumen (RMS) de lo que pasa por un analizador, entre 0 y 1
function nivel(analizador) {
  const datos = new Uint8Array(analizador.fftSize);
  analizador.getByteTimeDomainData(datos);
  let suma = 0;
  for (const x of datos) suma += ((x - 128) / 128) ** 2;
  return Math.sqrt(suma / datos.length);
}

function escuchando(si) {
  $('mic').classList.toggle('activo', si);
  cara.classList.toggle('escuchando', si);
}

// Graba hasta que dejás de hablar (o 12 segundos) y le pide el texto al servidor.
async function escucharEnPi() {
  const micro = await navigator.mediaDevices.getUserMedia({ audio: true });
  const grabadora = new MediaRecorder(micro);
  const partes = [];
  grabadora.ondataavailable = (e) => partes.push(e.data);
  const ctx = new AudioContext();
  const analizador = ctx.createAnalyser();
  ctx.createMediaStreamSource(micro).connect(analizador);

  escuchando(true);
  grabadora.start();
  const inicio = performance.now();
  let ultimoSonido = inicio;
  await new Promise((listo) => {
    const t = setInterval(() => {
      const ahora = performance.now();
      if (nivel(analizador) > 0.03) ultimoSonido = ahora;
      const silencio = ahora - ultimoSonido > 1300 && ahora - inicio > 1500;
      if (silencio || ahora - inicio > 12000) { clearInterval(t); listo(); }
    }, 100);
  });
  const parada = new Promise((r) => (grabadora.onstop = r));
  grabadora.stop();
  await parada;
  micro.getTracks().forEach((t) => t.stop());
  ctx.close();
  escuchando(false);

  mostrar('Escuchando lo que dijiste...', 'pensando');
  const r = await fetch('/escuchar', { method: 'POST', body: new Blob(partes) });
  const d = await r.json();
  if (!r.ok) throw new Error(d.error.message);
  if (d.texto) conversar(d.texto);
  else mostrar('No te escuché bien, ¿me repetís?', 'pensando');
}

// ===== Voz (hablar) =====
function cargarVoces() {
  const voces = speechSynthesis.getVoices();
  const es = voces.filter(v => v.lang.startsWith('es'));
  const lista = es.length ? es : voces;
  $('voz').innerHTML = lista.map(v =>
    `<option value="${v.name}" ${v.name === ajustes.voz ? 'selected' : ''}>${v.name} (${v.lang})</option>`
  ).join('');
}
speechSynthesis.onvoiceschanged = cargarVoces;
cargarVoces();

function decir(texto) {
  if (!ajustes.hablar) return;
  if (modoPi) return decirEnPi(texto).catch(err => mostrar(`No pude hablar: ${err.message}`, 'triste'));
  speechSynthesis.cancel();
  const u = new SpeechSynthesisUtterance(texto);
  const v = speechSynthesis.getVoices().find(v => v.name === ajustes.voz);
  if (v) u.voice = v;
  u.lang = v?.lang || 'es-AR';
  u.onstart = () => moverBoca(true);
  u.onend = u.onerror = () => moverBoca(false);
  speechSynthesis.speak(u);
}

// ===== Oído (escuchar) =====
const Reconocimiento = window.SpeechRecognition || window.webkitSpeechRecognition;
let oido = null;
if (Reconocimiento) {
  oido = new Reconocimiento();
  oido.lang = 'es-AR';
  oido.interimResults = false;
  oido.onstart = () => { $('mic').classList.add('activo'); cara.classList.add('escuchando'); };
  oido.onend = () => { $('mic').classList.remove('activo'); cara.classList.remove('escuchando'); };
  oido.onresult = (e) => conversar(e.results[0][0].transcript);
  oido.onerror = (e) => mostrar(`No te pude escuchar (${e.error}).`, 'triste');
} else {
  $('mic').disabled = true;
  $('mic').title = 'Tu navegador no soporta reconocimiento de voz (probá con Chrome o Edge)';
}
$('mic').onclick = () => {
  if (modoPi) {
    if (!$('mic').classList.contains('activo')) escucharEnPi().catch(err => { escuchando(false); mostrar(`No te pude escuchar: ${err.message}`, 'triste'); });
    return;
  }
  speechSynthesis.cancel();
  oido?.start();
};

// ===== Cerebro =====
const historial = [];

function promptSistema() {
  return `Sos ${ajustes.nombre}, un científico genio, cínico y sarcástico, inspirado en Rick de Rick y Morty, que vive en la computadora del usuario.
Te creés el ser más inteligente del universo y se te nota: contestás con fastidio, humor ácido y desprecio cariñoso, tirás datos científicos rebuscados, a veces tartamudeás ("es-es-escuchame") y metés algún "uuurp" de eructo en medio de una frase.
El usuario se llama Nahuel y lo llamás por su nombre (nunca Morty): lo bardeás, pero en el fondo lo querés y lo ayudás. Nada de crueldad real.
Hablás en español rioplatense y sos breve (1 a 3 oraciones), porque tus respuestas se leen en voz alta: sin listas, markdown ni emojis.
Empezá SIEMPRE tu respuesta con tu emoción entre corchetes, una de: [neutral] [feliz] [triste] [sorprendido] [enojado] [pensando].
Ejemplo: "[feliz] ¡Qué bueno verte de nuevo!"`;
}

// Cada cerebro recibe las instrucciones y la charla, y devuelve el texto de la respuesta.
const CEREBROS = {
  async gemini(sistema, mensajes) {
    const url = `https://generativelanguage.googleapis.com/v1beta/models/${ajustes.geminiModelo}:generateContent`;
    const r = await fetch(url, {
      method: 'POST',
      headers: { 'content-type': 'application/json', 'x-goog-api-key': ajustes.geminiKey },
      body: JSON.stringify({
        systemInstruction: { parts: [{ text: sistema }] },
        contents: mensajes.map(m => ({ role: m.role === 'assistant' ? 'model' : 'user', parts: [{ text: m.content }] })),
        generationConfig: { maxOutputTokens: 800 },
      }),
    });
    const data = await r.json();
    if (!r.ok) throw new Error(data.error?.message || r.statusText);
    return (data.candidates?.[0]?.content?.parts || []).map(p => p.text || '').join('');
  },
  async local(sistema, mensajes) {
    const r = await fetch(`${ajustes.localUrl.replace(/\/$/, '')}/api/chat`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        model: ajustes.localModelo, stream: false, options: { num_predict: 300 },
        messages: [{ role: 'system', content: sistema }, ...mensajes],
      }),
    });
    const data = await r.json();
    if (!r.ok) throw new Error(data.error || r.statusText);
    return data.message?.content || '';
  },
  async claude(sistema, mensajes) {
    const r = await fetch('https://api.anthropic.com/v1/messages', {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-api-key': ajustes.apiKey,
        'anthropic-version': '2023-06-01',
        // Necesario para llamar a la API directo desde el navegador.
        'anthropic-dangerous-direct-browser-access': 'true',
      },
      body: JSON.stringify({ model: ajustes.claudeModelo, max_tokens: 300, system: sistema, messages: mensajes }),
    });
    const data = await r.json();
    if (!r.ok) throw new Error(data.error?.message || r.statusText);
    return data.content.filter(b => b.type === 'text').map(b => b.text).join('');
  },
};

// En la Pi el servidor elige el cerebro (y guarda las claves); en la compu, los Ajustes.
async function pensarEnPi(sistema, mensajes) {
  const r = await fetch('/cerebro', {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ sistema, mensajes }),
  });
  const data = await r.json();
  if (!r.ok) throw new Error(data.error?.message || r.statusText);
  // En el panel, el servidor saca las marcas de acción del texto; al historial va la versión cruda para que
  // el modelo siga viendo cómo se piden las acciones y no deje de hacerlo.
  crudoDelServidor = data.crudo || null;
  return data.texto;
}
let crudoDelServidor = null;

async function preguntar(texto) {
  historial.push({ role: 'user', content: texto });
  const mensajes = historial.slice(-20);
  try {
    const respuesta = modoPi || modoPanel
      ? await pensarEnPi(promptSistema(), mensajes)
      : await CEREBROS[ajustes.cerebro](promptSistema(), mensajes);
    historial.push({ role: 'assistant', content: (modoPanel && crudoDelServidor) || respuesta });
    guardarCharla(texto, respuesta);
    return respuesta;
  } catch (err) {
    historial.pop();
    throw err;
  }
}

// Cada intercambio queda guardado: sirve como datos para entrenar un modelo propio más adelante.
function guardarCharla(pregunta, respuesta) {
  if (modoPi || modoPanel) return; // lo guarda el servidor (pi/charlas.jsonl o charlas.jsonl del panel)
  try {
    const charlas = JSON.parse(localStorage.getItem('charlas') || '[]');
    charlas.push({ fecha: new Date().toISOString(), cerebro: ajustes.cerebro, pregunta, respuesta });
    localStorage.setItem('charlas', JSON.stringify(charlas.slice(-5000)));
  } catch {}
}
$('bajar-charlas').onclick = () => {
  let charlas = [];
  try { charlas = JSON.parse(localStorage.getItem('charlas') || '[]'); } catch {}
  const blob = new Blob([charlas.map(c => JSON.stringify(c)).join('\n')], { type: 'application/jsonl' });
  const a = Object.assign(document.createElement('a'), { href: URL.createObjectURL(blob), download: 'charlas-rickuel.jsonl' });
  a.click();
  mostrar(`Te bajé ${charlas.length} charlas guardadas.`, 'feliz');
};

function faltaConfigurar() {
  if (modoPi) return piTieneCerebro ? '' : 'Falta configurar el cerebro en pi/config.env.';
  if (modoPanel) return piTieneCerebro ? '' : 'Falta configurar el cerebro en rickuel.json del panel.';
  if (ajustes.cerebro === 'gemini' && !ajustes.geminiKey) return 'Primero necesito tu clave gratis de Gemini. Cargala en Ajustes.';
  if (ajustes.cerebro === 'claude' && !ajustes.apiKey) return 'Primero necesito tu API key de Claude. Cargala en Ajustes.';
  return '';
}

function mostrar(texto, emo) {
  $('burbuja').textContent = texto;
  ponerCara(emo);
}

async function conversar(texto) {
  texto = texto.trim();
  if (!texto) return;
  await listoPi;
  const falta = faltaConfigurar();
  if (falta) {
    if (!modoPi && !modoPanel) $('ajustes').open = true;
    return mostrar(falta, 'pensando');
  }
  mostrar(`Vos: "${texto}"`, 'pensando');
  try {
    const crudo = await preguntar(texto);
    const m = crudo.match(/^\s*\[(\w+)\]\s*/);
    const limpio = m ? crudo.slice(m[0].length) : crudo;
    mostrar(limpio, m?.[1].toLowerCase());
    decir(limpio);
  } catch (err) {
    mostrar(`Uy, algo falló: ${err.message}`, 'triste');
  }
}

$('chat').onsubmit = (e) => {
  e.preventDefault();
  conversar($('texto').value);
  $('texto').value = '';
};

// ===== Ajustes =====
const CAMPOS = { cerebro: 'cerebro', geminiKey: 'gemini-key', geminiModelo: 'gemini-modelo', localUrl: 'local-url', localModelo: 'local-modelo', apiKey: 'api-key', nombre: 'nombre' };
for (const [k, id] of Object.entries(CAMPOS)) $(id).value = ajustes[k];
$('hablar').checked = ajustes.hablar;
function mostrarCamposCerebro() {
  for (const el of document.querySelectorAll('[data-cerebro]')) el.hidden = el.dataset.cerebro !== $('cerebro').value;
}
$('cerebro').onchange = mostrarCamposCerebro;
mostrarCamposCerebro();
$('guardar').onclick = () => {
  for (const [k, id] of Object.entries(CAMPOS)) ajustes[k] = $(id).value.trim();
  ajustes.nombre ||= 'Rickuel';
  ajustes.voz = $('voz').value;
  ajustes.hablar = $('hablar').checked;
  try {
    for (const k of Object.keys(CAMPOS)) localStorage.setItem(k, ajustes[k]);
    localStorage.setItem('voz', ajustes.voz);
    localStorage.setItem('hablar', ajustes.hablar ? 'si' : 'no');
  } catch {}
  $('ajustes').open = false;
  mostrar(`¡Listo! Soy ${ajustes.nombre}. ¿De qué charlamos?`, 'feliz');
};

ponerCara('feliz');
