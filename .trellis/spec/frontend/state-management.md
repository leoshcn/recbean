# State Management

There is no global store (no Pinia/Vuex). State lives where it is used:

| State | Where | Example |
|---|---|---|
| Server data for a page | `ref` in the view, reloaded by `load()` | `items`, `rec`, `days` |
| UI flags | `ref` in the view | `editingId`, `draft`, `copied`, `loaded`, `error` |
| Recorder state machine | `Record.vue` `state` ref: `idle → acquiring (→ needSys) → connecting → recording ⇄ reconnecting → stopping → saved \| interrupted` | don't add states without updating mark mapping & spec |
| Theme preference | `localStorage['recbean-theme']` via `theme.js` | values `light`/`dark`, absent = system |
| Route-derived | `useRoute()` + `computed` | `inMeetings` in `App.vue` |

## Loading pattern

Every data view follows `views/RecordingList.vue`:

```js
const items = ref([]); const loaded = ref(false); const error = ref('')
async function load() {
  try { items.value = await api.listRecordings('meeting'); error.value = '' }
  catch (e) { error.value = e.message }
  finally { loaded.value = true }
}
```

- `loaded` gates the empty state (`v-if="loaded && !items.length"`) so it never flashes
  before the first response.
- After a mutation, either `await load()` or patch the local object with the response —
  don't keep a second copy of server data.

## Live refresh (polling)

Live recordings are tracked by polling, not push:

- `onMounted`: `load()` then `setInterval(...)`; `onUnmounted`: `clearInterval(timer)`.
  Keep `let timer = null` at module scope of the script.
- Poll only while something is live (`items.value.some((r) => r.live)`), every 3 s;
  `NoteDays.vue` additionally refreshes every 9 s when idle; `Pairing.vue` polls every 2 s
  (codes expire).
- `RecordingDetail.vue` bumps `audioVersion` when a recording goes from live to finished
  so the `<audio>` reloads the final WAV.

## Persistence rules

`localStorage` only for per-browser preferences, every access wrapped in try/catch with a
safe default (`theme.js`). Anything other users/devices should see belongs on the server.
