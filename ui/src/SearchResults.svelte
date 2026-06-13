<!-- SearchResults.svelte -->
<script lang="ts">
  let { query, results, duration, isLoading, error, onResultClick } = $props();
</script>

{#if isLoading}
  <div class="loading">
    <div class="spinner"></div>
    <span>Searching document database...</span>
  </div>
{:else if error}
  <div class="error-message">
    <svg
      width="18"
      height="18"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      stroke-width="2"
    >
      <circle cx="12" cy="12" r="10" /><line
        x1="12"
        y1="8"
        x2="12"
        y2="12"
      /><line x1="12" y1="16" x2="12.01" y2="16" />
    </svg>
    <strong>Search failed:</strong>
    {error}
  </div>
{:else if !query || query.length < 2}
  <div class="empty-state">
    <svg
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      stroke-width="1.5"
    >
      <circle cx="11" cy="11" r="8"></circle>
      <path d="m21 21-4.35-4.35"></path>
    </svg>
    <h3>Begin your search</h3>
    <p>Submit a keyword phrase above to scan the indexed files.</p>
  </div>
{:else if results && results.results && results.results.length > 0}
  <div class="search-stats">
    <span
      >Returned {results.count} matches ({(duration / 1000).toFixed(2)}s)</span
    >
  </div>

  <div class="results-divider">
    <span>Matches</span>
  </div>

  <div class="search-results">
    {#each results.results as result}
      <button
        class="result-item"
        onclick={() => onResultClick(result)}
        type="button"
      >
        <div class="result-header">
          <span class="result-title">{result.file_name || "Untitled File"}</span
          >
          <span class="result-badge">Page {result.page_num || "N/A"}</span>
        </div>
        <p class="result-snippet">
          {@html result.snippet}
        </p>
        <div class="result-footer">
          <span
            >{result.num_pages}
            {result.num_pages === 1 ? "page" : "pages"} total</span
          >
        </div>
      </button>
    {/each}
  </div>
{:else}
  <div class="empty-state">
    <svg
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      stroke-width="1.5"
    >
      <circle cx="11" cy="11" r="8"></circle>
      <path d="m21 21-4.35-4.35"></path>
    </svg>
    <h3>No matches found</h3>
    <p>Try refining the query or check for typos.</p>
  </div>
{/if}

<style>
  .loading {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 5rem 1rem;
    gap: 1rem;
    color: var(--text-secondary);
  }

  .spinner {
    width: 2rem;
    height: 2rem;
    border: 3px solid var(--border);
    border-top-color: var(--primary);
    border-radius: 50%;
    animation: spin 1s linear infinite;
  }

  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }

  .empty-state {
    text-align: center;
    padding: 5rem 1rem;
    color: var(--text-secondary);
  }

  .empty-state svg {
    width: 3.5rem;
    height: 3.5rem;
    margin: 0 auto 1.25rem;
    opacity: 0.4;
    color: var(--text-secondary);
  }

  .empty-state h3 {
    color: var(--text-primary);
    font-size: 1.125rem;
    margin-bottom: 0.5rem;
  }

  .empty-state p {
    font-size: 0.875rem;
    max-width: 280px;
    margin: 0 auto;
  }

  .error-message {
    background: rgba(239, 68, 68, 0.05);
    border: 1px solid rgba(239, 68, 68, 0.15);
    color: var(--error);
    padding: 1rem 1.25rem;
    border-radius: 0.75rem;
    display: flex;
    align-items: center;
    gap: 0.75rem;
    font-size: 0.875rem;
  }

  .search-stats {
    font-size: 0.8125rem;
    color: var(--text-secondary);
    margin-bottom: 1.5rem;
    font-weight: 500;
  }

  .results-divider {
    display: flex;
    align-items: center;
    margin: 1.5rem 0;
    color: var(--text-muted);
    font-size: 0.75rem;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    font-weight: 600;
  }

  .results-divider::after {
    content: "";
    flex: 1;
    height: 1px;
    background: var(--border);
    margin-left: 1rem;
  }

  .search-results {
    display: flex;
    flex-direction: column;
    gap: 0.875rem;
  }

  .result-item {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border);
    border-radius: 0.75rem;
    padding: 1.25rem;
    cursor: pointer;
    text-align: left;
    width: 100%;
    transition: all 0.2s ease;
  }

  .result-item:hover {
    background: rgba(255, 255, 255, 0.04);
    border-color: rgba(79, 70, 229, 0.3);
    transform: translateY(-1px);
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
  }

  .result-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 1rem;
    margin-bottom: 0.75rem;
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
    font-size: 0.75rem;
    font-weight: 600;
    color: #818cf8;
    background: rgba(129, 140, 248, 0.1);
    padding: 0.125rem 0.5rem;
    border-radius: 0.375rem;
    white-space: nowrap;
  }

  .result-snippet {
    color: var(--text-secondary);
    font-size: 1.2rem;
    line-height: 1.5;
    margin-bottom: 0.75rem;
  }

  .result-snippet :global(mark) {
    background: rgba(245, 158, 11, 0.25);
    color: #f59e0b;
    padding: 0 2px;
    border-radius: 2px;
    font-weight: 500;
  }

  .result-footer {
    font-size: 0.75rem;
    color: var(--text-muted);
  }
</style>
