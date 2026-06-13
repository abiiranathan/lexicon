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
  // Canvas rendering
  //
  // All drawing goes through renderCanvas(). It is the single source of truth
  // for what appears on screen. Two concerns are kept strictly separate:
  //
  //   1. Sizing  — how big the canvas backing store and CSS box are.
  //   2. Drawing — painting the image into that sized context.
  //
  // DPR handling: the canvas backing store is always (CSS px × devicePixelRatio)
  // in each dimension. We set canvas.width/height in physical pixels, set
  // canvas.style.width/height in CSS pixels, then call ctx.scale(dpr, dpr)
  // ONCE so that every subsequent draw call works in CSS-pixel coordinates.
  // This produces physically sharp pixels on high-DPI screens.
  //
  // imageSmoothingQuality = "high" tells the browser to use a high-quality
  // downscaling filter (typically a Lanczos variant) when the source image is
  // larger than the draw destination — important for JPEG pages at 150 DPI
  // being displayed at screen scale.
  // ---------------------------------------------------------------------------

  /** Returns true when the current rotation swaps width and height axes. */
  const isLandscapeRotation = $derived(rotation === 90 || rotation === 270);

  /**
   * Sizes the canvas backing store to match the container at the current DPR
   * and configures the 2D context for crisp high-DPI rendering.
   * Returns the context ready for drawing, or null on failure.
   */
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

    // Map all subsequent draw calls from CSS pixels to physical pixels.
    ctx.scale(dpr, dpr);

    // High-quality downscaling filter — critical for JPEG source images
    // rendered at 150 DPI being displayed at a smaller CSS size.
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = "high";

    return ctx;
  }

  /**
   * Draws `img` into `ctx` centered on (`cx`, `cy`), rotated by `deg` degrees,
   * scaled to (`drawWidth` × `drawHeight`) in the rotated coordinate space.
   */
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

    // When rotated 90/270 the natural image axes are swapped relative to the
    // draw rectangle, so we pass drawHeight/drawWidth to drawImage instead.
    const dw = swapped ? drawHeight : drawWidth;
    const dh = swapped ? drawWidth : drawHeight;
    ctx.drawImage(img, -dw / 2, -dh / 2, dw, dh);

    ctx.restore();
  }

  /** Renders the current image into the canvas with all active transforms. */
  function renderCanvas(): void {
    if (!canvasElement || !containerElement || !loadedImage) return;

    const canvas = canvasElement;
    const container = containerElement;
    const img = loadedImage;

    // Logical image dimensions after accounting for rotation axis swap.
    const imgW = isLandscapeRotation ? img.height : img.width;
    const imgH = isLandscapeRotation ? img.width : img.height;
    const imgAspect = imgW / imgH;

    if (viewMode === "scroll") {
      // In scroll mode the canvas width is fixed to the container (min 600 px)
      // and the height follows the image aspect ratio. The user scrolls
      // vertically to read the page; no zoom/pan state is involved.
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
      // In fit mode the canvas fills the container exactly. The image is
      // letterboxed to fit, then zoom/pan/rotation transforms are applied
      // on top so the user can inspect details.
      const cssWidth = container.clientWidth;
      const cssHeight = container.clientHeight;

      const ctx = prepareContext(canvas, container, cssWidth, cssHeight);
      if (!ctx) return;

      ctx.fillStyle = "#ffffff";
      ctx.fillRect(0, 0, cssWidth, cssHeight);

      // Letterbox: fit image inside container preserving aspect ratio.
      const containerAspect = cssWidth / cssHeight;
      const drawWidth =
        imgAspect > containerAspect ? cssWidth : cssHeight * imgAspect;
      const drawHeight =
        imgAspect > containerAspect ? cssWidth / imgAspect : cssHeight;

      const cx = cssWidth / 2;
      const cy = cssHeight / 2;

      ctx.save();

      // Zoom and pan are applied around the canvas centre so that scaling
      // feels anchored to the middle of the viewport, not the top-left corner.
      ctx.translate(cx, cy);
      ctx.scale(scale, scale);
      ctx.translate(-cx + translateX, -cy + translateY);

      drawRotatedImage(ctx, img, cx, cy, drawWidth, drawHeight, rotation);

      ctx.restore();
    }
  }

  // Load image from blob whenever modalContent.imageBlob changes.
  $effect(() => {
    if (!modalContent?.imageBlob || !canvasElement || !containerElement) {
      imageLoaded = false;
      loadedImage = null;
      return;
    }

    // Reset transform state for the incoming page.
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
      // Initial render happens via the render effect below reacting to
      // imageLoaded becoming true; no explicit call needed here.
    };

    img.onerror = () => {
      URL.revokeObjectURL(url);
      imageLoaded = false;
      loadedImage = null;
    };

    img.src = url;
  });

  // Re-render whenever any piece of render state changes. Svelte 5 tracks
  // which $state variables are read inside an $effect, so listing them
  // explicitly here (rather than the `void x` hack) is both correct and
  // readable. renderCanvas() reads: loadedImage, viewMode, scale, translateX,
  // translateY, rotation, and isLandscapeRotation (derived from rotation).
  $effect(() => {
    if (!imageLoaded || !loadedImage) return;

    // Reading these here is what makes Svelte track them as dependencies of
    // this effect. Any change to any of them triggers a re-render.
    const _ = [scale, translateX, translateY, viewMode, rotation];
    void _;

    renderCanvas();
  });

  // Re-render on container resize using ResizeObserver, which fires only when
  // the element's size actually changes — more accurate than window 'resize'
  // (which misses element-level reflows) and avoids the cost of a global
  // listener when the modal is closed.
  $effect(() => {
    if (!isOpen || !containerElement) return;

    const observer = new ResizeObserver(() => {
      renderCanvas();
    });

    observer.observe(containerElement);

    return () => observer.disconnect();
  });

  // ---------------------------------------------------------------------------
  // Input handlers
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
              class="nav-button"
              onclick={rotateImage}
              aria-label="Rotate image 90 degrees"
              title="Rotate (Shift+R)"
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
                <path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8"
                ></path>
                <path d="M21 3v5h-5"></path>
              </svg>
            </button>
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
