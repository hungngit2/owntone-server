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
      <div class="field">
        <div class="control">
          <input
            v-model="authUsername"
            class="input"
            type="text"
            :placeholder="$t('settings.webinterface.auth-username')"
          />
        </div>
      </div>
      <div class="field">
        <div class="control">
          <input
            v-model="authPassword"
            class="input"
            type="password"
            autocomplete="new-password"
            :placeholder="$t('settings.webinterface.auth-password-placeholder')"
          />
        </div>
      </div>
      <div class="field is-grouped mt-5">
        <div class="control">
          <button
            class="button"
            :class="{ 'is-loading': authSaving }"
            @click="saveAuth"
            v-text="$t('actions.save')"
          />
        </div>
      </div>
      <div v-if="authError" class="help is-danger" v-text="authError" />
    </template>
  </content-with-heading>
</template>

<script setup>
import { computed, ref, watch } from 'vue'
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

/* The settings store only populates asynchronously (a websocket handshake
   after page load), so the initial ref() read above can run before it does.
   Fill in the field once the real value arrives, but only while it's still
   untouched -- never clobber whatever the user has already started typing. */
watch(
  () => settingsStore.get('webinterface', 'auth_username')?.value,
  (value) => {
    if (!authUsername.value && value) {
      authUsername.value = value
    }
  }
)

/* Never prefilled with the saved password (same masking convention as
   elsewhere in this app), and left blank on save means "leave the password
   unchanged" rather than "clear it" -- otherwise saving a username edit
   without retyping the password would wipe it, and an empty password
   disables authentication entirely (httpd_request_is_authorized). To
   actually disable auth via this form, clear the username instead: an
   empty username alone already triggers the same "either field empty"
   bypass, with no such ambiguity since it's a visible, prefilled field. */
const authPassword = ref('')
const authSaving = ref(false)
const authError = ref('')

/* Password is only included when non-empty -- see the comment above
   authPassword for why a blank value means "unchanged", not "clear". */
const buildAuthOptions = () => {
  const options = [
    { category: 'webinterface', name: 'auth_username', value: authUsername.value }
  ]
  if (authPassword.value) {
    options.push({
      category: 'webinterface',
      name: 'auth_password',
      value: authPassword.value
    })
  }
  return options
}

const saveAuth = async () => {
  authError.value = ''
  authSaving.value = true
  try {
    const options = buildAuthOptions()
    await Promise.all(options.map((option) => settings.update(option)))
    options.forEach((option) => settingsStore.update(option))
    authPassword.value = ''
  } catch {
    authError.value = t('settings.webinterface.auth-save-failed')
  } finally {
    authSaving.value = false
  }
}
</script>
