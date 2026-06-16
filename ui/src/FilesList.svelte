<!-- FilesList.svelte -->
<script lang="ts">
  import PDFLogo from "./PDFLogo.svelte";

  type FilesListProps = {
    files: FileListResult;
    isLoading: boolean;
    error: string | null;
    viewPage: (
      fileId: number,
      pageNum: number,
      numPages: number,
      fileName: string,
    ) => void;
  };

  let { files, isLoading, error, viewPage }: FilesListProps = $props();

  function handleFileClick(file: FileType) {
    viewPage(file.id, 1, file.num_pages, file.name);
  }
</script>

<div class="files-container">
  {#if isLoading}
    <div class="loading-state">
      <div class="loading-spinner"></div>
      <p>Retrieving documents...</p>
    </div>
  {:else if error}
    <div class="error-state">
      <svg
        width="24"
        height="24"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="2"
      >
        <circle cx="12" cy="12" r="10" />
        <line x1="12" y1="8" x2="12" y2="12" />
        <line x1="12" y1="16" x2="12.01" y2="16" />
      </svg>
      <span>Failed to load files: {error}</span>
    </div>
  {:else if files && files.results.length > 0}
    <div class="files-grid">
      {#each files.results as file}
        <button
          class="file-card"
          onclick={() => handleFileClick(file)}
          type="button"
        >
          <div class="logo-wrapper">
            <PDFLogo />
          </div>
          <div class="file-details">
            <div class="file-name" title={file.name}>{file.name}</div>
            <div class="file-meta">
              <span class="badge">
                {file.num_pages}
                {file.num_pages === 1 ? "page" : "pages"}
              </span>
            </div>
          </div>
          <div class="file-action-arrow">
            <svg
              width="18"
              height="18"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
            >
              <path d="M5 12h14M12 5l7 7-7 7" />
            </svg>
          </div>
        </button>
      {/each}
    </div>
  {:else}
    <div class="empty-state">
      <div class="empty-icon-wrapper">
        <svg
          class="empty-icon"
          viewBox="0 0 24 24"
          fill="none"
          stroke="currentColor"
          stroke-width="1.5"
        >
          <path
            d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"
          />
          <polyline points="14 2 14 8 20 8" />
          <line x1="16" y1="13" x2="8" y2="13" />
          <line x1="16" y1="17" x2="8" y2="17" />
          <polyline points="10 9 9 9 8 9" />
        </svg>
      </div>
      <h3 class="empty-title">No matching files found</h3>
      <p class="empty-description">
        Ensure your filter query is correct or import new files.
      </p>
    </div>
  {/if}
</div>

<style>
  .files-container {
    width: 100%;
  }

  .loading-state {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 5rem 1rem;
    gap: 1rem;
    color: var(--text-secondary);
  }

  .loading-spinner {
    width: 2.5rem;
    height: 2.5rem;
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

  .error-state {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 0.75rem;
    padding: 3rem 1rem;
    color: var(--error);
    background: rgba(239, 68, 68, 0.05);
    border: 1px dashed rgba(239, 68, 68, 0.2);
    border-radius: 1rem;
  }

  .files-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(400px, 1fr));
    gap: 1rem;
  }

  .file-card {
    display: flex;
    align-items: center;
    gap: 1.25rem;
    padding: 1.25rem;
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border);
    border-radius: 1rem;
    cursor: pointer;
    text-align: left;
    width: 100%;
    transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1);
    position: relative;
    overflow: hidden;
  }

  .file-card::before {
    content: "";
    position: absolute;
    top: 0;
    left: 0;
    width: 4px;
    height: 100%;
    background: var(--primary);
    transform: scaleY(0);
    transition: transform 0.2s ease;
  }

  .file-card:hover {
    background: rgba(255, 255, 255, 0.04);
    border-color: rgba(79, 70, 229, 0.3);
    transform: translateY(-2px);
    box-shadow: 0 8px 24px -8px rgba(0, 0, 0, 0.5);
  }

  .file-card:hover::before {
    transform: scaleY(1);
  }

  .file-card:focus-visible {
    outline: 2px solid var(--primary);
    outline-offset: 2px;
  }

  .logo-wrapper {
    background: rgba(239, 68, 68, 0.08);
    padding: 0.5rem;
    border-radius: 0.75rem;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .file-details {
    flex: 1;
    min-width: 0;
  }

  .file-name {
    font-size: 0.8rem;
    text-transform: uppercase;
    font-weight: 500;
    color: var(--text-primary);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    margin-bottom: 0.375rem;
  }

  .file-meta {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  .badge {
    font-size: 0.75rem;
    font-weight: 500;
    color: var(--text-secondary);
    background: rgba(255, 255, 255, 0.05);
    padding: 0.125rem 0.5rem;
    border-radius: 0.375rem;
    border: 1px solid var(--border);
  }

  .file-action-arrow {
    color: var(--text-muted);
    opacity: 0;
    transform: translateX(-4px);
    transition: all 0.2s ease;
  }

  .file-card:hover .file-action-arrow {
    opacity: 1;
    transform: translateX(0);
    color: var(--primary);
  }

  .empty-state {
    text-align: center;
    padding: 5rem 1.5rem;
  }

  .empty-icon-wrapper {
    background: rgba(255, 255, 255, 0.02);
    width: 4rem;
    height: 4rem;
    border-radius: 1.25rem;
    display: flex;
    align-items: center;
    justify-content: center;
    margin: 0 auto 1.5rem;
    border: 1px solid var(--border);
  }

  .empty-icon {
    width: 2rem;
    height: 2rem;
    color: var(--text-secondary);
  }

  .empty-title {
    font-size: 1.125rem;
    font-weight: 600;
    color: var(--text-primary);
    margin-bottom: 0.5rem;
  }

  .empty-description {
    font-size: 0.875rem;
    color: var(--text-secondary);
    max-width: 320px;
    margin: 0 auto;
  }

  @media (max-width: 768px) {
    .files-grid {
      grid-template-columns: 1fr;
    }
  }
</style>
