<script lang="ts">
  let { query, results, duration, isLoading, error, onResultClick } = $props();
</script>

{#if isLoading}
  <div class="loading">
    <div class="spinner"></div>
    <span>Searching documents...</span>
  </div>
{:else if error}
  <div class="error-message">
    <strong>Search failed:</strong>
    {error}
  </div>
{:else if !query || query.length < 2}
  <div class="empty-state">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
      <circle cx="11" cy="11" r="8"></circle>
      <path d="m21 21-4.35-4.35"></path>
    </svg>
    <h3>Start your search</h3>
    <p>Enter keywords to search through your PDF documents</p>
  </div>
{:else if results && results.results && results.results.length > 0}
  <div class="search-stats">
    <span>Found {results.count} results in {(duration / 1000).toFixed(0)}s</span
    >
  </div>

  <div class="results-divider">
    <span>Individual Search Results</span>
  </div>

  <div class="search-results">
    {#each results.results as result}
      <!-- svelte-ignore a11y_click_events_have_key_events -->
      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div
        class="result-item"
        onclick={() => onResultClick(result)}
        tabindex="0"
        role="link"
        onkeydown={(e: KeyboardEvent) => {
          if (e.key == "Enter") {
            onResultClick(result);
          }
        }}
      >
        <div class="result-header">
          <div class="result-title">{result.file_name || "Unknown File"}</div>
          <div class="result-meta">
            <span>Page {result.page_num || "N/A"}</span>
          </div>
        </div>
        <div class="result-snippet">
          {@html result.snippet}
        </div>
        <div class="result-footer">
          <span>{result.num_pages} page{result.num_pages != 1 ? "s" : ""}</span>
        </div>
      </div>
    {/each}
  </div>
{:else}
  <div class="empty-state">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
      <circle cx="11" cy="11" r="8"></circle>
      <path d="m21 21-4.35-4.35"></path>
    </svg>
    <h3>No results found</h3>
    <p>Try different keywords or check your spelling</p>
  </div>
{/if}

<style>
  /* Dark theme colors */
  :global(:root) {
    --ai-gradient-start: #1e3a5f;
    --ai-gradient-end: #2d4a6f;
    --ai-border: #3b82f6;
    --ai-accent: #60a5fa;
  }

  .loading {
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 3rem;
    color: var(--text-muted);
  }

  .spinner {
    width: 2rem;
    height: 2rem;
    border: 3px solid var(--border);
    border-top-color: var(--primary);
    border-radius: 50%;
    animation: spin 1s linear infinite;
    margin-right: 1rem;
  }

  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }

  .empty-state {
    text-align: center;
    padding: 3rem;
    color: var(--text-muted);
  }

  .empty-state svg {
    width: 4rem;
    height: 4rem;
    margin: 0 auto 1rem;
    opacity: 0.5;
  }

  .empty-state h3 {
    color: var(--text-secondary);
    margin-bottom: 0.5rem;
  }

  .error-message {
    background: rgba(239, 68, 68, 0.1);
    border: 1px solid rgba(239, 68, 68, 0.3);
    color: var(--error);
    padding: 1rem;
    border-radius: 0.75rem;
    margin: 1rem 0;
  }

  .search-stats {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 1.5rem;
    color: var(--text-muted);
    font-size: 0.875rem;
  }

  .results-divider {
    display: flex;
    align-items: center;
    margin: 2rem 0 1.5rem 0;
    color: var(--text-muted);
    font-size: 0.875rem;
    font-weight: 500;
  }

  .results-divider::before,
  .results-divider::after {
    content: "";
    flex: 1;
    height: 1px;
    background: var(--border);
  }

  .results-divider span {
    padding: 0 1rem;
  }

  .search-results {
    display: grid;
    gap: 1rem;
  }

  .result-item {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 1rem;
    padding: 1.5rem;
    transition: all 0.3s ease;
    cursor: pointer;
  }

  .result-item:hover {
    background: var(--surface-hover);
    border-color: var(--primary);
    transform: translateY(-2px);
    box-shadow: 0 10px 25px -5px rgba(37, 99, 235, 0.15);
  }

  .result-header {
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    margin-bottom: 0.75rem;
    gap: 1rem;
  }

  .result-title {
    font-weight: 600;
    color: var(--text-primary);
    font-size: 1.1rem;
    flex: 1;
  }

  .result-meta {
    display: flex;
    gap: 1rem;
    font-size: 0.875rem;
    color: var(--text-muted);
  }

  .result-snippet {
    color: var(--text-secondary);
    line-height: 1.6;
    margin-bottom: 0.75rem;
  }

  .result-snippet :global(mark) {
    background: rgba(245, 158, 11, 0.3);
    color: #fbbf24;
    padding: 0.125rem 0.25rem;
    border-radius: 0.25rem;
    font-weight: 600;
  }

  .result-footer {
    display: flex;
    justify-content: space-between;
    align-items: center;
    font-size: 0.875rem;
    color: var(--text-muted);
  }

  @media (max-width: 768px) {
    .result-header {
      flex-direction: column;
      align-items: flex-start;
    }

    .result-meta {
      margin-top: 0.5rem;
    }
  }
</style>
