<template>
  <header class="topbar">
    <router-link to="/notes" class="brand">录音豆</router-link>
    <nav>
      <router-link to="/notes" active-class="active">笔记</router-link>
      <router-link to="/meetings" :class="{ active: inMeetings }">会议</router-link>
      <router-link to="/hotwords" active-class="active">热词</router-link>
      <router-link to="/pairing" active-class="active">设备配对</router-link>
    </nav>
  </header>
  <main class="container">
    <router-view />
  </main>
</template>

<script setup>
import { computed } from 'vue'
import { useRoute } from 'vue-router'

const route = useRoute()
// meeting details live under /r/:id
const inMeetings = computed(() => route.path.startsWith('/meetings') || route.path.startsWith('/r/'))
</script>
