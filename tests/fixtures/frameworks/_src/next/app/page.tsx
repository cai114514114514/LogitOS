"use client";
import { useState } from "react";
import dynamic from "next/dynamic";

const Lazy = dynamic(() => import("./lazy"), { ssr: false, loading: () => <p>loading…</p> });

export default function Home() {
  const [count, setCount] = useState(0);
  const [showRoute, setShowRoute] = useState(true);
  return (
    <div id="app">
      <h1>next fixture</h1>
      <button id="inc" onClick={() => setCount((c) => c + 1)}>count is {count}</button>
      <button id="route" onClick={() => setShowRoute(true)}>open lazy route</button>
      {showRoute && <Lazy />}
    </div>
  );
}
