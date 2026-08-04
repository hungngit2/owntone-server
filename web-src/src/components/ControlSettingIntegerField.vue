<template>
  <control-setting
    :disabled="disabled"
    :placeholder="placeholder"
    :setting="setting"
  >
    <template #input="{ label, update }">
      <span v-text="label" />
      <input
        class="input"
        inputmode="numeric"
        :max="max"
        :min="min"
        :placeholder="placeholder"
        :step="step"
        :value="setting.value"
        @input="update($event, sanitise)"
      />
    </template>
    <template #help>
      <slot name="help" />
    </template>
  </control-setting>
</template>

<script setup>
import ControlSetting from '@/components/ControlSetting.vue'

const props = defineProps({
  disabled: Boolean,
  max: { default: null, type: Number },
  min: { default: 0, type: Number },
  placeholder: { default: '', type: String },
  setting: { required: true, type: Object },
  step: { default: null, type: Number }
})

const sanitise = (target) => {
  let value = parseInt(target.value.replace(/\D+/gu, ''), 10) || 0
  if (props.min !== null) {
    value = Math.max(value, props.min)
  }
  if (props.max !== null) {
    value = Math.min(value, props.max)
  }
  return (target.value = value)
}
</script>
