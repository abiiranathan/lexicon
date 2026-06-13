<!-- SearchSection.svelte -->
<script lang="ts">
  import type { LocalStore } from "./lib/localstorage.svelte";

  type Props = {
    searchQuery: LocalStore<string>;
    currentTab: LocalStore<string>;
    onsubmit: () => void;
    onTabSwitch: (tab: string) => void;
  };

  let { searchQuery, currentTab, onsubmit, onTabSwitch }: Props = $props();

  // Initialize mutable local state with the store's current value
  let inputValue = $derived(searchQuery.value);

  // Keep local state in sync if the store changes externally
  $effect(() => {
    inputValue = searchQuery.value;
  });

  // Safely propagate input changes back to the store
  $effect(() => {
    searchQuery.set(inputValue);
  });
</script>

<section class="search-section">
  <div class="search-container">
    <div class="input-wrapper">
      <svg
        class="search-icon"
        width="18"
        height="18"
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
        class="search-input"
        placeholder="Search term or phrase..."
        bind:value={inputValue}
        onkeydown={(e: KeyboardEvent) => e.key === "Enter" && onsubmit()}
        autocomplete="off"
        spellcheck="false"
      />

      {#if inputValue}
        <button
          class="clear-btn"
          onclick={() => {
            inputValue = "";
          }}
          aria-label="Clear search"
          type="button"
        >
          <svg
            width="14"
            height="14"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="2.5"
          >
            <line x1="18" y1="6" x2="6" y2="18" />
            <line x1="6" y1="6" x2="18" y2="18" />
          </svg>
        </button>
      {/if}
    </div>

    <button
      class="search-button"
      onclick={onsubmit}
      aria-label="Execute search"
      type="button"
    >
      Search
    </button>
  </div>

  <div class="tabs" role="tablist">
    <button
      class="tab"
      class:active={currentTab.value === "search"}
      onclick={() => onTabSwitch("search")}
      role="tab"
      aria-selected={currentTab.value === "search"}
    >
      <svg
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
      Search
    </button>
    <button
      class="tab"
      class:active={currentTab.value === "files"}
      onclick={() => onTabSwitch("files")}
      role="tab"
      aria-selected={currentTab.value === "files"}
    >
      <svg
        width="14"
        height="14"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="2"
        aria-hidden="true"
      >
        <path
          d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"
        ></path>
      </svg>
      Library
    </button>
  </div>
</section>

<style>
  .search-section {
    background: rgba(17, 24, 39, 0.5);
    border: 1px solid var(--border);
    border-radius: 1rem;
    padding: 1.25rem 1.5rem;
    margin-bottom: 2rem;
    box-shadow: 0 4px 24px rgba(0, 0, 0, 0.25);
  }

  .search-container {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    margin-bottom: 1rem;
    width: 100%;
  }

  /* Anchors internal elements correctly within the boundaries of the input block */
  .input-wrapper {
    position: relative;
    flex: 1;
    display: flex;
    align-items: center;
    min-width: 0; /* Prevents flex items from overflowing container */
  }

  .search-icon {
    position: absolute;
    left: 1rem;
    color: var(--text-muted);
    pointer-events: none;
    flex-shrink: 0;
  }

  .search-input {
    width: 100%;
    padding: 0.8125rem 2.25rem 0.8125rem 2.75rem; /* Room for icon on left, clear button on right */
    background: rgba(0, 0, 0, 0.3);
    border: 1px solid var(--border);
    border-radius: 0.75rem;
    color: var(--text-primary);
    font-size: 0.9375rem;
    transition:
      border-color 0.15s,
      box-shadow 0.15s;
  }

  .search-input::placeholder {
    color: var(--text-muted);
  }

  .search-input:focus {
    outline: none;
    border-color: var(--primary);
    box-shadow: 0 0 0 3px rgba(79, 70, 229, 0.15);
  }

  .clear-btn {
    position: absolute;
    right: 0.75rem;
    background: transparent;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    padding: 0.25rem;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 0.25rem;
    transition: color 0.15s;
  }

  .clear-btn:hover {
    color: var(--text-secondary);
  }

  .search-button {
    flex-shrink: 0;
    padding: 0.8125rem 1.5rem;
    background: var(--primary);
    border: none;
    border-radius: 0.75rem;
    cursor: pointer;
    color: white;
    font-size: 0.875rem;
    font-weight: 600;
    letter-spacing: 0.01em;
    transition:
      background 0.15s,
      transform 0.1s;
  }

  .search-button:hover {
    background: var(--primary-hover);
  }

  .search-button:active {
    transform: scale(0.98);
  }

  .tabs {
    display: flex;
    gap: 0.375rem;
  }

  .tab {
    display: inline-flex;
    align-items: center;
    gap: 0.4375rem;
    padding: 0.4375rem 1rem;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 0.5rem;
    cursor: pointer;
    transition: all 0.15s ease;
    color: var(--text-muted);
    font-weight: 500;
    font-size: 0.875rem;
  }

  .tab.active {
    background: var(--primary-light);
    color: #a5b4fc;
    border-color: rgba(99, 102, 241, 0.3);
  }

  .tab:hover:not(.active) {
    background: rgba(255, 255, 255, 0.04);
    color: var(--text-secondary);
  }

  /* Mobile viewport optimizations */
  @media (max-width: 480px) {
    .search-section {
      padding: 1rem;
    }

    .search-input {
      padding: 0.75rem 2rem 0.75rem 2.25rem;
      font-size: 0.875rem;
    }

    .search-icon {
      left: 0.75rem;
      width: 16px;
      height: 16px;
    }

    .clear-btn {
      right: 0.5rem;
    }

    .search-button {
      padding: 0.75rem 1rem;
      font-size: 0.8125rem;
    }
  }
</style>
