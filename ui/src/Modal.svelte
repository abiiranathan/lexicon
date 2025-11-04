<script lang="ts">
  import { searchAPI } from "./lib/httpclient";
  import SearchResults from "./SearchResults.svelte";

  type ModalProps = {
    isOpen: boolean;
    onClose: () => void;
    modalContent: ModalContentType | null;
    viewPage: (
      fileId: number,
      pageNum: number,
      numPages: number,
      fileName: string
    ) => void;
  };

  let { isOpen, onClose, modalContent, viewPage }: ModalProps = $props();

  let pageInputValue = $state("");
  let showPageInput = $state(false);

  // Canvas and image state
  let canvasElement = $state<HTMLCanvasElement | null>(null);
  let containerElement = $state<HTMLDivElement | null>(null);
  let imageLoaded = $state(false);
  let loadedImage = $state<HTMLImageElement | null>(null);

  // View mode: 'fit' for zoom/pan, 'scroll' for readable scrolling
  let viewMode = $state<"fit" | "scroll">("scroll");
  let searchQuery = $state("");

  // Pan and zoom state (only used in 'fit' mode)
  let scale = $state(1);
  let translateX = $state(0);
  let translateY = $state(0);
  let isDragging = $state(false);
  let lastTouchDistance = $state(0);
  let dragStartX = $state(0);
  let dragStartY = $state(0);

  let searchResults = $state<{ results: SearchResult[] }>({
    results: [],
  });
  let isSearching = $state(false);
  let searchError = $state<string | null>(null);

  // Sync URL state with modal
  $effect(() => {
    if (isOpen && modalContent) {
      const params = new URLSearchParams(window.location.search);
      params.set("file", modalContent.file_id.toString());
      params.set("page", modalContent.page.toString());
      const newUrl = `${window.location.pathname}?${params.toString()}`;
      window.history.replaceState({}, "", newUrl);
    } else if (!isOpen) {
      const params = new URLSearchParams(window.location.search);
      params.delete("file");
      params.delete("page");
      const newUrl = params.toString()
        ? `${window.location.pathname}?${params.toString()}`
        : window.location.pathname;
      window.history.replaceState({}, "", newUrl);
    }
  });

  $effect(() => {
    document.body.style.overflow = isOpen ? "hidden" : "auto";
  });

  // Load and render image blob to canvas
  $effect(() => {
    if (!modalContent?.imageBlob || !canvasElement || !containerElement) {
      imageLoaded = false;
      loadedImage = null;
      return;
    }

    const canvas = canvasElement;
    const ctx = canvas.getContext("2d", { alpha: false });
    if (!ctx) return;

    // Reset transform state when new image loads
    scale = 1;
    translateX = 0;
    translateY = 0;
    imageLoaded = false;
    loadedImage = null;

    const img = new Image();
    const url = URL.createObjectURL(modalContent.imageBlob);

    img.onload = () => {
      loadedImage = img;
      imageLoaded = true;
      renderCanvas();
      URL.revokeObjectURL(url);
    };

    img.onerror = () => {
      URL.revokeObjectURL(url);
      imageLoaded = false;
      loadedImage = null;
    };

    img.src = url;
  });

  // Re-render when transform state or view mode changes
  $effect(() => {
    if (!imageLoaded || !loadedImage) return;

    // Trigger re-render on state changes
    void scale;
    void translateX;
    void translateY;
    void viewMode;

    renderCanvas();
  });

  // Render image to canvas with current transform
  function renderCanvas() {
    if (!canvasElement || !containerElement || !loadedImage) return;

    const canvas = canvasElement;
    const ctx = canvas.getContext("2d", { alpha: false });
    if (!ctx) return;

    const img = loadedImage;
    const container = containerElement;
    const dpr = window.devicePixelRatio || 1;

    if (viewMode === "scroll") {
      // Scroll mode: render at full width, natural height
      const containerWidth = container.clientWidth;
      const imgAspect = img.width / img.height;

      // Set minimum width to ensure readability.
      const minDisplayWidth = 600; // Adjust this value based on your content
      const displayWidth = Math.max(containerWidth, minDisplayWidth);
      const displayHeight = displayWidth / imgAspect;

      // Set canvas size with device pixel ratio for crisp rendering
      canvas.width = displayWidth * dpr;
      canvas.height = displayHeight * dpr;

      // Set display size
      canvas.style.width = `${displayWidth}px`;
      canvas.style.height = `${displayHeight}px`;

      // Scale context for high DPI
      ctx.scale(dpr, dpr);

      // Clear and draw
      ctx.fillStyle = "#ffffff";
      ctx.fillRect(0, 0, displayWidth, displayHeight);
      ctx.drawImage(img, 0, 0, displayWidth, displayHeight);
    } else {
      // Fit mode: zoom/pan behavior
      const canvasWidth = container.clientWidth;
      const canvasHeight = container.clientHeight;

      // Set canvas size with device pixel ratio
      canvas.width = canvasWidth * dpr;
      canvas.height = canvasHeight * dpr;

      // Set display size
      canvas.style.width = `${canvasWidth}px`;
      canvas.style.height = `${canvasHeight}px`;

      // Scale context for high DPI
      ctx.scale(dpr, dpr);

      // Calculate scaled dimensions to fit image in canvas
      const imgAspect = img.width / img.height;
      const canvasAspect = canvasWidth / canvasHeight;

      let drawWidth, drawHeight;
      if (imgAspect > canvasAspect) {
        drawWidth = canvasWidth;
        drawHeight = canvasWidth / imgAspect;
      } else {
        drawHeight = canvasHeight;
        drawWidth = canvasHeight * imgAspect;
      }

      // Center the image
      const baseX = (canvasWidth - drawWidth) / 2;
      const baseY = (canvasHeight - drawHeight) / 2;

      // Clear canvas
      ctx.fillStyle = "#ffffff";
      ctx.fillRect(0, 0, canvasWidth, canvasHeight);

      // Apply transform
      ctx.save();
      ctx.translate(canvasWidth / 2, canvasHeight / 2);
      ctx.scale(scale, scale);
      ctx.translate(-canvasWidth / 2, -canvasHeight / 2);
      ctx.translate(translateX, translateY);

      ctx.drawImage(img, baseX, baseY, drawWidth, drawHeight);
      ctx.restore();
    }
  }

  // Handle resize
  $effect(() => {
    if (!isOpen) return;

    const handleResize = () => {
      renderCanvas();
    };

    window.addEventListener("resize", handleResize);

    return () => {
      window.removeEventListener("resize", handleResize);
    };
  });

  // Handle mouse wheel zoom (only in fit mode)
  function handleWheel(e: WheelEvent) {
    if (viewMode !== "fit") return;

    e.preventDefault();

    const delta = -e.deltaY * 0.001;
    const newScale = Math.max(0.5, Math.min(5, scale + delta));

    scale = newScale;
  }

  // Handle mouse drag (only in fit mode)
  function handleMouseDown(e: MouseEvent) {
    if (viewMode !== "fit") return;

    isDragging = true;
    dragStartX = e.clientX - translateX;
    dragStartY = e.clientY - translateY;
  }

  function handleMouseMove(e: MouseEvent) {
    if (viewMode !== "fit" || !isDragging) return;

    translateX = e.clientX - dragStartX;
    translateY = e.clientY - dragStartY;
  }

  function handleMouseUp() {
    isDragging = false;
  }

  // Handle touch gestures (only in fit mode)
  function handleTouchStart(e: TouchEvent) {
    if (viewMode !== "fit") return;

    if (e.touches.length === 2) {
      e.preventDefault();
      const touch1 = e.touches[0];
      const touch2 = e.touches[1];
      lastTouchDistance = Math.hypot(
        touch2.clientX - touch1.clientX,
        touch2.clientY - touch1.clientY
      );
    } else if (e.touches.length === 1) {
      isDragging = true;
      dragStartX = e.touches[0].clientX - translateX;
      dragStartY = e.touches[0].clientY - translateY;
    }
  }

  function handleTouchMove(e: TouchEvent) {
    if (viewMode !== "fit") return;

    if (e.touches.length === 2) {
      e.preventDefault();
      const touch1 = e.touches[0];
      const touch2 = e.touches[1];
      const distance = Math.hypot(
        touch2.clientX - touch1.clientX,
        touch2.clientY - touch1.clientY
      );

      if (lastTouchDistance > 0) {
        const delta = (distance - lastTouchDistance) * 0.01;
        const newScale = Math.max(0.5, Math.min(5, scale + delta));
        scale = newScale;
      }

      lastTouchDistance = distance;
    } else if (e.touches.length === 1 && isDragging) {
      e.preventDefault();
      translateX = e.touches[0].clientX - dragStartX;
      translateY = e.touches[0].clientY - dragStartY;
    }
  }

  function handleTouchEnd(e: TouchEvent) {
    if (e.touches.length < 2) {
      lastTouchDistance = 0;
    }
    if (e.touches.length === 0) {
      isDragging = false;
    }
  }

  // Reset zoom and pan
  function resetTransform() {
    scale = 1;
    translateX = 0;
    translateY = 0;
  }

  // Toggle view mode
  function toggleViewMode() {
    viewMode = viewMode === "fit" ? "scroll" : "fit";
    if (viewMode === "fit") {
      resetTransform();
    }
  }

  // Handle keyboard shortcuts at document level
  $effect(() => {
    if (!isOpen) return;

    const handleKeydown = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement;
      if (
        target.tagName === "INPUT" &&
        !target.classList.contains("page-jump-input")
      ) {
        return;
      }

      if (e.key === "Escape") {
        if (showPageInput) {
          showPageInput = false;
          pageInputValue = "";
        } else {
          onClose();
        }
      } else if (e.key === "ArrowLeft") {
        e.preventDefault();
        viewPrevPage();
      } else if (e.key === "ArrowRight") {
        e.preventDefault();
        viewNextPage();
      } else if (e.key === "g" && !showPageInput) {
        e.preventDefault();
        togglePageInput();
      } else if (e.key === "r") {
        e.preventDefault();
        resetTransform();
      } else if (e.key === "v") {
        e.preventDefault();
        toggleViewMode();
      } else if (viewMode === "fit") {
        if (e.key === "+" || e.key === "=") {
          e.preventDefault();
          scale = Math.min(5, scale + 0.2);
        } else if (e.key === "-") {
          e.preventDefault();
          scale = Math.max(0.5, scale - 0.2);
        }
      }
    };

    document.addEventListener("keydown", handleKeydown);

    return () => {
      document.removeEventListener("keydown", handleKeydown);
    };
  });

  const viewPrevPage = () => {
    if (!modalContent) return;
    if (modalContent.page == 1) return;

    viewPage(
      modalContent.file_id,
      modalContent.page - 1,
      modalContent.num_pages,
      modalContent?.filename
    );
  };

  const viewNextPage = () => {
    if (!modalContent) return;
    if (modalContent.page == modalContent.num_pages) return;

    viewPage(
      modalContent.file_id,
      modalContent.page + 1,
      modalContent.num_pages,
      modalContent.filename
    );
  };

  const handlePageJump = () => {
    if (!modalContent) return;

    const pageNum = parseInt(pageInputValue, 10);
    if (isNaN(pageNum) || pageNum < 1 || pageNum > modalContent.num_pages) {
      pageInputValue = "";
      showPageInput = false;
      return;
    }

    viewPage(
      modalContent.file_id,
      pageNum,
      modalContent.num_pages,
      modalContent.filename
    );

    pageInputValue = "";
    showPageInput = false;
  };

  const togglePageInput = () => {
    showPageInput = !showPageInput;
    if (showPageInput) {
      pageInputValue = "";
      setTimeout(() => {
        const input = document.querySelector(
          ".page-jump-input"
        ) as HTMLInputElement;
        input?.focus();
      }, 0);
    }
  };

  async function handleSearch(query: string) {
    if (!query || !modalContent || query.length < 2) {
      searchResults = { results: [] };
      searchError = null;
      return;
    }

    isSearching = true;
    searchError = null;

    try {
      const data = await searchAPI(query, { fileId: modalContent.file_id });
      searchResults = data;
    } catch (error: unknown) {
      searchError = (error as Error).message;
    } finally {
      isSearching = false;
    }
  }

  const handleResultClick = (result: SearchResult) => {
    viewPage(
      result.file_id,
      result.page_num,
      result.num_pages,
      result.file_name
    );
    searchResults = { results: [] };
    searchQuery = "";
  };

  function highlightText(text: string) {
    return text
      .replace(/<b>/g, "<mark>")
      .replace(/<\/b>/g, "</mark>")
      .replace(/\n/g, "<br/>")
      .replace(/<\/mark>\s*<mark>/g, " ");
  }
</script>

{#if isOpen && modalContent}
  <!-- svelte-ignore a11y_interactive_supports_focus -->
  <!-- svelte-ignore a11y_click_events_have_key_events -->
  <div
    class="modal"
    onclick={onClose}
    role="dialog"
    aria-modal="true"
    aria-labelledby="modal-title"
  >
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div class="modal-content" onclick={(e: Event) => e.stopPropagation()}>
      <div class="modal-header">
        <h3 id="modal-title">{modalContent.title}</h3>

        <div class="modal-controls">
          {#if modalContent.file_id && modalContent.filename && modalContent.page && modalContent.num_pages && modalContent.num_pages > 1}
            <div class="page-navigation">
              <button
                class="nav-button"
                onclick={viewPrevPage}
                disabled={modalContent.page === 1}
                aria-label="Previous page"
                title="Previous (←)"
              >
                <svg
                  width="20"
                  height="20"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                  stroke-linecap="round"
                  stroke-linejoin="round"
                >
                  <polyline points="15 18 9 12 15 6"></polyline>
                </svg>
              </button>

              {#if showPageInput}
                <form
                  class="page-jump-form"
                  onsubmit={(e: Event) => {
                    e.preventDefault();
                    handlePageJump();
                  }}
                >
                  <input
                    type="number"
                    class="page-jump-input"
                    bind:value={pageInputValue}
                    placeholder={modalContent.page.toString()}
                    min="1"
                    max={modalContent.num_pages}
                    aria-label="Jump to page"
                  />
                  <span class="page-max">/ {modalContent.num_pages}</span>
                </form>
              {:else}
                <button
                  class="page-indicator"
                  onclick={togglePageInput}
                  title="Jump to page (g)"
                  aria-label="Jump to page"
                >
                  {modalContent.page} / {modalContent.num_pages}
                </button>
              {/if}

              <button
                class="nav-button"
                onclick={viewNextPage}
                disabled={modalContent.page === modalContent.num_pages}
                aria-label="Next page"
                title="Next (→)"
              >
                <svg
                  width="20"
                  height="20"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                  stroke-linecap="round"
                  stroke-linejoin="round"
                >
                  <polyline points="9 18 15 12 9 6"></polyline>
                </svg>
              </button>
            </div>
          {/if}

          {#if imageLoaded}
            <button
              class="nav-button view-mode-toggle"
              onclick={toggleViewMode}
              aria-label={viewMode === "scroll"
                ? "Switch to zoom mode"
                : "Switch to scroll mode"}
              title={viewMode === "scroll"
                ? "Zoom mode (v)"
                : "Scroll mode (v)"}
            >
              {#if viewMode === "scroll"}
                <svg
                  width="20"
                  height="20"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                  stroke-linecap="round"
                  stroke-linejoin="round"
                >
                  <circle cx="11" cy="11" r="8"></circle>
                  <line x1="11" y1="8" x2="11" y2="14"></line>
                  <line x1="8" y1="11" x2="14" y2="11"></line>
                  <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
                </svg>
              {:else}
                <svg
                  width="20"
                  height="20"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                  stroke-linecap="round"
                  stroke-linejoin="round"
                >
                  <path d="M8 3H5a2 2 0 0 0-2 2v3"></path>
                  <path d="M21 8V5a2 2 0 0 0-2-2h-3"></path>
                  <path d="M3 16v3a2 2 0 0 0 2 2h3"></path>
                  <path d="M16 21h3a2 2 0 0 0 2-2v-3"></path>
                </svg>
              {/if}
            </button>

            {#if viewMode === "fit"}
              <div class="zoom-controls">
                <button
                  class="nav-button"
                  onclick={() => (scale = Math.max(0.5, scale - 0.2))}
                  aria-label="Zoom out"
                  title="Zoom out (-)"
                >
                  <svg
                    width="20"
                    height="20"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="2"
                    stroke-linecap="round"
                    stroke-linejoin="round"
                  >
                    <circle cx="11" cy="11" r="8"></circle>
                    <line x1="8" y1="11" x2="14" y2="11"></line>
                    <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
                  </svg>
                </button>

                <span class="zoom-level" title="Reset zoom (r)">
                  {Math.round(scale * 100)}%
                </span>

                <button
                  class="nav-button"
                  onclick={() => (scale = Math.min(5, scale + 0.2))}
                  aria-label="Zoom in"
                  title="Zoom in (+)"
                >
                  <svg
                    width="20"
                    height="20"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="2"
                    stroke-linecap="round"
                    stroke-linejoin="round"
                  >
                    <circle cx="11" cy="11" r="8"></circle>
                    <line x1="11" y1="8" x2="11" y2="14"></line>
                    <line x1="8" y1="11" x2="14" y2="11"></line>
                    <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
                  </svg>
                </button>

                <button
                  class="nav-button"
                  onclick={resetTransform}
                  aria-label="Reset view"
                  title="Reset view (r)"
                >
                  <svg
                    width="20"
                    height="20"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="2"
                    stroke-linecap="round"
                    stroke-linejoin="round"
                  >
                    <path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"
                    ></path>
                    <path d="M21 3v5h-5"></path>
                    <path
                      d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"
                    ></path>
                    <path d="M3 21v-5h5"></path>
                  </svg>
                </button>
              </div>
            {/if}
          {/if}
        </div>

        <div class="search-wrapper">
          <input
            type="text"
            class="search_book"
            id="search_book"
            placeholder="Search this book"
            bind:value={searchQuery}
            onkeydown={(e: KeyboardEvent) => {
              if (e.key == "Enter") {
                handleSearch(searchQuery);
              }
            }}
          />
          <!-- Search results dropdown -->
          {#if searchQuery.length > 0 && (isSearching || searchError || searchResults.results.length > 0)}
            <div class="search-results-dropdown">
              {#if isSearching}
                <div class="result-message">
                  Searching for "{searchQuery}"...
                </div>
              {:else if searchError}
                <div class="result-message error-message-dropdown">
                  Search Error: {searchError}
                </div>
              {:else if searchResults.results.length === 0}
                <div class="result-message">
                  No results found for "{searchQuery}"
                </div>
              {:else}
                <ul class="results-list">
                  {#each searchResults.results as result (result.file_id + "-" + result.page_num)}
                    <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
                    <li
                      class="search-result-item"
                      onclick={() => handleResultClick(result)}
                    >
                      <div class="result-header-row">
                        <span class="result-title">{result.file_name}</span>
                        <span class="result-page">Page {result.page_num}</span>
                      </div>
                      <p class="result-snippet">
                        {@html highlightText(result.snippet)}
                      </p>
                    </li>
                  {/each}
                </ul>
                <div class="result-footer">
                  {searchResults.results.length} results found.
                </div>
              {/if}
            </div>
          {/if}
        </div>
        <button
          class="modal-close"
          onclick={onClose}
          aria-label="Close"
          title="Close (Esc)">&times;</button
        >
      </div>

      <div class="modal-body" class:scroll-mode={viewMode === "scroll"}>
        {#if modalContent.error}
          <div class="error-message">
            Failed to load: {modalContent.error}
          </div>
        {:else if modalContent.imageBlob}
          <!-- svelte-ignore a11y_no_static_element_interactions -->
          <div
            class="canvas-container"
            class:scroll-mode={viewMode === "scroll"}
            bind:this={containerElement}
            onwheel={handleWheel}
            onmousedown={handleMouseDown}
            onmousemove={handleMouseMove}
            onmouseup={handleMouseUp}
            onmouseleave={handleMouseUp}
            ontouchstart={handleTouchStart}
            ontouchmove={handleTouchMove}
            ontouchend={handleTouchEnd}
          >
            <canvas
              bind:this={canvasElement}
              class="page-canvas"
              class:dragging={isDragging}
              class:fit-mode={viewMode === "fit"}
            ></canvas>
            {#if !imageLoaded}
              <div class="loading-indicator">Loading...</div>
            {/if}
          </div>
        {/if}
      </div>
    </div>
  </div>
{/if}

<style>
  .modal {
    position: fixed;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: rgba(2, 6, 23, 0.85);
    backdrop-filter: blur(4px);
    z-index: 9999;
    padding: 1rem;
  }

  .modal-content {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 0.75rem;
    max-width: 1600px;
    width: 100%;
    height: 100%;
    display: flex;
    flex-direction: column;
    color: var(--text-primary);
    box-shadow: 0 20px 40px rgba(2, 6, 23, 0.6);
  }

  .modal-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 0.75rem 1rem;
    border-bottom: 1px solid var(--border);
    flex-shrink: 0;
    gap: 1rem;
    position: relative;
  }

  .modal-header h3 {
    flex: 0 1 auto;
    max-width: 30%;
  }

  .modal-header h3 {
    margin: 0;
    color: var(--text-primary);
    font-size: 1.125rem;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    flex: 1;
    min-width: 0;
  }

  .modal-controls {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    flex-shrink: 0;
  }

  .search-wrapper {
    position: relative;
    flex: 1; /* Allow wrapper to take available space */
    min-width: 200px; /* Ensure minimum space */
    max-width: 500px; /* Control max width of search area */
  }

  .page-navigation,
  .zoom-controls {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.25rem 0.5rem;
    background: var(--background, rgba(0, 0, 0, 0.1));
    border-radius: 0.5rem;
    border: 1px solid var(--border);
  }

  .nav-button {
    background: transparent;
    border: none;
    color: var(--text-primary);
    cursor: pointer;
    padding: 0.25rem;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 0.25rem;
    transition: all 0.2s;
  }

  .nav-button:hover:not(:disabled) {
    background: rgba(255, 255, 255, 0.1);
    color: var(--accent, #60a5fa);
  }

  .nav-button:disabled {
    opacity: 0.3;
    cursor: not-allowed;
  }

  .view-mode-toggle {
    padding: 0.25rem 0.5rem;
    background: var(--background, rgba(0, 0, 0, 0.1));
    border: 1px solid var(--border);
    border-radius: 0.5rem;
  }

  .page-indicator,
  .zoom-level {
    font-size: 0.875rem;
    color: var(--text-secondary);
    min-width: 4rem;
    text-align: center;
    font-variant-numeric: tabular-nums;
  }

  .page-indicator {
    background: transparent;
    border: none;
    cursor: pointer;
    padding: 0.25rem 0.5rem;
    border-radius: 0.25rem;
    transition: all 0.2s;
  }

  .page-indicator:hover {
    background: rgba(255, 255, 255, 0.1);
    color: var(--text-primary);
  }

  .zoom-level {
    min-width: 3rem;
    padding: 0.25rem 0.5rem;
    cursor: default;
  }

  .page-jump-form {
    display: flex;
    align-items: center;
    gap: 0.25rem;
  }

  .page-jump-input {
    width: 3.5rem;
    padding: 0.25rem 0.5rem;
    font-size: 0.875rem;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 0.25rem;
    color: var(--text-primary);
    text-align: center;
    font-variant-numeric: tabular-nums;
  }

  .page-jump-input:focus {
    outline: none;
    border-color: var(--accent, #60a5fa);
  }

  .page-max {
    font-size: 0.875rem;
    color: var(--text-secondary);
    font-variant-numeric: tabular-nums;
  }

  .modal-close {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    font-size: 1.5rem;
    cursor: pointer;
    padding: 0.25rem 0.5rem;
    line-height: 1;
    transition: color 0.2s;
    border-radius: 0.25rem;
  }

  .modal-close:hover {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.1);
  }

  .modal-body {
    flex: 1;
    overflow: hidden;
    padding: 0;
    min-height: 0;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .modal-body.scroll-mode {
    overflow-y: auto;
    align-items: flex-start;
  }

  .error-message {
    background: rgba(239, 68, 68, 0.1);
    border: 1px solid rgba(239, 68, 68, 0.3);
    color: var(--error);
    padding: 1rem;
    border-radius: 0.75rem;
    margin: 1rem;
  }

  .canvas-container {
    position: relative;
    width: 100%;
    height: 100%;
    overflow: hidden;
    touch-action: none;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .canvas-container.scroll-mode {
    height: auto;
    overflow-x: auto;
    overflow-y: visible;
    touch-action: auto;
  }

  .page-canvas {
    display: block;
  }

  .page-canvas.fit-mode {
    width: 100%;
    height: 100%;
  }

  .page-canvas.fit-mode.dragging {
    cursor: grabbing;
  }

  .page-canvas.fit-mode:not(.dragging) {
    cursor: grab;
  }

  .loading-indicator {
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    color: var(--text-secondary);
    font-size: 0.875rem;
  }

  .search_book {
    width: 100%;
    outline: none;
    padding: 0.5rem 1rem;
    border-radius: 30px;
    border: 2px solid var(--border, #222); /* Using CSS var for consistency */
    background: var(--surface, #1e293b);
    color: var(--text-primary, #f8fafc);
  }

  .search-results-dropdown {
    position: absolute;
    top: 100%; /* Position right below the input */
    left: 0;
    right: 0;
    z-index: 10000; /* Higher than modal-content but below close button if necessary */
    background: var(--surface-light, #334155);
    border: 1px solid var(--border, #475569);
    border-radius: 0 0 0.5rem 0.5rem;
    box-shadow: 0 8px 16px rgba(0, 0, 0, 0.3);
    max-height: 400px;
    overflow-y: auto;
    margin-top: 2px; /* Small gap from the input */
  }

  .results-list {
    list-style: none;
    padding: 0;
    margin: 0;
  }

  .search-result-item {
    padding: 0.75rem 1rem;
    cursor: pointer;
    transition: background-color 0.15s;
    border-bottom: 1px solid var(--border-light, #334155);
  }

  .search-result-item:hover {
    background: var(--hover-bg, #475569);
  }

  .result-header-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 0.25rem;
  }

  .result-title {
    font-weight: 600;
    color: var(--text-primary);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .result-page {
    font-size: 0.75rem;
    color: var(--accent, #60a5fa);
    font-weight: 500;
    flex-shrink: 0;
    margin-left: 0.5rem;
  }

  .result-snippet {
    font-size: 1rem;
    color: var(--text-secondary);
    margin: 0;
    display: -webkit-box;
    -webkit-line-clamp: 4;
    line-clamp: 4;
    -webkit-box-orient: vertical;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .result-message,
  .result-footer {
    padding: 0.75rem 1rem;
    text-align: center;
    font-size: 0.875rem;
    color: var(--text-secondary);
  }

  .error-message-dropdown {
    color: var(--error);
  }

  @media (max-width: 768px) {
    .modal {
      padding: 0;
      align-items: stretch;
    }

    .modal-content {
      height: 100vh;
      max-height: 100vh;
      border-radius: 0;
    }

    .modal-header {
      padding: 0.5rem;
      flex-wrap: wrap;
    }

    .modal-header h3 {
      font-size: 0.9rem;
      order: 1;
      flex: 0 1 auto;
      max-width: calc(100% - 3rem);
    }

    .modal-close {
      order: 2;
      font-size: 1.75rem;
      padding: 0.25rem;
    }

    .modal-controls {
      order: 3;
      flex: 1 1 100%;
      justify-content: center;
      gap: 0.5rem;
      margin-top: 0.5rem;
    }

    .page-navigation,
    .zoom-controls {
      padding: 0.25rem 0.5rem;
      gap: 0.5rem;
    }

    .search-wrapper {
      order: 4; /* Place search below controls on small screens */
      flex: 1 1 100%;
      max-width: 100%;
      margin-top: 0.5rem;
    }

    .search-results-dropdown {
      max-height: 300px;
    }

    .nav-button svg {
      width: 18px;
      height: 18px;
    }

    .page-indicator,
    .zoom-level {
      font-size: 0.8125rem;
      min-width: 3.5rem;
      padding: 0.25rem;
    }

    .zoom-level {
      min-width: 2.5rem;
    }

    .page-jump-input {
      width: 3rem;
      font-size: 0.8125rem;
    }

    .page-max {
      font-size: 0.8125rem;
    }
  }

  @media (max-width: 480px) {
    .modal-header h3 {
      font-size: 0.8125rem;
    }

    .page-navigation,
    .zoom-controls {
      padding: 0.2rem 0.4rem;
    }

    .nav-button {
      padding: 0.2rem;
    }

    .nav-button svg {
      width: 16px;
      height: 16px;
    }

    .page-indicator,
    .zoom-level {
      font-size: 0.75rem;
      min-width: 3rem;
    }

    .zoom-level {
      min-width: 2rem;
    }

    .page-jump-input {
      width: 2.5rem;
      font-size: 0.75rem;
    }

    .page-max {
      font-size: 0.75rem;
    }
  }
</style>
