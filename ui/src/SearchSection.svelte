<script lang="ts">
  import type { LocalStore } from "./lib/localstorage.svelte";

  type Props = {
    searchQuery: LocalStore<string>;
    currentTab: LocalStore<string>;
    onsubmit: () => void;
    onTabSwitch: (tab: string) => void;
  };

  let { searchQuery, currentTab, onsubmit, onTabSwitch }: Props = $props();
  let query = $state(searchQuery.value);

  $effect(() => {
    searchQuery.set(query);
  });
</script>

<section class="search-section">
  <div class="search-container">
    <input
      type="text"
      class="search-input"
      placeholder="Search through your documents..."
      bind:value={query}
      onkeypress={(e) => e.key === "Enter" && onsubmit()}
      autocomplete="off"
    />
    <!-- svelte-ignore a11y_consider_explicit_label -->
    <button class="search-button" onclick={onsubmit}>
      <svg
        width="20"
        height="20"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="2"
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
      Search Results
    </button>
    <button
      class="tab"
      class:active={currentTab.value === "files"}
      onclick={() => onTabSwitch("files")}
    >
      Browse Files
    </button>
  </div>
</section>

<style>
  .search-section {
    background: rgba(30, 41, 59, 0.8);
    backdrop-filter: blur(20px);
    border: 1px solid rgba(148, 163, 184, 0.2);
    border-radius: 1.5rem;
    padding: 2rem;
    margin-bottom: 2rem;
    box-shadow: 0 20px 25px -5px rgba(0, 0, 0, 0.3);
    animation: fadeInUp 0.6s ease-out 0.1s both;
  }

  .search-container {
    position: relative;
    margin-bottom: 1.5rem;
  }

  .search-input {
    width: 100%;
    padding: 1rem 3.5rem 1rem 1.5rem;
    background: var(--surface);
    border: 2px solid var(--border);
    border-radius: 1rem;
    color: var(--text-primary);
    font-size: 1.1rem;
    transition: all 0.3s ease;
  }

  .search-input:focus {
    outline: none;
    border-color: var(--primary);
    box-shadow: 0 0 0 3px rgba(37, 99, 235, 0.1);
  }

  .search-button {
    position: absolute;
    right: 0.5rem;
    top: 50%;
    transform: translateY(-50%);
    background: var(--primary);
    border: none;
    padding: 0.75rem;
    border-radius: 0.75rem;
    cursor: pointer;
    transition: all 0.3s ease;
    color: white;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .search-button:hover {
    background: var(--primary-hover);
    transform: translateY(-50%) scale(1.05);
  }

  .tabs {
    display: flex;
    gap: 0.5rem;
  }

  .tab {
    padding: 0.75rem 1.5rem;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 0.75rem;
    cursor: pointer;
    transition: all 0.3s ease;
    color: var(--text-secondary);
    font-weight: 500;
  }

  .tab.active {
    background: var(--primary);
    color: white;
    border-color: var(--primary);
  }

  .tab:hover:not(.active) {
    background: var(--surface-hover);
    border-color: var(--text-muted);
  }

  @media (max-width: 768px) {
    .search-section {
      padding: 1.5rem;
    }

    .tabs {
      flex-wrap: wrap;
    }
  }
</style>
