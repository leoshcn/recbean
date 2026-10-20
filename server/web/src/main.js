import { createApp } from 'vue'
import { createRouter, createWebHashHistory } from 'vue-router'
import App from './App.vue'
import NoteDays from './views/NoteDays.vue'
import NoteDay from './views/NoteDay.vue'
import RecordingList from './views/RecordingList.vue'
import RecordingDetail from './views/RecordingDetail.vue'
import Hotwords from './views/Hotwords.vue'
import Pairing from './views/Pairing.vue'
import Record from './views/Record.vue'
import './style.css'

const router = createRouter({
  history: createWebHashHistory(),
  routes: [
    { path: '/', redirect: '/notes' },
    { path: '/notes', component: NoteDays },
    { path: '/notes/:date', component: NoteDay, props: true },
    { path: '/meetings', component: RecordingList },
    { path: '/r/:id', component: RecordingDetail, props: true },
    { path: '/record', component: Record },
    { path: '/hotwords', component: Hotwords },
    { path: '/pairing', component: Pairing },
  ],
})

createApp(App).use(router).mount('#app')
