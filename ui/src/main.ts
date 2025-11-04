import { mount } from 'svelte'
import './app.css'
import App from './App.svelte'

const app = mount(App, {
  target: document.getElementById('app')!,
})

if (navigator.serviceWorker) {
  if (import.meta.env.MODE === "development") {
    navigator.serviceWorker.getRegistration().then((reg) => {
      reg?.unregister();
    })
  } else {
    navigator.serviceWorker.register("/sw.js", { scope: "/" }).then((reg) => {
      console.log("sw registered");
    })
  }
}

export default app
