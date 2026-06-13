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

  // View mode: 'fit' for zoom/pan, 'scroll' for readable scrolling
  let viewMode = $state<"fit" | "scroll">("scroll");
  let searchQuery = $state("");
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

  let searchResults = $state<{ results: SearchResult[] }>({ results: [] });
  let isSearching = $state(false);
  let searchError = $state<string | null>(null);

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
  // Canvas rendering (logic intact)
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
  // Input handlers (logic intact)
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
  // Page navigation
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
  // Search
  // ---------------------------------------------------------------------------

  async function handleSearch(query: string): Promise<void> {
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
    } finally {
      isSearching = false;
    }
  }

  const handleResultClick = (result: SearchResult): void => {
    viewPage(
      result.file_id,
      result.page_num,
      result.num_pages,
      result.file_name,
    );
    searchResults = { results: [] };
    searchQuery = "";
    showMobileSearch = false;
  };

  function highlightText(text: string): string {
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
      <!-- Top header bar: persistent on desktop, slimmed on mobile -->
      <div class="modal-header">
        <div class="header-left">
          <button
            class="modal-close-btn mobile-only"
            onclick={onClose}
            aria-label="Close"
          >
            <svg
              width="24"
              height="24"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
            >
              <line x1="18" y1="6" x2="6" y2="18" />
              <line x1="6" y1="6" x2="18" y2="18" />
            </svg>
          </button>
          <h3 id="modal-title">{modalContent.title}</h3>
        </div>

        <!-- Desktop Controls Block -->
        <div class="desktop-controls-wrapper desktop-only">
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
                  width="18"
                  height="18"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
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
                  width="18"
                  height="18"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                >
                  <polyline points="9 18 15 12 9 6"></polyline>
                </svg>
              </button>
            </div>
          {/if}

          {#if imageLoaded}
            <button
              class="nav-button"
              onclick={rotateImage}
              aria-label="Rotate image 90 degrees"
              title="Rotate (Shift+R)"
            >
              <svg
                width="18"
                height="18"
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
              class="nav-button"
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
                  width="18"
                  height="18"
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
                  width="18"
                  height="18"
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

            {#if viewMode === "fit"}
              <div class="zoom-controls">
                <button
                  class="nav-button"
                  onclick={() => (scale = Math.max(0.5, scale - 0.2))}
                  aria-label="Zoom out"
                  title="Zoom out (-)"
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
                    <line x1="8" y1="11" x2="14" y2="11"></line>
                    <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
                  </svg>
                </button>

                <span class="zoom-level">
                  {Math.round(scale * 100)}%
                </span>

                <button
                  class="nav-button"
                  onclick={() => (scale = Math.min(5, scale + 0.2))}
                  aria-label="Zoom in"
                  title="Zoom in (+)"
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
                    <line x1="11" y1="8" x2="11" y2="14"></line>
                    <line x1="8" y1="11" x2="14" y2="11"></line>
                    <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
                  </svg>
                </button>
              </div>
            {/if}
          {/if}
        </div>

        <div class="header-right">
          <div class="search-wrapper" class:mobile-active={showMobileSearch}>
            <input
              type="text"
              class="search_book"
              placeholder="Search in file..."
              bind:value={searchQuery}
              onkeydown={(e: KeyboardEvent) => {
                if (e.key === "Enter") {
                  handleSearch(searchQuery);
                }
              }}
            />
            {#if showMobileSearch}
              <button
                class="mobile-search-close"
                onclick={() => {
                  showMobileSearch = false;
                  searchQuery = "";
                }}
              >
                Cancel
              </button>
            {/if}

            <!-- Search dropdown results -->
            {#if searchQuery.length > 0 && (isSearching || searchError || searchResults.results.length > 0)}
              <div class="search-results-dropdown">
                {#if isSearching}
                  <div class="result-message">Searching inside document...</div>
                {:else if searchError}
                  <div class="result-message error-message-dropdown">
                    Error: {searchError}
                  </div>
                {:else if searchResults.results.length === 0}
                  <div class="result-message">No results found</div>
                {:else}
                  <ul class="results-list">
                    {#each searchResults.results as result}
                      <!-- svelte-ignore a11y_click_events_have_key_events -->
                      <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
                      <li
                        class="search-result-item"
                        onclick={() => handleResultClick(result)}
                      >
                        <div class="result-header-row">
                          <span class="result-page">Page {result.page_num}</span
                          >
                        </div>
                        <p class="result-snippet">
                          {@html highlightText(result.snippet)}
                        </p>
                      </li>
                    {/each}
                  </ul>
                {/if}
              </div>
            {/if}
          </div>

          <button
            class="mobile-search-trigger mobile-only"
            onclick={() => (showMobileSearch = true)}
            aria-label="Search inside document"
          >
            <svg
              width="20"
              height="20"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
            >
              <circle cx="11" cy="11" r="8"></circle>
              <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
            </svg>
          </button>

          <button
            class="modal-close-btn desktop-only"
            onclick={onClose}
            aria-label="Close Dialog"
            title="Close (Esc)"
          >
            <svg
              width="24"
              height="24"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
            >
              <line x1="18" y1="6" x2="6" y2="18" />
              <line x1="6" y1="6" x2="18" y2="18" />
            </svg>
          </button>
        </div>
      </div>

      <!-- Main viewport for canvas -->
      <div class="modal-body" class:scroll-mode={viewMode === "scroll"}>
        {#if modalContent.error}
          <div class="error-message">
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
              <div class="loading-indicator">
                <div class="spinner"></div>
              </div>
            {/if}
          </div>
        {/if}
      </div>

      <!-- Mobile Floating Navigation / HUD Bar -->
      {#if imageLoaded && modalContent.num_pages > 1}
        <div class="mobile-hud mobile-only">
          <button
            class="hud-btn"
            onclick={viewPrevPage}
            disabled={modalContent.page === 1}
            aria-label="Previous page"
          >
            <svg
              width="20"
              height="20"
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
                />
              </form>
            {:else}
              <button class="hud-page-indicator" onclick={togglePageInput}>
                {modalContent.page} / {modalContent.num_pages}
              </button>
            {/if}
          </div>

          <div class="hud-actions">
            <button
              class="hud-action-btn"
              onclick={rotateImage}
              aria-label="Rotate view"
            >
              <svg
                width="18"
                height="18"
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
              class="hud-action-btn"
              onclick={toggleViewMode}
              aria-label="View mode toggle"
            >
              {#if viewMode === "scroll"}
                <svg
                  width="18"
                  height="18"
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
                  width="18"
                  height="18"
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
          >
            <svg
              width="20"
              height="20"
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
  .modal {
    position: fixed;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: rgba(3, 7, 18, 0.9);
    backdrop-filter: blur(8px);
    z-index: 9999;
    padding: 1.5rem;
  }

  .modal-content {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 1rem;
    max-width: 1400px;
    width: 100%;
    height: 100%;
    display: flex;
    flex-direction: column;
    color: var(--text-primary);
    box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.8);
    position: relative;
    overflow: hidden;
  }

  .modal-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 1rem 1.5rem;
    border-bottom: 1px solid var(--border);
    flex-shrink: 0;
    gap: 1.5rem;
    background: rgba(17, 24, 39, 0.6);
  }

  .header-left {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    min-width: 0;
    flex: 1;
  }

  .header-left h3 {
    margin: 0;
    font-size: 1rem;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .header-right {
    display: flex;
    align-items: center;
    gap: 1rem;
  }

  .desktop-controls-wrapper {
    display: flex;
    align-items: center;
    gap: 2rem;
  }

  /* Shared HUD/Nav element layouts */
  .page-navigation,
  .zoom-controls {
    display: flex;
    align-items: center;
    gap: 0.25rem;
    padding: 0.25rem;
    background: rgba(0, 0, 0, 0.2);
    border-radius: 0.5rem;
    border: 1px solid var(--border);
  }

  .nav-button {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    cursor: pointer;
    padding: 0.375rem;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 0.375rem;
    transition: all 0.15s;
  }

  .nav-button:hover:not(:disabled) {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary);
  }

  .nav-button:disabled {
    opacity: 0.3;
    cursor: not-allowed;
  }

  .page-indicator,
  .zoom-level {
    font-size: 0.8125rem;
    color: var(--text-primary);
    padding: 0.25rem 0.5rem;
    font-weight: 500;
    min-width: 3.5rem;
    text-align: center;
    font-variant-numeric: tabular-nums;
  }

  .page-indicator {
    background: transparent;
    border: none;
    cursor: pointer;
    border-radius: 0.375rem;
  }

  .page-indicator:hover {
    background: rgba(255, 255, 255, 0.05);
  }

  .page-jump-form {
    display: flex;
    align-items: center;
    gap: 0.25rem;
  }

  .page-jump-input {
    width: 2.75rem;
    padding: 0.125rem 0.25rem;
    font-size: 0.8125rem;
    background: var(--background);
    border: 1px solid var(--border);
    border-radius: 0.25rem;
    color: var(--text-primary);
    text-align: center;
  }

  .page-jump-input:focus {
    outline: none;
    border-color: var(--primary);
  }

  .page-max {
    font-size: 0.8125rem;
    color: var(--text-secondary);
  }

  .modal-close-btn {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    cursor: pointer;
    padding: 0.375rem;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 0.5rem;
    transition: all 0.15s;
    border: 1px solid transparent;
  }

  .modal-close-btn:hover {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.05);
    border-color: var(--border);
  }

  /* Inline Document Search styling */
  .search-wrapper {
    position: relative;
    width: 220px;
  }

  .search_book {
    width: 100%;
    outline: none;
    padding: 0.45rem 1rem;
    border-radius: 0.5rem;
    border: 1px solid var(--border);
    background: rgba(0, 0, 0, 0.25);
    color: var(--text-primary);
    font-size: 0.875rem;
    transition: all 0.15s;
  }

  .search_book:focus {
    border-color: var(--primary);
    background: rgba(0, 0, 0, 0.4);
    box-shadow: 0 0 0 2px rgba(79, 70, 229, 0.25);
  }

  .search-results-dropdown {
    position: absolute;
    top: calc(100% + 0.5rem);
    right: 0;
    width: 300px;
    z-index: 1000;
    background: #1f2937;
    border: 1px solid var(--border);
    border-radius: 0.75rem;
    box-shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.5);
    max-height: 350px;
    overflow-y: auto;
  }

  .results-list {
    list-style: none;
    padding: 0;
    margin: 0;
  }

  .search-result-item {
    padding: 0.75rem 1rem;
    cursor: pointer;
    border-bottom: 1px solid rgba(255, 255, 255, 0.05);
    transition: background 0.15s;
  }

  .search-result-item:hover {
    background: rgba(255, 255, 255, 0.04);
  }

  .result-header-row {
    margin-bottom: 0.25rem;
  }

  .result-page {
    font-size: 0.75rem;
    color: #818cf8;
    font-weight: 600;
  }

  .result-snippet {
    color: var(--text-secondary);
    margin: 0;
    line-height: 1.4;
    display: -webkit-box;
    -webkit-line-clamp: 3;
    line-clamp: 3;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }

  .result-snippet :global(mark) {
    background: rgba(245, 158, 11, 0.3);
    color: #f59e0b;
    border-radius: 2px;
    padding: 0 1px;
  }

  .result-message {
    padding: 1rem;
    text-align: center;
    font-size: 0.875rem;
    color: var(--text-secondary);
  }

  /* Viewport canvas workspace wrapper */
  .modal-body {
    flex: 1;
    overflow: hidden;
    padding: 0;
    min-height: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: #0f1319;
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
    padding: 2rem 0;
  }

  .page-canvas {
    display: block;
    box-shadow: 0 10px 30px rgba(0, 0, 0, 0.6);
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
    background: rgba(0, 0, 0, 0.8);
    padding: 1rem;
    border-radius: 50%;
  }

  .spinner {
    width: 1.5rem;
    height: 1.5rem;
    border: 2px solid rgba(255, 255, 255, 0.2);
    border-top-color: var(--primary);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  .error-message {
    background: rgba(239, 68, 68, 0.1);
    border: 1px solid rgba(239, 68, 68, 0.3);
    color: var(--error);
    padding: 1rem 1.5rem;
    border-radius: 0.5rem;
    font-size: 0.875rem;
  }

  /* Desktop and Mobile Responsiveness Controls styling */
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
      height: 100vh;
      border-radius: 0;
      border: none;
    }

    .modal-header {
      padding: 0.75rem 1rem;
      gap: 0.5rem;
    }

    /* Expanding search bar on mobile */
    .search-wrapper {
      display: none;
      width: 100%;
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

    .mobile-search-close {
      background: transparent;
      border: none;
      color: var(--text-secondary);
      font-size: 0.875rem;
      font-weight: 500;
      white-space: nowrap;
    }

    .search-results-dropdown {
      top: 100%;
      left: 0;
      right: 0;
      width: 100%;
      border-radius: 0 0 1rem 1rem;
      border-top: none;
      max-height: calc(100vh - 120px);
    }

    .mobile-search-trigger {
      background: transparent;
      border: none;
      color: var(--text-secondary);
      padding: 0.5rem;
      display: flex;
      align-items: center;
      justify-content: center;
    }

    /* Floating bottom HUD pill design */
    .mobile-hud {
      position: absolute;
      bottom: 1.5rem;
      left: 50%;
      transform: translateX(-50%);
      background: rgba(31, 41, 55, 0.85);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border: 1px solid var(--border);
      border-radius: 9999px;
      padding: 0.5rem 0.75rem;
      display: flex;
      align-items: center;
      gap: 1rem;
      box-shadow: 0 10px 25px rgba(0, 0, 0, 0.5);
      z-index: 100;
    }

    .hud-btn {
      background: rgba(255, 255, 255, 0.08);
      border: none;
      color: var(--text-primary);
      width: 2.25rem;
      height: 2.25rem;
      border-radius: 50%;
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
    }

    .hud-btn:disabled {
      opacity: 0.3;
    }

    .hud-center {
      display: flex;
      align-items: center;
      justify-content: center;
    }

    .hud-page-indicator {
      background: transparent;
      border: none;
      color: var(--text-primary);
      font-size: 0.875rem;
      font-weight: 600;
      white-space: nowrap;
    }

    .hud-form {
      display: flex;
    }

    .hud-input {
      width: 3rem;
      background: rgba(0, 0, 0, 0.3);
      border: 1px solid var(--border);
      border-radius: 0.25rem;
      color: white;
      text-align: center;
      font-size: 0.875rem;
      padding: 0.125rem;
    }

    .hud-actions {
      display: flex;
      align-items: center;
      gap: 0.5rem;
      border-left: 1px solid var(--border);
      border-right: 1px solid var(--border);
      padding: 0 0.75rem;
    }

    .hud-action-btn {
      background: transparent;
      border: none;
      color: var(--text-secondary);
      padding: 0.25rem;
      display: flex;
      align-items: center;
      justify-content: center;
    }
  }
</style>
