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
  let query = $derived(searchQuery.value);

  $effect(() => {
    searchQuery.set(query);
  });
</script>

<section class="search-section">
  <div class="search-container">
    <input
      type="text"
      class="search-input"
      placeholder="Search term or phrase..."
      bind:value={query}
      onkeypress={(e: any) => e.key === "Enter" && onsubmit()}
      autocomplete="off"
    />
    <button
      class="search-button"
      onclick={onsubmit}
      aria-label="Execute search"
    >
      <svg
        width="18"
        height="18"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="2.5"
      >
        <circle cx="11" cy="11" r="8"></circle>
        <path d="m21 21-4.35-4.35"></path>
      </svg>
    </button>
  </div>

  <div class="tabs">
    <button
      class="tab"
      class:active={currentTab.value === "search"}
      onclick={() => onTabSwitch("search")}
    >
      Search
    </button>
    <button
      class="tab"
      class:active={currentTab.value === "files"}
      onclick={() => onTabSwitch("files")}
    >
      Library
    </button>
  </div>
</section>

<style>
  .search-section {
    background: rgba(17, 24, 39, 0.4);
    border: 1px solid var(--border);
    border-radius: 1rem;
    padding: 1.5rem;
    margin-bottom: 2rem;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.2);
  }

  .search-container {
    position: relative;
    margin-bottom: 1.25rem;
  }

  .search-input {
    width: 100%;
    padding: 0.875rem 3.5rem 0.875rem 1.25rem;
    background: rgba(0, 0, 0, 0.3);
    border: 1px solid var(--border);
    border-radius: 0.75rem;
    color: var(--text-primary);
    font-size: 1rem;
    transition: all 0.2s ease;
  }

  .search-input:focus {
    outline: none;
    border-color: var(--primary);
    box-shadow: 0 0 0 3px rgba(79, 70, 229, 0.15);
  }

  .search-button {
    position: absolute;
    right: 0.375rem;
    top: 50%;
    transform: translateY(-50%);
    background: var(--primary);
    border: none;
    width: 2.25rem;
    height: 2.25rem;
    border-radius: 0.5rem;
    cursor: pointer;
    transition: all 0.15s ease;
    color: white;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .search-button:hover {
    background: var(--primary-hover);
  }

  .tabs {
    display: flex;
    gap: 0.5rem;
  }

  .tab {
    padding: 0.5rem 1.25rem;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 0.5rem;
    cursor: pointer;
    transition: all 0.15s ease;
    color: var(--text-secondary);
    font-weight: 500;
    font-size: 0.875rem;
  }

  .tab.active {
    background: var(--primary-light);
    color: #a5b4fc;
    border-color: rgba(99, 102, 241, 0.3);
  }

  .tab:hover:not(.active) {
    background: rgba(255, 255, 255, 0.03);
    color: var(--text-primary);
  }
</style>
