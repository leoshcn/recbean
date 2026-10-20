# Component Guidelines

## Shape of a view

All components use `<template>` → `<script setup>` → `<style scoped>` in that order
(e.g. `views/RecordingList.vue`). Plain JS, no TypeScript.

- Route params arrive as props (`defineProps({ id: { type: String, required: true } })` in
  `RecordingDetail.vue`, enabled by `props: true` in the route).
- Local state is `ref`/`reactive`/`computed` declared at the top of the script; see
  [state-management.md](./state-management.md).
- Navigation via `useRouter().push(...)`; links via `<router-link>`. Encode ids:
  ``router.push(`/r/${encodeURIComponent(r.id)}`)``.

## Inline editing pattern

Views edit in place with an `editingX` ref + `draft` ref (`NoteDay.vue`, `RecordingDetail.vue`,
`Hotwords.vue`):

- `startEdit(item)` copies the value into `draft` and sets `editingId = item.id`.
- Enter saves (`@keydown.enter.exact.prevent` on textarea, `@keyup.enter` on input), Esc
  cancels (`@keyup.esc="editingId = null"`); show the hint `Enter 保存，Esc 取消`.
- Trim and ignore empty drafts before calling the API; update the local item from the
  response.

## Destructive actions

`if (!confirm('确定删除…无法恢复。')) return` before the API call; describe what is lost.
Buttons that would conflict with a live recording are `:disabled="rec.live"` (server
would answer 409).

## Templates & styling

- Use shared classes from `style.css` (`.card`, `.row`, `.spacer`, `.page-head`, `.badge.<status>`,
  `button.primary/.danger/.link`, `.mono`, `.empty`, `.alert.*`) before writing scoped CSS.
  Token and VI rules: [theming-and-brand.md](./theming-and-brand.md).
- Status text comes from `STATUS_TEXT` in `util.js` with `class="badge" :class="status"`.
- Times/durations always via `fmtDuration` / `fmtTime` / `fmtDay`, rendered in `.mono`.
- Clickable cards: `@click="open(x)"` plus `tabindex="0"` and `@keydown.enter.self="open(x)"`;
  inner buttons use `@click.stop`.
- UI copy is Simplified Chinese; code comments English (Chinese allowed where they quote UI).

## Components

`components/BrandMark.vue` is the only shared component (logo with `idle/rec/flat` states).
Extract a component only when markup is reused across views; don't create wrappers around
single native elements.
