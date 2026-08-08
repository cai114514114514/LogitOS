import './style.css'

let count = 0
const app = document.querySelector<HTMLDivElement>('#app')!
app.innerHTML = `
  <h1>vite fixture</h1>
  <button id="inc" type="button">count is 0</button>
  <button id="route" type="button">open lazy route</button>
  <p id="lazy"></p>`

const inc = document.getElementById('inc')!
inc.addEventListener('click', () => { count++; inc.textContent = `count is ${count}` })
document.getElementById('route')!.addEventListener('click', async () => {
  const m = await import('./lazy')
  document.getElementById('lazy')!.textContent = m.lazyMessage()
})

// Fired at load as well as on click: the probe injects no input, so a chunk
// loader that only runs on a click is a chunk loader this corpus never measures.
import('./lazy').then(m => { document.getElementById('lazy')!.textContent = m.lazyMessage() })
