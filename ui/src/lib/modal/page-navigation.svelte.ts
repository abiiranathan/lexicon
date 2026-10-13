// PageNavigation owns page state traversal: prev/next, jump-to-page input
// and scroll reset on navigation.

import type { ModalPorts } from "./modal-types";

export class PageNavigation {
  private ports: ModalPorts;
  private getScrollContainer: () => HTMLElement | null | undefined;

  pageInputValue = $state("");
  showPageInput = $state(false);

  constructor(
    ports: ModalPorts,
    getScrollContainer: () => HTMLElement | null | undefined,
  ) {
    this.ports = ports;
    this.getScrollContainer = getScrollContainer;
  }

  navigateToPage = (
    fileId: number,
    pageNum: number,
    numPages: number,
    fileName: string,
  ): void => {
    const scrollContainer = this.getScrollContainer();
    if (scrollContainer) scrollContainer.scrollTop = 0;
    this.ports.viewPage(fileId, pageNum, numPages, fileName);
  };

  viewPrevPage = (): void => {
    const content = this.ports.content;
    if (!content || content.page === 1) return;
    this.navigateToPage(
      content.file_id,
      content.page - 1,
      content.num_pages,
      content.filename,
    );
  };

  viewNextPage = (): void => {
    const content = this.ports.content;
    if (!content || content.page === content.num_pages) return;
    this.navigateToPage(
      content.file_id,
      content.page + 1,
      content.num_pages,
      content.filename,
    );
  };

  handlePageJump = (): void => {
    const content = this.ports.content;
    if (!content) return;

    const pageNum = parseInt(this.pageInputValue, 10);
    if (isNaN(pageNum) || pageNum < 1 || pageNum > content.num_pages) {
      this.pageInputValue = "";
      this.showPageInput = false;
      return;
    }

    this.navigateToPage(
      content.file_id,
      pageNum,
      content.num_pages,
      content.filename,
    );

    this.pageInputValue = "";
    this.showPageInput = false;
  };

  togglePageInput = (): void => {
    this.showPageInput = !this.showPageInput;
    if (this.showPageInput) {
      this.pageInputValue = "";
      setTimeout(() => {
        (
          document.querySelector(".page-jump-input") as HTMLInputElement
        )?.focus();
      }, 0);
    }
  };
}
