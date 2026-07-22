# Configurable HTTP Basic Auth (username/password + LAN-bypass flag)

## Problem

OwnTone's web UI/JSON API already gate every request behind HTTP Basic Auth
(`httpd_request_is_authorized()` in `src/httpd.c`), but:

- The username is hardcoded to `"admin"` — not configurable.
- The password (`admin_password`) only comes from the static `owntone.conf`
  file, requiring a full restart to change.
- Local-network peers already bypass auth entirely via the `trusted_networks`
  conffile option (defaults to `{lan}`), with no way to require auth for LAN
  clients too without hand-editing `owntone.conf` and restarting.

The user wants this manageable from the web UI at `/#/settings/webinterface`:
a configurable username/password, and a toggle to require auth on the local
network as well as the internet (mandatory for internet access is already the
case today and stays that way).

## Scope

- New DB-backed, hot-reloadable settings (via the existing `settings.c`
  mechanism, same pattern as `services.youtube_api_key`): username, password,
  and a `require_auth_lan` boolean.
- `owntone.conf`'s `admin_password` is kept as a first-run fallback only (used
  to seed the initial password if the DB setting is empty) — not removed, not
  the live source of truth going forward.
- `trusted_networks` conffile option and `net_peer_address_is_trusted()` are
  reused unchanged — they still define "what counts as local network."
- New Settings > Web Interface UI section: username field, password field,
  and a "Require login on local network too" toggle.

Out of scope: multi-user accounts, OAuth/session-based auth, changing the
Basic Auth wire protocol itself, changing `trusted_networks`' CIDR/keyword
parsing.

## Design

### Settings category (`src/settings.c`)

Add a new category, e.g. `"webinterface_auth"`:

| key                | type   | default                                    |
|--------------------|--------|---------------------------------------------|
| `auth_username`    | str    | `"admin"`                                    |
| `auth_password`    | str    | `""` (empty = fall back to conffile, see below) |
| `require_auth_lan` | bool   | `false`                                       |

### Auth decision (`src/httpd.c`)

In `httpd_request_is_authorized()`:

1. Resolve effective username: `settings` `auth_username` (always has a
   default, so no conffile fallback needed here).
2. Resolve effective password: `settings` `auth_password` if non-empty, else
   conffile `admin_password` (first-run compatibility — lets existing
   `owntone.conf`-only configs keep working until the user sets a password in
   the UI). If both are empty, behavior is unchanged from today: deny all
   non-trusted requests (misconfiguration guard already in place).
3. Trust check: call the existing `httpd_request_is_trusted()`. If trusted
   AND `require_auth_lan` is false → skip auth (today's behavior). If
   `require_auth_lan` is true → always require auth regardless of trust.
4. Otherwise → `httpd_basic_auth()` with the resolved username/password
   (unchanged mechanics).

No changes to `net_peer_address_is_trusted()`, `trusted_networks` parsing, or
`httpd_basic_auth()` itself.

### API

Reuse the existing generic settings read/write JSON API (the same one that
already backs other Settings-page toggles/fields) — no new bespoke endpoint
needed, just registering the new category/options so they show up through it.

### Validation

Reject enabling `require_auth_lan` (either via the settings API, or with a
guard in `httpd_request_is_authorized()` treated as informational-only) when
no effective password is set — enabling it with no password would lock out
LAN access to the UI with no way back in short of editing the DB/conffile
directly. Surface this as a clear inline validation error in the frontend
before the save request is even made, plus a defensive check in the settings
save handler.

### Frontend

In the Settings > Web Interface page (`PageSettingsWebinterface.vue` or
equivalent — confirm exact file/route during implementation):

- Username text field.
- Password field (masked; save-on-submit, same UX as the existing YouTube API
  key field — not shown in plaintext once saved).
- Toggle: "Require login on local network too" (bound to
  `require_auth_lan`), disabled/blocked with an inline error if no password
  is set yet.

## Testing

- Manual verification on the real host (chainedbox): set username/password
  via the UI, confirm immediate effect (no restart) both for LAN and via
  internet-origin access; toggle `require_auth_lan` on/off and confirm LAN
  behavior changes accordingly; confirm the empty-password guard blocks
  enabling the LAN-required toggle.
- No new automated test suite exists for HTTP auth in this codebase
  currently; rely on manual verification as with the session's other
  live-host changes.
