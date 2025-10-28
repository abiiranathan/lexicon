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

  let searchResults = $state<SearchResult[]>([]);
  let files = $state<FileType[]>([]);
  let isSearching = $state(false);
  let isLoadingFiles = $state(false);
  let searchError = $state<string | null>(null);
  let filesError = $state<string | null>(null);
  let modalContent = $state<ModalContentType | null>(null);
  let searchCache = $state(new Map());
  let searchTimeout: any;

  // URL state
  let params = new SvelteURLSearchParams(location.search);

  onMount(() => {
    loadFiles();
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
      searchCache.set(query, data);
      searchResults = data;
    } catch (error: unknown) {
      searchError = (error as Error).message;
    } finally {
      isSearching = false;
    }
  }

  async function loadFiles() {
    isLoadingFiles = true;
    filesError = null;

    try {
      files = await loadAllFiles();
    } catch (error: unknown) {
      filesError = (error as Error).message;
    } finally {
      isLoadingFiles = false;
    }
  }

  function handleSearchInput(
    event: Event & { currentTarget: EventTarget & HTMLInputElement }
  ) {
    const query = event.currentTarget.value.trim();
    searchQuery.set(query);
    clearTimeout(searchTimeout);
    searchTimeout = setTimeout(() => {
      handleSearch(query);
    }, 300);
  }

  function handleSearchSubmit() {
    clearTimeout(searchTimeout);
    if (searchQuery.value) {
      handleSearch(searchQuery.value);
    }
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
    if (tab === "files" && !files) {
      loadFiles();
    }
  }

  function handleModalClose() {
    modalContent = null;
  }
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
    oninput={handleSearchInput}
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
      <FilesList
        {files}
        isLoading={isLoadingFiles}
        error={filesError}
        {viewPage}
      />
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
