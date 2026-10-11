<script lang="ts">
  import PDFLogo from "./PDFLogo.svelte";

  type FilesListProps = {
    files: BookListResult;
    filter: string;
    isLoading: boolean;
    error: string | null;
    viewPage: (
      fileId: number,
      pageNum: number,
      numPages: number,
      fileName: string,
    ) => void;
  };

  type FolderNode = {
    name: string;
    children: Map<string, FolderNode>;
    files: FileType[];
    bookCount: number;
  };

  let { files, filter, isLoading, error, viewPage }: FilesListProps = $props();

  let tree = $derived.by(() => {
    const root: FolderNode = {
      name: "",
      children: new Map(),
      files: [],
      bookCount: 0,
    };
    const query = filter.trim().toLocaleLowerCase();

    for (const file of files.results) {
      if (!file.name.toLocaleLowerCase().includes(query)) continue;

      const segments = file.path
        .split(/[\\/]+/)
        .filter((segment) => segment && segment !== "." && segment !== "..");
      segments.pop();
      let folder = root;
      folder.bookCount++;

      for (const name of segments) {
        let child = folder.children.get(name);
        if (!child) {
          child = { name, children: new Map(), files: [], bookCount: 0 };
          folder.children.set(name, child);
        }
        folder = child;
        folder.bookCount++;
      }

      folder.files.push(file);
    }

    return root;
  });

  function handleFileClick(file: FileType) {
    viewPage(file.id, 1, file.num_pages, file.name);
  }
</script>

<div class="files-container">
  {#snippet renderFile(file: FileType)}
    <li class="tree-file">
      <button
        class="tree-file-button"
        onclick={() => handleFileClick(file)}
        type="button"
      >
        <span class="file-icon"><PDFLogo /></span>
        <span class="file-details">
          <span class="file-name" title={file.name}>{file.name}</span>
          <span class="file-meta">
            <span class="badge">
              {file.num_pages}
              {file.num_pages === 1 ? "page" : "pages"}
            </span>
          </span>
        </span>
        <svg
          class="file-action-arrow"
          width="18"
          height="18"
          viewBox="0 0 24 24"
          fill="none"
          stroke="currentColor"
          stroke-width="2"
          aria-hidden="true"
        >
          <path d="M5 12h14M12 5l7 7-7 7" />
        </svg>
      </button>
    </li>
  {/snippet}

  {#snippet renderFolder(folder: FolderNode)}
    <li class="tree-folder">
      <details>
        <summary>
          <svg
            class="folder-icon"
            width="19"
            height="19"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="1.8"
            aria-hidden="true"
          >
            <path
              d="M3 7a2 2 0 0 1 2-2h5l2 2h7a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"
            />
          </svg>
          <span class="folder-name">{folder.name}</span>
          <span class="folder-count">{folder.bookCount}</span>
        </summary>
        <ul class="tree-children">
          {#each [...folder.children.values()].sort((a, b) => a.name.localeCompare(b.name)) as child (child.name)}
            {@render renderFolder(child)}
          {/each}
          {#each [...folder.files].sort((a, b) => a.name.localeCompare(b.name)) as file (file.id)}
            {@render renderFile(file)}
          {/each}
        </ul>
      </details>
    </li>
  {/snippet}

  {#if isLoading}
    <div class="loading-state">
      <div class="loading-spinner"></div>
      <p>Retrieving books...</p>
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
        aria-hidden="true"
      >
        <circle cx="12" cy="12" r="10" />
        <line x1="12" y1="8" x2="12" y2="12" />
        <line x1="12" y1="16" x2="12.01" y2="16" />
      </svg>
      <span>Failed to load books: {error}</span>
    </div>
  {:else if tree.bookCount > 0}
    <div class="tree-view">
      <ul class="tree-list">
        {#each [...tree.children.values()].sort((a, b) => a.name.localeCompare(b.name)) as folder (folder.name)}
          {@render renderFolder(folder)}
        {/each}
        {#each [...tree.files].sort((a, b) => a.name.localeCompare(b.name)) as file (file.id)}
          {@render renderFile(file)}
        {/each}
      </ul>
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
          aria-hidden="true"
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
      <h3 class="empty-title">No matching books found</h3>
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

  .tree-view {
    border: 1px solid var(--border);
    border-radius: 1rem;
    background: rgba(255, 255, 255, 0.02);
    padding: 0.75rem;
  }

  .tree-list,
  .tree-children {
    list-style: none;
  }

  .tree-children {
    margin-left: 1.2rem;
    padding-left: 0.75rem;
    border-left: 1px solid var(--border);
  }

  .tree-folder > details > summary {
    display: flex;
    align-items: center;
    gap: 0.625rem;
    min-height: 2.75rem;
    padding: 0.375rem 0.625rem;
    border-radius: 0.625rem;
    color: var(--text-primary);
    cursor: pointer;
    list-style: none;
  }

  .tree-folder > details > summary::-webkit-details-marker {
    display: none;
  }

  .tree-folder > details > summary::before {
    content: "";
    width: 0;
    height: 0;
    border-top: 0.3rem solid transparent;
    border-bottom: 0.3rem solid transparent;
    border-left: 0.35rem solid var(--text-muted);
    transition: transform 0.15s ease;
  }

  .tree-folder > details[open] > summary::before {
    transform: rotate(90deg);
  }

  .tree-folder > details > summary:hover {
    background: rgba(255, 255, 255, 0.04);
  }

  .folder-icon {
    color: #a5b4fc;
    flex: none;
  }

  .folder-name {
    overflow-wrap: anywhere;
  }

  .folder-count {
    margin-left: auto;
    padding: 0.05rem 0.5rem;
    border: 1px solid var(--border);
    border-radius: 999px;
    color: var(--text-secondary);
    font-size: 0.75rem;
  }

  .tree-file {
    padding: 0.2rem 0;
  }

  .tree-file-button {
    display: flex;
    align-items: center;
    gap: 0.875rem;
    width: 100%;
    padding: 0.75rem;
    border: 1px solid transparent;
    border-radius: 0.75rem;
    background: transparent;
    color: inherit;
    text-align: left;
    cursor: pointer;
    transition: background 0.15s ease, border-color 0.15s ease;
  }

  .tree-file-button:hover {
    background: rgba(255, 255, 255, 0.04);
    border-color: var(--border);
  }

  .tree-file-button:focus-visible {
    outline: 2px solid var(--primary);
    outline-offset: 2px;
  }

  .file-icon {
    display: flex;
    flex: none;
    padding: 0.375rem;
    border-radius: 0.625rem;
    background: rgba(239, 68, 68, 0.08);
  }

  .file-details {
    display: flex;
    flex: 1;
    flex-direction: column;
    gap: 0.25rem;
    min-width: 0;
  }

  .file-name {
    overflow: hidden;
    color: var(--text-primary);
    font-size: 0.875rem;
    font-weight: 500;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .file-meta {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  .badge {
    padding: 0.05rem 0.45rem;
    border: 1px solid var(--border);
    border-radius: 0.375rem;
    background: rgba(255, 255, 255, 0.05);
    color: var(--text-secondary);
    font-size: 0.7rem;
  }

  .file-action-arrow {
    flex: none;
    color: var(--text-muted);
    opacity: 0;
    transform: translateX(-4px);
    transition: all 0.2s ease;
  }

  .tree-file-button:hover .file-action-arrow {
    color: var(--primary);
    opacity: 1;
    transform: translateX(0);
  }

  .empty-state {
    padding: 5rem 1.5rem;
    text-align: center;
  }

  .empty-icon-wrapper {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 4rem;
    height: 4rem;
    margin: 0 auto 1.5rem;
    border: 1px solid var(--border);
    border-radius: 1.25rem;
    background: rgba(255, 255, 255, 0.02);
  }

  .empty-icon {
    width: 2rem;
    height: 2rem;
    color: var(--text-secondary);
  }

  .empty-title {
    margin-bottom: 0.5rem;
    color: var(--text-primary);
    font-size: 1.125rem;
    font-weight: 600;
  }

  .empty-description {
    max-width: 320px;
    margin: 0 auto;
    color: var(--text-secondary);
    font-size: 0.875rem;
  }
</style>
