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
      <div class="field is-grouped is-grouped-centered mt-4">
        <div class="control">
          <button
            class="button is-small"
            :disabled="!items.length"
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
import ContentWithSearch from '@/templates/ContentWithSearch.vue'
import ListTracksYoutube from '@/components/ListTracksYoutube.vue'
import services from '@/api/services'
import { useI18n } from 'vue-i18n'
import { useRouter } from 'vue-router'
import { useSearchStore } from '@/stores/search'

const LIMIT = 25

const { t } = useI18n()
const router = useRouter()
const searchStore = useSearchStore()

const items = ref([])
const results = ref(new Map())
const queueMessage = ref('')
const searchError = ref('')

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

const search = async () => {
  if (!searchStore.query) {
    return
  }
  searchStore.query = searchStore.query.trim()
  resetFeedback()
  try {
    const { results: found } = await services.youtube.search(
      searchStore.query,
      LIMIT
    )
    applyResults(found)
    searchStore.add(searchStore.query)
  } catch {
    applyResults([])
    searchError.value = t('page.search.youtube.search-failed')
  }
}

const buildQueueMessage = (queued, skipped) => {
  let message = t('page.search.youtube.queued', { count: queued?.length ?? 0 })
  if (skipped?.length) {
    message += ` ${t('page.search.youtube.skipped', { count: skipped.length })}`
  }
  return message
}

const queueAll = async () => {
  resetFeedback()
  try {
    const { items: queued, skipped } = await services.youtube.queueAll(
      items.value.map((item) => item.url)
    )
    queueMessage.value = buildQueueMessage(queued, skipped)
  } catch {
    searchError.value = t('page.search.youtube.queue-failed')
  }
}

const searchLibrary = () => router.push({ name: 'search-library' })
const searchSpotify = () => router.push({ name: 'search-spotify' })

const openSearch = (query) => {
  searchStore.query = query
  search()
}
</script>
