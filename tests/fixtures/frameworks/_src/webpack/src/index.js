let count = 0;
const app = document.getElementById('app');
app.innerHTML =
  '<h1>webpack fixture</h1>' +
  '<button id="inc" type="button">count is 0</button>' +
  '<button id="route" type="button">open lazy route</button>' +
  '<p id="lazy"></p>';

const inc = document.getElementById('inc');
inc.addEventListener('click', () => { count++; inc.textContent = 'count is ' + count; });
document.getElementById('route').addEventListener('click', async () => {
  const m = await import('./lazy');
  document.getElementById('lazy').textContent = m.lazyMessage();
});

// See the note in the vite fixture: the chunk loader has to run without input.
import('./lazy').then(m => { document.getElementById('lazy').textContent = m.lazyMessage(); });
