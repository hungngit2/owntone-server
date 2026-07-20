import { SpotifyApi } from '@spotify/web-api-ts-sdk'
import api from '@/api'

export default {
  lastfm: {
    get() {
      return api.get('./api/lastfm')
    },
    login(credentials) {
      return api.post('./api/lastfm-login', credentials)
    },
    logout() {
      return api.get('./api/lastfm-logout')
    }
  },
  listenbrainz: {
    addToken(token) {
      return api.post('./api/listenbrainz/token', { token })
    },
    get() {
      return api.get('./api/listenbrainz')
    },
    removeToken() {
      return api.delete('./api/listenbrainz/token')
    }
  },
  spotify: {
    get: async () => {
      const configuration = await api.get('./api/spotify')
      const sdk = SpotifyApi.withAccessToken(
        configuration.webapi_client_id,
        { access_token: configuration.webapi_token },
        { errorHandler: { handleErrors: () => true } }
      )
      return { api: sdk, configuration }
    },
    logout() {
      return api.get('./api/spotify-logout')
    }
  },
  youtube: {
    get() {
      return api.get('./api/youtube')
    },
    queueAll(urls) {
      return api.post('./api/youtube/queue', { urls })
    },
    resolve(url) {
      return api.post('./api/youtube/resolve', { url })
    },
    saveApiKey(apiKey) {
      return api.put('./api/settings/services/youtube_api_key', { value: apiKey })
    },
    search(query, limit) {
      return api.post('./api/youtube/search', { limit, query })
    }
  }
}
