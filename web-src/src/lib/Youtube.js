const SEARCH_URL = 'https://www.googleapis.com/youtube/v3/search'
const VIDEOS_URL = 'https://www.googleapis.com/youtube/v3/videos'
const PLAYLIST_ITEMS_URL = 'https://www.googleapis.com/youtube/v3/playlistItems'
const VIDEOS_BATCH_SIZE = 50
const PLAYLIST_MAX_ITEMS = 50

const VIDEO_ID_PATTERNS = [
  /[?&]v=(?<id>[\w-]{11})/u,
  /youtu\.be\/(?<id>[\w-]{11})/u,
  /youtube\.com\/shorts\/(?<id>[\w-]{11})/u
]

export const extractVideoId = (input) => {
  for (const pattern of VIDEO_ID_PATTERNS) {
    const match = pattern.exec(input || '')
    if (match) {
      return match.groups.id
    }
  }
  return null
}

export const extractPlaylistId = (input) => {
  const match = /[?&]list=(?<id>[\w-]+)/u.exec(input || '')
  if (!match) {
    return null
  }
  return match.groups.id
}

export const isMixPlaylistId = (id) => /^RD/u.test(id || '')

const parseIso8601Duration = (iso) => {
  const match =
    /^PT(?:(?<hours>\d+)H)?(?:(?<minutes>\d+)M)?(?:(?<seconds>\d+)S)?$/u.exec(
      iso || ''
    )
  if (!match) {
    return 0
  }
  const { hours, minutes, seconds } = match.groups
  return (
    parseInt(hours || '0', 10) * 3600 +
    parseInt(minutes || '0', 10) * 60 +
    parseInt(seconds || '0', 10)
  )
}

const fetchJson = async (url) => {
  const response = await fetch(url)
  const data = await response.json()
  if (data.error) {
    throw new Error(data.error.message || 'YouTube API request failed')
  }
  return data
}

/* Batches videos.list calls (the API's cap on comma-separated ids per
   request) -- a single search/playlist page never exceeds
   VIDEOS_BATCH_SIZE, so this is always a single call in practice. */
const fetchDurationsById = async (apiKey, videoIds) => {
  const durationById = {}
  for (let i = 0; i < videoIds.length; i += VIDEOS_BATCH_SIZE) {
    const batch = videoIds.slice(i, i + VIDEOS_BATCH_SIZE)
    const url = `${VIDEOS_URL}?key=${encodeURIComponent(apiKey)}&part=contentDetails&id=${batch.join(',')}`
    // eslint-disable-next-line no-await-in-loop
    const data = await fetchJson(url)
    ;(data.items || []).forEach((item) => {
      durationById[item.id] = parseIso8601Duration(item.contentDetails?.duration)
    })
  }
  return durationById
}

const toResultFromSearchItem = (item, durationById) => {
  const videoId = item.id?.videoId
  const thumbnails = item.snippet?.thumbnails || {}
  return {
    channel: item.snippet?.channelTitle || '',
    duration: durationById[videoId] || 0,
    thumbnail: (thumbnails.medium || thumbnails.default || {}).url || '',
    title: item.snippet?.title || '',
    url: `https://www.youtube.com/watch?v=${videoId}`,
    video_id: videoId
  }
}

const toResultFromVideoItem = (item) => {
  const thumbnails = item.snippet?.thumbnails || {}
  return {
    channel: item.snippet?.channelTitle || '',
    duration: parseIso8601Duration(item.contentDetails?.duration),
    thumbnail: (thumbnails.medium || thumbnails.default || {}).url || '',
    title: item.snippet?.title || '',
    url: `https://www.youtube.com/watch?v=${item.id}`,
    video_id: item.id
  }
}

const toResultFromPlaylistItem = (item, durationById) => {
  const videoId = item.snippet?.resourceId?.videoId
  const thumbnails = item.snippet?.thumbnails || {}
  return {
    channel: item.snippet?.videoOwnerChannelTitle || item.snippet?.channelTitle || '',
    duration: durationById[videoId] || 0,
    thumbnail: (thumbnails.medium || thumbnails.default || {}).url || '',
    title: item.snippet?.title || '',
    url: `https://www.youtube.com/watch?v=${videoId}`,
    video_id: videoId
  }
}

/* Runs entirely in the browser against the official YouTube Data API v3 --
   deliberately not routed through the backend, which added two blocking
   HTTP round trips on an already resource-constrained host. */
export const search = async (apiKey, query, limit) => {
  const searchUrl =
    `${SEARCH_URL}?key=${encodeURIComponent(apiKey)}` +
    `&part=snippet&type=video&maxResults=${limit}` +
    `&q=${encodeURIComponent(query)}`

  const data = await fetchJson(searchUrl)
  const items = (data.items || []).filter((item) => item.id?.videoId)
  const durationById = await fetchDurationsById(
    apiKey,
    items.map((item) => item.id.videoId)
  )

  return items.map((item) => toResultFromSearchItem(item, durationById))
}

// A single pasted video/shorts URL -- resolved directly to one result.
export const resolveVideo = async (apiKey, videoId) => {
  const url = `${VIDEOS_URL}?key=${encodeURIComponent(apiKey)}&part=snippet,contentDetails&id=${videoId}`
  const data = await fetchJson(url)
  const [item] = data.items || []
  if (!item) {
    return []
  }
  return [toResultFromVideoItem(item)]
}

const fetchPlaylistPage = (apiKey, playlistId, pageToken) => {
  let pageParam = ''
  if (pageToken) {
    pageParam = `&pageToken=${pageToken}`
  }
  const url = `${PLAYLIST_ITEMS_URL}?key=${encodeURIComponent(apiKey)}&part=snippet&maxResults=50&playlistId=${encodeURIComponent(playlistId)}${pageParam}`
  return fetchJson(url)
}

const nextPlaylistPageToken = (items, data) => {
  if (items.length >= PLAYLIST_MAX_ITEMS) {
    return null
  }
  return data.nextPageToken || null
}

const fetchAllPlaylistItems = async (apiKey, playlistId) => {
  const items = []
  let pageToken = null
  let hasNextPage = true

  while (hasNextPage) {
    // eslint-disable-next-line no-await-in-loop
    const data = await fetchPlaylistPage(apiKey, playlistId, pageToken)
    items.push(...(data.items || []))
    pageToken = nextPlaylistPageToken(items, data)
    hasNextPage = Boolean(pageToken)
  }

  return items
}

/* A real YouTube playlist (list=... in the URL, not a Mix/Radio -- those
   can't be read through playlistItems, see isMixPlaylistId). Paginates up
   to PLAYLIST_MAX_ITEMS, then batches a single videos.list call for
   durations, same as search(). */
export const resolvePlaylist = async (apiKey, playlistId) => {
  const items = await fetchAllPlaylistItems(apiKey, playlistId)

  const videoIds = items
    .map((item) => item.snippet?.resourceId?.videoId)
    .filter(Boolean)
  const durationById = await fetchDurationsById(apiKey, videoIds)

  return items
    .filter((item) => item.snippet?.resourceId?.videoId)
    .slice(0, PLAYLIST_MAX_ITEMS)
    .map((item) => toResultFromPlaylistItem(item, durationById))
}
