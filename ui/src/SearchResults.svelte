<!-- SearchResults.svelte -->
<script lang="ts">
  type Props = {
    query: string;
    results: { results: SearchResult[] };
    duration: number;
    isLoading: boolean;
    error: string | null;
    onResultClick: (result: SearchResult) => void;
  };

  let { query, results, duration, isLoading, error, onResultClick }: Props =
    $props();

  // Mirrors the highlightText logic from Modal.svelte so snippets render
  // consistently: server returns <b>…</b> which we map to <mark>…</mark>.
  // The merge step collapses adjacent marks that only differ by a space.
  function highlightText(text: string): string {
    return text
      .replace(/<b>/g, "<mark>")
      .replace(/<\/b>/g, "</mark>")
      .replace(/\n/g, "<br/>")
      .replace(/<\/mark>\s*<mark>/g, " ");
  }

  // `results.count` does not exist on the type — derive from the array length.
  const resultCount = $derived(results?.results?.length ?? 0);
</script>

{#if isLoading}
  <div class="state-wrapper">
    <div class="spinner-ring"></div>
    <span class="state-label">Scanning indexed documents…</span>
  </div>
{:else if error}
  <div class="alert alert-error" role="alert">
    <svg
      width="18"
      height="18"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      stroke-width="2"
      aria-hidden="true"
    >
      <circle cx="12" cy="12" r="10" /><line
        x1="12"
        y1="8"
        x2="12"
        y2="12"
      /><line x1="12" y1="16" x2="12.01" y2="16" />
    </svg>
    <span><strong>Search failed:</strong> {error}</span>
  </div>
{:else if !query || query.length < 2}
  <div class="empty-state">
    <div class="empty-icon-wrap">
      <svg
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="1.5"
        aria-hidden="true"
      >
        <circle cx="11" cy="11" r="8"></circle>
        <path d="m21 21-4.35-4.35"></path>
      </svg>
    </div>
    <h3>Start searching</h3>
    <p>Enter a keyword or phrase to scan your indexed PDF library.</p>
  </div>
{:else if resultCount > 0}
  <div class="results-meta">
    <span class="result-count"
      >{resultCount} {resultCount === 1 ? "match" : "matches"}</span
    >
    <span class="result-duration">{(duration / 1000).toFixed(2)}s</span>
  </div>

  <div class="search-results" role="list">
    {#each results.results as result (result.file_id + "-" + result.page_num)}
      <!-- svelte-ignore a11y_no_interactive_element_to_noninteractive_role -->
      <button
        class="result-item"
        onclick={() => onResultClick(result)}
        type="button"
        role="listitem"
        aria-label="Open {result.file_name}, page {result.page_num}"
      >
        <div class="result-header">
          <span class="result-title">{result.file_name || "Untitled"}</span>
          <span class="result-badge">p. {result.page_num ?? "?"}</span>
        </div>
        <p class="result-snippet">
          {@html highlightText(result.snippet)}
        </p>
        <div class="result-footer">
          <span
            >{result.num_pages}
            {result.num_pages === 1 ? "page" : "pages"}</span
          >
          <svg
            width="14"
            height="14"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="2"
            aria-hidden="true"
          >
            <path d="M5 12h14M12 5l7 7-7 7" />
          </svg>
        </div>
      </button>
    {/each}
  </div>
{:else}
  <div class="empty-state">
    <div class="empty-icon-wrap">
      <svg
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="1.5"
        aria-hidden="true"
      >
        <circle cx="11" cy="11" r="8"></circle>
        <path d="m21 21-4.35-4.35"></path>
      </svg>
    </div>
    <h3>No matches</h3>
    <p>Try different keywords or check for typos.</p>
  </div>
{/if}

<style>
  /* ── Loading ── */
  .state-wrapper {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 5rem 1rem;
    gap: 1rem;
    color: var(--text-secondary);
  }

  .spinner-ring {
    width: 2rem;
    height: 2rem;
    border: 2px solid var(--border);
    border-top-color: var(--primary);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  .state-label {
    font-size: 0.875rem;
  }

  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }

  /* ── Error ── */
  .alert {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    padding: 1rem 1.25rem;
    border-radius: 0.75rem;
    font-size: 0.875rem;
  }

  .alert-error {
    background: rgba(239, 68, 68, 0.06);
    border: 1px solid rgba(239, 68, 68, 0.2);
    color: var(--error);
  }

  /* ── Empty ── */
  .empty-state {
    text-align: center;
    padding: 5rem 1rem;
    color: var(--text-secondary);
  }

  .empty-icon-wrap {
    width: 3.5rem;
    height: 3.5rem;
    margin: 0 auto 1.25rem;
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border);
    border-radius: 1rem;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .empty-icon-wrap svg {
    width: 1.75rem;
    height: 1.75rem;
    opacity: 0.5;
  }

  .empty-state h3 {
    font-size: 1.0625rem;
    font-weight: 600;
    color: var(--text-primary);
    margin-bottom: 0.375rem;
  }

  .empty-state p {
    font-size: 0.875rem;
    max-width: 280px;
    margin: 0 auto;
    line-height: 1.6;
  }

  /* ── Meta bar ── */
  .results-meta {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 1.25rem;
    padding-bottom: 0.875rem;
    border-bottom: 1px solid var(--border);
  }

  .result-count {
    font-size: 0.8125rem;
    font-weight: 600;
    color: var(--text-secondary);
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }

  .result-duration {
    font-size: 0.75rem;
    color: var(--text-muted);
    font-variant-numeric: tabular-nums;
  }

  /* ── Result cards ── */
  .search-results {
    display: flex;
    flex-direction: column;
    gap: 0.625rem;
  }

  .result-item {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border);
    border-radius: 0.875rem;
    padding: 1.125rem 1.25rem;
    cursor: pointer;
    text-align: left;
    width: 100%;
    transition:
      background 0.15s,
      border-color 0.15s,
      transform 0.15s,
      box-shadow 0.15s;
  }

  .result-item:hover {
    background: rgba(255, 255, 255, 0.04);
    border-color: rgba(79, 70, 229, 0.35);
    transform: translateY(-1px);
    box-shadow: 0 6px 16px rgba(0, 0, 0, 0.35);
  }

  .result-item:focus-visible {
    outline: 2px solid var(--primary);
    outline-offset: 2px;
  }

  .result-header {
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    gap: 1rem;
    margin-bottom: 0.625rem;
  }

  .result-title {
    font-weight: 600;
    color: var(--text-primary);
    font-size: 0.9375rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    flex: 1;
  }

  .result-badge {
    flex-shrink: 0;
    font-size: 0.75rem;
    font-weight: 600;
    color: #818cf8;
    background: rgba(129, 140, 248, 0.12);
    border: 1px solid rgba(129, 140, 248, 0.2);
    padding: 0.125rem 0.5rem;
    border-radius: 0.375rem;
    white-space: nowrap;
    font-variant-numeric: tabular-nums;
  }

  .result-snippet {
    color: var(--text-secondary);
    font-size: 0.875rem;
    line-height: 1.6;
    margin-bottom: 0.75rem;
    display: -webkit-box;
    -webkit-line-clamp: 3;
    line-clamp: 3;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }

  .result-snippet :global(mark) {
    background: rgba(245, 158, 11, 0.2);
    color: #f59e0b;
    padding: 0 2px;
    border-radius: 2px;
    font-weight: 500;
  }

  .result-footer {
    display: flex;
    align-items: center;
    justify-content: space-between;
    font-size: 0.75rem;
    color: var(--text-muted);
  }

  .result-footer svg {
    opacity: 0;
    transform: translateX(-4px);
    transition:
      opacity 0.15s,
      transform 0.15s;
    color: var(--primary);
  }

  .result-item:hover .result-footer svg {
    opacity: 1;
    transform: translateX(0);
  }
</style>
