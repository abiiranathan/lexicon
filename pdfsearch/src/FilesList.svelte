<script lang="ts">
  import PDFLogo from "./PDFLogo.svelte";

  type FilesListProps = {
    files: FileType[];
    isLoading: boolean;
    error: string | null;
    viewPage: (
      fileId: number,
      pageNum: number,
      numPages: number,
      fileName: string
    ) => void;
  };

  let { files, isLoading, error, viewPage }: FilesListProps = $props();

  let filesToDisplay = $derived(files || []);

  function handleFileClick(file: FileType) {
    viewPage(file.id, 1, file.num_pages, file.name);
  }
</script>

<div class="files-container">
  {#if isLoading}
    <div class="loading-state">Loading files...</div>
  {:else if error}
    <div class="error-state">Failed to load files: {error}</div>
  {:else if files && files.length > 0}
    <div class="files-grid">
      {#each filesToDisplay as file}
        <!-- svelte-ignore a11y_interactive_supports_focus -->
        <!-- svelte-ignore a11y_click_events_have_key_events -->
        <div
          class="file-card"
          onclick={() => handleFileClick(file)}
          role="button"
          tabindex="0"
          onkeyup={(e) => {
            if (e.key === "Enter") {
              handleFileClick(file);
            }
          }}
        >
          <PDFLogo />
          <div class="file-info">
            <div class="file-name">{file.name}</div>
            <div class="file-meta">
              {file.num_pages}
              {file.num_pages === 1 ? "page" : "pages"}
            </div>
          </div>
        </div>
      {/each}
    </div>
  {:else}
    <div class="empty-state">
      <svg
        class="empty-icon"
        viewBox="0 0 64 64"
        fill="none"
        xmlns="http://www.w3.org/2000/svg"
      >
        <rect
          x="16"
          y="12"
          width="32"
          height="40"
          rx="2"
          stroke="currentColor"
          stroke-width="2"
          opacity="0.3"
        />
        <line
          x1="24"
          y1="24"
          x2="40"
          y2="24"
          stroke="currentColor"
          stroke-width="2"
          stroke-linecap="round"
          opacity="0.3"
        />
        <line
          x1="24"
          y1="32"
          x2="40"
          y2="32"
          stroke="currentColor"
          stroke-width="2"
          stroke-linecap="round"
          opacity="0.3"
        />
        <line
          x1="24"
          y1="40"
          x2="34"
          y2="40"
          stroke="currentColor"
          stroke-width="2"
          stroke-linecap="round"
          opacity="0.3"
        />
      </svg>
      <p class="empty-title">No files found</p>
      <p class="empty-description">Upload some PDF files to get started</p>
    </div>
  {/if}
</div>

<style>
  .files-container {
    width: 100%;
  }

  .loading-state,
  .error-state {
    text-align: center;
    padding: 3rem 1rem;
    color: var(--text-secondary);
  }

  .error-state {
    color: var(--error, #ef4444);
  }

  .files-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
    gap: 1rem;
  }

  .file-card {
    display: flex;
    align-items: center;
    gap: 1rem;
    padding: 1rem;
    background: var(--surface, #ffffff);
    border: 1px solid var(--border, #e2e8f0);
    border-radius: 0.75rem;
    cursor: pointer;
    transition: all 0.2s ease;
  }

  .file-card:hover {
    background: var(--surface-hover, #f8fafc);
    border-color: var(--accent, #60a5fa);
    transform: translateY(-2px);
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.08);
  }

  .file-card:focus {
    outline: 2px solid var(--accent, #60a5fa);
    outline-offset: 2px;
  }

  .file-info {
    flex: 1;
    min-width: 0;
  }

  .file-name {
    font-weight: 600;
    color: var(--text-primary);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    margin-bottom: 0.25rem;
  }

  .file-meta {
    font-size: 0.875rem;
    color: var(--text-secondary);
  }

  .empty-state {
    text-align: center;
    padding: 4rem 1rem;
    color: var(--text-secondary);
  }

  .empty-icon {
    width: 80px;
    height: 80px;
    margin: 0 auto 1.5rem;
    color: var(--text-secondary);
  }

  .empty-title {
    font-size: 1.25rem;
    font-weight: 600;
    color: var(--text-primary);
    margin: 0 0 0.5rem 0;
  }

  .empty-description {
    font-size: 0.9375rem;
    color: var(--text-secondary);
    margin: 0;
  }

  @media (max-width: 640px) {
    .files-grid {
      /* grid-template-columns: 1fr; */
      gap: 0.75rem;
    }

    .file-card {
      padding: 0.875rem;
      gap: 0.875rem;
    }

    .file-name {
      font-size: 0.9375rem;
    }

    .file-meta {
      font-size: 0.8125rem;
    }

    .empty-state {
      padding: 3rem 1rem;
    }

    .empty-icon {
      width: 64px;
      height: 64px;
    }

    .empty-title {
      font-size: 1.125rem;
    }

    .empty-description {
      font-size: 0.875rem;
    }
  }
</style>
