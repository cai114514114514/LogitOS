import { useState, lazy, Suspense } from 'react'

const Lazy = lazy(() => import('./Lazy'))

export default function App() {
  const [count, setCount] = useState(0)
  const [showRoute, setShowRoute] = useState(true)
  return (
    <div id="app">
      <h1>react fixture</h1>
      <button id="inc" onClick={() => setCount(c => c + 1)}>count is {count}</button>
      <button id="route" onClick={() => setShowRoute(true)}>open lazy route</button>
      {showRoute && <Suspense fallback={<p>loading…</p>}><Lazy /></Suspense>}
    </div>
  )
}
