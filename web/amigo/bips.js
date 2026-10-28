// Ruiditos de robot del amigo (bips, trinos, zumbidos), estilo droide: se fabrican en el momento con Web Audio,
// sin archivos de sonido. Cada emoción tiene su "frase" y cada vez sale un poco distinta.
// Uso: bip('feliz'). bips.grabar(emocion, ctx) dibuja la frase en otro contexto (por ejemplo, uno offline).
const bips = (() => {
  let ctx = null;
  const azar = (a, b) => a + Math.random() * (b - a);

  // Una nota que se desliza de f0 a f1 Hz en dur segundos, con un vibrato opcional
  function nota(c, salida, t, f0, f1, dur, { tipo = 'sine', vol = 0.18, vibrato = 0, ataque = 0.01 } = {}) {
    const osc = c.createOscillator(), env = c.createGain();
    osc.type = tipo;
    osc.frequency.setValueAtTime(f0, t);
    osc.frequency.exponentialRampToValueAtTime(Math.max(20, f1), t + dur);
    if (vibrato) {
      const lfo = c.createOscillator(), prof = c.createGain();
      lfo.frequency.value = azar(9, 14);
      prof.gain.value = vibrato;
      lfo.connect(prof).connect(osc.frequency);
      lfo.start(t); lfo.stop(t + dur);
    }
    env.gain.setValueAtTime(0.0001, t);
    env.gain.exponentialRampToValueAtTime(vol, t + ataque);
    env.gain.exponentialRampToValueAtTime(0.0001, t + dur);
    osc.connect(env).connect(salida);
    osc.start(t); osc.stop(t + dur + 0.02);
    return t + dur;
  }

  // Ruido filtrado que barre de f0 a f1 Hz (el "slurp" del café, la respiración al dormir)
  function soplido(c, salida, t, f0, f1, dur, vol = 0.12) {
    const buf = c.createBuffer(1, Math.ceil(c.sampleRate * dur), c.sampleRate);
    const d = buf.getChannelData(0);
    for (let i = 0; i < d.length; i++) d[i] = Math.random() * 2 - 1;
    const src = c.createBufferSource(), filtro = c.createBiquadFilter(), env = c.createGain();
    src.buffer = buf;
    filtro.type = 'bandpass'; filtro.Q.value = 6;
    filtro.frequency.setValueAtTime(f0, t);
    filtro.frequency.exponentialRampToValueAtTime(f1, t + dur);
    env.gain.setValueAtTime(0.0001, t);
    env.gain.exponentialRampToValueAtTime(vol, t + dur * 0.3);
    env.gain.exponentialRampToValueAtTime(0.0001, t + dur);
    src.connect(filtro).connect(env).connect(salida);
    src.start(t); src.stop(t + dur);
    return t + dur;
  }

  const FRASES = {
    neutral(c, s, t) { return nota(c, s, t, azar(900, 1100), azar(1150, 1400), 0.09); },
    feliz(c, s, t) {   // trinito que sube
      let f = azar(800, 1100);
      for (let i = 0, n = Math.round(azar(3, 5)); i < n; i++) {
        t = nota(c, s, t, f, f * azar(1.2, 1.5), azar(0.05, 0.08), { tipo: 'triangle' }) + 0.015;
        f *= azar(1.1, 1.3);
      }
      return t;
    },
    triste(c, s, t) {  // "buuu" que baja despacio
      t = nota(c, s, t, azar(650, 750), azar(330, 380), 0.55, { vibrato: 12, ataque: 0.05 });
      return nota(c, s, t + 0.05, 340, 260, 0.3, { vol: 0.1, ataque: 0.04 });
    },
    sorprendido(c, s, t) {   // "¿¡bip!?" que sube de golpe
      t = nota(c, s, t, azar(350, 450), azar(2000, 2400), 0.16, { tipo: 'triangle' });
      return nota(c, s, t + 0.04, 2300, 2500, 0.06, { vol: 0.14 });
    },
    enojado(c, s, t) {   // zumbido grave y cortado
      for (let i = 0; i < 4; i++) {
        const f = azar(150, 210);
        t = nota(c, s, t, f, f * 0.85, 0.07, { tipo: 'sawtooth', vol: 0.13 }) + 0.03;
      }
      return t;
    },
    pensando(c, s, t) {   // burbujeo "bip-bup-bip"
      for (let i = 0, n = Math.round(azar(4, 6)); i < n; i++) {
        const f = azar(450, 1200);
        t = nota(c, s, t, f, f * azar(1.3, 1.8), 0.045, { vol: 0.12 }) + azar(0.03, 0.08);
      }
      return t;
    },
    enamorado(c, s, t) {   // "uiiii-uuu" con temblorcito
      t = nota(c, s, t, 1100, 1650, 0.22, { vibrato: 25, ataque: 0.03 });
      return nota(c, s, t + 0.02, 1600, 850, 0.4, { vibrato: 25, ataque: 0.03, vol: 0.15 });
    },
    guino(c, s, t) {   // dos blips pícaros
      t = nota(c, s, t, 1500, 1500, 0.05, { tipo: 'square', vol: 0.11 }) + 0.06;
      return nota(c, s, t, 2200, 2400, 0.07, { tipo: 'square', vol: 0.11 });
    },
    cafe(c, s, t) {   // slurp y "ahh" satisfecho
      t = soplido(c, s, t, 700, 2500, 0.35);
      return nota(c, s, t + 0.08, 620, 480, 0.45, { vibrato: 8, ataque: 0.06, vol: 0.13 });
    },
    dormido(c, s, t) {   // ronquido de robot y silbidito
      t = nota(c, s, t, 85, 110, 0.6, { tipo: 'sawtooth', vol: 0.12, ataque: 0.25 });
      t = soplido(c, s, t, 300, 600, 0.3, 0.1);
      return nota(c, s, t + 0.05, 1300, 900, 0.35, { vol: 0.1, ataque: 0.08 });
    },
  };

  function grabar(emocion, c, t = 0) {
    const e = (emocion || '').normalize('NFD').replace(/[̀-ͯ]/g, '');
    return (FRASES[e] || FRASES.neutral)(c, c.destination, t);
  }

  function bip(emocion) {
    try {
      ctx ??= new AudioContext();
      if (ctx.state === 'suspended') ctx.resume();
      grabar(emocion, ctx, ctx.currentTime + 0.02);
    } catch {}
  }
  // El navegador no deja sonar nada hasta que tocás la página: se aprovecha el primer toque.
  addEventListener('pointerdown', () => { try { ctx ??= new AudioContext(); ctx.resume(); } catch {} }, { once: true, capture: true });

  return { bip, grabar, FRASES };
})();
const bip = bips.bip;
