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

  const BASE_URL = import.meta.env.BASE_URL;

  let currentTab = useLocalStorage("currentTab", "search");
  let searchQuery = useLocalStorage("search-query", "");
  let fileNameFilter = useLocalStorage("file-name-filter", "");

  let searchResults = $state<{ results: SearchResult[] }>({
    results: [],
  });

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
  let duration = $state(0);
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
      searchResults = { results: [] };
      searchError = null;
      return;
    }

    if (searchCache.has(query)) {
      searchResults = searchCache.get(query);
      return;
    }

    isSearching = true;
    searchError = null;

    const start = performance.now();
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
    duration = performance.now() - start;
  }

  async function loadFiles(
    page: number = 1,
    limit: number = 25,
    nameFilter: string = "",
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
    event: Event & { currentTarget: EventTarget & HTMLInputElement },
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
    fileName: string,
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
      <a href="/"
        ><img
          src="{BASE_URL}favicon-96x96.png"
          alt="Logo"
          width="96"
          height="96"
        /></a
      >
      <div class="logo-text">
        <h1><a href="/">Lexicon</a></h1>
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
        {duration}
        error={searchError}
        onResultClick={(result: SearchResult) => {
          viewPage(
            result.file_id,
            result.page_num,
            result.num_pages,
            result.file_name,
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
            onchange={(e: Event) =>
              handleLimitChange(
                Number((e.currentTarget as HTMLSelectElement).value),
              )}
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
