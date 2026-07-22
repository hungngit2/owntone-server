<template>
  <list-item
    v-for="item in items"
    :key="item.video_id"
    :image="item.thumbnail ? { url: item.thumbnail } : null"
    :is-playable="!loadingIds.has(item.video_id)"
    :lines="[item.title, item.channel, statusText(item)]"
    @open="playNow(item)"
    @open-details="openDetails(item)"
  />
  <modal-dialog-track-youtube
    :item="selectedItem"
    :show="showDetailsModal"
    @close="showDetailsModal = false"
  />
</template>

<script setup>
import Formatters from '@/lib/Formatters'
import ListItem from '@/components/ListItem.vue'
import ModalDialogTrackYoutube from '@/components/ModalDialogTrackYoutube.vue'
import api from '@/api'
import { ref } from 'vue'
import services from '@/api/services'
import { useI18n } from 'vue-i18n'
import { useNotificationsStore } from '@/stores/notifications'

const YOUTUBE_REFERER_HEADER = 'Referer: https://www.youtube.com/'

defineProps({
  items: { required: true, type: Array }
})

const { t } = useI18n()
const notifications = useNotificationsStore()
const loadingIds = ref(new Set())
const selectedItem = ref({})
const showDetailsModal = ref(false)

const durationText = (item) => {
  if (!item.duration) {
    return ''
  }
  return Formatters.toTimecode(item.duration * 1000)
}

const statusText = (item) => {
  if (loadingIds.value.has(item.video_id)) {
    return t('page.search.youtube.resolving')
  }
  return durationText(item)
}

const notify = (text, type) => {
  notifications.add({ text, timeout: 4000, type })
}

const setQueueItemMetadata = async (itemId, item) => {
  try {
    await api.put(`./api/queue/items/${itemId}`, null, {
      params: {
        artist: item.channel || '',
        artwork_url: item.thumbnail || '',
        title: item.title || ''
      }
    })
  } catch {
    /* Metadata is cosmetic -- a failure here shouldn't surface as a
     * play/queue error, the track is already playing/queued either way. */
  }
}

const withLoading = async (item, action) => {
  if (loadingIds.value.has(item.video_id)) {
    return
  }
  loadingIds.value = new Set(loadingIds.value).add(item.video_id)
  try {
    await action()
  } finally {
    const next = new Set(loadingIds.value)
    next.delete(item.video_id)
    loadingIds.value = next
  }
}

const playNow = (item) =>
  withLoading(item, async () => {
    try {
      const { stream_url: streamUrl, success } =
        await services.youtube.resolve(item.url)
      if (!success || !streamUrl) {
        notify(t('page.search.youtube.play-failed'), 'danger')
        return
      }
      const added = await api.post('./api/queue/items/add', null, {
        params: {
          clear: 'true',
          headers: YOUTUBE_REFERER_HEADER,
          playback: 'start',
          uris: streamUrl
        }
      })
      const newItem = added.items?.[0]
      if (newItem) {
        await setQueueItemMetadata(newItem.id, item)
      }
    } catch {
      notify(t('page.search.youtube.play-failed'), 'danger')
    }
  })

const openDetails = (item) => {
  selectedItem.value = item
  showDetailsModal.value = true
}
</script>
