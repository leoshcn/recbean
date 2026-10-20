<template>
  <!-- rb-mark / rb-mark-sm geometry from branding/recbean-brand-identity.html:
       ellipse rx40 ry26 rotated -24deg, cut by a sine slit (width 7, or 10.5 at <= 24px). -->
  <svg class="rb-mark" :class="`is-${state}`" viewBox="10 19 80 62" :width="width" :height="size"
       :role="label ? 'img' : null" :aria-label="label || null" :aria-hidden="label ? null : 'true'"
       focusable="false">
    <defs>
      <mask :id="maskId" maskUnits="userSpaceOnUse" x="-20" y="-20" width="140" height="140">
        <rect x="-20" y="-20" width="140" height="140" fill="white" />
        <!-- REC: a longer wave flowing left, one period (80) per 1.6 s -->
        <g v-if="state === 'rec'" class="wave-run">
          <path d="M-30 50 C-20 66 0 66 10 50 C20 34 40 34 50 50 C60 66 80 66 90 50 C100 34 120 34 130 50 C140 66 160 66 170 50 C180 34 200 34 210 50"
                fill="none" stroke="black" :stroke-width="slit" stroke-linecap="round" />
        </g>
        <!-- idle: the standard slit; flat: same command list with the curve pulled straight,
             so browsers that animate `d` morph between them -->
        <path v-else class="seam" :d="state === 'flat' ? FLAT : WAVE" fill="none" stroke="black"
              :stroke-width="slit" stroke-linecap="round" />
      </mask>
    </defs>
    <g transform="rotate(-24 50 50)">
      <ellipse cx="50" cy="50" rx="40" ry="26" fill="currentColor" :mask="`url(#${maskId})`" />
    </g>
  </svg>
</template>

<script setup>
import { computed, useId } from 'vue'

const props = defineProps({
  size: { type: Number, default: 24 }, // rendered height in px
  state: { type: String, default: 'idle' }, // 'idle' | 'rec' | 'flat'
  label: { type: String, default: '' },
})

const WAVE = 'M-30 50 C-20 66 0 66 10 50 C20 34 40 34 50 50 C60 66 80 66 90 50 C100 34 120 34 130 50'
const FLAT = 'M-30 50 C-20 50 0 50 10 50 C20 50 40 50 50 50 C60 50 80 50 90 50 C100 50 120 50 130 50'

const maskId = `rb-cut-${useId()}`
const width = computed(() => Math.round((props.size * 80) / 62))
// small sizes use the thick slit (rb-mark-sm) so it does not close up
const slit = computed(() => (props.size <= 24 ? 10.5 : 7))
</script>

<style scoped>
.rb-mark { display: block; flex: none; color: var(--accent); }
.seam { transition: d .35s ease; }
.wave-run { animation: rb-wave 1.6s linear infinite; }
@keyframes rb-wave { to { transform: translateX(-80px); } }
@media (prefers-reduced-motion: reduce) {
  .wave-run { animation: none; }
  .seam { transition: none; }
}
</style>
