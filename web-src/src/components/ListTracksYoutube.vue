<template>
  <list-item
    v-for="item in items"
    :key="item.video_id"
    :image="item.thumbnail ? { url: item.thumbnail } : null"
    :is-playable="true"
    :lines="[item.title, item.channel]"
    @open="queueOne(item)"
  />
  <p v-if="feedback" class="help has-text-centered" v-text="feedback" />
</template>

<script setup>
import ListItem from '@/components/ListItem.vue'
import { ref } from 'vue'
import services from '@/api/services'
import { useI18n } from 'vue-i18n'

defineProps({
  items: { required: true, type: Array }
})

const { t } = useI18n()
const feedback = ref('')

const queueOne = async (item) => {
  feedback.value = ''
  try {
    const { items: queued } = await services.youtube.queueAll([item.url])
    if (queued?.length) {
      feedback.value = t('page.search.youtube.queued', { count: queued.length })
    } else {
      feedback.value = t('page.search.youtube.queue-failed')
    }
  } catch {
    feedback.value = t('page.search.youtube.queue-failed')
  }
}
</script>
