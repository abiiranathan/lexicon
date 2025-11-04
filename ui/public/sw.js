// @ts-check
/// <reference lib="webworker" />

/** @type {ServiceWorkerGlobalScope & typeof globalThis} */
const sw = /** @type {any} */ (self);

const version = 'v1';
const STATIC_CACHE = `static-cache-${version}`;
const DYNAMIC_CACHE = `dynamic-cache-${version}`;
const excludeFromCache = ['/search'];

// Extensions that should be cached when fetched
const cacheableExtensions = new Set([
    '.js', '.css', '.png', '.jpg', '.jpeg',
    '.gif', '.svg', '.ico',
]);

const tocache = [
    "/",
    "/favicon.ico",
    "/favicon.svg",
    "/web-app-manifest-512x512.png",
    "/web-app-manifest-192x192.png",
    "/apple-touch-icon.png",
    "/favicon-96x96.png",
    "/site.webmanifest"
];

/**
 * Checks if URL should not be cached.
 * @param {URL} url - The URL to check.
 * @returns {boolean} True if the URL should be excluded from caching.
 */
function shouldExcludeFromCache(url) {
    return excludeFromCache.some((route) => url.pathname.includes(route));
}

/**
 * Checks if URL is a cacheable static asset.
 * @param {URL} url - The URL to check.
 * @returns {boolean} True if the URL is a cacheable static asset.
 */
function isCacheableAsset(url) {
    const pathname = url.pathname.toLowerCase();
    const ext = pathname.substring(pathname.lastIndexOf('.'));
    return cacheableExtensions.has(ext);
}

/**
 * @param {ExtendableEvent} event
 */
sw.addEventListener('install', (event) => {
    event.waitUntil(
        caches
            .open(STATIC_CACHE)
            .then((cache) => cache.addAll(tocache))
            .then(() => sw.skipWaiting())
    );
});

/**
 * @param {ExtendableEvent} event
 */
sw.addEventListener('activate', (event) => {
    event.waitUntil(
        caches.keys().then(async (keys) => {
            for (const key of keys) {
                if (key !== STATIC_CACHE && key !== DYNAMIC_CACHE) {
                    await caches.delete(key);
                }
            }
            await sw.clients.claim();
        })
    );
});

/**
 * @param {FetchEvent} event
 */
sw.addEventListener('fetch', (event) => {
    const { request } = event;
    const url = new URL(request.url);

    // Only handle GET requests over HTTP(S)
    if (request.method !== 'GET' || !url.protocol.startsWith('http') || request.headers.has('range')) {
        return;
    }

    event.respondWith(handleFetch(request, url));
});

/**
 * Handles fetch requests with appropriate caching strategy.
 * @param {Request} request - The fetch request.
 * @param {URL} url - The parsed URL from the request.
 * @returns {Promise<Response>} The response from cache or network.
 */
async function handleFetch(request, url) {
    // Skip caching for excluded routes
    if (shouldExcludeFromCache(url)) {
        try {
            return await fetch(request);
        } catch (err) {
            return new Response('You are currently offline', { status: 503 });
        }
    }

    // Cache-first strategy for static/cacheable assets
    if (isCacheableAsset(url)) {
        const cachedResponse = await caches.match(request);
        if (cachedResponse) {
            return cachedResponse;
        }

        try {
            const networkResponse = await fetch(request);
            if (networkResponse.status === 200) {
                const dynamicCache = await caches.open(DYNAMIC_CACHE);
                dynamicCache.put(request, networkResponse.clone());
            }
            return networkResponse;
        } catch (err) {
            return new Response('You are currently offline', { status: 503 });
        }
    }

    // Network-first strategy for everything else
    try {
        const response = await fetch(request);
        if (!(response instanceof Response)) {
            throw new Error('invalid response from fetch');
        }

        if (response.status === 200) {
            const dynamicCache = await caches.open(DYNAMIC_CACHE);
            dynamicCache.put(request, response.clone());
        }
        return response;
    } catch (err) {
        const cachedResponse = await caches.match(request);
        if (cachedResponse) {
            return cachedResponse;
        }
        return new Response('You are currently offline', { status: 503 });
    }
}
