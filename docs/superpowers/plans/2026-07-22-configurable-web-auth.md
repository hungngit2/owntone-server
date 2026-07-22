# Configurable Web Auth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the web UI's HTTP Basic Auth username/password hot-reloadable from Settings > Web Interface, and add a `require_auth_lan` toggle that forces auth even for trusted/local-network peers (default `false`, preserving today's behavior).

**Architecture:** Add three options (`auth_username`, `auth_password`, `require_auth_lan`) to the existing `settings.c` `"webinterface"` category — this makes them automatically available through the already-generic `/api/settings/webinterface/<option>` GET/PUT/DELETE endpoints, no new backend routes needed. `httpd_request_is_authorized()` in `src/httpd.c` reads these instead of the hardcoded `"admin"` username and conffile-only `admin_password`, falling back to conffile `admin_password` when the DB-backed password is empty (first-run compatibility). The frontend adds a new "Authentication" section to `PageSettingsWebinterface.vue` using the existing `ControlSettingTextField`/`ControlSettingSwitch` components, plus a new `ControlSettingPasswordField` component (masked input) for the password.

**Tech Stack:** C (libconfuse `cfg_getstr`, existing `settings.c`/`settings.h` DB-backed settings, libevent httpd), Vue 3 `<script setup>`, Pinia (`useSettingsStore`), vue-i18n.

## Global Constraints

- No shell/subprocess execution involved in this feature — pure settings + auth-check logic.
- `admin_password` and `trusted_networks` conffile options are kept working exactly as today; `trusted_networks`' parsing/semantics are untouched.
- `require_auth_lan` defaults to `false` (matches today's behavior of `trusted_networks {lan}` bypassing auth).
- Enabling `require_auth_lan` with no effective password configured (DB `auth_password` empty AND conffile `admin_password` unset) must be rejected, both in the backend (defensive) and pre-empted in the frontend (disabled toggle + inline message).
- Follow existing code style: 2-space indent, brace-on-own-line C style in `src/`, Composition API `<script setup>` Vue style already used in `PageSettingsWebinterface.vue`.
- i18n: add real (non-English-copy) translations to all 5 locale files (`en.json`, `de.json`, `fr.json`, `zh-CN.json`, `zh-TW.json`) for every new key, matching the tone of existing entries in each file.

---

### Task 1: Add `webinterface` settings options + auth resolution in httpd.c

**Files:**
- Modify: `src/settings.c:28-41` (`webinterface_options[]`)
- Modify: `src/httpd.c:44` (includes), `src/httpd.c:1375-1407` (`httpd_request_is_authorized`)

**Interfaces:**
- Consumes: existing `settings_category_get(const char *name)` → `struct settings_category *`, `settings_option_get(struct settings_category *, const char *name)` → `struct settings_option *`, `settings_option_getstr(struct settings_option *)` → `char *` (from `src/settings.h`, unchanged).
- Produces: three new settings options readable via `SETTINGS_GETSTR("webinterface", "auth_username")`, `SETTINGS_GETSTR("webinterface", "auth_password")`, `SETTINGS_GETBOOL("webinterface", "require_auth_lan")` (macros already defined in `src/settings.h:62-64`) — later tasks (frontend, Task 2's validation guard) rely on these exact category/option name strings.

- [ ] **Step 1: Add the new options to `webinterface_options[]`**

In `src/settings.c`, change:

```c
static struct settings_option webinterface_options[] =
  {
      { "show_composer_now_playing", SETTINGS_TYPE_BOOL },
      { "show_filepath_now_playing", SETTINGS_TYPE_BOOL },
      { "show_composer_for_genre", SETTINGS_TYPE_STR },
      { "show_menu_item_playlists", SETTINGS_TYPE_BOOL, { true } },
      { "show_menu_item_music", SETTINGS_TYPE_BOOL, { true } },
      { "show_menu_item_podcasts", SETTINGS_TYPE_BOOL, { true } },
      { "show_menu_item_audiobooks", SETTINGS_TYPE_BOOL, { true } },
      { "show_menu_item_radio", SETTINGS_TYPE_BOOL, { false } },
      { "show_menu_item_files", SETTINGS_TYPE_BOOL, { true } },
      { "show_menu_item_search", SETTINGS_TYPE_BOOL, { true } },
      { "recently_added_limit", SETTINGS_TYPE_INT, { 100 } },
  };
```

to:

```c
static struct settings_option webinterface_options[] =
  {
      { "show_composer_now_playing", SETTINGS_TYPE_BOOL },
      { "show_filepath_now_playing", SETTINGS_TYPE_BOOL },
      { "show_composer_for_genre", SETTINGS_TYPE_STR },
      { "show_menu_item_playlists", SETTINGS_TYPE_BOOL, { true } },
      { "show_menu_item_music", SETTINGS_TYPE_BOOL, { true } },
      { "show_menu_item_podcasts", SETTINGS_TYPE_BOOL, { true } },
      { "show_menu_item_audiobooks", SETTINGS_TYPE_BOOL, { true } },
      { "show_menu_item_radio", SETTINGS_TYPE_BOOL, { false } },
      { "show_menu_item_files", SETTINGS_TYPE_BOOL, { true } },
      { "show_menu_item_search", SETTINGS_TYPE_BOOL, { true } },
      { "recently_added_limit", SETTINGS_TYPE_INT, { 100 } },
      { "auth_username", SETTINGS_TYPE_STR },
      { "auth_password", SETTINGS_TYPE_STR },
      { "require_auth_lan", SETTINGS_TYPE_BOOL, { false } },
  };
```

(`auth_username` has no compile-time default because `union settings_default_value.strval` needs a real fallback string; the `"admin"` default is applied at read time in Step 2 instead, since `settings_option_getstr()`'s no-default path returns `NULL`.)

- [ ] **Step 2: Rewrite `httpd_request_is_authorized()` to use the new settings**

In `src/httpd.c`, add the include (near the other feature includes, alongside `conffile.h`):

```c
#include "settings.h"
```

Replace:

```c
bool
httpd_request_is_authorized(struct httpd_request *hreq)
{
  const char *passwd;
  int ret;

  if (httpd_request_is_trusted(hreq))
    return true;

  passwd = cfg_getstr(cfg_getsec(cfg, "general"), "admin_password");
  if (!passwd)
    {
      DPRINTF(E_LOG, L_HTTPD, "Web interface request to '%s' denied: No password set in the config\n", hreq->uri);

      httpd_send_error(hreq, HTTP_FORBIDDEN, "Forbidden");
      return false;
    }

  ret = httpd_basic_auth(hreq, "admin", passwd, PACKAGE " web interface");
  if (ret != 0)
    {
      // httpd_basic_auth has sent a reply (and logged an error, if relevant)
      return false;
    }

  return true;
}
```

with:

```c
static const char *
httpd_auth_username(void)
{
  const char *username;

  username = SETTINGS_GETSTR("webinterface", "auth_username");
  if (username && *username)
    return username;

  return "admin";
}

// Falls back to the static conffile password when no DB-backed password has
// been set yet, so existing owntone.conf-only setups keep working until the
// user sets one via Settings > Web Interface.
static const char *
httpd_auth_password(void)
{
  const char *passwd;

  passwd = SETTINGS_GETSTR("webinterface", "auth_password");
  if (passwd && *passwd)
    return passwd;

  return cfg_getstr(cfg_getsec(cfg, "general"), "admin_password");
}

bool
httpd_request_is_authorized(struct httpd_request *hreq)
{
  const char *passwd;
  int ret;

  if (httpd_request_is_trusted(hreq) && !SETTINGS_GETBOOL("webinterface", "require_auth_lan"))
    return true;

  passwd = httpd_auth_password();
  if (!passwd || !*passwd)
    {
      DPRINTF(E_LOG, L_HTTPD, "Web interface request to '%s' denied: No password set\n", hreq->uri);

      httpd_send_error(hreq, HTTP_FORBIDDEN, "Forbidden");
      return false;
    }

  ret = httpd_basic_auth(hreq, httpd_auth_username(), passwd, PACKAGE " web interface");
  if (ret != 0)
    {
      // httpd_basic_auth has sent a reply (and logged an error, if relevant)
      return false;
    }

  return true;
}
```

- [ ] **Step 3: Compile-check on the real host (macOS dev env lacks Linux-only headers used elsewhere in this codebase)**

Run (over SSH, from the repo checked out on chainedbox, or any Linux box with the project's build deps):
```bash
make -C src owntone 2>&1 | tail -40
```
Expected: no new warnings/errors introduced by this change (pre-existing warnings elsewhere are not this task's concern).

- [ ] **Step 4: Commit**

```bash
git add src/settings.c src/httpd.c
git commit -m "Make web auth username/password/LAN-requirement hot-reloadable"
```

---

### Task 2: Backend guard — reject enabling `require_auth_lan` with no password set

**Files:**
- Modify: `src/httpd_jsonapi.c:1061-1128` (`jsonapi_reply_settings_option_put`)

**Interfaces:**
- Consumes: `settings_category_get("webinterface")`, `settings_option_get(category, "auth_password")`, `settings_option_getstr()` (all from `src/settings.h`, already used elsewhere in this same file, e.g. lines 1746-1753); `cfg_getstr(cfg_getsec(cfg, "general"), "admin_password")` (from `src/conffile.c`, already included via `conffile.h` at the top of this file).
- Produces: `jsonapi_reply_settings_option_put` returns `HTTP_BADREQUEST` (no JSON body needed — matches this function's existing validation-failure style at lines 1080-1082, 1088-1090, 1116-1117) when the guard trips; the frontend (Task 3) treats any non-2xx from `PUT /api/settings/webinterface/require_auth_lan` as a failed save via the existing `ControlSetting.vue` error-icon mechanism.

- [ ] **Step 1: Add the guard**

In `src/httpd_jsonapi.c`, inside `jsonapi_reply_settings_option_put`, change the bool-handling branch from:

```c
  else if (option->type == SETTINGS_TYPE_BOOL && jparse_contains_key(request, "value", json_type_boolean))
    {
      boolval = jparse_bool_from_obj(request, "value");
      ret = settings_option_setbool(option, boolval);
    }
```

to:

```c
  else if (option->type == SETTINGS_TYPE_BOOL && jparse_contains_key(request, "value", json_type_boolean))
    {
      boolval = jparse_bool_from_obj(request, "value");

      if (boolval && strcasecmp(categoryname, "webinterface") == 0 && strcasecmp(optionname, "require_auth_lan") == 0)
	{
	  struct settings_option *password_option = settings_option_get(category, "auth_password");
	  const char *db_password = password_option ? settings_option_getstr(password_option) : NULL;
	  const char *cfg_password = cfg_getstr(cfg_getsec(cfg, "general"), "admin_password");

	  if ((!db_password || !*db_password) && (!cfg_password || !*cfg_password))
	    {
	      DPRINTF(E_LOG, L_WEB, "Refusing to enable 'require_auth_lan' with no password set\n");
	      return HTTP_BADREQUEST;
	    }
	}

      ret = settings_option_setbool(option, boolval);
    }
```

(No `jparse_free(request)` before this `return` — the existing invalid-value branch at line 1116-1117 in this same function returns `HTTP_BADREQUEST` on its early-exit path without freeing `request` either, so this matches the function's established, if imperfect, convention rather than introducing a new one.)

- [ ] **Step 2: Compile-check on the real host**

```bash
make -C src owntone 2>&1 | tail -40
```
Expected: no new warnings/errors.

- [ ] **Step 3: Manual verification (no automated test harness exists for this handler)**

Over SSH against the real host, with no password configured yet:
```bash
curl -s -o /dev/null -w '%{http_code}\n' -X PUT http://127.0.0.1:3689/api/settings/webinterface/require_auth_lan \
  -H 'Content-Type: application/json' -d '{"value": true}'
```
Expected: `400`.

Then set a password and retry:
```bash
curl -s -X PUT http://127.0.0.1:3689/api/settings/webinterface/auth_password \
  -H 'Content-Type: application/json' -d '{"value": "testpass123"}'
curl -s -o /dev/null -w '%{http_code}\n' -X PUT http://127.0.0.1:3689/api/settings/webinterface/require_auth_lan \
  -H 'Content-Type: application/json' -d '{"value": true}'
```
Expected: `204`.

Clean up afterwards (reset both to safe defaults so the real host isn't left mid-test):
```bash
curl -s -o /dev/null -w '%{http_code}\n' -X PUT http://127.0.0.1:3689/api/settings/webinterface/require_auth_lan \
  -H 'Content-Type: application/json' -d '{"value": false}'
curl -s -o /dev/null -w '%{http_code}\n' -X DELETE http://127.0.0.1:3689/api/settings/webinterface/auth_password
```

- [ ] **Step 4: Commit**

```bash
git add src/httpd_jsonapi.c
git commit -m "Reject enabling require_auth_lan when no password is configured"
```

---

### Task 3: Frontend — password field component

**Files:**
- Create: `web-src/src/components/ControlSettingPasswordField.vue`

**Interfaces:**
- Consumes: `ControlSetting.vue`'s `#input="{ label, update }"` slot contract (same as `ControlSettingTextField.vue`, `src/components/ControlSettingTextField.vue:1-20` — `update(event, sanitiseFn)` is called on `@input`, `sanitiseFn` receives `event.target` and returns the value to persist).
- Produces: a `<control-setting-password-field :setting="..." />` component usable exactly like `ControlSettingTextField`, but rendering `type="password"` and never populating the input with the currently-saved value (masked input stays blank on load/save — same UX note as the existing YouTube API key field).

- [ ] **Step 1: Create the component**

```vue
<template>
  <control-setting :placeholder="placeholder" :setting="setting">
    <template #input="{ label, update }">
      <span v-text="label" />
      <input
        class="input"
        type="password"
        autocomplete="new-password"
        :placeholder="placeholder"
        :value="localValue"
        @input="onInput($event, update)"
      />
    </template>
    <template #help>
      <slot name="help" />
    </template>
  </control-setting>
</template>

<script setup>
import { ref } from 'vue'
import ControlSetting from '@/components/ControlSetting.vue'

defineProps({
  placeholder: { default: '', type: String },
  setting: { required: true, type: Object }
})

// Never reflects the saved password back into the input -- only tracks
// what the user is currently typing in this field.
const localValue = ref('')

const sanitise = (target) => target.value

const onInput = (event, update) => {
  localValue.value = event.target.value
  update(event, sanitise)
}
</script>
```

- [ ] **Step 2: Manual check (no unit test harness exists for individual Vue components in this project)**

Run the web dev server and confirm the component renders as a masked input once wired up in Task 4 (this step is effectively folded into Task 4's manual verification since the component has no standalone route).

- [ ] **Step 3: Commit**

```bash
git add web-src/src/components/ControlSettingPasswordField.vue
git commit -m "Add masked password settings field component"
```

---

### Task 4: Frontend — Authentication section in Settings > Web Interface

**Files:**
- Modify: `web-src/src/pages/PageSettingsWebinterface.vue`
- Modify: `web-src/src/i18n/en.json`, `web-src/src/i18n/de.json`, `web-src/src/i18n/fr.json`, `web-src/src/i18n/zh-CN.json`, `web-src/src/i18n/zh-TW.json`

**Interfaces:**
- Consumes: `ControlSettingTextField` (`web-src/src/components/ControlSettingTextField.vue`), `ControlSettingPasswordField` (Task 3), `ControlSettingSwitch` (`web-src/src/components/ControlSettingSwitch.vue`), `useSettingsStore().get(categoryName, optionName)` → `{ category, name, value, ... }` (`web-src/src/stores/settings.js:26-33`, unchanged).
- Produces: nothing consumed by later tasks — this is the final UI task.

- [ ] **Step 1: Add the Authentication section**

In `web-src/src/pages/PageSettingsWebinterface.vue`, add a new `content-with-heading` block (after the existing "Recently added" block, before the closing `</template>` of the main template):

```vue
  <content-with-heading>
    <template #heading>
      <pane-title :content="{ title: $t('settings.webinterface.authentication') }" />
    </template>
    <template #content>
      <div
        class="notification is-size-7"
        v-text="$t('settings.webinterface.authentication-info')"
      />
      <control-setting-text-field
        :placeholder="$t('settings.webinterface.auth-username')"
        :setting="settingsStore.get('webinterface', 'auth_username')"
      />
      <control-setting-password-field
        :placeholder="$t('settings.webinterface.auth-password')"
        :setting="settingsStore.get('webinterface', 'auth_password')"
      />
      <control-setting-switch
        :disabled="!settingsStore.hasAuthPassword"
        :setting="settingsStore.get('webinterface', 'require_auth_lan')"
      >
        <template v-if="!settingsStore.hasAuthPassword" #help>
          <p
            class="help is-warning"
            v-text="$t('settings.webinterface.require-auth-lan-needs-password')"
          />
        </template>
      </control-setting-switch>
    </template>
  </content-with-heading>
```

And add the new import in the `<script setup>` block:

```js
import ControlSettingPasswordField from '@/components/ControlSettingPasswordField.vue'
```

(alphabetically ordered alongside the existing `ControlSetting*` imports, matching this file's existing import ordering.)

- [ ] **Step 2: Add the `hasAuthPassword` getter**

In `web-src/src/stores/settings.js`, add to the `getters` object (alphabetically, near `showComposerForGenre`):

```js
    hasAuthPassword: (state) =>
      Boolean(state.get('webinterface', 'auth_password')?.value),
```

- [ ] **Step 3: Add i18n keys**

In `web-src/src/i18n/en.json`, inside the existing `"webinterface": { ... }` object under `settings` (around line 417), add:

```json
      "auth-password": "Password",
      "auth-username": "Username",
      "authentication": "Authentication",
      "authentication-info": "Required for internet access. Local network access is exempt unless the toggle below is enabled.",
      "require-auth-lan": "Require login on local network too",
      "require-auth-lan-needs-password": "Set a password above before enabling this.",
```

In `web-src/src/i18n/de.json`, `web-src/src/i18n/fr.json`, `web-src/src/i18n/zh-CN.json`, `web-src/src/i18n/zh-TW.json`, add the same 6 keys to each file's `settings.webinterface` object, with real translations matching that file's existing tone (read a few neighboring keys in the same object first, e.g. `navigation-item-selection-info`, to match register/formality):

- `de.json`: `"auth-password": "Passwort"`, `"auth-username": "Benutzername"`, `"authentication": "Authentifizierung"`, `"authentication-info": "Für den Zugriff aus dem Internet erforderlich. Der Zugriff aus dem lokalen Netzwerk ist davon ausgenommen, sofern der folgende Schalter nicht aktiviert ist."`, `"require-auth-lan": "Anmeldung auch im lokalen Netzwerk verlangen"`, `"require-auth-lan-needs-password": "Setzen Sie zunächst oben ein Passwort, bevor Sie dies aktivieren."`
- `fr.json`: `"auth-password": "Mot de passe"`, `"auth-username": "Nom d'utilisateur"`, `"authentication": "Authentification"`, `"authentication-info": "Requis pour l'accès depuis Internet. L'accès depuis le réseau local en est exempté, sauf si l'option ci-dessous est activée."`, `"require-auth-lan": "Exiger la connexion aussi sur le réseau local"`, `"require-auth-lan-needs-password": "Définissez d'abord un mot de passe ci-dessus avant d'activer cette option."`
- `zh-CN.json`: `"auth-password": "密码"`, `"auth-username": "用户名"`, `"authentication": "身份验证"`, `"authentication-info": "互联网访问必须进行身份验证。除非启用下方开关，否则本地网络访问不受此限制。"`, `"require-auth-lan": "本地网络也需要登录"`, `"require-auth-lan-needs-password": "请先在上方设置密码，然后再启用此选项。"`
- `zh-TW.json`: `"auth-password": "密碼"`, `"auth-username": "使用者名稱"`, `"authentication": "驗證"`, `"authentication-info": "透過網際網路存取時必須驗證。除非啟用下方開關，否則區域網路存取不受此限制。"`, `"require-auth-lan": "區域網路也需要登入"`, `"require-auth-lan-needs-password": "請先在上方設定密碼，再啟用此選項。"`

- [ ] **Step 4: Build the frontend**

```bash
cd web-src && npm run build
```
Expected: build succeeds, `htdocs/assets/index.js`/`index.css` regenerated (commit them alongside source, matching this project's established practice).

- [ ] **Step 5: Manual verification**

Run the web dev server (or deploy to chainedbox) and, in a browser:
1. Navigate to `/#/settings/webinterface`, confirm the new "Authentication" section renders with username field (default placeholder shows, empty value initially since `auth_username` has no DB row yet — falls back to `"admin"` only on the *backend* read path, so the field may show blank; this is expected and matches the existing `show_composer_for_genre` text field's blank-until-set behavior), password field (masked), and the LAN toggle (disabled, with the warning help text, since no password is set yet).
2. Set a username and password; confirm the toggle becomes enabled once a password is saved.
3. Enable the toggle; confirm it saves successfully (no error icon).
4. Reload the page from a fresh tab hitting the server directly (not localhost/trusted) or via the real host's WAN-facing path, and confirm Basic Auth is now requested with the new username/password.
5. Reset `require_auth_lan` back to off and clear the test password afterward so the real host isn't left in a locked-down state for the next work session, unless the user wants to keep it enabled.

- [ ] **Step 6: Commit**

```bash
git add web-src/src/pages/PageSettingsWebinterface.vue web-src/src/stores/settings.js \
  web-src/src/i18n/en.json web-src/src/i18n/de.json web-src/src/i18n/fr.json \
  web-src/src/i18n/zh-CN.json web-src/src/i18n/zh-TW.json htdocs/assets/index.js htdocs/assets/index.css
git commit -m "Add Authentication settings section to Web Interface settings page"
```

---

### Task 5: Deploy and confirm on the real host

Not a subagent task — requires the user's real host and their own WAN-vs-LAN access to confirm the internet-exemption/LAN-requirement split actually behaves as designed. After Tasks 1-4 are merged:

- [ ] Push a new release tag, let `release-arm64.yml` build, run `install.sh` on chainedbox (matching this session's established release flow).
- [ ] User sets username/password and toggles `require_auth_lan` from the real Settings page, confirms LAN and WAN access behave as expected without a restart.
