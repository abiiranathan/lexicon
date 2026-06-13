<!-- Modal.svelte -->
<script lang="ts">
  import { searchAPI } from "./lib/httpclient";

  type ModalProps = {
    isOpen: boolean;
    onClose: () => void;
    modalContent: ModalContentType | null;
    viewPage: (
      fileId: number,
      pageNum: number,
      numPages: number,
      fileName: string,
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

  // View mode: default to 'fit' on small screens (< 760px) and 'scroll' on larger screens
  let viewMode = $state<"fit" | "scroll">(
    typeof window !== "undefined" && window.innerWidth < 760 ? "fit" : "scroll",
  );

  // Search state — reset whenever the active file changes.
  let searchQuery = $state("");
  let searchResults = $state<{ results: SearchResult[] }>({ results: [] });
  let isSearching = $state(false);
  let searchError = $state<string | null>(null);
  let searchDebounceTimer: ReturnType<typeof setTimeout> | undefined;
  let showMobileSearch = $state(false);

  // Pan and zoom state (only used in 'fit' mode)
  let scale = $state(1);
  let translateX = $state(0);
  let translateY = $state(0);
  let isDragging = $state(false);
  let lastTouchDistance = $state(0);
  let dragStartX = $state(0);
  let dragStartY = $state(0);
  let rotation = $state(0); // 0, 90, 180, 270

  // ---------------------------------------------------------------------------
  // Reset search state when the viewed file changes so stale results
  // from a previous document are never shown in the current one.
  // ---------------------------------------------------------------------------
  let lastFileId = $state<number | null>(null);

  $effect(() => {
    if (!modalContent) return;
    if (modalContent.file_id !== lastFileId) {
      lastFileId = modalContent.file_id;
      searchQuery = "";
      searchResults = { results: [] };
      searchError = null;
      isSearching = false;
    }
  });

  // Sync URL state with modal
  $effect(() => {
    if (isOpen && modalContent) {
      const params = new URLSearchParams(window.location.search);
      params.set("file", modalContent.file_id.toString());
      params.set("page", modalContent.page.toString());
      window.history.replaceState(
        {},
        "",
        `${window.location.pathname}?${params.toString()}`,
      );
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

  // ---------------------------------------------------------------------------
  // Canvas rendering — logic preserved verbatim from original.
  // ---------------------------------------------------------------------------

  const isLandscapeRotation = $derived(rotation === 90 || rotation === 270);

  function prepareContext(
    canvas: HTMLCanvasElement,
    container: HTMLDivElement,
    cssWidth: number,
    cssHeight: number,
  ): CanvasRenderingContext2D | null {
    const dpr = window.devicePixelRatio || 1;

    canvas.width = Math.round(cssWidth * dpr);
    canvas.height = Math.round(cssHeight * dpr);
    canvas.style.width = `${cssWidth}px`;
    canvas.style.height = `${cssHeight}px`;

    const ctx = canvas.getContext("2d", { alpha: false });
    if (!ctx) return null;

    ctx.scale(dpr, dpr);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = "high";

    return ctx;
  }

  function drawRotatedImage(
    ctx: CanvasRenderingContext2D,
    img: HTMLImageElement,
    cx: number,
    cy: number,
    drawWidth: number,
    drawHeight: number,
    deg: number,
  ): void {
    const rad = (deg * Math.PI) / 180;
    const swapped = deg === 90 || deg === 270;

    ctx.save();
    ctx.translate(cx, cy);
    ctx.rotate(rad);

    const dw = swapped ? drawHeight : drawWidth;
    const dh = swapped ? drawWidth : drawHeight;
    ctx.drawImage(img, -dw / 2, -dh / 2, dw, dh);

    ctx.restore();
  }

  function renderCanvas(): void {
    if (!canvasElement || !containerElement || !loadedImage) return;

    const canvas = canvasElement;
    const container = containerElement;
    const img = loadedImage;

    const imgW = isLandscapeRotation ? img.height : img.width;
    const imgH = isLandscapeRotation ? img.width : img.height;
    const imgAspect = imgW / imgH;

    if (viewMode === "scroll") {
      const cssWidth = Math.max(container.clientWidth, 600);
      const cssHeight = cssWidth / imgAspect;

      const ctx = prepareContext(canvas, container, cssWidth, cssHeight);
      if (!ctx) return;

      ctx.fillStyle = "#ffffff";
      ctx.fillRect(0, 0, cssWidth, cssHeight);

      drawRotatedImage(
        ctx,
        img,
        cssWidth / 2,
        cssHeight / 2,
        cssWidth,
        cssHeight,
        rotation,
      );
    } else {
      const cssWidth = container.clientWidth;
      const cssHeight = container.clientHeight;

      const ctx = prepareContext(canvas, container, cssWidth, cssHeight);
      if (!ctx) return;

      ctx.fillStyle = "#ffffff";
      ctx.fillRect(0, 0, cssWidth, cssHeight);

      const containerAspect = cssWidth / cssHeight;
      const drawWidth =
        imgAspect > containerAspect ? cssWidth : cssHeight * imgAspect;
      const drawHeight =
        imgAspect > containerAspect ? cssWidth / imgAspect : cssHeight;

      const cx = cssWidth / 2;
      const cy = cssHeight / 2;

      ctx.save();
      ctx.translate(cx, cy);
      ctx.scale(scale, scale);
      ctx.translate(-cx + translateX, -cy + translateY);

      drawRotatedImage(ctx, img, cx, cy, drawWidth, drawHeight, rotation);

      ctx.restore();
    }
  }

  $effect(() => {
    if (!modalContent?.imageBlob || !canvasElement || !containerElement) {
      imageLoaded = false;
      loadedImage = null;
      return;
    }

    scale = 1;
    translateX = 0;
    translateY = 0;
    rotation = 0;
    imageLoaded = false;
    loadedImage = null;

    const url = URL.createObjectURL(modalContent.imageBlob);
    const img = new Image();

    img.onload = () => {
      loadedImage = img;
      imageLoaded = true;
      URL.revokeObjectURL(url);
    };

    img.onerror = () => {
      URL.revokeObjectURL(url);
      imageLoaded = false;
      loadedImage = null;
    };

    img.src = url;
  });

  $effect(() => {
    if (!imageLoaded || !loadedImage) return;
    const _ = [scale, translateX, translateY, viewMode, rotation];
    void _;
    renderCanvas();
  });

  $effect(() => {
    if (!isOpen || !containerElement) return;

    const observer = new ResizeObserver(() => {
      renderCanvas();
    });

    observer.observe(containerElement);

    return () => observer.disconnect();
  });

  // ---------------------------------------------------------------------------
  // Input handlers — logic preserved verbatim from original.
  // ---------------------------------------------------------------------------

  function handleWheel(e: WheelEvent): void {
    if (viewMode !== "fit") return;
    e.preventDefault();
    scale = Math.max(0.5, Math.min(5, scale - e.deltaY * 0.001));
  }

  function handleMouseDown(e: MouseEvent): void {
    if (viewMode !== "fit") return;
    isDragging = true;
    dragStartX = e.clientX - translateX;
    dragStartY = e.clientY - translateY;
  }

  function handleMouseMove(e: MouseEvent): void {
    if (viewMode !== "fit" || !isDragging) return;
    translateX = e.clientX - dragStartX;
    translateY = e.clientY - dragStartY;
  }

  function handleMouseUp(): void {
    isDragging = false;
  }

  function handleTouchStart(e: TouchEvent): void {
    if (viewMode !== "fit") return;

    if (e.touches.length === 2) {
      e.preventDefault();
      lastTouchDistance = Math.hypot(
        e.touches[1].clientX - e.touches[0].clientX,
        e.touches[1].clientY - e.touches[0].clientY,
      );
    } else if (e.touches.length === 1) {
      isDragging = true;
      dragStartX = e.touches[0].clientX - translateX;
      dragStartY = e.touches[0].clientY - translateY;
    }
  }

  function handleTouchMove(e: TouchEvent): void {
    if (viewMode !== "fit") return;

    if (e.touches.length === 2) {
      e.preventDefault();
      const distance = Math.hypot(
        e.touches[1].clientX - e.touches[0].clientX,
        e.touches[1].clientY - e.touches[0].clientY,
      );
      if (lastTouchDistance > 0) {
        scale = Math.max(
          0.5,
          Math.min(5, scale + (distance - lastTouchDistance) * 0.01),
        );
      }
      lastTouchDistance = distance;
    } else if (e.touches.length === 1 && isDragging) {
      e.preventDefault();
      translateX = e.touches[0].clientX - dragStartX;
      translateY = e.touches[0].clientY - dragStartY;
    }
  }

  function handleTouchEnd(e: TouchEvent): void {
    if (e.touches.length < 2) lastTouchDistance = 0;
    if (e.touches.length === 0) isDragging = false;
  }

  function resetTransform(): void {
    scale = 1;
    translateX = 0;
    translateY = 0;
  }

  function toggleViewMode(): void {
    viewMode = viewMode === "fit" ? "scroll" : "fit";
    if (viewMode === "fit") resetTransform();
  }

  function rotateImage(): void {
    rotation = (rotation + 90) % 360;
  }

  // Keyboard shortcuts
  $effect(() => {
    if (!isOpen) return;

    const handleKeydown = (e: KeyboardEvent): void => {
      const target = e.target as HTMLElement;
      if (
        target.tagName === "INPUT" &&
        !target.classList.contains("page-jump-input")
      ) {
        return;
      }

      switch (e.key) {
        case "Escape":
          if (showPageInput) {
            showPageInput = false;
            pageInputValue = "";
          } else {
            onClose();
          }
          break;
        case "ArrowLeft":
          e.preventDefault();
          viewPrevPage();
          break;
        case "ArrowRight":
          e.preventDefault();
          viewNextPage();
          break;
        case "g":
          if (!showPageInput) {
            e.preventDefault();
            togglePageInput();
          }
          break;
        case "r":
          e.preventDefault();
          if (e.shiftKey) rotateImage();
          else resetTransform();
          break;
        case "R":
          e.preventDefault();
          rotateImage();
          break;
        case "v":
          e.preventDefault();
          toggleViewMode();
          break;
        case "+":
        case "=":
          if (viewMode === "fit") {
            e.preventDefault();
            scale = Math.min(5, scale + 0.2);
          }
          break;
        case "-":
          if (viewMode === "fit") {
            e.preventDefault();
            scale = Math.max(0.5, scale - 0.2);
          }
          break;
      }
    };

    document.addEventListener("keydown", handleKeydown);
    return () => document.removeEventListener("keydown", handleKeydown);
  });

  // ---------------------------------------------------------------------------
  // Page navigation — logic preserved verbatim from original.
  // ---------------------------------------------------------------------------

  const viewPrevPage = (): void => {
    if (!modalContent || modalContent.page === 1) return;
    viewPage(
      modalContent.file_id,
      modalContent.page - 1,
      modalContent.num_pages,
      modalContent.filename,
    );
  };

  const viewNextPage = (): void => {
    if (!modalContent || modalContent.page === modalContent.num_pages) return;
    viewPage(
      modalContent.file_id,
      modalContent.page + 1,
      modalContent.num_pages,
      modalContent.filename,
    );
  };

  const handlePageJump = (): void => {
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
      modalContent.filename,
    );

    pageInputValue = "";
    showPageInput = false;
  };

  const togglePageInput = (): void => {
    showPageInput = !showPageInput;
    if (showPageInput) {
      pageInputValue = "";
      setTimeout(() => {
        (
          document.querySelector(".page-jump-input") as HTMLInputElement
        )?.focus();
      }, 0);
    }
  };

  // ---------------------------------------------------------------------------
  // Search — FIXED:
  //   1. handleSearchInput debounces auto-search (300 ms) so the user gets
  //      live results without pressing Enter.
  //   2. handleSearchSubmit fires immediately on Enter / button click.
  //   3. highlightText converts <b>…</b> → <mark>…</mark> for all snippets.
  //   4. Clearing the query clears results immediately.
  // ---------------------------------------------------------------------------

  function highlightText(text: string): string {
    return text
      .replace(/<b>/g, "<mark>")
      .replace(/<\/b>/g, "</mark>")
      .replace(/\n/g, "<br/>")
      .replace(/<\/mark>\s*<mark>/g, " ");
  }

  async function runSearch(query: string): Promise<void> {
    if (!query || !modalContent || query.length < 2) {
      searchResults = { results: [] };
      searchError = null;
      return;
    }

    isSearching = true;
    searchError = null;

    try {
      searchResults = await searchAPI(query, { fileId: modalContent.file_id });
    } catch (error: unknown) {
      searchError = (error as Error).message;
      searchResults = { results: [] };
    } finally {
      isSearching = false;
    }
  }

  function handleSearchInput(e: Event): void {
    const value = (e.currentTarget as HTMLInputElement).value;
    searchQuery = value;

    clearTimeout(searchDebounceTimer);

    if (!value || value.length < 2) {
      searchResults = { results: [] };
      searchError = null;
      return;
    }

    searchDebounceTimer = setTimeout(() => runSearch(value), 300);
  }

  function handleSearchSubmit(): void {
    clearTimeout(searchDebounceTimer);
    runSearch(searchQuery);
  }

  function clearSearch(): void {
    clearTimeout(searchDebounceTimer);
    searchQuery = "";
    searchResults = { results: [] };
    searchError = null;
  }

  const handleResultClick = (result: SearchResult): void => {
    viewPage(
      result.file_id,
      result.page_num,
      result.num_pages,
      result.file_name,
    );
    clearSearch();
    showMobileSearch = false;
  };

  const dropdownVisible = $derived(
    searchQuery.length > 0 &&
      (isSearching || !!searchError || searchResults.results.length > 0),
  );
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
      <!-- ── Header ── -->
      <div class="modal-header">
        <!-- Left: close (mobile) + title -->
        <div class="header-left">
          <button
            class="icon-btn mobile-only"
            onclick={onClose}
            aria-label="Close"
            type="button"
          >
            <svg
              width="20"
              height="20"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
            >
              <line x1="18" y1="6" x2="6" y2="18" /><line
                x1="6"
                y1="6"
                x2="18"
                y2="18"
              />
            </svg>
          </button>
          <h3 id="modal-title" class="modal-title">{modalContent.title}</h3>
        </div>

        <!-- Centre: page nav + view controls (desktop) -->
        <div class="desktop-controls desktop-only">
          {#if modalContent.file_id && modalContent.filename && modalContent.num_pages > 1}
            <div class="control-group">
              <button
                class="icon-btn"
                onclick={viewPrevPage}
                disabled={modalContent.page === 1}
                aria-label="Previous page"
                title="Previous (←)"
                type="button"
              >
                <svg
                  width="16"
                  height="16"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2.5"
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
                  <span class="page-sep">/ {modalContent.num_pages}</span>
                </form>
              {:else}
                <button
                  class="page-indicator"
                  onclick={togglePageInput}
                  title="Jump to page (g)"
                  aria-label="Jump to page"
                  type="button"
                >
                  {modalContent.page} / {modalContent.num_pages}
                </button>
              {/if}

              <button
                class="icon-btn"
                onclick={viewNextPage}
                disabled={modalContent.page === modalContent.num_pages}
                aria-label="Next page"
                title="Next (→)"
                type="button"
              >
                <svg
                  width="16"
                  height="16"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2.5"
                >
                  <polyline points="9 18 15 12 9 6"></polyline>
                </svg>
              </button>
            </div>
          {/if}

          {#if imageLoaded}
            <!-- Rotate -->
            <button
              class="icon-btn"
              onclick={rotateImage}
              aria-label="Rotate 90°"
              title="Rotate (Shift+R)"
              type="button"
            >
              <svg
                width="16"
                height="16"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
              >
                <path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8"
                ></path>
                <path d="M21 3v5h-5"></path>
              </svg>
            </button>

            <!-- View mode toggle -->
            <button
              class="icon-btn"
              class:icon-btn--active={viewMode === "fit"}
              onclick={toggleViewMode}
              aria-label={viewMode === "scroll"
                ? "Switch to zoom mode"
                : "Switch to scroll mode"}
              title={viewMode === "scroll"
                ? "Zoom mode (v)"
                : "Scroll mode (v)"}
              type="button"
            >
              {#if viewMode === "scroll"}
                <svg
                  width="16"
                  height="16"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                >
                  <circle cx="11" cy="11" r="8"></circle>
                  <line x1="11" y1="8" x2="11" y2="14"></line>
                  <line x1="8" y1="11" x2="14" y2="11"></line>
                  <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
                </svg>
              {:else}
                <svg
                  width="16"
                  height="16"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                >
                  <path d="M8 3H5a2 2 0 0 0-2 2v3"></path>
                  <path d="M21 8V5a2 2 0 0 0-2-2h-3"></path>
                  <path d="M3 16v3a2 2 0 0 0 2 2h3"></path>
                  <path d="M16 21h3a2 2 0 0 0 2-2v-3"></path>
                </svg>
              {/if}
            </button>

            <!-- Zoom controls (fit mode only) -->
            {#if viewMode === "fit"}
              <div class="control-group">
                <button
                  class="icon-btn"
                  onclick={() => (scale = Math.max(0.5, scale - 0.2))}
                  aria-label="Zoom out"
                  title="Zoom out (-)"
                  type="button"
                >
                  <svg
                    width="16"
                    height="16"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="2"
                  >
                    <circle cx="11" cy="11" r="8"></circle>
                    <line x1="8" y1="11" x2="14" y2="11"></line>
                    <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
                  </svg>
                </button>
                <span class="zoom-label">{Math.round(scale * 100)}%</span>
                <button
                  class="icon-btn"
                  onclick={() => (scale = Math.min(5, scale + 0.2))}
                  aria-label="Zoom in"
                  title="Zoom in (+)"
                  type="button"
                >
                  <svg
                    width="16"
                    height="16"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="2"
                  >
                    <circle cx="11" cy="11" r="8"></circle>
                    <line x1="11" y1="8" x2="11" y2="14"></line>
                    <line x1="8" y1="11" x2="14" y2="11"></line>
                    <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
                  </svg>
                </button>
              </div>
            {/if}
          {/if}
        </div>

        <!-- Right: search + close -->
        <div class="header-right">
          <!-- In-document search widget -->
          <div class="search-wrapper" class:mobile-active={showMobileSearch}>
            <div class="search-field-wrap">
              <svg
                class="search-field-icon"
                width="14"
                height="14"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
                aria-hidden="true"
              >
                <circle cx="11" cy="11" r="8"></circle>
                <path d="m21 21-4.35-4.35"></path>
              </svg>
              <input
                type="text"
                class="search-field"
                placeholder="Search in document…"
                value={searchQuery}
                oninput={handleSearchInput}
                onkeydown={(e: KeyboardEvent) => {
                  if (e.key === "Enter") handleSearchSubmit();
                  if (e.key === "Escape") clearSearch();
                }}
                aria-label="Search inside document"
                spellcheck="false"
                autocomplete="off"
              />
              {#if searchQuery}
                <button
                  class="search-clear"
                  onclick={clearSearch}
                  type="button"
                  aria-label="Clear search"
                >
                  <svg
                    width="12"
                    height="12"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="2.5"
                  >
                    <line x1="18" y1="6" x2="6" y2="18" /><line
                      x1="6"
                      y1="6"
                      x2="18"
                      y2="18"
                    />
                  </svg>
                </button>
              {/if}
            </div>

            {#if showMobileSearch}
              <button
                class="mobile-search-cancel"
                onclick={() => {
                  showMobileSearch = false;
                  clearSearch();
                }}
                type="button"
              >
                Cancel
              </button>
            {/if}

            <!-- Results dropdown -->
            {#if dropdownVisible}
              <div
                class="search-dropdown"
                role="listbox"
                aria-label="Search results"
              >
                {#if isSearching}
                  <div class="dropdown-status">
                    <div class="dropdown-spinner"></div>
                    Searching…
                  </div>
                {:else if searchError}
                  <div class="dropdown-status dropdown-status--error">
                    <svg
                      width="14"
                      height="14"
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      stroke-width="2"
                      aria-hidden="true"
                    >
                      <circle cx="12" cy="12" r="10" /><line
                        x1="12"
                        y1="8"
                        x2="12"
                        y2="12"
                      />
                    </svg>
                    {searchError}
                  </div>
                {:else if searchResults.results.length === 0}
                  <div class="dropdown-status">No results found</div>
                {:else}
                  <div class="dropdown-header">
                    {searchResults.results.length}
                    {searchResults.results.length === 1 ? "result" : "results"}
                  </div>
                  <ul class="dropdown-list">
                    {#each searchResults.results as result (result.file_id + "-" + result.page_num)}
                      <!-- svelte-ignore a11y_click_events_have_key_events -->
                      <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
                      <li
                        class="dropdown-item"
                        onclick={() => handleResultClick(result)}
                        role="option"
                        aria-selected="false"
                      >
                        <span class="dropdown-page">Page {result.page_num}</span
                        >
                        <p class="dropdown-snippet">
                          {@html highlightText(result.snippet)}
                        </p>
                      </li>
                    {/each}
                  </ul>
                {/if}
              </div>
            {/if}
          </div>

          <!-- Mobile: search trigger -->
          <button
            class="icon-btn mobile-only"
            onclick={() => (showMobileSearch = true)}
            aria-label="Search inside document"
            type="button"
          >
            <svg
              width="18"
              height="18"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
            >
              <circle cx="11" cy="11" r="8"></circle>
              <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
            </svg>
          </button>

          <!-- Close (desktop) -->
          <button
            class="icon-btn icon-btn--close desktop-only"
            onclick={onClose}
            aria-label="Close dialog"
            title="Close (Esc)"
            type="button"
          >
            <svg
              width="18"
              height="18"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
            >
              <line x1="18" y1="6" x2="6" y2="18" /><line
                x1="6"
                y1="6"
                x2="18"
                y2="18"
              />
            </svg>
          </button>
        </div>
      </div>

      <!-- ── Canvas viewport ── -->
      <div class="modal-body" class:scroll-mode={viewMode === "scroll"}>
        {#if modalContent.error}
          <div class="render-error">
            <svg
              width="20"
              height="20"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
              aria-hidden="true"
            >
              <circle cx="12" cy="12" r="10" /><line
                x1="12"
                y1="8"
                x2="12"
                y2="12"
              /><line x1="12" y1="16" x2="12.01" y2="16" />
            </svg>
            Failed to render: {modalContent.error}
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
              <div class="canvas-loading">
                <div class="canvas-spinner"></div>
              </div>
            {/if}
          </div>
        {/if}
      </div>

      <!-- ── Mobile HUD ── -->
      {#if imageLoaded && modalContent.num_pages > 1}
        <div
          class="mobile-hud mobile-only"
          role="toolbar"
          aria-label="Page navigation"
        >
          <button
            class="hud-btn"
            onclick={viewPrevPage}
            disabled={modalContent.page === 1}
            aria-label="Previous page"
            type="button"
          >
            <svg
              width="18"
              height="18"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2.5"
            >
              <polyline points="15 18 9 12 15 6"></polyline>
            </svg>
          </button>

          <div class="hud-center">
            {#if showPageInput}
              <form
                class="hud-form"
                onsubmit={(e: Event) => {
                  e.preventDefault();
                  handlePageJump();
                }}
              >
                <input
                  type="number"
                  class="hud-input"
                  bind:value={pageInputValue}
                  placeholder={modalContent.page.toString()}
                  min="1"
                  max={modalContent.num_pages}
                  aria-label="Go to page"
                />
              </form>
            {:else}
              <button class="hud-page" onclick={togglePageInput} type="button">
                {modalContent.page} / {modalContent.num_pages}
              </button>
            {/if}
          </div>

          <div class="hud-actions">
            <button
              class="hud-action"
              onclick={rotateImage}
              aria-label="Rotate"
              type="button"
            >
              <svg
                width="16"
                height="16"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
              >
                <path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8"
                ></path>
                <path d="M21 3v5h-5"></path>
              </svg>
            </button>
            <button
              class="hud-action"
              onclick={toggleViewMode}
              aria-label="Toggle view mode"
              type="button"
            >
              {#if viewMode === "scroll"}
                <svg
                  width="16"
                  height="16"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                >
                  <circle cx="11" cy="11" r="8"></circle>
                  <line x1="11" y1="8" x2="11" y2="14"></line>
                  <line x1="8" y1="11" x2="14" y2="11"></line>
                  <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
                </svg>
              {:else}
                <svg
                  width="16"
                  height="16"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                >
                  <path d="M8 3H5a2 2 0 0 0-2 2v3"></path>
                  <path d="M21 8V5a2 2 0 0 0-2-2h-3"></path>
                  <path d="M3 16v3a2 2 0 0 0 2 2h3"></path>
                  <path d="M16 21h3a2 2 0 0 0 2-2v-3"></path>
                </svg>
              {/if}
            </button>
          </div>

          <button
            class="hud-btn"
            onclick={viewNextPage}
            disabled={modalContent.page === modalContent.num_pages}
            aria-label="Next page"
            type="button"
          >
            <svg
              width="18"
              height="18"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2.5"
            >
              <polyline points="9 18 15 12 9 6"></polyline>
            </svg>
          </button>
        </div>
      {/if}
    </div>
  </div>
{/if}

<style>
  /* ── Overlay ── */
  .modal {
    position: fixed;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: rgba(3, 7, 18, 0.88);
    backdrop-filter: blur(10px);
    -webkit-backdrop-filter: blur(10px);
    z-index: 9999;
    padding: 1rem;
  }

  /* ── Panel ── */
  .modal-content {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 1.125rem;
    max-width: 1400px;
    width: 100%;
    height: 100%;
    display: flex;
    flex-direction: column;
    color: var(--text-primary);
    box-shadow:
      0 0 0 1px rgba(255, 255, 255, 0.04) inset,
      0 32px 64px -16px rgba(0, 0, 0, 0.9);
    position: relative;
    overflow: hidden;
  }

  /* ── Header ── */
  .modal-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0.75rem 1rem;
    border-bottom: 1px solid var(--border);
    flex-shrink: 0;
    gap: 0.75rem;
    background: rgba(11, 15, 25, 0.6);
    min-height: 56px;
  }

  .header-left {
    display: flex;
    align-items: center;
    gap: 0.625rem;
    min-width: 0;
    flex: 1;
  }

  .modal-title {
    margin: 0;
    font-size: 0.875rem;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: var(--text-secondary);
  }

  .header-right {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    flex-shrink: 0;
  }

  /* ── Desktop controls ── */
  .desktop-controls {
    display: flex;
    align-items: center;
    gap: 0.375rem;
  }

  .control-group {
    display: flex;
    align-items: center;
    gap: 0.25rem;
    background: rgba(0, 0, 0, 0.25);
    border: 1px solid var(--border);
    border-radius: 0.5rem;
    padding: 0.25rem;
  }

  /* ── Icon buttons ── */
  .icon-btn {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    cursor: pointer;
    padding: 0.375rem;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 0.375rem;
    transition:
      background 0.12s,
      color 0.12s;
    line-height: 1;
  }

  .icon-btn:hover:not(:disabled) {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary);
  }

  .icon-btn:disabled {
    opacity: 0.3;
    cursor: not-allowed;
  }

  .icon-btn--active {
    color: #818cf8;
    background: rgba(129, 140, 248, 0.1);
  }

  .icon-btn--close:hover {
    background: rgba(239, 68, 68, 0.1);
    color: var(--error);
  }

  /* ── Page indicator ── */
  .page-indicator {
    background: transparent;
    border: none;
    color: var(--text-primary);
    cursor: pointer;
    font-size: 0.8125rem;
    font-weight: 500;
    font-variant-numeric: tabular-nums;
    padding: 0.25rem 0.5rem;
    border-radius: 0.25rem;
    min-width: 4rem;
    text-align: center;
    transition: background 0.12s;
  }

  .page-indicator:hover {
    background: rgba(255, 255, 255, 0.06);
  }

  .page-jump-form {
    display: flex;
    align-items: center;
    gap: 0.25rem;
    padding: 0 0.25rem;
  }

  .page-jump-input {
    width: 2.75rem;
    padding: 0.125rem 0.25rem;
    font-size: 0.8125rem;
    background: var(--background);
    border: 1px solid var(--border-focus);
    border-radius: 0.25rem;
    color: var(--text-primary);
    text-align: center;
    font-variant-numeric: tabular-nums;
  }

  .page-jump-input:focus {
    outline: none;
    box-shadow: 0 0 0 2px rgba(79, 70, 229, 0.25);
  }

  .page-sep {
    font-size: 0.8125rem;
    color: var(--text-muted);
  }

  .zoom-label {
    font-size: 0.75rem;
    color: var(--text-secondary);
    font-variant-numeric: tabular-nums;
    min-width: 2.5rem;
    text-align: center;
  }

  /* ── Search widget ── */
  .search-wrapper {
    position: relative;
  }

  .search-field-wrap {
    position: relative;
    display: flex;
    align-items: center;
  }

  .search-field-icon {
    position: absolute;
    left: 0.625rem;
    color: var(--text-muted);
    pointer-events: none;
  }

  .search-field {
    width: 200px;
    padding: 0.4375rem 2rem 0.4375rem 2rem;
    background: rgba(0, 0, 0, 0.3);
    border: 1px solid var(--border);
    border-radius: 0.5rem;
    color: var(--text-primary);
    font-size: 0.8125rem;
    transition:
      border-color 0.15s,
      box-shadow 0.15s,
      width 0.2s;
  }

  .search-field::placeholder {
    color: var(--text-muted);
  }

  .search-field:focus {
    outline: none;
    border-color: var(--primary);
    box-shadow: 0 0 0 2px rgba(79, 70, 229, 0.2);
    width: 240px;
  }

  .search-clear {
    position: absolute;
    right: 0.5rem;
    background: transparent;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    display: flex;
    align-items: center;
    padding: 0.25rem;
    border-radius: 0.25rem;
    transition: color 0.12s;
  }

  .search-clear:hover {
    color: var(--text-secondary);
  }

  /* ── Search dropdown ── */
  .search-dropdown {
    position: absolute;
    top: calc(100% + 0.5rem);
    right: 0;
    width: 320px;
    background: #1a2232;
    border: 1px solid var(--border);
    border-radius: 0.75rem;
    box-shadow: 0 16px 40px -8px rgba(0, 0, 0, 0.7);
    max-height: 380px;
    overflow-y: auto;
    z-index: 1000;
  }

  .dropdown-header {
    padding: 0.625rem 1rem;
    font-size: 0.6875rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--text-muted);
    border-bottom: 1px solid var(--border);
  }

  .dropdown-status {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 0.5rem;
    padding: 1.25rem 1rem;
    font-size: 0.8125rem;
    color: var(--text-muted);
  }

  .dropdown-status--error {
    color: var(--error);
  }

  .dropdown-spinner {
    width: 0.875rem;
    height: 0.875rem;
    border: 1.5px solid var(--border);
    border-top-color: var(--primary);
    border-radius: 50%;
    animation: spin 0.7s linear infinite;
  }

  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }

  .dropdown-list {
    list-style: none;
    margin: 0;
    padding: 0.25rem 0;
  }

  .dropdown-item {
    padding: 0.625rem 1rem;
    cursor: pointer;
    border-bottom: 1px solid rgba(255, 255, 255, 0.04);
    transition: background 0.12s;
  }

  .dropdown-item:last-child {
    border-bottom: none;
  }

  .dropdown-item:hover {
    background: rgba(255, 255, 255, 0.04);
  }

  .dropdown-page {
    display: block;
    font-size: 0.6875rem;
    font-weight: 700;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: #818cf8;
    margin-bottom: 0.25rem;
  }

  .dropdown-snippet {
    font-size: 0.8125rem;
    color: var(--text-secondary);
    line-height: 1.5;
    margin: 0;
    display: -webkit-box;
    -webkit-line-clamp: 3;
    line-clamp: 3;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }

  .dropdown-snippet :global(mark) {
    background: rgba(245, 158, 11, 0.2);
    color: #f59e0b;
    border-radius: 2px;
    padding: 0 1px;
    font-weight: 500;
  }

  /* ── Canvas body ── */
  .modal-body {
    flex: 1;
    overflow: hidden;
    min-height: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: #0c1018;
  }

  .modal-body.scroll-mode {
    overflow-y: auto;
    align-items: flex-start;
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
    box-shadow: 0 12px 40px rgba(0, 0, 0, 0.7);
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

  .canvas-loading {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: #0c1018;
  }

  .canvas-spinner {
    width: 1.75rem;
    height: 1.75rem;
    border: 2px solid rgba(255, 255, 255, 0.1);
    border-top-color: var(--primary);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  .render-error {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    background: rgba(239, 68, 68, 0.08);
    border: 1px solid rgba(239, 68, 68, 0.25);
    color: var(--error);
    padding: 1rem 1.5rem;
    border-radius: 0.625rem;
    font-size: 0.875rem;
    max-width: 480px;
  }

  /* ── Mobile HUD ── */
  .mobile-hud {
    position: absolute;
    bottom: 1.25rem;
    left: 50%;
    transform: translateX(-50%);
    background: rgba(17, 24, 39, 0.9);
    backdrop-filter: blur(16px);
    -webkit-backdrop-filter: blur(16px);
    border: 1px solid rgba(255, 255, 255, 0.1);
    border-radius: 9999px;
    padding: 0.375rem 0.625rem;
    display: flex;
    align-items: center;
    gap: 0.75rem;
    box-shadow: 0 12px 32px rgba(0, 0, 0, 0.6);
    z-index: 100;
  }

  .hud-btn {
    background: rgba(255, 255, 255, 0.07);
    border: none;
    color: var(--text-primary);
    width: 2rem;
    height: 2rem;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.12s;
  }

  .hud-btn:disabled {
    opacity: 0.3;
    cursor: not-allowed;
  }

  .hud-btn:hover:not(:disabled) {
    background: rgba(255, 255, 255, 0.12);
  }

  .hud-center {
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .hud-page {
    background: transparent;
    border: none;
    color: var(--text-primary);
    font-size: 0.8125rem;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
    cursor: pointer;
    padding: 0.125rem 0.375rem;
  }

  .hud-form {
    display: flex;
  }

  .hud-input {
    width: 2.75rem;
    background: rgba(0, 0, 0, 0.4);
    border: 1px solid var(--border);
    border-radius: 0.25rem;
    color: white;
    text-align: center;
    font-size: 0.8125rem;
    padding: 0.125rem;
  }

  .hud-actions {
    display: flex;
    align-items: center;
    gap: 0.25rem;
    border-left: 1px solid rgba(255, 255, 255, 0.1);
    border-right: 1px solid rgba(255, 255, 255, 0.1);
    padding: 0 0.5rem;
  }

  .hud-action {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    padding: 0.375rem;
    display: flex;
    align-items: center;
    cursor: pointer;
    border-radius: 0.375rem;
    transition:
      color 0.12s,
      background 0.12s;
  }

  .hud-action:hover {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.06);
  }

  /* ── Mobile search ── */
  .mobile-search-cancel {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    font-size: 0.875rem;
    font-weight: 500;
    cursor: pointer;
    padding: 0.25rem;
    white-space: nowrap;
  }

  /* ── Responsive ── */
  .desktop-only {
    display: flex;
  }
  .mobile-only {
    display: none !important;
  }

  @media (max-width: 768px) {
    .desktop-only {
      display: none !important;
    }
    .mobile-only {
      display: flex !important;
    }

    .modal {
      padding: 0;
    }

    .modal-content {
      height: 100dvh;
      border-radius: 0;
      border: none;
    }

    .modal-header {
      padding: 0.625rem 0.875rem;
    }

    .search-wrapper {
      display: none;
    }

    .search-wrapper.mobile-active {
      display: flex;
      align-items: center;
      gap: 0.5rem;
      position: absolute;
      inset: 0;
      background: var(--surface);
      padding: 0.5rem 1rem;
      z-index: 10;
    }

    .search-wrapper.mobile-active .search-field {
      width: 100%;
      flex: 1;
    }

    .search-dropdown {
      top: 100%;
      left: 0;
      right: 0;
      width: 100%;
      border-radius: 0 0 0.75rem 0.75rem;
      border-top: none;
      max-height: calc(100dvh - 120px);
    }
  }
</style>
