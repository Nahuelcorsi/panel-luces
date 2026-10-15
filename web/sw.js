// Copia de la página del panel para que la app abra aunque el servidor del celular todavía no arrancó
// (por ejemplo, el primer minuto después de prenderlo). Siempre intenta primero la versión del servidor.
const CACHE = 'panel-v1';
const ARCHIVOS = ['/', '/manifest.webmanifest', '/icono.svg'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(ARCHIVOS)).catch(() => {}));
  self.skipWaiting();
});
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));

self.addEventListener('fetch', e => {
  const url = new URL(e.request.url);
  if (e.request.method !== 'GET' || url.pathname.startsWith('/api/') || url.pathname === '/salud') return;   // los datos siempre en vivo
  e.respondWith(
    fetch(e.request)
      .then(r => { const copia = r.clone(); caches.open(CACHE).then(c => c.put(e.request, copia)); return r; })
      .catch(() => caches.match(e.request, { ignoreSearch: true }).then(r => r || caches.match('/')))
  );
});
