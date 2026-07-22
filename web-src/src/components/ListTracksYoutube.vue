<template>
  <list-item
    v-for="item in items"
    :key="item.video_id"
    :image="item.thumbnail ? { url: item.thumbnail } : null"
    :is-playable="true"
    :lines="[item.title, item.channel, durationText(item)]"
    @open="playNow(item)"
    @open-details="addToQueue(item)"
  />
  <p v-if="feedback" class="help has-text-centered" v-text="feedback" />
</template>

<script setup>
import Formatters from '@/lib/Formatters'
import ListItem from '@/components/ListItem.vue'
import api from '@/api'
import { ref } from 'vue'
import services from '@/api/services'
import { useI18n } from 'vue-i18n'

const YOUTUBE_REFERER_HEADER = 'Referer: https://www.youtube.com/'

defineProps({
  items: { required: true, type: Array }
})

const { t } = useI18n()
const feedback = ref('')

const durationText = (item) => {
  if (!item.duration) {
    return ''
  }
  return Formatters.toTimecode(item.duration * 1000)
}

const playNow = async (item) => {
  feedback.value = ''
  try {
    const { stream_url: streamUrl, success } = await services.youtube.resolve(
      item.url
    )
    if (!success || !streamUrl) {
      feedback.value = t('page.search.youtube.play-failed')
      return
    }
    await api.post('./api/queue/items/add', null, {
      params: {
        clear: 'true',
        headers: YOUTUBE_REFERER_HEADER,
        playback: 'start',
        uris: streamUrl
      }
    })
  } catch {
    feedback.value = t('page.search.youtube.play-failed')
  }
}

const addToQueue = async (item) => {
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
