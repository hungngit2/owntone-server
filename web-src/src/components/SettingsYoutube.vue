<template>
  <content-with-heading>
    <template #heading>
      <pane-title :content="{ title: $t('settings.services.youtube.title') }" />
    </template>
    <template #content>
      <div class="notification help" v-text="$t('settings.services.youtube.info')" />
      <div class="field is-grouped mt-5">
        <div class="control is-expanded">
          <input
            v-model="youtubeApiKey"
            class="input"
            type="password"
            :placeholder="$t('settings.services.youtube.api-key')"
          />
        </div>
        <div class="control">
          <button class="button" @click="saveYoutubeApiKey" v-text="$t('actions.save')" />
        </div>
      </div>
      <div v-if="servicesStore.youtube.configured" class="help is-success">
        {{ $t('settings.services.youtube.configured') }}
      </div>
      <div class="field is-grouped mt-5">
        <div class="control is-expanded">
          <input
            v-model="youtubeQuery"
            class="input"
            type="text"
            :placeholder="$t('settings.services.youtube.search-placeholder')"
            @keyup.enter="searchYoutube"
          />
        </div>
        <div class="control">
          <button
            class="button"
            @click="searchYoutube"
            v-text="$t('settings.services.youtube.search')"
          />
        </div>
      </div>
      <div v-if="youtubeError" class="help is-danger" v-text="youtubeError" />
      <div v-if="youtubeQueueMessage" class="help is-success" v-text="youtubeQueueMessage" />
      <div v-if="youtubeResults.length" class="mt-5">
        <div
          v-for="result in youtubeResults"
          :key="result.video_id"
          class="media"
        >
          <figure class="media-left">
            <p class="image is-64x64">
              <img :src="result.thumbnail" :alt="result.title" />
            </p>
          </figure>
          <div class="media-content">
            <div><strong>{{ result.title }}</strong></div>
            <div>{{ result.channel }}</div>
          </div>
        </div>
        <div class="field mt-5">
          <div class="control">
            <button
              class="button"
              :disabled="!youtubeResults.length"
              @click="queueAllYoutube"
              v-text="$t('settings.services.youtube.queue-all')"
            />
          </div>
        </div>
      </div>
    </template>
  </content-with-heading>
</template>

<script setup>
import ContentWithHeading from '@/templates/ContentWithHeading.vue'
import PaneTitle from '@/components/PaneTitle.vue'
import { ref } from 'vue'
import services from '@/api/services'
import { useI18n } from 'vue-i18n'
import { useServicesStore } from '@/stores/services'

const servicesStore = useServicesStore()
const { t } = useI18n()

const youtubeApiKey = ref('')
const youtubeQuery = ref('')
const youtubeError = ref('')
const youtubeResults = ref([])
const youtubeQueueMessage = ref('')

const saveYoutubeApiKey = async () => {
  try {
    await services.youtube.saveApiKey(youtubeApiKey.value)
    await servicesStore.initialise()
  } catch {
    youtubeError.value = t('settings.services.youtube.search-failed')
  }
}

const searchResults = async () => {
  const { results } = await services.youtube.search(youtubeQuery.value, 15)
  youtubeResults.value = results || []
}

const searchResultsOrError = async () => {
  try {
    await searchResults()
  } catch {
    youtubeError.value = t('settings.services.youtube.search-failed')
  }
}

const searchYoutube = async () => {
  youtubeError.value = ''
  youtubeQueueMessage.value = ''
  youtubeResults.value = []
  if (!servicesStore.youtube.configured) {
    youtubeError.value = t('settings.services.youtube.api-key-required')
    return
  }
  if (!youtubeQuery.value.trim()) {
    return
  }
  await searchResultsOrError()
}

const queueMessage = (items, skipped) => {
  const messages = [t('settings.services.youtube.queued', { count: items?.length || 0 })]
  if (skipped?.length) {
    messages.push(t('settings.services.youtube.skipped', { count: skipped.length }))
  }
  return messages.join(' ')
}

const queueResults = async () => {
  const urls = youtubeResults.value.map((result) => result.url)
  const { items, skipped } = await services.youtube.queueAll(urls)
  youtubeQueueMessage.value = queueMessage(items, skipped)
}

const queueAllYoutube = async () => {
  youtubeError.value = ''
  youtubeQueueMessage.value = ''
  if (!youtubeResults.value.length) {
    return
  }

  try {
    await queueResults()
  } catch {
    youtubeError.value = t('settings.services.youtube.search-failed')
  }
}
</script>
