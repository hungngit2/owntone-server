<template>
  <content-with-search
    :components="components"
    :expanded="true"
    :get-items="getItems"
    :history="history"
    :results="results"
    @search="search"
    @search-library="searchLibrary"
    @search-query="openSearch"
    @search-youtube="search"
    @search-spotify="searchSpotify"
  >
    <template #help>
      <p v-if="searching" class="help has-text-centered" v-text="$t('page.search.youtube.searching')" />
      <p
        v-else-if="hasSearched && !items.length && !searchError"
        class="help has-text-centered"
        v-text="$t('page.search.no-results')"
      />
      <div class="field is-grouped is-grouped-centered mt-4">
        <div class="control">
          <button
            class="button is-small"
            :class="{ 'is-loading': queueing }"
            :disabled="!items.length || queueing"
            @click="queueAll"
            v-text="$t('page.search.youtube.queue-all')"
          />
        </div>
      </div>
      <p v-if="queueMessage" class="help has-text-centered" v-text="queueMessage" />
      <p v-if="searchError" class="help is-danger has-text-centered" v-text="searchError" />
    </template>
  </content-with-search>
</template>

<script setup>
import { computed, ref } from 'vue'
import {
  extractPlaylistId,
  extractVideoId,
  isMixPlaylistId,
  resolvePlaylist,
  resolveVideo,
  search as searchYoutube
} from '@/lib/Youtube'
import ContentWithSearch from '@/templates/ContentWithSearch.vue'
import ListTracksYoutube from '@/components/ListTracksYoutube.vue'
import api from '@/api'
import player from '@/api/player'
import queue from '@/api/queue'
import services from '@/api/services'
import { useI18n } from 'vue-i18n'
import { useRouter } from 'vue-router'
import { useSearchStore } from '@/stores/search'
import { useServicesStore } from '@/stores/services'

const LIMIT = 25

const { t } = useI18n()
const router = useRouter()
const searchStore = useSearchStore()
const servicesStore = useServicesStore()

const items = ref([])
const results = ref(new Map())
const queueMessage = ref('')
const searchError = ref('')
const searching = ref(false)
const queueing = ref(false)
const hasSearched = ref(false)

const components = { track: ListTracksYoutube }

const history = computed(() =>
  searchStore.history.filter((q) => !q.startsWith('query:'))
)

const getItems = (list) => list

const resetFeedback = () => {
  queueMessage.value = ''
  searchError.value = ''
}

const applyResults = (found) => {
  items.value = found || []
  results.value = new Map([['track', items.value]])
}

/* Runs `action` while `guard` is false, ignoring re-entrant calls (a second
   Enter/click while one is already in flight) and always releasing the
   guard afterwards, success or failure. */
const runGuarded = async (guard, action) => {
  if (guard.value) {
    return
  }
  guard.value = true
  try {
    await action()
  } finally {
    // eslint-disable-next-line require-atomic-updates -- No real race: guard is only ever touched from this one guarded call path.
    guard.value = false
  }
}

/* A pasted link is resolved directly (single video, real playlist, or a
   Mix/Radio list) instead of being treated as a text query. */
const dispatchSearch = async (apiKey, query) => {
  const playlistId = extractPlaylistId(query)
  if (playlistId && isMixPlaylistId(playlistId)) {
    const { results: found } = await services.youtube.resolvePlaylist(query)
    return found || []
  }
  if (playlistId) {
    return resolvePlaylist(apiKey, playlistId)
  }
  const videoId = extractVideoId(query)
  if (videoId) {
    return resolveVideo(apiKey, videoId)
  }
  return searchYoutube(apiKey, query, LIMIT)
}

const performSearch = async () => {
  try {
    const found = await dispatchSearch(servicesStore.youtube.api_key, searchStore.query)
    applyResults(found)
    searchStore.add(searchStore.query)
  } catch {
    applyResults([])
    searchError.value = t('page.search.youtube.search-failed')
  } finally {
    hasSearched.value = true
  }
}

const search = () => {
  if (!searchStore.query) {
    return
  }
  if (!servicesStore.youtube.api_key) {
    searchError.value = t('page.search.youtube.api-key-required')
    return
  }
  searchStore.query = searchStore.query.trim()
  resetFeedback()
  runGuarded(searching, performSearch)
}

const buildQueueMessage = (queued, skipped) => {
  let message = t('page.search.youtube.queued', { count: queued?.length ?? 0 })
  if (skipped?.length) {
    message += ` ${t('page.search.youtube.skipped', { count: skipped.length })}`
  }
  return message
}

/* Sets real title/channel/thumbnail on each queued item (a raw CDN url has
   no embedded tags of its own) and matches queued[] to the search results
   that were actually queued (skipped ones excluded, order preserved). */
const applyQueuedMetadata = async (queued, skippedUrls) => {
  const queuedSourceItems = items.value.filter(
    (item) => !skippedUrls.includes(item.url)
  )
  await Promise.all(
    queued.map((queueItem, index) => {
      const source = queuedSourceItems[index]
      if (!source) {
        return null
      }
      return api.put(`./api/queue/items/${queueItem.id}`, null, {
        params: {
          artist: source.channel || '',
          artwork_url: source.thumbnail || '',
          title: source.title || ''
        }
      })
    })
  )
}

const queueAllAndPlay = async () => {
  try {
    await queue.clear()
    const { items: queued, skipped } = await services.youtube.queueAll(
      items.value.map((item) => item.url)
    )
    queueMessage.value = buildQueueMessage(queued, skipped)
    if (queued?.length) {
      await applyQueuedMetadata(queued, skipped || [])
      await player.play()
    }
  } catch {
    searchError.value = t('page.search.youtube.queue-failed')
  }
}

const queueAll = () => {
  resetFeedback()
  return runGuarded(queueing, queueAllAndPlay)
}

const searchLibrary = () => router.push({ name: 'search-library' })
const searchSpotify = () => router.push({ name: 'search-spotify' })

const openSearch = (query) => {
  searchStore.query = query
  search()
}
</script>
