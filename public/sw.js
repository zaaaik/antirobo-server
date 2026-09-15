// Service worker: solo se encarga de mostrar la notificación push y de
// abrir/enfocar la app cuando se toca. No cachea nada más.

self.addEventListener('push', (evento) => {
  let datos = { title: 'Sistema Antirobo', body: 'Hay una novedad.' };
  try { datos = evento.data.json(); } catch (e) {}

  evento.waitUntil(
    self.registration.showNotification(datos.title || 'Sistema Antirobo', {
      body: datos.body || '',
      tag: 'alarma-antirobo', // reemplaza la notificación anterior en vez de acumular
      renotify: true,
      data: { url: datos.url || '/' }
    })
  );
});

self.addEventListener('notificationclick', (evento) => {
  evento.notification.close();
  const url = (evento.notification.data && evento.notification.data.url) || '/';

  evento.waitUntil(
    self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then((listaClientes) => {
      for (const cliente of listaClientes) {
        if ('focus' in cliente) return cliente.focus();
      }
      if (self.clients.openWindow) return self.clients.openWindow(url);
    })
  );
});
