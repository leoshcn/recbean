<template>
  <header class="topbar">
    <router-link to="/" class="brand">录音豆</router-link>
    <nav>
      <router-link to="/" exact-active-class="active">录音</router-link>
      <router-link to="/pairing" active-class="active">设备配对</router-link>
    </nav>
  </header>
  <main class="container">
    <router-view />
  </main>
</template>
