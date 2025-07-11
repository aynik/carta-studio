import { createApp } from 'vue'
import App from './App.vue'
import './style.css'

// PWA registration
if ('serviceWorker' in navigator) {
  import('virtual:pwa-register').then(({ registerSW }) => {
    registerSW({
      immediate: true,
      onNeedRefresh() {
        console.log('New content available, please refresh.')
      },
      onOfflineReady() {
        console.log('App ready to work offline.')
      },
    })
  })
}

createApp(App).mount('#app')
