<script lang="ts">
  let count = $state(0)
  let Lazy = $state<any>(null)
  async function openRoute() {
    Lazy = (await import('./lib/Lazy.svelte')).default
  }
  openRoute()
</script>
<div id="app">
  <h1>svelte fixture</h1>
  <button id="inc" onclick={() => count++}>count is {count}</button>
  <button id="route" onclick={openRoute}>open lazy route</button>
  {#if Lazy}<Lazy />{/if}
</div>
