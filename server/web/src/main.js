import { createApp } from 'vue'
import { createRouter, createWebHashHistory } from 'vue-router'
import App from './App.vue'
import RecordingList from './views/RecordingList.vue'
import RecordingDetail from './views/RecordingDetail.vue'
import Pairing from './views/Pairing.vue'
import './style.css'

const router = createRouter({
  history: createWebHashHistory(),
  routes: [
    { path: '/', component: RecordingList },
    { path: '/r/:id', component: RecordingDetail, props: true },
    { path: '/pairing', component: Pairing },
  ],
})

createApp(App).use(router).mount('#app')
