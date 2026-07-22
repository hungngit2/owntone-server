<template>
  <modal-dialog
    :actions="actions"
    :show="show"
    :title="playable.name"
    @close="$emit('close')"
  >
    <template #content>
      <list-properties :item="playable" />
    </template>
  </modal-dialog>
</template>

<script setup>
import { computed, ref } from 'vue'
import Formatters from '@/lib/Formatters'
import ListProperties from '@/components/ListProperties.vue'
import ModalDialog from '@/components/ModalDialog.vue'
import api from '@/api'
import services from '@/api/services'
import { useI18n } from 'vue-i18n'
import { useNotificationsStore } from '@/stores/notifications'
import { useQueueStore } from '@/stores/queue'

const YOUTUBE_REFERER_HEADER = 'Referer: https://www.youtube.com/'

const props = defineProps({
  item: { required: true, type: Object },
  show: Boolean
})

const emit = defineEmits(['close'])

const { t } = useI18n()
const notifications = useNotificationsStore()
const queueStore = useQueueStore()
const busy = ref(false)

const playable = computed(() => ({
  image: props.item.thumbnail,
  name: props.item.title,
  properties: [
    { key: 'property.album-artist', value: props.item.channel },
    {
      key: 'property.duration',
      value: Formatters.toTimecode((props.item.duration || 0) * 1000)
    }
  ]
}))

const notify = (text, type) => {
  notifications.add({ text, timeout: 4000, type })
}

const resolveStreamUrl = async () => {
  const { stream_url: streamUrl, success } = await services.youtube.resolve(
    props.item.url
  )
  if (!success || !streamUrl) {
    throw new Error('YouTube resolve failed')
  }
  return streamUrl
}

const setMetadata = (itemId) =>
  api.put(`./api/queue/items/${itemId}`, null, {
    params: {
      artist: props.item.channel || '',
      artwork_url: props.item.thumbnail || '',
      title: props.item.title || ''
    }
  })

const addToQueueAt = async (params) => {
  const streamUrl = await resolveStreamUrl()
  const added = await api.post('./api/queue/items/add', null, {
    params: { ...params, headers: YOUTUBE_REFERER_HEADER, uris: streamUrl }
  })
  const newItem = added.items?.[0]
  if (newItem) {
    await setMetadata(newItem.id)
  }
}

const runAction = async (action, onSuccess) => {
  if (busy.value) {
    return
  }
  busy.value = true
  emit('close')
  try {
    await action()
    onSuccess?.()
  } catch {
    notify(t('page.search.youtube.queue-failed'), 'danger')
  } finally {
    // eslint-disable-next-line require-atomic-updates -- No real race: busy is only ever touched from this one guarded call path.
    busy.value = false
  }
}

const notifyQueued = () => notify(t('page.search.youtube.queued', { count: 1 }), 'success')

const addToQueue = () => runAction(() => addToQueueAt({}), notifyQueued)

const addNextToQueue = () =>
  runAction(() => {
    const position = queueStore.current?.position
    const params = {}
    if (Number.isInteger(position)) {
      params.position = position + 1
    }
    return addToQueueAt(params)
  }, notifyQueued)

const play = () =>
  runAction(async () => {
    const streamUrl = await resolveStreamUrl()
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
      await setMetadata(newItem.id)
    }
  })

const actions = [
  { handler: addToQueue, icon: 'playlist-plus', key: 'actions.add' },
  { handler: addNextToQueue, icon: 'playlist-play', key: 'actions.add-next' },
  { handler: play, icon: 'play', key: 'actions.play' }
]
</script>
