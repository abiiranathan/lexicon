// DocumentSearch owns in-document search: debounced querying scoped to the
// open file, result dropdown state and navigation into results.

import { searchAPI } from "../httpclient";
import type { ModalPorts } from "./modal-types";
import type { PageNavigation } from "./page-navigation.svelte";

export class DocumentSearch {
  private ports: ModalPorts;
  private nav: PageNavigation;
  private searchDebounceTimer: ReturnType<typeof setTimeout> | undefined;

  // Search state — reset whenever the active file changes.
  searchQuery = $state("");
  searchResults = $state<{ results: SearchResult[] }>({ results: [] });
  isSearching = $state(false);
  searchError = $state<string | null>(null);
  showMobileSearch = $state(false);
  lastFileId = $state<number | null>(null);

  dropdownVisible = $derived(
    this.searchQuery.length > 0 &&
      (this.isSearching ||
        !!this.searchError ||
        this.searchResults.results.length > 0),
  );

  constructor(ports: ModalPorts, nav: PageNavigation) {
    this.ports = ports;
    this.nav = nav;

    // Reset search state when the viewed file changes so stale results
    // from a previous document are never shown in the current one.
    $effect(() => {
      const content = this.ports.content;
      if (!content) return;
      if (content.file_id !== this.lastFileId) {
        this.lastFileId = content.file_id;
        this.searchQuery = "";
        this.searchResults = { results: [] };
        this.searchError = null;
        this.isSearching = false;
      }
    });
  }

  highlightText = (text: string): string => {
    return text
      .replace(/<b>/g, "<mark>")
      .replace(/<\/b>/g, "</mark>")
      .replace(/\n/g, "<br/>")
      .replace(/<\/mark>\s*<mark>/g, " ");
  };

  runSearch = async (query: string): Promise<void> => {
    const content = this.ports.content;
    if (!query || !content || query.length < 2) {
      this.searchResults = { results: [] };
      this.searchError = null;
      return;
    }

    this.isSearching = true;
    this.searchError = null;

    try {
      this.searchResults = await searchAPI(query, {
        fileId: content.file_id,
      });
    } catch (error: unknown) {
      this.searchError = (error as Error).message;
      this.searchResults = { results: [] };
    } finally {
      this.isSearching = false;
    }
  };

  handleSearchInput = (e: Event): void => {
    const value = (e.currentTarget as HTMLInputElement).value;
    this.searchQuery = value;

    clearTimeout(this.searchDebounceTimer);

    if (!value || value.length < 2) {
      this.searchResults = { results: [] };
      this.searchError = null;
      return;
    }

    this.searchDebounceTimer = setTimeout(() => this.runSearch(value), 300);
  };

  handleSearchSubmit = (): void => {
    clearTimeout(this.searchDebounceTimer);
    this.runSearch(this.searchQuery);
  };

  clearSearch = (): void => {
    clearTimeout(this.searchDebounceTimer);
    this.searchQuery = "";
    this.searchResults = { results: [] };
    this.searchError = null;
  };

  handleResultClick = (result: SearchResult): void => {
    this.nav.navigateToPage(
      result.file_id,
      result.page_num,
      result.num_pages,
      result.file_name,
    );
    this.clearSearch();
    this.showMobileSearch = false;
  };
}
