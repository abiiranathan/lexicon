<!-- Modal.svelte -->
<script lang="ts">
  import { ModalController } from "./lib/modal/modal-controller.svelte";
  import type { ModalProps } from "./lib/modal/modal-types";

  let { isOpen, onClose, modalContent, viewPage }: ModalProps = $props();

  const modal = new ModalController({
    get content() {
      return modalContent;
    },
    get isOpen() {
      return isOpen;
    },
    get onClose() {
      return onClose;
    },
    get viewPage() {
      return viewPage;
    },
  });
</script>

{#if isOpen && modalContent}
  <!-- svelte-ignore a11y_interactive_supports_focus -->
  <!-- svelte-ignore a11y_click_events_have_key_events -->
  <div
    class="modal"
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
                onclick={modal.nav.viewPrevPage}
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

              {#if modal.nav.showPageInput}
                <form
                  class="page-jump-form"
                  onsubmit={(e: Event) => {
                    e.preventDefault();
                    modal.nav.handlePageJump();
                  }}
                >
                  <input
                    type="number"
                    class="page-jump-input"
                    bind:value={modal.nav.pageInputValue}
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
                  onclick={modal.nav.togglePageInput}
                  title="Jump to page (g)"
                  aria-label="Jump to page"
                  type="button"
                >
                  {modalContent.page} / {modalContent.num_pages}
                </button>
              {/if}

              <button
                class="icon-btn"
                onclick={modal.nav.viewNextPage}
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

          {#if modal.canvas.imageLoaded}
            <!-- Text View Panel Toggle -->
            <button
              class="icon-btn"
              class:icon-btn--active={modal.text.renderTextSetting.value}
              onclick={() => modal.text.renderTextSetting.set(!modal.text.renderTextSetting.value)}
              aria-label={modal.text.renderTextSetting.value
                ? "Hide text panel"
                : "Show text panel"}
              title={modal.text.renderTextSetting.value
                ? "Hide text panel"
                : "Show text panel"}
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
                <polyline points="4 7 4 4 20 4 20 7"></polyline>
                <line x1="9" y1="20" x2="15" y2="20"></line>
                <line x1="12" y1="4" x2="12" y2="20"></line>
              </svg>
            </button>

            <!-- Rotate -->
            <button
              class="icon-btn"
              onclick={modal.canvas.rotateImage}
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
              class:icon-btn--active={modal.canvas.viewMode === "fit"}
              onclick={modal.canvas.toggleViewMode}
              aria-label={modal.canvas.viewMode === "scroll"
                ? "Switch to zoom mode"
                : "Switch to scroll mode"}
              title={modal.canvas.viewMode === "scroll"
                ? "Zoom mode (v)"
                : "Scroll mode (v)"}
              type="button"
            >
              {#if modal.canvas.viewMode === "scroll"}
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
            {#if modal.canvas.viewMode === "fit"}
              <div class="control-group">
                <button
                  class="icon-btn"
                  onclick={() => (modal.canvas.scale = Math.max(0.5, modal.canvas.scale - 0.2))}
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
                <span class="zoom-label">{Math.round(modal.canvas.scale * 100)}%</span>
                <button
                  class="icon-btn"
                  onclick={() => (modal.canvas.scale = Math.min(5, modal.canvas.scale + 0.2))}
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
          {#if modal.canvas.imageLoaded}
            <button
              class="icon-btn text-selection-toggle"
              class:icon-btn--active={modal.text.textSelectionMode}
              onclick={modal.toggleTextSelection}
              aria-pressed={modal.text.textSelectionMode}
              aria-label={modal.text.textSelectionMode
                ? "Stop selecting text"
                : "Select text on page"}
              title="Select text on page"
              type="button"
            >
              S
            </button>
            <button
              class="icon-btn annotation-toggle"
              class:icon-btn--active={modal.canvas.annotationMode}
              onclick={modal.toggleAnnotation}
              aria-pressed={modal.canvas.annotationMode}
              aria-label={modal.canvas.annotationMode ? "Stop annotating" : "Annotate page"}
              title={modal.canvas.annotationMode
                ? "Stop annotating"
                : "Annotate page (hold Shift while drawing for a horizontal line)"}
              type="button"
            >
              A
            </button>
            {#if modal.canvas.annotationMode}
              <button
                class="icon-btn annotation-undo"
                onclick={modal.canvas.undoAnnotation}
                disabled={(modal.canvas.annotations[modal.canvas.annotationKey() ?? ""] ?? [])
                  .length === 0}
                aria-label="Undo last annotation stroke"
                title="Undo last stroke"
                type="button"
              >
                Undo
              </button>
            {/if}
          {/if}

          <!-- In-document search widget -->
          <div class="search-wrapper" class:mobile-active={modal.search.showMobileSearch}>
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
                value={modal.search.searchQuery}
                oninput={modal.search.handleSearchInput}
                onkeydown={(e: KeyboardEvent) => {
                  if (e.key === "Enter") modal.search.handleSearchSubmit();
                  if (e.key === "Escape") modal.search.clearSearch();
                }}
                aria-label="Search inside document"
                spellcheck="false"
                autocomplete="off"
              />
              {#if modal.search.searchQuery}
                <button
                  class="search-clear"
                  onclick={modal.search.clearSearch}
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

            {#if modal.search.showMobileSearch}
              <button
                class="mobile-search-cancel"
                onclick={() => {
                  modal.search.showMobileSearch = false;
                  modal.search.clearSearch();
                }}
                type="button"
              >
                Cancel
              </button>
            {/if}

            <!-- Results dropdown -->
            {#if modal.search.dropdownVisible}
              <div
                class="search-dropdown"
                role="listbox"
                aria-label="Search results"
              >
                {#if modal.search.isSearching}
                  <div class="dropdown-status">
                    <div class="dropdown-spinner"></div>
                    Searching…
                  </div>
                {:else if modal.search.searchError}
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
                    {modal.search.searchError}
                  </div>
                {:else if modal.search.searchResults.results.length === 0}
                  <div class="dropdown-status">No results found</div>
                {:else}
                  <div class="dropdown-header">
                    {modal.search.searchResults.results.length}
                    {modal.search.searchResults.results.length === 1 ? "result" : "results"}
                  </div>
                  <ul class="dropdown-list">
                    {#each modal.search.searchResults.results as result (result.file_id + "-" + result.page_num)}
                      <!-- svelte-ignore a11y_click_events_have_key_events -->
                      <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
                      <li
                        class="dropdown-item"
                        onclick={() => modal.search.handleResultClick(result)}
                        role="option"
                        aria-selected="false"
                      >
                        <span class="dropdown-page">Page {result.page_num}</span
                        >
                        <p class="dropdown-snippet">
                          {@html modal.search.highlightText(result.snippet)}
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
            onclick={() => (modal.search.showMobileSearch = true)}
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
      <div class="modal-body" class:scroll-mode={modal.canvas.viewMode === "scroll"}>
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
            class:scroll-mode={modal.canvas.viewMode === "scroll"}
            class:text-selection-mode={modal.text.textSelectionMode}
            bind:this={modal.canvas.containerElement}
            onwheel={modal.canvas.handleWheel}
            onpointerdown={modal.canvas.handlePointerDown}
            onpointermove={modal.canvas.handlePointerMove}
            onpointerup={modal.canvas.handlePointerUp}
            onpointercancel={modal.canvas.handlePointerUp}
            onmousedown={modal.canvas.handleMouseDown}
            onmousemove={modal.canvas.handleMouseMove}
            onmouseup={modal.canvas.handleMouseUp}
            onmouseleave={modal.canvas.handleMouseUp}
            ontouchstart={modal.canvas.handleTouchStart}
            ontouchmove={modal.canvas.handleTouchMove}
            ontouchend={modal.canvas.handleTouchEnd}
          >
            <canvas
              bind:this={modal.canvas.canvasElement}
              class="page-canvas"
              class:annotation-mode={modal.canvas.annotationMode}
              class:dragging={modal.canvas.isDragging}
              class:fit-mode={modal.canvas.viewMode === "fit"}
            ></canvas>

            {#if modal.text.textSelectionMode && modal.text.textLayer}
              <div
                class="selectable-text-layer"
                class:annotation-disabled={modal.canvas.annotationMode}
                bind:this={modal.text.textLayerElement}
                style={modal.text.textLayerStyle()}
                aria-label="Selectable page text"
                oncopy={modal.text.handleSelectableTextCopy}
              >
                {#each modal.text.textLayerColumns as column, columnIndex (columnIndex)}
                  <div
                    class="selectable-column"
                    style={modal.text.selectableColumnStyle(column)}
                  >
                    {#each column.lines as line, lineIndex (lineIndex)}
                      <div
                        class="selectable-line"
                        style={modal.text.selectableLineStyle(column, line, lineIndex)}
                      >
                        {#each line.words as word, wordIndex (wordIndex)}{#if wordIndex > 0}<span
                              class="selectable-gap"
                              style={modal.text.selectableGapStyle(line, wordIndex)}
                              >{" "}</span
                            >{/if}<span
                            class="selectable-word"
                            data-text={word.text}
                            data-column={columnIndex}
                            data-left={word.l}
                            data-right={word.r}
                            data-bottom={word.b}
                            data-top={word.t}
                            style={modal.text.selectableWordStyle(word, line)}
                            ><span class="selectable-word-text"
                              >{word.text}</span
                            ></span
                          >{/each}
                      </div>
                    {/each}
                  </div>
                {/each}
              </div>
            {/if}
            {#if modal.text.textSelectionMode && (modal.text.textLayerLoading || modal.text.textLayerError)}
              <div class="text-layer-status" role="status">
                {#if modal.text.textLayerLoading}
                  Preparing selectable text…
                {:else}
                  Could not load selectable text: {modal.text.textLayerError}
                  <button
                    class="plain-text-btn"
                    onclick={() => (modal.text.textLayerReload += 1)}
                    type="button"
                  >
                    Retry
                  </button>
                {/if}
              </div>
            {/if}

            {#if !modal.canvas.imageLoaded}
              <div class="canvas-loading">
                <div class="canvas-spinner"></div>
              </div>
            {/if}
          </div>
          <!-- Reconstructed Plain Text View Overlay -->
          {#if modal.text.renderTextSetting.value && modal.text.pageText && !modal.canvas.annotationMode && !modal.text.textSelectionMode}
            <div class="plain-text-container">
              <div class="plain-text-header">
                <span class="plain-text-title"
                  >Page {modalContent.page} Text</span
                >
                <div class="plain-text-actions">
                  <button
                    class="plain-text-btn"
                    onclick={modal.text.copyToClipboard}
                    type="button"
                  >
                    {modal.text.copySuccess ? "Copied" : "Copy Text"}
                  </button>
                  <button
                    class="plain-text-btn plain-text-btn--close"
                    onclick={() => modal.text.renderTextSetting.set(false)}
                    type="button"
                  >
                    Dismiss
                  </button>
                </div>
              </div>
              <div class="plain-text-content">
                {modal.text.pageText}
              </div>
            </div>
          {/if}
        {/if}
      </div>

      <!-- ── Mobile HUD ── -->
      {#if modal.canvas.imageLoaded && modalContent.num_pages > 1}
        <div
          class="mobile-hud mobile-only"
          role="toolbar"
          aria-label="Page navigation"
        >
          <button
            class="hud-btn"
            onclick={modal.nav.viewPrevPage}
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
            {#if modal.nav.showPageInput}
              <form
                class="hud-form"
                onsubmit={(e: Event) => {
                  e.preventDefault();
                  modal.nav.handlePageJump();
                }}
              >
                <input
                  type="number"
                  class="hud-input"
                  bind:value={modal.nav.pageInputValue}
                  placeholder={modalContent.page.toString()}
                  min="1"
                  max={modalContent.num_pages}
                  aria-label="Go to page"
                />
              </form>
            {:else}
              <button class="hud-page" onclick={modal.nav.togglePageInput} type="button">
                {modalContent.page} / {modalContent.num_pages}
              </button>
            {/if}
          </div>

          <div class="hud-actions">
            <button
              class="hud-action"
              class:icon-btn--active={modal.text.renderTextSetting.value}
              onclick={() => modal.text.renderTextSetting.set(!modal.text.renderTextSetting.value)}
              aria-label="Toggle text overlay"
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
                <polyline points="4 7 4 4 20 4 20 7"></polyline>
                <line x1="9" y1="20" x2="15" y2="20"></line>
                <line x1="12" y1="4" x2="12" y2="20"></line>
              </svg>
            </button>
            <button
              class="hud-action"
              onclick={modal.canvas.rotateImage}
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
              onclick={modal.canvas.toggleViewMode}
              aria-label="Toggle view mode"
              type="button"
            >
              {#if modal.canvas.viewMode === "scroll"}
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
            onclick={modal.nav.viewNextPage}
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
    background: rgba(37, 36, 36, 0.3);
    border: 1px solid #4d5070;
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
    -webkit-line-clamp: 5;
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
    position: relative !important; /* Critical to anchor the absolute child overlay panel */
  }

  .modal-body.scroll-mode {
    overflow-y: auto;
    align-items: flex-start;
  }

  .modal-body:has(.plain-text-container) {
    overflow-y: hidden;
    background-color: black;
    .page-canvas {
      display: none;
    }
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

  .canvas-container.text-selection-mode {
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
  .page-canvas.annotation-mode,
  .page-canvas.fit-mode.annotation-mode {
    cursor: crosshair;
  }

  .annotation-toggle,
  .text-selection-toggle {
    min-width: 2rem;
    font-size: 0.9rem;
    font-weight: 700;
  }

  .selectable-text-layer {
    position: absolute;
    z-index: 3;
    transform-origin: center;
    pointer-events: auto;
    user-select: none;
    -webkit-user-select: none;
  }

  .selectable-text-layer * {
    -webkit-user-select: none;
    user-select: none;
  }

  .selectable-text-layer.annotation-disabled {
    pointer-events: none;
  }

  .selectable-word {
    position: relative;
    display: inline-block;
    vertical-align: top;
    overflow: visible;
    color: transparent;
    white-space: pre;
    cursor: text;
    user-select: text;
    -webkit-user-select: text;
  }

  .selectable-line {
    position: relative;
    display: block;
    box-sizing: border-box;
    max-width: 100%;
    white-space: pre;
    user-select: text;
    font-size: 0;
    -webkit-user-select: text;
  }

  .selectable-column {
    position: absolute;
    top: 0;
    bottom: 0;
    user-select: text;
    -webkit-user-select: text;
  }

  .selectable-word-text {
    color: transparent;
    font-family: Arial, sans-serif;
    font-weight: 400;
    line-height: inherit;
    letter-spacing: inherit;
    white-space: pre;
    user-select: text;
    -webkit-user-select: text;
  }

  /* Selectable inter-word spacer (replaces unpaintable margin gaps). */
  .selectable-gap {
    position: relative;
    display: inline-block;
    vertical-align: top;
    overflow: visible;
    color: transparent;
    white-space: pre;
    cursor: text;
    user-select: text;
    -webkit-user-select: text;
  }

  /* Uniform selection highlight: the inter-word gaps belong to the
     parent line/column/layer, not the word spans. Without this rule those
     gaps fall back to the UA default (deep skyblue) while words use the
     custom color, producing alternating blocks. */
  .selectable-text-layer::selection,
  .selectable-text-layer ::selection {
    color: transparent;
    background: rgba(59, 130, 246, 0.3);
  }

  .selectable-text-layer::-moz-selection,
  .selectable-text-layer ::-moz-selection {
    color: transparent;
    background: rgba(59, 130, 246, 0.3);
  }

  .selectable-text-layer :global(.end-of-content) {
    position: absolute;
    top: 100%;
    right: 0;
    bottom: 0;
    left: 0;
    z-index: -1;
    cursor: default;
    user-select: none;
    -webkit-user-select: none;
  }

  :global(.selectable-text-layer.selecting .end-of-content) {
    top: 0;
  }

  .text-layer-status {
    position: absolute;
    top: 0.75rem;
    left: 50%;
    z-index: 4;
    display: flex;
    align-items: center;
    gap: 0.5rem;
    max-width: calc(100% - 1.5rem);
    padding: 0.5rem 0.75rem;
    transform: translateX(-50%);
    border: 1px solid var(--border);
    border-radius: 0.5rem;
    background: rgba(17, 24, 39, 0.94);
    color: var(--text-primary);
    font-size: 0.75rem;
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.35);
  }

  .annotation-undo {
    font-size: 0.75rem;
  }

  /* ── Plain Text Overlay Panel ── */
  .plain-text-container {
    position: absolute !important;
    top: 1.25rem !important;
    left: 1.25rem !important;
    right: 1.25rem !important;
    bottom: 1.25rem !important;
    background-color: #0f172a !important; /* Force solid background color */
    border: 1px solid var(--border) !important;
    border-radius: 0.75rem !important;
    display: flex !important;
    flex-direction: column !important;
    z-index: 50 !important;
    box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.75) !important;
    overflow: hidden !important;
  }

  .plain-text-header {
    display: flex !important;
    align-items: center !important;
    justify-content: space-between !important;
    padding: 0.75rem 1.25rem !important;
    border-bottom: 1px solid var(--border) !important;
    background-color: #1e293b !important; /* solid Slate 800 */
    flex-shrink: 0 !important;
  }

  .plain-text-title {
    font-size: 0.8125rem !important;
    font-weight: 600 !important;
    color: var(--text-secondary) !important;
  }

  .plain-text-actions {
    display: flex !important;
    gap: 0.5rem !important;
  }

  .plain-text-btn {
    background-color: rgba(255, 255, 255, 0.08) !important;
    border: 1px solid var(--border) !important;
    color: var(--text-primary) !important;
    padding: 0.375rem 0.75rem !important;
    font-size: 0.75rem !important;
    font-weight: 500 !important;
    border-radius: 0.375rem !important;
    cursor: pointer !important;
    transition:
      background-color 0.12s,
      border-color 0.12s !important;
  }

  .plain-text-btn:hover {
    background-color: rgba(255, 255, 255, 0.15) !important;
    border-color: var(--text-muted) !important;
  }

  .plain-text-btn--close {
    background-color: rgba(239, 68, 68, 0.1) !important;
    border-color: rgba(239, 68, 68, 0.2) !important;
    color: #f87171 !important;
  }

  .plain-text-btn--close:hover {
    background-color: rgba(239, 68, 68, 0.2) !important;
    border-color: rgba(239, 68, 68, 0.4) !important;
  }

  .plain-text-content {
    flex: 1 !important;
    overflow-y: auto !important;
    padding: 1.5rem !important;
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas,
      monospace !important;
    font-size: 1.2rem !important;
    line-height: 1.7 !important;
    color: #18212a !important;
    white-space: pre-line !important;
    user-select: text !important;
    -webkit-user-select: text !important;
    background-color: white !important;
  }

  .plain-text-content :global(mark) {
    background-color: rgba(245, 158, 11, 0.3) !important;
    color: #fbbf24 !important;
    border-radius: 2px !important;
    padding: 0 2px !important;
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
