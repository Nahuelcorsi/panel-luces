// Látigo del amigo: un iconito arriba a la derecha; al tocarlo aparece un látigo que sigue al puntero (o al dedo).
// Con un agitón chasquea (sonido + el amigo se sobresalta) y cae. Física Verlet adaptada de OpenWhip (MIT).
(() => {
  if (new URLSearchParams(location.search).has('kiosko')) return;   // la Raspberry no tiene mouse ni dedo

  // ---- Ajustes (tocá acá para cambiar la sensación) ----
  const P = {
    segmentos: 28, largoSegmento: 25, afinado: 0.6,
    gravedad: 1.2, gravedadCaida: 0.95, amortiguacion: 0.96, iteraciones: 20, estiramientoMax: 1.2,
    anguloBase: -1.12, aimMouseX: 0.4, aimMouseY: 0.2, aimMax: 2.0, resorte: 0.7, amortAngular: 0.078,
    segmentosGuiados: 2, rigidezGuiaIni: 0.9, rigidezGuiaFin: 0.8,
    curvaMangoGrados: 16, curvaPuntaGrados: 130, rigidezCurvaIni: 0.8, rigidezCurvaFin: 0.12,
    rebote: 0.42, friccionBorde: 0.86,
    velocidadChasquido: 110,     // velocidad de la punta (px por frame) para que chasquee
    esperaChasquidoMs: 1000,     // no chasquea durante el primer segundo
    pausaEntreChasquidosMs: 1500,
    caeTrasChasquidoMs: 500,     // el látigo sigue visible un ratito y después cae
    anchoMango: 7, anchoPunta: 5, borde: 3, extraMango: 5, eslabonesMango: 2,
    arcoAncho: 260, arcoAlto: 185,
  };
  const SONIDOS = ['A', 'B', 'C', 'D', 'E'].map(l => `sonidos/${l}.mp3`);
  const PASO = 1000 / 60;   // la física está calibrada a 60 cuadros por segundo

  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const lerp = (a, b, t) => a + (b - a) * t;
  const envolver = a => { while (a > Math.PI) a -= 2 * Math.PI; while (a < -Math.PI) a += 2 * Math.PI; return a; };
  const largoEslabon = i => P.largoSegmento * (1 - (i / (P.segmentos - 1)) * (1 - P.afinado));

  // ---- Icono ----
  const boton = document.createElement('button');
  boton.id = 'boton-latigo';
  boton.type = 'button';
  boton.title = 'Látigo';
  boton.setAttribute('aria-label', 'Sacar el látigo');
  boton.innerHTML = '<svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 20l5-5"/><path d="M9 15c0-5 4-6 6-4s-1 5 2 6 3-3 3-7"/></svg>';
  const estilo = document.createElement('style');
  estilo.textContent = `
    #boton-latigo { position: fixed; z-index: 20; top: calc(12px + env(safe-area-inset-top)); right: 12px; width: 40px; height: 40px;
      padding: 0; display: grid; place-items: center; border-radius: 50%; border: 1px solid #ffffff22;
      background: #14151dcc; color: #f2f2f2; cursor: pointer; opacity: .75; transition: opacity .15s, transform .15s; }
    #boton-latigo:hover, #boton-latigo:focus-visible { opacity: 1; transform: scale(1.08); }
    #boton-latigo[hidden] { display: none !important; }
    #capa-latigo { position: fixed; inset: 0; z-index: 30; touch-action: none; user-select: none; -webkit-user-select: none; }
    #capa-latigo.con-latigo { cursor: none; }
  `;
  document.head.appendChild(estilo);
  document.body.appendChild(boton);

  // ---- Estado ----
  let capa = null, ctx = null, W = 0, H = 0;
  let latigo = null, cayendo = false;
  let mx = 0, my = 0, pmx = 0, pmy = 0, tocando = false, ptr = 'mouse';
  let angulo = P.anguloBase, velAng = 0;
  let nacio = 0, ultimoChasquido = 0, raf = 0, ultimoT = 0, acum = 0, cierre = 0, rebajarTimer = 0;

  function sonar() {
    const a = new Audio(SONIDOS[Math.floor(Math.random() * SONIDOS.length)]);
    a.play().catch(() => {});
  }

  function reaccionar() {
    // El amigo se sobresalta un momento (usa ponerCara de app.js si está)
    try {
      if (typeof ponerCara === 'function') {
        const antes = typeof emocion === 'string' ? emocion : 'feliz';
        ponerCara('sorprendido');
        setTimeout(() => ponerCara(antes), 1800);
      }
    } catch { /* si cambia app.js, el látigo sigue funcionando */ }
  }

  function crear(x, y) {
    const pts = [];
    for (let i = 0; i < P.segmentos; i++) {
      const t = i / (P.segmentos - 1);
      const px = x + t * P.arcoAncho, py = y - Math.sin(t * Math.PI * 0.75) * P.arcoAlto;
      pts.push({ x: px, y: py, px, py });
    }
    return pts;
  }

  // ---- Física ----
  function apuntarMango() {
    if (cayendo) return;
    const delta = clamp((mx - pmx) * P.aimMouseX + (my - pmy) * P.aimMouseY, -P.aimMax, P.aimMax);
    velAng += envolver(P.anguloBase + delta - angulo) * P.resorte;
    velAng *= P.amortAngular;
    angulo = envolver(angulo + velAng);
  }
  function poseBase() {
    if (!latigo || cayendo) return;
    const dx = Math.cos(angulo), dy = Math.sin(angulo);
    const n = Math.min(P.segmentosGuiados, latigo.length - 1);
    for (let i = 1; i <= n; i++) {
      const rig = lerp(P.rigidezGuiaIni, P.rigidezGuiaFin, (i - 1) / Math.max(n - 1, 1));
      const a = latigo[i - 1], p = latigo[i], l = largoEslabon(i - 1);
      p.x = lerp(p.x, a.x + dx * l, rig);
      p.y = lerp(p.y, a.y + dy * l, rig);
    }
  }
  function limitarCurvas() {
    for (let i = 1; i < latigo.length - 1; i++) {
      const a = latigo[i - 1], b = latigo[i], c = latigo[i + 1];
      const v1x = a.x - b.x, v1y = a.y - b.y, v2x = c.x - b.x, v2y = c.y - b.y;
      const l1 = Math.hypot(v1x, v1y) || 1e-4, l2 = Math.hypot(v2x, v2y) || 1e-4;
      const n1x = v1x / l1, n1y = v1y / l1, n2x = v2x / l2, n2y = v2y / l2;
      const angulo2 = Math.acos(clamp(n1x * n2x + n1y * n2y, -1, 1));
      const t = i / (latigo.length - 2);
      const maxCurva = lerp(P.curvaMangoGrados, P.curvaPuntaGrados, t) * Math.PI / 180;
      if (Math.PI - angulo2 <= maxCurva) continue;
      const signo = n1x * n2y - n1y * n2x >= 0 ? 1 : -1;
      const objetivo = Math.atan2(n1y, n1x) + signo * (Math.PI - maxCurva);
      const rig = lerp(P.rigidezCurvaIni, P.rigidezCurvaFin, t);
      c.x = lerp(c.x, b.x + Math.cos(objetivo) * l2, rig);
      c.y = lerp(c.y, b.y + Math.sin(objetivo) * l2, rig);
    }
  }
  function limitarEstiramiento() {
    for (let i = 0; i < latigo.length - 1; i++) {
      const a = latigo[i], b = latigo[i + 1];
      const dx = b.x - a.x, dy = b.y - a.y, d = Math.hypot(dx, dy) || 1e-4, max = largoEslabon(i) * P.estiramientoMax;
      if (d > max) { b.x = a.x + dx * max / d; b.y = a.y + dy * max / d; }
    }
  }
  function bordes() {
    if (cayendo) return;
    for (let i = 1; i < latigo.length; i++) {
      const p = latigo[i];
      let vx = p.x - p.px, vy = p.y - p.py, choco = false;
      if (p.x < 0) { p.x = 0; if (vx < 0) vx = -vx * P.rebote; vy *= P.friccionBorde; choco = true; }
      else if (p.x > W) { p.x = W; if (vx > 0) vx = -vx * P.rebote; vy *= P.friccionBorde; choco = true; }
      if (p.y < 0) { p.y = 0; if (vy < 0) vy = -vy * P.rebote; vx *= P.friccionBorde; choco = true; }
      else if (p.y > H) { p.y = H; if (vy > 0) vy = -vy * P.rebote; vx *= P.friccionBorde; choco = true; }
      if (choco) { p.px = p.x - vx; p.py = p.y - vy; }
    }
  }

  function paso() {
    if (!latigo) return;
    const g = cayendo ? P.gravedadCaida : P.gravedad;
    apuntarMango();
    for (let i = cayendo ? 0 : 1; i < latigo.length; i++) {
      const p = latigo[i];
      const vx = (p.x - p.px) * P.amortiguacion, vy = (p.y - p.py) * P.amortiguacion;
      p.px = p.x; p.py = p.y; p.x += vx; p.y += vy + g;
    }
    if (!cayendo) { latigo[0].x = latigo[0].px = mx; latigo[0].y = latigo[0].py = my; }
    limitarEstiramiento(); bordes(); poseBase();
    for (let it = 0; it < P.iteraciones; it++) {
      for (let i = 0; i < latigo.length - 1; i++) {
        const a = latigo[i], b = latigo[i + 1];
        const dx = b.x - a.x, dy = b.y - a.y, d = Math.hypot(dx, dy) || 1e-4;
        const dif = (d - largoEslabon(i)) / d * 0.5, ox = dx * dif, oy = dy * dif;
        if (i === 0 && !cayendo) { b.x -= ox * 2; b.y -= oy * 2; }
        else { a.x += ox; a.y += oy; b.x -= ox; b.y -= oy; }
      }
      limitarCurvas();
      if (!cayendo) poseBase();
      limitarEstiramiento(); bordes();
    }
    const punta = latigo[latigo.length - 1];
    const vel = Math.hypot(punta.x - punta.px, punta.y - punta.py);
    const ahora = performance.now();
    if (!cayendo && vel > P.velocidadChasquido && ahora - nacio >= P.esperaChasquidoMs && ahora - ultimoChasquido > P.pausaEntreChasquidosMs) {
      ultimoChasquido = ahora;
      sonar(); reaccionar();
      rebajarTimer = setTimeout(soltar, P.caeTrasChasquidoMs);
    }
    if (cayendo && latigo.every(p => p.y > H + 60)) cerrar();
    pmx = mx; pmy = my;
  }

  // ---- Dibujo ----
  function curva(i) {
    const n = latigo.length;
    const punto = j => j < 0 ? { x: 2 * latigo[0].x - latigo[1].x, y: 2 * latigo[0].y - latigo[1].y }
      : j >= n ? { x: 2 * latigo[n - 1].x - latigo[n - 2].x, y: 2 * latigo[n - 1].y - latigo[n - 2].y } : latigo[j];
    const p0 = punto(i - 1), p1 = latigo[i], p2 = latigo[i + 1], p3 = punto(i + 2);
    return [p1.x + (p2.x - p0.x) / 6, p1.y + (p2.y - p0.y) / 6, p2.x - (p3.x - p1.x) / 6, p2.y - (p3.y - p1.y) / 6, p2.x, p2.y];
  }
  function dibujar() {
    ctx.clearRect(0, 0, W, H);
    if (!latigo) return;
    ctx.lineCap = ctx.lineJoin = 'round';
    ctx.strokeStyle = '#fff';
    ctx.beginPath(); ctx.moveTo(latigo[0].x, latigo[0].y);
    for (let i = 0; i < latigo.length - 1; i++) ctx.bezierCurveTo(...curva(i));
    ctx.lineWidth = P.anchoPunta + P.borde * 2; ctx.stroke();
    ctx.beginPath(); ctx.moveTo(latigo[0].x, latigo[0].y);
    for (let i = 0; i < Math.min(P.eslabonesMango, latigo.length - 1); i++) ctx.bezierCurveTo(...curva(i));
    ctx.lineWidth = P.anchoMango + P.extraMango + P.borde * 2; ctx.stroke();
    ctx.strokeStyle = '#111';
    for (let i = 0; i < latigo.length - 1; i++) {
      ctx.lineWidth = lerp(P.anchoMango, P.anchoPunta, i / Math.max(1, latigo.length - 2)) + (i < P.eslabonesMango ? P.extraMango : 0);
      ctx.beginPath(); ctx.moveTo(latigo[i].x, latigo[i].y); ctx.bezierCurveTo(...curva(i)); ctx.stroke();
    }
  }

  function cuadro(t) {
    raf = requestAnimationFrame(cuadro);
    acum = Math.min(acum + (t - ultimoT), PASO * 5);   // si la pestaña se durmió, no recuperar de golpe
    ultimoT = t;
    while (acum >= PASO) { paso(); acum -= PASO; }
    dibujar();
  }

  // ---- Entradas ----
  function ajustar() {
    if (!capa) return;
    const dpr = window.devicePixelRatio || 1;
    W = innerWidth; H = innerHeight;
    capa.width = W * dpr; capa.height = H * dpr;
    capa.style.width = W + 'px'; capa.style.height = H + 'px';
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
  function alMover(e) { ptr = e.pointerType; mx = e.clientX; my = e.clientY; }
  function alApretar(e) {
    alMover(e); tocando = true;
    if (e.pointerType === 'mouse') soltar();   // con mouse, un clic suelta el látigo
    else { pmx = mx; pmy = my; }               // con el dedo, el látigo se agarra mientras tocás
  }
  function alLevantar(e) {
    tocando = false;
    if (e.pointerType !== 'mouse') soltar();   // con el dedo, al levantarlo se suelta
  }

  function sacar() {
    if (capa) return;
    capa = document.createElement('canvas');
    capa.id = 'capa-latigo';
    capa.className = 'con-latigo';
    document.body.appendChild(capa);
    ctx = capa.getContext('2d');
    ajustar();
    addEventListener('resize', ajustar);
    capa.addEventListener('pointermove', alMover);
    capa.addEventListener('pointerdown', alApretar);
    capa.addEventListener('pointerup', alLevantar);
    capa.addEventListener('pointercancel', alLevantar);
    if (!mx && !my) { mx = W / 2 - P.arcoAncho / 3; my = H * .6; }   // sin puntero conocido: arrancar abajo al centro
    pmx = mx; pmy = my;
    angulo = P.anguloBase; velAng = 0; cayendo = false;
    latigo = crear(mx, my);
    nacio = performance.now(); ultimoChasquido = 0; acum = 0; ultimoT = performance.now();
    boton.hidden = true;
    raf = requestAnimationFrame(cuadro);
  }
  function soltar() {
    if (!latigo || cayendo) return;
    clearTimeout(rebajarTimer);
    cayendo = true;
    capa.classList.remove('con-latigo');
  }
  function cerrar() {
    cancelAnimationFrame(raf); clearTimeout(rebajarTimer);
    removeEventListener('resize', ajustar);
    capa?.remove(); capa = ctx = null; latigo = null; cayendo = false;
    boton.hidden = false;
  }

  // Guarda la última posición del puntero para que el látigo nazca cerca de él
  addEventListener('pointermove', e => { if (!capa) { mx = e.clientX; my = e.clientY; } }, { passive: true });
  boton.addEventListener('click', e => {
    // Nace debajo del icono (centrado si se tocó con el dedo) para que el arco quepa en pantalla
    mx = clamp(e.clientX, 40, innerWidth - 40);
    my = Math.max(e.clientY + 160, innerHeight * .55);
    sacar();
  });
  addEventListener('keydown', e => { if (e.key === 'Escape' && capa) soltar(); });
})();
