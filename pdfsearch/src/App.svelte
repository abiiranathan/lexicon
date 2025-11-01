<script lang="ts">
  import { onMount } from "svelte";
  import SearchSection from "./SearchSection.svelte";
  import SearchResults from "./SearchResults.svelte";
  import FilesList from "./FilesList.svelte";
  import Modal from "./Modal.svelte";

  import "./app.css";
  import {
    fetchPage,
    getFileByID,
    loadAllFiles,
    searchAPI,
  } from "./lib/httpclient";
  import { useLocalStorage } from "./lib/localstorage.svelte";
  import { SvelteURLSearchParams } from "svelte/reactivity";
  import Logo from "./Logo.svelte";

  let currentTab = useLocalStorage("currentTab", "search");
  let searchQuery = useLocalStorage("search-query", "");
  let fileNameFilter = useLocalStorage("file-name-filter", "");

  let searchResults = $state<SearchResult[]>([]);

  let files = $state<FileListResult>({
    results: [],
    page: 1,
    total_count: 0,
    limit: 25,
    has_next: false,
    has_prev: false,
    total_pages: 0,
  });

  let currentPage = $state(1);
  let pageLimit = $state(25);
  let isSearching = $state(false);
  let isLoadingFiles = $state(false);
  let searchError = $state<string | null>(null);
  let filesError = $state<string | null>(null);
  let modalContent = $state<ModalContentType | null>(null);
  let searchCache = $state(new Map());
  const maxCacheSize = 100;
  let searchTimeout: any;
  let fileFilterTimeout: any;

  // URL state
  let params = new SvelteURLSearchParams(location.search);

  onMount(() => {
    loadFiles(currentPage, pageLimit);
    handleSearch(searchQuery.value);

    let fileIdString = params.get("file");
    let pageString = params.get("page");
    if (fileIdString && pageString) {
      resetModalState(parseInt(fileIdString), parseInt(pageString));
    }
  });

  async function resetModalState(fileId: number, page: number) {
    if (!fileId || !page) return;

    try {
      const file = await getFileByID(fileId);
      viewPage(fileId, page, file.num_pages, file.name);
    } catch (error) {
      modalContent = {
        title: "Error",
        error: (error as Error).message,
        num_pages: 0,
        page: 0,
        file_id: fileId,
        filename: "",
      };
    }
  }

  async function handleSearch(query: string) {
    if (!query || query.length < 2) {
      searchResults = [];
      searchError = null;
      return;
    }

    if (searchCache.has(query)) {
      searchResults = searchCache.get(query);
      return;
    }

    isSearching = true;
    searchError = null;

    try {
      const data = await searchAPI(query);

      // Bounded cache avoid excessive memory consumption.
      if (searchCache.size < maxCacheSize) {
        searchCache.set(query, data);
      }
      searchResults = data;
    } catch (error: unknown) {
      searchError = (error as Error).message;
    } finally {
      isSearching = false;
    }
  }

  async function loadFiles(
    page: number = 1,
    limit: number = 25,
    nameFilter: string = ""
  ) {
    isLoadingFiles = true;
    filesError = null;

    const params: FileSearchParams = {
      page,
      limit,
    };

    if (nameFilter && nameFilter.trim().length > 0) {
      params.name = nameFilter.trim();
    }

    try {
      files = await loadAllFiles(params);
      currentPage = page;
      pageLimit = limit;
    } catch (error: unknown) {
      filesError = (error as Error).message;
    } finally {
      isLoadingFiles = false;
    }
  }

  function handleSearchSubmit() {
    clearTimeout(searchTimeout);
    if (searchQuery.value) {
      handleSearch(searchQuery.value);
    }
  }

  function handleFileNameFilterInput(
    event: Event & { currentTarget: EventTarget & HTMLInputElement }
  ) {
    const filter = event.currentTarget.value;
    fileNameFilter.set(filter);
    clearTimeout(fileFilterTimeout);
    fileFilterTimeout = setTimeout(() => {
      currentPage = 1;
      loadFiles(1, pageLimit, filter);
    }, 300);
  }

  function handlePageChange(page: number) {
    if (page < 1) return;
    loadFiles(page, pageLimit, fileNameFilter.value);
  }

  function handleLimitChange(limit: number) {
    pageLimit = limit;
    currentPage = 1;
    loadFiles(1, limit, fileNameFilter.value);
  }

  async function viewPage(
    fileId: number,
    pageNum: number,
    numPages: number,
    fileName: string
  ) {
    try {
      const blob = await fetchPage(fileId, pageNum);

      modalContent = {
        title: `${fileName}`,
        imageBlob: blob,
        num_pages: numPages,
        page: pageNum,
        file_id: fileId,
        filename: fileName,
      };
    } catch (error: unknown) {
      modalContent = {
        title: "Error",
        error: (error as Error).message,
        num_pages: numPages,
        page: pageNum,
        file_id: fileId,
        filename: fileName,
      };
    }
  }

  function switchTab(tab: string) {
    currentTab.set(tab);
    if (tab === "files" && files.results.length === 0) {
      loadFiles(currentPage, pageLimit);
    }
  }

  function handleModalClose() {
    modalContent = null;
  }

  $effect(() => {
    // Clean up timeouts on component destroy
    return () => {
      clearTimeout(searchTimeout);
      clearTimeout(fileFilterTimeout);
    };
  });
</script>

<div class="container">
  <header class="header">
    <div class="logo-section">
      <Logo />
      <div class="logo-text">
        <h1>DocuVault</h1>
        <p class="tagline">
          Lightning-fast semantic search across your entire document library
        </p>
      </div>
    </div>
  </header>

  <SearchSection
    {searchQuery}
    {currentTab}
    onsubmit={handleSearchSubmit}
    onTabSwitch={switchTab}
  />

  <section class="content-section">
    {#if currentTab.value === "search"}
      <SearchResults
        query={searchQuery.value}
        results={searchResults}
        isLoading={isSearching}
        error={searchError}
        onResultClick={(result: SearchResult) => {
          viewPage(
            result.file_id,
            result.page_num,
            result.num_pages,
            result.file_name
          );
        }}
      />
    {:else}
      <div class="files-controls">
        <input
          type="text"
          class="file-name-filter"
          placeholder="Filter by file name..."
          value={fileNameFilter.value}
          oninput={handleFileNameFilterInput}
        />
        <div class="limit-selector">
          <label for="limit-select">Items per page:</label>
          <select
            id="limit-select"
            value={pageLimit}
            onchange={(e) => handleLimitChange(Number(e.currentTarget.value))}
          >
            <option value={10}>10</option>
            <option value={25}>25</option>
            <option value={50}>50</option>
            <option value={100}>100</option>
          </select>
        </div>
      </div>

      <FilesList
        {files}
        isLoading={isLoadingFiles}
        error={filesError}
        {viewPage}
      />

      {#if files.total_count > 0}
        <div class="pagination">
          <button
            class="pagination-btn"
            disabled={!files.has_prev || isLoadingFiles}
            onclick={() => handlePageChange(currentPage - 1)}
          >
            Previous
          </button>

          <div class="pagination-info">
            <span class="page-numbers">
              Page {files.page} of {Math.ceil(files.total_count / files.limit)}
            </span>
            <span class="total-count">
              ({files.total_count} total files)
            </span>
          </div>

          <button
            class="pagination-btn"
            disabled={!files.has_next || isLoadingFiles}
            onclick={() => handlePageChange(currentPage + 1)}
          >
            Next
          </button>
        </div>
      {/if}
    {/if}
  </section>
</div>

<Modal
  isOpen={modalContent !== null}
  onClose={handleModalClose}
  {modalContent}
  {viewPage}
/>

<style>
  .header {
    margin-bottom: 2.5rem;
  }

  .logo-section {
    display: flex;
    align-items: center;
    gap: 1.25rem;
  }

  .logo-text {
    flex: 1;
  }

  .logo-text h1 {
    margin: 0;
    font-size: 2.25rem;
    font-weight: 700;
    background: linear-gradient(135deg, #60a5fa 0%, #a78bfa 100%);
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
    background-clip: text;
    letter-spacing: -0.02em;
    line-height: 1.2;
  }

  .tagline {
    margin: 0.375rem 0 0 0;
    font-size: 0.9375rem;
    color: var(--text-secondary, #94a3b8);
    font-weight: 500;
    letter-spacing: 0.01em;
  }

  .content-section {
    margin-top: 2rem;
  }

  .files-controls {
    display: flex;
    gap: 1rem;
    align-items: center;
    margin-bottom: 1.5rem;
    flex-wrap: wrap;
  }

  .file-name-filter {
    flex: 1;
    min-width: 200px;
    padding: 0.625rem 1rem;
    background: var(--surface, #1e293b);
    border: 1px solid var(--border, #334155);
    border-radius: 0.5rem;
    color: var(--text-primary, #f1f5f9);
    font-size: 0.9375rem;
    transition: all 0.2s ease;
  }

  .file-name-filter:focus {
    outline: none;
    border-color: var(--primary, #60a5fa);
    box-shadow: 0 0 0 3px rgba(96, 165, 250, 0.1);
  }

  .file-name-filter::placeholder {
    color: var(--text-tertiary, #64748b);
  }

  .limit-selector {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  .limit-selector label {
    font-size: 0.875rem;
    color: var(--text-secondary, #94a3b8);
    white-space: nowrap;
  }

  .limit-selector select {
    padding: 0.5rem 0.75rem;
    background: var(--surface, #1e293b);
    border: 1px solid var(--border, #334155);
    border-radius: 0.375rem;
    color: var(--text-primary, #f1f5f9);
    font-size: 0.875rem;
    cursor: pointer;
    transition: all 0.2s ease;
  }

  .limit-selector select:focus {
    outline: none;
    border-color: var(--primary, #60a5fa);
  }

  .pagination {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
    margin-top: 2rem;
    padding: 1.25rem;
    background: var(--surface, #1e293b);
    border: 1px solid var(--border, #334155);
    border-radius: 0.75rem;
  }

  .pagination-btn {
    padding: 0.625rem 1.25rem;
    background: var(--primary, #60a5fa);
    color: white;
    border: none;
    border-radius: 0.5rem;
    font-size: 0.9375rem;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.2s ease;
    min-width: 100px;
  }

  .pagination-btn:hover:not(:disabled) {
    background: var(--primary-hover, #3b82f6);
    transform: translateY(-1px);
  }

  .pagination-btn:disabled {
    background: var(--surface-elevated, #334155);
    color: var(--text-tertiary, #64748b);
    cursor: not-allowed;
    opacity: 0.5;
  }

  .pagination-info {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 0.25rem;
  }

  .page-numbers {
    font-size: 0.9375rem;
    font-weight: 600;
    color: var(--text-primary, #f1f5f9);
  }

  .total-count {
    font-size: 0.8125rem;
    color: var(--text-secondary, #94a3b8);
  }

  @media (max-width: 640px) {
    .container {
      padding: 1rem;
    }

    .logo-section {
      gap: 1rem;
    }

    .logo-text h1 {
      font-size: 1.75rem;
    }

    .tagline {
      font-size: 0.875rem;
    }

    .header {
      margin-bottom: 2rem;
    }

    .files-controls {
      flex-direction: column;
      align-items: stretch;
    }

    .file-name-filter {
      width: 100%;
      min-width: unset;
    }

    .limit-selector {
      justify-content: space-between;
    }

    .pagination {
      flex-direction: column;
      gap: 1rem;
    }

    .pagination-btn {
      width: 100%;
    }
  }

  @media (max-width: 480px) {
    .logo-section {
      flex-direction: column;
      align-items: center;
    }

    .logo-text h1 {
      font-size: 1.5rem;
    }

    .tagline {
      font-size: 0.8125rem;
    }
  }
</style>
