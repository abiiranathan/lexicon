<script lang="ts">
  import { onMount, untrack } from "svelte";
  import SearchSection from "./SearchSection.svelte";
  import SearchResults from "./SearchResults.svelte";
  import FilesList from "./FilesList.svelte";
  import Modal from "./Modal.svelte";

  import "./app.css";
  import {
    fetchPage,
    getFileByID,
    loadAllBooks,
    searchAPI,
  } from "./lib/httpclient";
  import { useLocalStorage } from "./lib/localstorage.svelte";
  import { SvelteURLSearchParams } from "svelte/reactivity";

  const BASE_URL = import.meta.env.VITE_BASE_PATH;

  const DEFAULT_RENDER: RenderParams = { format: "jpg", scale: 1.5 };

  let currentTab = useLocalStorage("currentTab", "search");
  let searchQuery = useLocalStorage("search-query", "");
  let fileNameFilter = useLocalStorage("file-name-filter", "");
  // Persist render preferences across sessions.
  let savedFormat = useLocalStorage<RenderFormat>(
    "render-format",
    DEFAULT_RENDER.format,
  );
  let savedScale = useLocalStorage<number>(
    "render-scale",
    DEFAULT_RENDER.scale,
  );

  let renderParams = $state<RenderParams>({
    format: savedFormat.value,
    scale: savedScale.value,
  });

  let searchResults = $state<{ results: SearchResult[] }>({ results: [] });

  let files = $state<BookListResult>({
    results: [],
    total_count: 0,
  });

  let isSearching = $state(false);
  let duration = $state(0);
  let isLoadingFiles = $state(false);
  let searchError = $state<string | null>(null);
  let filesError = $state<string | null>(null);
  let modalContent = $state<ModalContentType | null>(null);
  let searchCache = $state(new Map());
  const maxCacheSize = 100;
  let searchTimeout: any;

  // URL state
  let params = new SvelteURLSearchParams(location.search);

  onMount(() => {
    loadFiles();
    handleSearch(searchQuery.value);

    const fileIdString = params.get("file");
    const pageString = params.get("page");
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
        renderParams: { ...renderParams },
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

  async function loadFiles() {
    isLoadingFiles = true;
    filesError = null;

    try {
      files = await loadAllBooks();
    } catch (error: unknown) {
      filesError = (error as Error).message;
    } finally {
      isLoadingFiles = false;
    }
  }

  function handleSearchSubmit() {
    clearTimeout(searchTimeout);
    if (searchQuery.value) handleSearch(searchQuery.value);
  }

  async function viewPage(
    fileId: number,
    pageNum: number,
    numPages: number,
    fileName: string,
  ) {
    try {
      const blob = await fetchPage(fileId, pageNum, renderParams);
      modalContent = {
        title: fileName,
        imageBlob: blob,
        num_pages: numPages,
        page: pageNum,
        file_id: fileId,
        filename: fileName,
        renderParams: { ...renderParams },
      };
    } catch (error: unknown) {
      modalContent = {
        title: "Error",
        error: (error as Error).message,
        num_pages: numPages,
        page: pageNum,
        file_id: fileId,
        filename: fileName,
        renderParams: { ...renderParams },
      };
    }
  }

  // Persist render preferences whenever they change.
  $effect(() => {
    savedFormat.set(renderParams.format);
    savedScale.set(renderParams.scale);
  });

  // Re-fetch the current page only when renderParams changes,
  // not when modalContent itself changes.
  $effect(() => {
    const { format, scale } = renderParams; // tracked — this is the trigger
    const mc = untrack(() => modalContent); // untracked — read without subscribing

    if (!mc || !mc.filename) return;
    viewPage(mc.file_id, mc.page, mc.num_pages, mc.filename);
  });

  function handleFormatChange(format: RenderFormat) {
    renderParams = { ...renderParams, format };
  }

  function handleScaleChange(scale: number) {
    // Clamp to [1.0, 4.0] with one decimal place.
    renderParams = {
      ...renderParams,
      scale: Math.round(Math.min(4.0, Math.max(1.0, scale)) * 10) / 10,
    };
  }

  function switchTab(tab: string) {
    currentTab.set(tab);
  }

  function handleModalClose() {
    modalContent = null;
  }

  $effect(() => {
    return () => {
      clearTimeout(searchTimeout);
    };
  });
</script>

<div class="container">
  <header class="header">
    <div class="logo-section">
      <a href="/">
        <img src="{BASE_URL}/favicon-96x96.png" alt="Logo" class="logo-img" />
      </a>
      <div class="logo-text">
        <h1><a href="/">Lexicon</a></h1>
        <p class="tagline">PDF Semantic Search Engine</p>
      </div>
    </div>

    <div class="render-controls">
      <label class="render-label" for="render-format">Format</label>
      <div class="render-toggle" role="group" aria-label="Image format">
        <button
          type="button"
          class="toggle-btn"
          class:active={renderParams.format === "png"}
          onclick={() => handleFormatChange("png")}>PNG</button
        >
        <button
          type="button"
          class="toggle-btn"
          class:active={renderParams.format === "jpg"}
          onclick={() => handleFormatChange("jpg")}>JPG</button
        >
      </div>

      <label class="render-label" for="render-scale">
        Scale <span class="scale-value">{renderParams.scale.toFixed(1)}×</span>
      </label>
      <div class="scale-control">
        <button
          type="button"
          class="scale-step"
          disabled={renderParams.scale <= 1.5}
          onclick={() => handleScaleChange(renderParams.scale - 0.5)}
          aria-label="Decrease scale">−</button
        >
        <input
          id="render-scale"
          type="range"
          min="1.0"
          max="4.0"
          step="0.5"
          value={renderParams.scale}
          oninput={(e) =>
            handleScaleChange(
              Number((e.currentTarget as HTMLInputElement).value),
            )}
          aria-label="Render scale"
          class="scale-slider"
        />
        <button
          type="button"
          class="scale-step"
          disabled={renderParams.scale >= 4.0}
          onclick={() => handleScaleChange(renderParams.scale + 0.5)}
          aria-label="Increase scale">+</button
        >
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
          oninput={(e: Event) =>
            fileNameFilter.set((e.currentTarget as HTMLInputElement).value)}
        />
        <span class="library-count">{files.total_count} books</span>
      </div>

      <FilesList
        {files}
        filter={fileNameFilter.value}
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
