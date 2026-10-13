// ModalController composes the modal's state classes and owns cross-cutting
// concerns: text/annotation mode coordination, keyboard shortcuts, URL sync
// and body scroll locking. The Svelte component stays presentation-only.

import { CanvasViewer } from "./canvas-viewer.svelte";
import { DocumentSearch } from "./document-search.svelte";
import type { ModalPorts } from "./modal-types";
import { PageNavigation } from "./page-navigation.svelte";
import { TextLayerState } from "./text-layer.svelte";

export class ModalController {
  private ports: ModalPorts;

  readonly canvas: CanvasViewer;
  readonly nav: PageNavigation;
  readonly search: DocumentSearch;
  readonly text: TextLayerState;

  constructor(ports: ModalPorts) {
    this.ports = ports;
    const canvas = new CanvasViewer(ports, {
      isTextSelectionActive: () => this.text.textSelectionMode,
    });
    const nav = new PageNavigation(
      ports,
      () => canvas.containerElement?.parentElement,
    );
    const search = new DocumentSearch(ports, nav);
    const text = new TextLayerState(ports, canvas);
    this.canvas = canvas;
    this.nav = nav;
    this.search = search;
    this.text = text;

    // Sync URL state with modal
    $effect(() => {
      const content = this.ports.content;
      if (this.ports.isOpen && content) {
        const params = new URLSearchParams(window.location.search);
        params.set("file", content.file_id.toString());
        params.set("page", content.page.toString());
        window.history.replaceState(
          {},
          "",
          `${window.location.pathname}?${params.toString()}`,
        );
      } else if (!this.ports.isOpen) {
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
      document.body.style.overflow = this.ports.isOpen ? "hidden" : "auto";
    });

    // Keyboard shortcuts
    $effect(() => {
      if (!this.ports.isOpen) return;

      document.addEventListener("keydown", this.onKeydown);
      return () => document.removeEventListener("keydown", this.onKeydown);
    });
  }

  toggleTextSelection = (): void => {
    this.text.textSelectionMode = !this.text.textSelectionMode;
    if (this.text.textSelectionMode) {
      this.canvas.annotationMode = false;
      this.canvas.activeAnnotationStroke = null;
    }
  };

  toggleAnnotation = (): void => {
    this.canvas.annotationMode = !this.canvas.annotationMode;
    this.text.textSelectionMode = false;
    this.canvas.activeAnnotationStroke = null;
  };

  private onKeydown = (e: KeyboardEvent): void => {
    const target = e.target as HTMLElement;
    if (
      target.tagName === "INPUT" &&
      !target.classList.contains("page-jump-input")
    ) {
      return;
    }

    switch (e.key) {
      case "Escape":
        if (this.nav.showPageInput) {
          this.nav.showPageInput = false;
          this.nav.pageInputValue = "";
        } else {
          this.ports.onClose();
        }
        break;
      case "ArrowLeft":
        e.preventDefault();
        this.nav.viewPrevPage();
        break;
      case "ArrowRight":
        e.preventDefault();
        this.nav.viewNextPage();
        break;
      case "g":
        if (!this.nav.showPageInput) {
          e.preventDefault();
          this.nav.togglePageInput();
        }
        break;
      case "r":
        e.preventDefault();
        if (e.shiftKey) this.canvas.rotateImage();
        else this.canvas.resetTransform();
        break;
      case "R":
        e.preventDefault();
        this.canvas.rotateImage();
        break;
      case "v":
        e.preventDefault();
        this.canvas.toggleViewMode();
        break;
      case "+":
      case "=":
        if (this.canvas.viewMode === "fit") {
          e.preventDefault();
          this.canvas.scale = Math.min(5, this.canvas.scale + 0.2);
        }
        break;
      case "-":
        if (this.canvas.viewMode === "fit") {
          e.preventDefault();
          this.canvas.scale = Math.max(0.5, this.canvas.scale - 0.2);
        }
        break;
    }
  };
}
