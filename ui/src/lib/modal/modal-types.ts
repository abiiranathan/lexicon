// Shared prop/port types for the modal. The component keeps destructured
// $props() for its template; classes receive live getters so they always
// read current values through Svelte's reactivity.

export type ModalProps = {
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

export type ModalPorts = {
  readonly content: ModalContentType | null;
  readonly isOpen: boolean;
  readonly onClose: () => void;
  readonly viewPage: (
    fileId: number,
    pageNum: number,
    numPages: number,
    fileName: string,
  ) => void;
};
