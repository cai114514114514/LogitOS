<script setup lang="ts">
import { ref, defineAsyncComponent } from 'vue'
const Lazy = defineAsyncComponent(() => import('./components/Lazy.vue'))
const count = ref(0)
const showRoute = ref(true)
</script>
<template>
  <div id="app">
    <h1>vue fixture</h1>
    <button id="inc" @click="count++">count is {{ count }}</button>
    <button id="route" @click="showRoute = true">open lazy route</button>
    <Lazy v-if="showRoute" />
  </div>
</template>
