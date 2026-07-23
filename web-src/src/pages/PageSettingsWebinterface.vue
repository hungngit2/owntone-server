<template>
  <tabs-settings />
  <content-with-heading>
    <template #heading>
      <pane-title :content="{ title: $t('settings.webinterface.language') }" />
    </template>
    <template #content>
      <control-dropdown
        v-model:value="locale"
        :options="settingsStore.locales"
      />
    </template>
  </content-with-heading>
  <content-with-heading>
    <template #heading>
      <pane-title :content="{ title: $t('settings.appearance.title') }" />
    </template>
    <template #content>
      <control-dropdown
        v-model:value="appearance"
        :options="settingsStore.appearances"
      />
    </template>
  </content-with-heading>
  <content-with-heading>
    <template #heading>
      <pane-title
        :content="{ title: $t('settings.webinterface.navigation-items') }"
      />
    </template>
    <template #content>
      <div v-text="$t('settings.webinterface.navigation-item-selection')" />
      <div
        class="notification is-size-7"
        v-text="$t('settings.webinterface.navigation-item-selection-info')"
      />
      <control-setting-switch
        v-for="setting in settingsStore.settings('webinterface', 'show_menu')"
        :key="setting.name"
        :setting="setting"
      />
    </template>
  </content-with-heading>
  <content-with-heading>
    <template #heading>
      <pane-title
        :content="{ title: $t('settings.webinterface.player-page') }"
      />
    </template>
    <template #content>
      <control-setting-switch
        :setting="
          settingsStore.get('webinterface', 'show_filepath_now_playing')
        "
      />
      <control-setting-switch
        :setting="
          settingsStore.get('webinterface', 'show_composer_now_playing')
        "
      />
      <control-setting-text-field
        :disabled="!settingsStore.showComposerNowPlaying"
        :placeholder="$t('settings.webinterface.genres')"
        :setting="settingsStore.get('webinterface', 'show_composer_for_genre')"
      >
        <template #help>
          <i18n-t
            tag="p"
            class="help"
            keypath="settings.webinterface.show-composer-genres-help"
            scope="global"
          >
            <slot><br /></slot>
          </i18n-t>
        </template>
      </control-setting-text-field>
    </template>
  </content-with-heading>
  <content-with-heading>
    <template #heading>
      <pane-title
        :content="{ title: $t('settings.webinterface.recently-added-page') }"
      />
    </template>
    <template #content>
      <control-setting-integer-field
        :setting="settingsStore.get('webinterface', 'recently_added_limit')"
      />
    </template>
  </content-with-heading>
  <content-with-heading>
    <template #heading>
      <pane-title
        :content="{ title: $t('settings.webinterface.authentication') }"
      />
    </template>
    <template #content>
      <div
        class="notification is-size-7"
        v-text="$t('settings.webinterface.authentication-info')"
      />
      <div class="field is-grouped mt-5">
        <div class="control is-expanded">
          <input
            v-model="authUsername"
            class="input"
            type="text"
            :placeholder="$t('settings.webinterface.auth-username')"
          />
        </div>
        <div class="control">
          <button
            class="button"
            :class="{ 'is-loading': authUsernameSaving }"
            @click="saveAuthUsername"
            v-text="$t('actions.save')"
          />
        </div>
      </div>
      <div
        v-if="authUsernameError"
        class="help is-danger"
        v-text="authUsernameError"
      />
      <div class="field is-grouped mt-5">
        <div class="control is-expanded">
          <input
            v-model="authPassword"
            class="input"
            type="password"
            autocomplete="new-password"
            :placeholder="$t('settings.webinterface.auth-password')"
          />
        </div>
        <div class="control">
          <button
            class="button"
            :class="{ 'is-loading': authPasswordSaving }"
            @click="saveAuthPassword"
            v-text="$t('actions.save')"
          />
        </div>
      </div>
      <div
        v-if="authPasswordError"
        class="help is-danger"
        v-text="authPasswordError"
      />
    </template>
  </content-with-heading>
</template>

<script setup>
import { computed, ref } from 'vue'
import ContentWithHeading from '@/templates/ContentWithHeading.vue'
import ControlDropdown from '@/components/ControlDropdown.vue'
import ControlSettingIntegerField from '@/components/ControlSettingIntegerField.vue'
import ControlSettingSwitch from '@/components/ControlSettingSwitch.vue'
import ControlSettingTextField from '@/components/ControlSettingTextField.vue'
import PaneTitle from '@/components/PaneTitle.vue'
import TabsSettings from '@/components/TabsSettings.vue'
import settings from '@/api/settings'
import { useI18n } from 'vue-i18n'
import { useSettingsStore } from '@/stores/settings'

const settingsStore = useSettingsStore()
const { t } = useI18n()

const appearance = computed({
  get: () => settingsStore.currentAppearance(),
  set: (value) => settingsStore.setAppearance(value)
})
const locale = computed({
  get: () => settingsStore.currentLocale(),
  set: (value) => settingsStore.setLocale(value)
})

/* Prefilled with the current value -- the username isn't secret, and saving
   it back unchanged (rather than blank) avoids accidentally clearing it,
   which would disable authentication entirely (either field empty disables
   auth in httpd_request_is_authorized). */
const authUsername = ref(
  settingsStore.get('webinterface', 'auth_username')?.value ?? ''
)
const authUsernameSaving = ref(false)
const authUsernameError = ref('')

/* Never prefilled with the saved password (same masking convention as
   elsewhere in this app) -- saved only when the user explicitly clicks Save,
   not on every keystroke, so a partial/in-progress edit is never persisted. */
const authPassword = ref('')
const authPasswordSaving = ref(false)
const authPasswordError = ref('')

const saveAuthUsername = async () => {
  authUsernameError.value = ''
  authUsernameSaving.value = true
  try {
    const option = {
      category: 'webinterface',
      name: 'auth_username',
      value: authUsername.value
    }
    await settings.update(option)
    settingsStore.update(option)
  } catch {
    authUsernameError.value = t('settings.webinterface.auth-save-failed')
  } finally {
    authUsernameSaving.value = false
  }
}

const saveAuthPassword = async () => {
  authPasswordError.value = ''
  authPasswordSaving.value = true
  try {
    const option = {
      category: 'webinterface',
      name: 'auth_password',
      value: authPassword.value
    }
    await settings.update(option)
    settingsStore.update(option)
    // eslint-disable-next-line require-atomic-updates -- No real race: authPassword is only ever touched from this one guarded save handler.
    authPassword.value = ''
  } catch {
    authPasswordError.value = t('settings.webinterface.auth-save-failed')
  } finally {
    authPasswordSaving.value = false
  }
}
</script>
