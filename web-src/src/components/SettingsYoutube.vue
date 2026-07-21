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
      <div v-if="youtubeError" class="help is-danger" v-text="youtubeError" />
      <i18n-t
        tag="p"
        class="help"
        keypath="settings.services.youtube.search-hint"
        scope="global"
      >
        <template #link>
          <router-link :to="{ name: 'search-youtube' }">
            {{ $t('settings.services.youtube.search-hint-link') }}
          </router-link>
        </template>
      </i18n-t>
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
const youtubeError = ref('')

const saveYoutubeApiKey = async () => {
  try {
    await services.youtube.saveApiKey(youtubeApiKey.value)
    await servicesStore.initialise()
  } catch {
    youtubeError.value = t('settings.services.youtube.save-failed')
  }
}
</script>
