<template>
  <control-setting :placeholder="placeholder" :setting="setting">
    <template #input="{ label, update }">
      <span v-text="label" />
      <input
        class="input"
        type="password"
        autocomplete="new-password"
        :placeholder="placeholder"
        :value="localValue"
        @input="onInput($event, update)"
      />
    </template>
    <template #help>
      <slot name="help" />
    </template>
  </control-setting>
</template>

<script setup>
import { ref } from 'vue'
import ControlSetting from '@/components/ControlSetting.vue'

defineProps({
  placeholder: { default: '', type: String },
  setting: { required: true, type: Object }
})

// Never reflects the saved password back into the input -- only tracks
// what the user is currently typing in this field.
const localValue = ref('')

const sanitise = (target) => target.value

const onInput = (event, update) => {
  localValue.value = event.target.value
  update(event, sanitise)
}
</script>
