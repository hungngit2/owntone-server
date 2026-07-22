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

/* The item is captured as a plain snapshot at the start of each action (not
   read again from props.item after an await) -- the same modal instance
   stays mounted across selections, so props.item can point at a
   different track by the time an earlier action's own awaits resolve. */
const resolveStreamUrl = async (item) => {
  const { stream_url: streamUrl, success } = await services.youtube.resolve(
    item.url
  )
  if (!success || !streamUrl) {
    throw new Error('YouTube resolve failed')
  }
  return streamUrl
}

const setMetadata = (itemId, item) =>
  api.put(`./api/queue/items/${itemId}`, null, {
    params: {
      artist: item.channel || '',
      artwork_url: item.thumbnail || '',
      title: item.title || ''
    }
  })

const addToQueueAt = async (item, params) => {
  const streamUrl = await resolveStreamUrl(item)
  const added = await api.post('./api/queue/items/add', null, {
    params: { ...params, headers: YOUTUBE_REFERER_HEADER, uris: streamUrl }
  })
  const newItem = added.items?.[0]
  if (newItem) {
    await setMetadata(newItem.id, item)
  }
}

/* Deliberately serialized (one yt-dlp resolve in flight at a time) rather
   than letting several clicks run concurrent yt-dlp subprocesses -- this
   runs on a memory-constrained host also running other services, and each
   resolve is a real Python process, not a cheap network call. Buttons show
   as disabled while busy instead of silently dropping the click; for
   genuinely adding several tracks, use "Queue all" instead, which already
   resolves sequentially server-side. */
const runAction = async (item, action, onSuccess) => {
  if (busy.value) {
    return
  }
  busy.value = true
  emit('close')
  try {
    await action(item)
    onSuccess?.()
  } catch {
    notify(t('page.search.youtube.queue-failed'), 'danger')
  } finally {
    // eslint-disable-next-line require-atomic-updates -- No real race: busy is only ever touched from this one guarded call path.
    busy.value = false
  }
}

const notifyQueued = () => notify(t('page.search.youtube.queued', { count: 1 }), 'success')

const addToQueue = () =>
  runAction(props.item, (item) => addToQueueAt(item, {}), notifyQueued)

const addNextToQueue = () =>
  runAction(
    props.item,
    (item) => {
      const position = queueStore.current?.position
      const params = {}
      if (Number.isInteger(position)) {
        params.position = position + 1
      }
      return addToQueueAt(item, params)
    },
    notifyQueued
  )

const play = () =>
  runAction(props.item, async (item) => {
    const streamUrl = await resolveStreamUrl(item)
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
      await setMetadata(newItem.id, item)
    }
  })

const actions = computed(() => [
  {
    disabled: busy.value,
    handler: addToQueue,
    icon: 'playlist-plus',
    key: 'actions.add'
  },
  {
    disabled: busy.value,
    handler: addNextToQueue,
    icon: 'playlist-play',
    key: 'actions.add-next'
  },
  { disabled: busy.value, handler: play, icon: 'play', key: 'actions.play' }
])
</script>
