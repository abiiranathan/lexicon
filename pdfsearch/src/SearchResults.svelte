<script lang="ts">
  let { query, results, isLoading, error, onResultClick } = $props();

  function highlightText(text: string) {
    return text
      .replace(/<b>/g, "<mark>")
      .replace(/<\/b>/g, "</mark>")
      .replace(/\n/g, "<br/>")
      .replace(/<\/mark>\s*<mark>/g, " "); // Merge adjacent marks with space
  }
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
    <span>Found {results.count} results for "{query}"</span>
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
        onkeydown={(e) => {
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
          {@html highlightText(result.snippet || "No preview available")}
        </div>
        <div class="result-footer">
          <span>{result.num_pages} page{result.num_pages != 1 ? "s" : ""}</span>
          <div class="rank-badge">
            Relevance: {(result.rank || 0).toFixed(3)}
          </div>
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
    box-shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.2);
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
    background: rgba(245, 158, 11, 0.9);
    color: #1a1a1a;
    padding: 0.125rem 0.25rem;
    border-radius: 0.25rem;
    font-weight: 600;
    box-shadow: 0 0 8px rgba(245, 158, 11, 0.3);
  }

  .result-footer {
    display: flex;
    justify-content: space-between;
    align-items: center;
    font-size: 0.875rem;
    color: var(--text-muted);
  }

  .rank-badge {
    background: rgba(16, 185, 129, 0.2);
    color: var(--success);
    padding: 0.25rem 0.5rem;
    border-radius: 0.5rem;
    font-size: 0.75rem;
    font-weight: 600;
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
