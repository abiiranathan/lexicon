<!-- Modal.svelte -->
<script lang="ts">
  import { searchAPI, fetchPageText, fetchTextLayer } from "./lib/httpclient";
  import { useLocalStorage } from "./lib/localstorage.svelte";

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
  let canvasSize = $state({ width: 0, height: 0 });
  let imageLoaded = $state(false);
  let loadedImage = $state<HTMLImageElement | null>(null);
  type AnnotationPoint = { x: number; y: number };
  type AnnotationStroke = {
    points: AnnotationPoint[];
    width: number;
  };
  let annotations = $state<Record<string, AnnotationStroke[]>>({});
  let annotationMode = $state(false);
  let activeAnnotationStroke: AnnotationStroke | null = null;
  let annotationStartY = 0;
  let textSelectionMode = $state(false);
  let textLayer = $state<TextLayerResult | null>(null);
  let textLayerElement = $state<HTMLDivElement | null>(null);
  let textLayerColumns = $derived.by(() => {
    if (!textLayer) return [];
    const pageWidth = textLayer.width;

    const words = [...textLayer.words].sort((a, b) => {
      const centerDifference = (b.t + b.b - a.t - a.b) * 0.5;
      return centerDifference || a.l - b.l;
    });

    const lines: {
      top: number;
      bottom: number;
      words: TextLayerResult["words"];
    }[] = [];

    for (const word of words) {
      const line = lines[lines.length - 1];
      if (line) {
        const center = (line.top + line.bottom) * 0.5;
        const wordCenter = (word.t + word.b) * 0.5;
        const tolerance =
          Math.min(line.top - line.bottom, word.t - word.b) * 0.5;
        if (Math.abs(center - wordCenter) <= tolerance) {
          line.top = Math.max(line.top, word.t);
          line.bottom = Math.min(line.bottom, word.b);
          line.words.push(word);
          continue;
        }
      }
      lines.push({ top: word.t, bottom: word.b, words: [word] });
    }

    for (const line of lines) {
      line.words.sort((a, b) => a.l - b.l);
    }

    // Column cuts are detected on the original geometry below (cutLines):
    // removing markers first would merge the neighbouring gaps into wide
    // phantom gutters that forge false cuts, split a column mid-row and
    // scramble DOM order — lower rows then highlight while selecting row 1.
    const cutLines = lines.map((l) => ({
      top: l.top,
      bottom: l.bottom,
      words: [...l.words],
    }));

    // Ignore super/subscript words (e.g. citation numerals like 12, 13–18
    // or 5,19,23–41): left in place they split into stray lines that break
    // the margin-stacked layout and push following lines out of position, so
    // the next line's selection highlight is no longer shown (the text itself
    // is still selected), and they pollute copied text. A word is
    // super/subscript when it sits off the row baseline (raised or lowered),
    // at any size: markers are often full-size but raised, and clusters like
    // 5,19,23–41 defeat a pure size test because comma tails dip back down
    // to the baseline. Citation-shaped tokens (digits with , ; : . - – —
    // brackets) therefore get a strict box test, while baseline-aligned
    // numbers (19 patients, 1,000, table rows, B12) are kept.
    const BASE_RATIO = 0.25;
    const NARROW_RATIO = 0.25;
    const CITE_RE = /^[\d,;:.·\-–—\[\]()]*\d[\d,;:.·\-–—\[\]()]*$/;
    const lowerMedian = (values: number[]): number => {
      const sorted = [...values].sort((a, b) => a - b);
      return sorted[(sorted.length - 1) >> 1];
    };
    type LayerWord = TextLayerResult["words"][number];
    // Geometry reference for a row: plain (non-citation) words, so marker
    // boxes cannot poison the baseline — unless plain is only small
    // punctuation (e.g. a lone dash), in which case fall back to all words.
    const statsOf = (ws: LayerWord[]): {
      maxH: number;
      baseline: number;
      rowTop: number;
    } => {
      const plain = ws.filter((w) => !CITE_RE.test(w.text));
      const lineMaxH = Math.max(...ws.map((w) => w.t - w.b));
      const ref =
        plain.length > 0 &&
        Math.max(...plain.map((w) => w.t - w.b)) >= lineMaxH * 0.5
          ? plain
          : ws;
      const maxH = Math.max(...ref.map((w) => w.t - w.b));
      const tall = ref.filter(
        (w) => !(maxH > 0 && w.t - w.b < maxH * 0.7),
      );
      const use = tall.length > 0 ? tall : ref;
      return {
        maxH,
        baseline: lowerMedian(use.map((w) => w.b)),
        rowTop: lowerMedian(use.map((w) => w.t)),
      };
    };
    const bands = lines.map((l) => ({ top: l.top, bottom: l.bottom }));

    lines.forEach((line, index) => {
      const st = statsOf(line.words);
      const limit = st.maxH * BASE_RATIO;
      const hasPlain = line.words.some((w) => !CITE_RE.test(w.text));
      line.words = line.words.filter((w) => {
        if (!CITE_RE.test(w.text) || !hasPlain) {
          // Body text: bottom rests on the baseline; below it only with a
          // normal top (descenders); centred punctuation straddling the
          // baseline is kept too.
          return (
            (w.b <= st.baseline + limit && w.b >= st.baseline - limit) ||
            w.t >= st.rowTop - limit ||
            (w.b < st.baseline + limit && w.t > st.baseline - limit)
          );
        }
        // Citation candidate: keep only when the box sits exactly on the
        // row geometry (top may be short for small digits, but the bottom
        // must rest on the baseline — comma tails dipping down fail this).
        const h = w.t - w.b;
        const bOk = w.b <= st.baseline + limit && w.b >= st.baseline - limit;
        const tOk = w.t <= st.rowTop + limit && w.t >= st.rowTop - limit;
        return bOk && (tOk || !(st.maxH > 0 && h >= st.maxH * 0.7));
      });

      // Narrow debris lines (stray markers that grouped alone): drop them
      // when they strictly overlap a neighbouring row yet sit off its
      // baseline (this overlap is what breaks the layout), or when they are
      // small floaters just above a row (raised sup). Full-width rows,
      // touching-but-aligned rows (punctuation), distant small rows
      // (footnotes) and tight neighbouring rows are kept.
      const neighbourIndexes: number[] = [];
      if (index > 0) neighbourIndexes.push(index - 1);
      if (index < lines.length - 1) neighbourIndexes.push(index + 1);
      const ownH =
        line.words.length > 0
          ? Math.max(...line.words.map((w) => w.t - w.b))
          : 0;
      const lineWidth =
        line.words.length > 0
          ? Math.max(...line.words.map((w) => w.r)) - line.words[0].l
          : 0;
      if (line.words.length > 0 && lineWidth < pageWidth * NARROW_RATIO) {
        let refH = 0;
        for (const j of neighbourIndexes)
          refH = Math.max(refH, statsOf(lines[j].words).maxH);
        const smallLine = refH > 0 && ownH < refH * 0.7;
        for (const j of neighbourIndexes) {
          if (lines[j].words.length === 0) continue;
          const band = bands[j];
          const nst = statsOf(lines[j].words);
          const nLimit = nst.maxH * BASE_RATIO;
          const raised = line.bottom > nst.baseline + nLimit;
          const lowered = line.bottom < nst.baseline - nLimit;
          const overlaps = line.top > band.bottom && line.bottom < band.top;
          const dAbove = line.bottom - band.top;
          const nearAbove = dAbove > 0 && dAbove <= ownH * 0.5;
          if (
            (overlaps && (raised || lowered)) ||
            (smallLine && nearAbove && raised)
          ) {
            line.words = [];
            break;
          }
        }
      }
      if (line.words.length > 0) {
        line.top = Math.max(...line.words.map((w) => w.t));
        line.bottom = Math.min(...line.words.map((w) => w.b));
      }
    });
    for (let i = lines.length - 1; i >= 0; i--) {
      if (lines[i].words.length === 0) lines.splice(i, 1);
    }

    const gapCandidates: { center: number; lineIndex: number }[] = [];
    cutLines.forEach((line, lineIndex) => {
      for (let index = 1; index < line.words.length; index++) {
        const previous = line.words[index - 1];
        const current = line.words[index];
        const gap = current.l - previous.r;
        const lineHeight = Math.min(
          previous.t - previous.b,
          current.t - current.b,
        );
        if (gap >= Math.max(pageWidth * 0.02, lineHeight * 2)) {
          gapCandidates.push({
            center: (previous.r + current.l) * 0.5,
            lineIndex,
          });
        }
      }
    });

    gapCandidates.sort((a, b) => a.center - b.center);
    const candidateGroups: {
      center: number;
      centers: number[];
      lineIndexes: Set<number>;
    }[] = [];
    for (const candidate of gapCandidates) {
      const group = candidateGroups[candidateGroups.length - 1];
      if (group && candidate.center - group.center <= pageWidth * 0.05) {
        group.centers.push(candidate.center);
        group.lineIndexes.add(candidate.lineIndex);
        group.center =
          group.centers.reduce((sum, center) => sum + center, 0) /
          group.centers.length;
      } else {
        candidateGroups.push({
          center: candidate.center,
          centers: [candidate.center],
          lineIndexes: new Set([candidate.lineIndex]),
        });
      }
    }

    const minimumColumnLines = Math.max(3, Math.ceil(cutLines.length * 0.15));
    const columnCuts = candidateGroups
      .filter((group) => group.lineIndexes.size >= minimumColumnLines)
      .map((group) => group.center)
      .sort((a, b) => a - b)
      .filter(
        (cut, index, cuts) =>
          index === 0 || cut - cuts[index - 1] > pageWidth * 0.08,
      );

    const columns = Array.from(
      { length: columnCuts.length + 1 },
      (_, index) => {
        const left = index === 0 ? 0 : columnCuts[index - 1];
        const right =
          index === columnCuts.length ? pageWidth : columnCuts[index];
        return { left, right, lines: [] as typeof lines };
      },
    );

    for (const line of lines) {
      const wordsByColumn = columns.map(() => [] as typeof line.words);
      for (const word of line.words) {
        const wordCenter = (word.l + word.r) * 0.5;
        const columnIndex = columnCuts.findIndex((cut) => wordCenter < cut);
        wordsByColumn[columnIndex < 0 ? columns.length - 1 : columnIndex].push(
          word,
        );
      }
      wordsByColumn.forEach((columnWords, index) => {
        if (columnWords.length === 0) return;
        columns[index].lines.push({
          top: Math.max(...columnWords.map((word) => word.t)),
          bottom: Math.min(...columnWords.map((word) => word.b)),
          words: columnWords,
        });
      });
    }

    return columns.filter((column) => column.lines.length > 0);
  });
  let textLayerLoading = $state(false);
  let textLayerError = $state<string | null>(null);
  let textLayerReload = $state(0);

  // Text layer state
  let renderTextSetting = useLocalStorage("render-text", true);
  let pageText = $state<string>("");

  // View mode: default to 'fit' on small screens (< 760px) and 'scroll' on larger screens
  let viewMode = $state<"fit" | "scroll">(
    typeof window !== "undefined" && window.innerWidth < 760 ? "fit" : "scroll",
  );

  // Search state — reset whenever the active file changes.
  let searchQuery = $state("");
  let searchResults = $state<{ results: SearchResult[] }>({ results: [] });
  let isSearching = $state(false);
  let searchError = $state<string | null>(null);
  let searchDebounceTimer: ReturnType<typeof setTimeout> | undefined;
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

  // ---------------------------------------------------------------------------
  // Reset search state when the viewed file changes so stale results
  // from a previous document are never shown in the current one.
  // ---------------------------------------------------------------------------
  let lastFileId = $state<number | null>(null);

  $effect(() => {
    if (!modalContent) return;
    if (modalContent.file_id !== lastFileId) {
      lastFileId = modalContent.file_id;
      searchQuery = "";
      searchResults = { results: [] };
      searchError = null;
      isSearching = false;
    }
  });

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

  $effect(() => {
    const reload = textLayerReload;
    void reload;
    if (!isOpen || !imageLoaded || !textSelectionMode || !modalContent) {
      textLayer = null;
      textLayerLoading = false;
      textLayerError = null;
      return;
    }

    let active = true;
    textLayer = null;
    textLayerLoading = true;
    textLayerError = null;

    fetchTextLayer(modalContent.file_id, modalContent.page)
      .then((result) => {
        if (active) textLayer = result;
      })
      .catch((error: unknown) => {
        if (active) {
          textLayerError =
            error instanceof Error ? error.message : "Unable to load page text";
        }
      })
      .finally(() => {
        if (active) textLayerLoading = false;
      });

    return () => {
      active = false;
    };
  });

  $effect(() => {
    const layer = textLayerElement;
    if (!layer) return;

    const end = document.createElement("div");
    end.className = "end-of-content";
    layer.append(end);

    const park = (): void => {
      layer.classList.remove("selecting");
      layer.append(end);
    };

    const onMouseDown = (): void => {
      layer.classList.add("selecting");
    };

    const onMouseUp = (): void => park();

    const onSelectionChange = (): void => {
      if (!layer.classList.contains("selecting")) return;
      const selection = document.getSelection();
      if (!selection || selection.rangeCount === 0) return;

      const { anchorNode, focusNode } = selection;
      if (!anchorNode || !focusNode) return;

      const focusElement =
        focusNode instanceof Element ? focusNode : focusNode.parentElement;
      const span = focusElement?.closest<HTMLElement>(".selectable-word");
      if (!span || !layer.contains(span)) return;

      const forward =
        anchorNode === focusNode
          ? selection.anchorOffset <= selection.focusOffset
          : Boolean(
              anchorNode.compareDocumentPosition(focusNode) &
                Node.DOCUMENT_POSITION_FOLLOWING,
            );
      span.parentElement?.insertBefore(end, forward ? span.nextSibling : span);
    };

    layer.addEventListener("mousedown", onMouseDown);
    window.addEventListener("mouseup", onMouseUp);
    document.addEventListener("selectionchange", onSelectionChange);

    return () => {
      layer.removeEventListener("mousedown", onMouseDown);
      window.removeEventListener("mouseup", onMouseUp);
      document.removeEventListener("selectionchange", onSelectionChange);
      layer.classList.remove("selecting");
      end.remove();
    };
  });

  // Fetch text layer coordinates asynchronously
  $effect(() => {
    if (!isOpen || !modalContent) {
      pageText = "";
      return;
    }

    const fileId = modalContent.file_id;
    const pageNum = modalContent.page;

    if (!renderTextSetting.value) {
      pageText = "";
      return;
    }

    let active = true;

    fetchPageText(fileId, pageNum)
      .then((data) => {
        if (active) {
          pageText = data;
        }
      })
      .catch((err) => {
        if (active) {
          pageText = "";
        }
      });

    return () => {
      active = false;
    };
  });

  let copySuccess = $state(false);

  function copyToClipboard(): void {
    if (!pageText) return;

    navigator.clipboard
      .writeText(pageText)
      .then(() => {
        copySuccess = true;
        setTimeout(() => {
          copySuccess = false;
        }, 2000);
      })
      .catch((err) => {
        console.error("Failed to copy text:", err);
      });
  }

  // ---------------------------------------------------------------------------
  // Canvas rendering — pure image scaling and rotation
  // ---------------------------------------------------------------------------

  const isLandscapeRotation = $derived(rotation === 90 || rotation === 270);

  function textLayerLayout(): {
    left: number;
    top: number;
    width: number;
    height: number;
    transform: string;
  } | null {
    if (!canvasElement || !loadedImage || !containerElement) return null;
    const canvasWidth = canvasSize.width || canvasElement.clientWidth;
    const canvasHeight = canvasSize.height || canvasElement.clientHeight;
    const imageWidth = isLandscapeRotation
      ? loadedImage.height
      : loadedImage.width;
    const imageHeight = isLandscapeRotation
      ? loadedImage.width
      : loadedImage.height;
    const aspect = imageWidth / imageHeight;
    const containerAspect = canvasWidth / canvasHeight;
    const drawWidth =
      viewMode === "scroll"
        ? canvasWidth
        : aspect > containerAspect
          ? canvasWidth
          : canvasHeight * aspect;
    const drawHeight =
      viewMode === "scroll"
        ? canvasHeight
        : aspect > containerAspect
          ? canvasWidth / aspect
          : canvasHeight;

    const pageWidth = isLandscapeRotation ? drawHeight : drawWidth;
    const pageHeight = isLandscapeRotation ? drawWidth : drawHeight;
    const left = canvasElement.offsetLeft + (canvasWidth - pageWidth) / 2;
    const top = canvasElement.offsetTop + (canvasHeight - pageHeight) / 2;
    const pageScale = viewMode === "fit" ? scale : 1;
    const panX = viewMode === "fit" ? translateX * scale : 0;
    const panY = viewMode === "fit" ? translateY * scale : 0;

    return {
      left,
      top,
      width: pageWidth,
      height: pageHeight,
      transform: `translate(${panX}px,${panY}px) scale(${pageScale}) rotate(${rotation}deg)`,
    };
  }

  function textLayerStyle(): string {
    const layout = textLayerLayout();
    if (!layout) return "";
    return `left:${layout.left}px;top:${layout.top}px;width:${layout.width}px;height:${layout.height}px;transform:${layout.transform}`;
  }

  let textMeasureContext: CanvasRenderingContext2D | null = null;

  function selectableWordStyle(
    word: TextLayerResult["words"][number],
    line: (typeof textLayerColumns)[number]["lines"][number],
  ): string {
    if (!textLayer) return "";
    const layout = textLayerLayout();
    if (!layout) return "";

    const boxHeight = ((word.t - word.b) / textLayer.height) * layout.height;
    const boxWidth = ((word.r - word.l) / textLayer.width) * layout.width;
    const fontSize = Math.max(1, boxHeight);
    textMeasureContext ??= document.createElement("canvas").getContext("2d");
    let letterSpacing = 0;
    if (textMeasureContext) {
      textMeasureContext.font = `${fontSize}px Arial`;
      // Stretch (or compress) the text so its advance matches the PDF word
      // box exactly. Divide by the character count (not count - 1) because
      // browsers add letter-spacing after every glyph including the last one;
      // dividing by count - 1 overflows the box and the overlapping
      // translucent highlights paint darker seams between words.
      const naturalWidth = textMeasureContext.measureText(word.text).width;
      const characterCount = [...word.text].length;
      if (characterCount > 0 && naturalWidth > 0) {
        letterSpacing = (boxWidth - naturalWidth) / characterCount;
      }
    }

    const verticalOffset =
      ((line.top - word.t) / textLayer.height) * layout.height;
    return `top:${verticalOffset}px;width:${boxWidth}px;height:${boxHeight}px;font-size:${fontSize}px;line-height:${boxHeight}px;letter-spacing:${letterSpacing}px;`;
  }

  function selectableLineStyle(
    column: (typeof textLayerColumns)[number],
    line: (typeof textLayerColumns)[number]["lines"][number],
    index: number,
  ): string {
    if (!textLayer) return "";
    const layout = textLayerLayout();
    if (!layout) return "";
    const top =
      ((textLayer.height - line.top) / textLayer.height) * layout.height;
    const previous = column.lines[index - 1];
    const previousBottom = previous
      ? ((textLayer.height - previous.bottom) / textLayer.height) *
        layout.height
      : 0;
    const marginTop = Math.max(0, top - previousBottom);
    const height =
      ((line.top - line.bottom) / textLayer.height) * layout.height;
    const left =
      ((line.words[0].l - column.left) / textLayer.width) * layout.width;
    const lineWidth =
      ((line.words[line.words.length - 1].r - line.words[0].l) /
        textLayer.width) *
      layout.width;
    return `margin-top:${marginTop}px;margin-left:${left}px;height:${height}px;width:${lineWidth}px;`;
  }

  function selectableWordGapWidth(
    line: (typeof textLayerColumns)[number]["lines"][number],
    index: number,
  ): number {
    if (!textLayer || index === 0) return 0;
    const layout = textLayerLayout();
    if (!layout) return 0;
    const gap = line.words[index].l - line.words[index - 1].r;
    return Math.max(0, (gap / textLayer.width) * layout.width);
  }

  // Inter-word gaps are real selectable spacers, not margins: native
  // ::selection never paints margins, so margin gaps stay unpainted (white)
  // while words get the custom color. A single space stretched with
  // letter-spacing to exactly the PDF gap width paints continuously with the
  // same color as the words.
  function selectableGapStyle(
    line: (typeof textLayerColumns)[number]["lines"][number],
    index: number,
  ): string {
    if (!textLayer) return "";
    const layout = textLayerLayout();
    if (!layout) return "";
    const gapWidth = selectableWordGapWidth(line, index);
    if (gapWidth <= 0) return "display:none;";
    const height =
      ((line.top - line.bottom) / textLayer.height) * layout.height;
    const fontSize = Math.max(1, height);
    textMeasureContext ??= document.createElement("canvas").getContext("2d");
    let letterSpacing = 0;
    if (textMeasureContext) {
      textMeasureContext.font = `${fontSize}px Arial`;
      letterSpacing = gapWidth - textMeasureContext.measureText(" ").width;
    }
    return `width:${gapWidth}px;height:${height}px;font-size:${fontSize}px;line-height:${height}px;letter-spacing:${letterSpacing}px;`;
  }

  function selectableColumnStyle(
    column: (typeof textLayerColumns)[number],
  ): string {
    if (!textLayer) return "";
    const layout = textLayerLayout();
    if (!layout) return "";
    const left = (column.left / textLayer.width) * layout.width;
    const width =
      ((column.right - column.left) / textLayer.width) * layout.width;
    return `left:${left}px;width:${width}px;`;
  }

  function handleSelectableTextCopy(event: ClipboardEvent): void {
    const selection = window.getSelection();
    if (!selection || selection.isCollapsed || selection.rangeCount === 0)
      return;

    const range = selection.getRangeAt(0);
    const selectionRects = Array.from(range.getClientRects());

    type CopyWord = {
      text: string;
      column: number;
      left: number;
      right: number;
      bottom: number;
      top: number;
    };

    // Collect the words whose spans intersect the selection (and its rects).
    const words: CopyWord[] = Array.from(
      (event.currentTarget as HTMLElement).querySelectorAll<HTMLElement>(
        ".selectable-word",
      ),
    )
      .filter((element) => {
        if (!range.intersectsNode(element)) return false;
        if (selectionRects.length === 0) return true;
        const rect = element.getBoundingClientRect();
        return selectionRects.some(
          (selected) =>
            rect.left < selected.right &&
            rect.right > selected.left &&
            rect.top < selected.bottom &&
            rect.bottom > selected.top,
        );
      })
      .map((element) => ({
        text: (element.dataset.text ?? "").trim(),
        column: Number(element.dataset.column),
        left: Number(element.dataset.left),
        right: Number(element.dataset.right),
        bottom: Number(element.dataset.bottom),
        top: Number(element.dataset.top),
      }))
      .filter(
        (word) =>
          word.text.length > 0 &&
          Number.isFinite(word.column) &&
          Number.isFinite(word.left) &&
          Number.isFinite(word.right) &&
          Number.isFinite(word.bottom) &&
          Number.isFinite(word.top),
      );

    if (words.length === 0) return;

    // Keep each column in reading order before moving to the next column.
    words.sort((a, b) => a.column - b.column || b.top - a.top);

    const lines: { column: number; words: CopyWord[] }[] = [];
    for (const word of words) {
      const line = lines[lines.length - 1];
      if (line) {
        const ref = line.words[0];
        const tolerance =
          Math.min(ref.top - ref.bottom, word.top - word.bottom) * 0.5;
        if (
          line.column === word.column &&
          Math.abs(ref.top - word.top) <= tolerance
        ) {
          line.words.push(word);
          continue;
        }
      }
      lines.push({ column: word.column, words: [word] });
    }

    const closingPunct = /^[,.;:!?%)\]}»”’]+$/;
    const openingPunct = /^[(\[{«“‘]+$/;

    const lineTexts = lines.map(({ words: line }) => {
      // Reading order within a line is left-to-right.
      line.sort((a, b) => a.left - b.left);

      let out = "";
      let prev: CopyWord | undefined;
      for (const word of line) {
        if (prev) {
          const height = Math.max(1, word.top - word.bottom);
          const gap = word.left - prev.right;
          // No space before closing punctuation or after opening punctuation
          // when the glyphs are visually adjacent; otherwise separate words.
          const attach =
            (closingPunct.test(word.text) && gap < height * 0.3) ||
            (openingPunct.test(prev.text) && gap < height * 0.3);
          if (!attach) out += " ";
        }
        out += word.text;
        prev = word;
      }
      return out;
    });

    const copiedText = lineTexts.join("\n");
    if (copiedText && event.clipboardData) {
      event.preventDefault();
      event.clipboardData.setData("text/plain", copiedText);
    }
  }

  function toggleTextSelection(): void {
    textSelectionMode = !textSelectionMode;
    if (textSelectionMode) {
      annotationMode = false;
      activeAnnotationStroke = null;
    }
  }

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
    canvasSize = { width: cssWidth, height: cssHeight };

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

    const key = getAnnotationKey();
    const strokes = key ? (annotations[key] ?? []) : [];
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    ctx.strokeStyle = "#e11d48";
    for (const stroke of strokes) {
      if (stroke.points.length === 0) continue;
      ctx.beginPath();
      ctx.lineWidth = stroke.width;
      if (stroke.points.length === 1) {
        const point = stroke.points[0];
        ctx.arc(
          (point.x / img.width) * dw - dw / 2,
          (point.y / img.height) * dh - dh / 2,
          stroke.width / 2,
          0,
          2 * Math.PI,
        );
        ctx.fillStyle = ctx.strokeStyle;
        ctx.fill();
        continue;
      }
      ctx.moveTo(
        (stroke.points[0].x / img.width) * dw - dw / 2,
        (stroke.points[0].y / img.height) * dh - dh / 2,
      );
      for (const point of stroke.points.slice(1)) {
        ctx.lineTo(
          (point.x / img.width) * dw - dw / 2,
          (point.y / img.height) * dh - dh / 2,
        );
      }
      ctx.stroke();
    }

    ctx.restore();
  }

  function getAnnotationKey(): string | null {
    return modalContent ? `${modalContent.file_id}:${modalContent.page}` : null;
  }

  function getCanvasPoint(
    clientX: number,
    clientY: number,
    lockY?: number,
  ): {
    canvasX: number;
    canvasY: number;
    imageX: number;
    imageY: number;
    imageUnitsPerPixel: number;
  } | null {
    if (!canvasElement || !containerElement || !loadedImage) return null;

    const rect = canvasElement.getBoundingClientRect();
    const canvasX = clientX - rect.left;
    const canvasY = (lockY ?? clientY) - rect.top;
    const img = loadedImage;
    const imgW = isLandscapeRotation ? img.height : img.width;
    const imgH = isLandscapeRotation ? img.width : img.height;
    const imgAspect = imgW / imgH;
    const cssWidth = rect.width;
    const cssHeight = rect.height;
    const containerAspect = cssWidth / cssHeight;
    const drawWidth =
      viewMode === "scroll"
        ? cssWidth
        : imgAspect > containerAspect
          ? cssWidth
          : cssHeight * imgAspect;
    const drawHeight =
      viewMode === "scroll"
        ? cssHeight
        : imgAspect > containerAspect
          ? cssWidth / imgAspect
          : cssHeight;
    const dw = isLandscapeRotation ? drawHeight : drawWidth;
    const dh = isLandscapeRotation ? drawWidth : drawHeight;
    const cx = cssWidth / 2;
    const cy = cssHeight / 2;
    const pointX =
      viewMode === "fit" ? (canvasX - cx) / scale - translateX : canvasX - cx;
    const pointY =
      viewMode === "fit" ? (canvasY - cy) / scale - translateY : canvasY - cy;
    const rad = (-rotation * Math.PI) / 180;
    const rotatedX = pointX * Math.cos(rad) - pointY * Math.sin(rad);
    const rotatedY = pointX * Math.sin(rad) + pointY * Math.cos(rad);

    return {
      canvasX,
      canvasY,
      imageX: (rotatedX / dw + 0.5) * img.width,
      imageY: (rotatedY / dh + 0.5) * img.height,
      imageUnitsPerPixel: img.width / dw,
    };
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
      activeAnnotationStroke = null;
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
  // Input handlers — panning, zooming, rotation, keyboard bindings
  // ---------------------------------------------------------------------------

  function handleWheel(e: WheelEvent): void {
    if (viewMode !== "fit") return;
    e.preventDefault();
    scale = Math.max(0.5, Math.min(5, scale - e.deltaY * 0.001));
  }

  function handlePointerDown(e: PointerEvent): void {
    if (!annotationMode || !e.isPrimary || e.button !== 0) return;
    const point = getCanvasPoint(e.clientX, e.clientY);
    const key = getAnnotationKey();
    const img = loadedImage;
    if (
      !point ||
      !key ||
      !img ||
      point.imageX < 0 ||
      point.imageY < 0 ||
      point.imageX > img.width ||
      point.imageY > img.height
    ) {
      return;
    }

    e.preventDefault();
    (e.currentTarget as HTMLDivElement).setPointerCapture(e.pointerId);
    const stroke: AnnotationStroke = {
      points: [{ x: point.imageX, y: point.imageY }],
      width: 3 * point.imageUnitsPerPixel,
    };
    annotations[key] = [...(annotations[key] ?? []), stroke];
    activeAnnotationStroke = stroke;
    annotationStartY = e.clientY;
    renderCanvas();
  }

  function handlePointerMove(e: PointerEvent): void {
    if (!annotationMode || !activeAnnotationStroke) return;
    const point = getCanvasPoint(
      e.clientX,
      e.clientY,
      e.shiftKey ? annotationStartY : undefined,
    );
    const img = loadedImage;
    const key = getAnnotationKey();
    if (!point || !img || !key) return;

    const strokes = annotations[key];
    const stroke = strokes?.[strokes.length - 1];
    if (!stroke) return;
    const last = stroke.points.at(-1);
    const nextPoint = {
      x: Math.max(0, Math.min(img.width, point.imageX)),
      y: Math.max(0, Math.min(img.height, point.imageY)),
    };
    if (!last || Math.hypot(nextPoint.x - last.x, nextPoint.y - last.y) >= 1) {
      stroke.points = [...stroke.points, nextPoint];
      renderCanvas();
    }
  }

  function handlePointerUp(e: PointerEvent): void {
    if (activeAnnotationStroke) {
      activeAnnotationStroke = null;
      const target = e.currentTarget as HTMLDivElement;
      if (target.hasPointerCapture(e.pointerId)) {
        target.releasePointerCapture(e.pointerId);
      }
    }
  }

  function handleMouseDown(e: MouseEvent): void {
    // Let the browser handle text selection anywhere inside the text layer,
    // including the gaps between words, instead of starting a pan drag.
    if (
      annotationMode ||
      (e.target as HTMLElement).closest(".selectable-text-layer")
    ) {
      return;
    }

    if (viewMode !== "fit") return;
    isDragging = true;
    dragStartX = e.clientX - translateX;
    dragStartY = e.clientY - translateY;
  }

  function handleMouseMove(e: MouseEvent): void {
    if (annotationMode || viewMode !== "fit" || !isDragging) return;
    translateX = e.clientX - dragStartX;
    translateY = e.clientY - dragStartY;
  }

  function handleMouseUp(): void {
    isDragging = false;
  }

  function handleTouchStart(e: TouchEvent): void {
    if (annotationMode || textSelectionMode) return;
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
    if (annotationMode || textSelectionMode) return;
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

  function undoAnnotation(): void {
    const key = getAnnotationKey();
    if (!key) return;
    annotations[key]?.pop();
    activeAnnotationStroke = null;
    renderCanvas();
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

  const navigateToPage = (
    fileId: number,
    pageNum: number,
    numPages: number,
    fileName: string,
  ): void => {
    const scrollContainer = containerElement?.parentElement;
    if (scrollContainer) scrollContainer.scrollTop = 0;
    viewPage(fileId, pageNum, numPages, fileName);
  };

  const viewPrevPage = (): void => {
    if (!modalContent || modalContent.page === 1) return;
    navigateToPage(
      modalContent.file_id,
      modalContent.page - 1,
      modalContent.num_pages,
      modalContent.filename,
    );
  };

  const viewNextPage = (): void => {
    if (!modalContent || modalContent.page === modalContent.num_pages) return;
    navigateToPage(
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

    navigateToPage(
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

  function highlightText(text: string): string {
    return text
      .replace(/<b>/g, "<mark>")
      .replace(/<\/b>/g, "</mark>")
      .replace(/\n/g, "<br/>")
      .replace(/<\/mark>\s*<mark>/g, " ");
  }

  async function runSearch(query: string): Promise<void> {
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
      searchResults = { results: [] };
    } finally {
      isSearching = false;
    }
  }

  function handleSearchInput(e: Event): void {
    const value = (e.currentTarget as HTMLInputElement).value;
    searchQuery = value;

    clearTimeout(searchDebounceTimer);

    if (!value || value.length < 2) {
      searchResults = { results: [] };
      searchError = null;
      return;
    }

    searchDebounceTimer = setTimeout(() => runSearch(value), 300);
  }

  function handleSearchSubmit(): void {
    clearTimeout(searchDebounceTimer);
    runSearch(searchQuery);
  }

  function clearSearch(): void {
    clearTimeout(searchDebounceTimer);
    searchQuery = "";
    searchResults = { results: [] };
    searchError = null;
  }

  const handleResultClick = (result: SearchResult): void => {
    navigateToPage(
      result.file_id,
      result.page_num,
      result.num_pages,
      result.file_name,
    );
    clearSearch();
    showMobileSearch = false;
  };

  const dropdownVisible = $derived(
    searchQuery.length > 0 &&
      (isSearching || !!searchError || searchResults.results.length > 0),
  );
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
                onclick={viewPrevPage}
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
                  <span class="page-sep">/ {modalContent.num_pages}</span>
                </form>
              {:else}
                <button
                  class="page-indicator"
                  onclick={togglePageInput}
                  title="Jump to page (g)"
                  aria-label="Jump to page"
                  type="button"
                >
                  {modalContent.page} / {modalContent.num_pages}
                </button>
              {/if}

              <button
                class="icon-btn"
                onclick={viewNextPage}
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

          {#if imageLoaded}
            <!-- Text View Panel Toggle -->
            <button
              class="icon-btn"
              class:icon-btn--active={renderTextSetting.value}
              onclick={() => renderTextSetting.set(!renderTextSetting.value)}
              aria-label={renderTextSetting.value
                ? "Hide text panel"
                : "Show text panel"}
              title={renderTextSetting.value
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
              onclick={rotateImage}
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
              class:icon-btn--active={viewMode === "fit"}
              onclick={toggleViewMode}
              aria-label={viewMode === "scroll"
                ? "Switch to zoom mode"
                : "Switch to scroll mode"}
              title={viewMode === "scroll"
                ? "Zoom mode (v)"
                : "Scroll mode (v)"}
              type="button"
            >
              {#if viewMode === "scroll"}
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
            {#if viewMode === "fit"}
              <div class="control-group">
                <button
                  class="icon-btn"
                  onclick={() => (scale = Math.max(0.5, scale - 0.2))}
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
                <span class="zoom-label">{Math.round(scale * 100)}%</span>
                <button
                  class="icon-btn"
                  onclick={() => (scale = Math.min(5, scale + 0.2))}
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
          {#if imageLoaded}
            <button
              class="icon-btn text-selection-toggle"
              class:icon-btn--active={textSelectionMode}
              onclick={toggleTextSelection}
              aria-pressed={textSelectionMode}
              aria-label={textSelectionMode
                ? "Stop selecting text"
                : "Select text on page"}
              title="Select text on page"
              type="button"
            >
              S
            </button>
            <button
              class="icon-btn annotation-toggle"
              class:icon-btn--active={annotationMode}
              onclick={() => {
                annotationMode = !annotationMode;
                textSelectionMode = false;
                activeAnnotationStroke = null;
              }}
              aria-pressed={annotationMode}
              aria-label={annotationMode ? "Stop annotating" : "Annotate page"}
              title={annotationMode
                ? "Stop annotating"
                : "Annotate page (hold Shift while drawing for a horizontal line)"}
              type="button"
            >
              A
            </button>
            {#if annotationMode}
              <button
                class="icon-btn annotation-undo"
                onclick={undoAnnotation}
                disabled={(annotations[getAnnotationKey() ?? ""] ?? [])
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
          <div class="search-wrapper" class:mobile-active={showMobileSearch}>
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
                value={searchQuery}
                oninput={handleSearchInput}
                onkeydown={(e: KeyboardEvent) => {
                  if (e.key === "Enter") handleSearchSubmit();
                  if (e.key === "Escape") clearSearch();
                }}
                aria-label="Search inside document"
                spellcheck="false"
                autocomplete="off"
              />
              {#if searchQuery}
                <button
                  class="search-clear"
                  onclick={clearSearch}
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

            {#if showMobileSearch}
              <button
                class="mobile-search-cancel"
                onclick={() => {
                  showMobileSearch = false;
                  clearSearch();
                }}
                type="button"
              >
                Cancel
              </button>
            {/if}

            <!-- Results dropdown -->
            {#if dropdownVisible}
              <div
                class="search-dropdown"
                role="listbox"
                aria-label="Search results"
              >
                {#if isSearching}
                  <div class="dropdown-status">
                    <div class="dropdown-spinner"></div>
                    Searching…
                  </div>
                {:else if searchError}
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
                    {searchError}
                  </div>
                {:else if searchResults.results.length === 0}
                  <div class="dropdown-status">No results found</div>
                {:else}
                  <div class="dropdown-header">
                    {searchResults.results.length}
                    {searchResults.results.length === 1 ? "result" : "results"}
                  </div>
                  <ul class="dropdown-list">
                    {#each searchResults.results as result (result.file_id + "-" + result.page_num)}
                      <!-- svelte-ignore a11y_click_events_have_key_events -->
                      <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
                      <li
                        class="dropdown-item"
                        onclick={() => handleResultClick(result)}
                        role="option"
                        aria-selected="false"
                      >
                        <span class="dropdown-page">Page {result.page_num}</span
                        >
                        <p class="dropdown-snippet">
                          {@html highlightText(result.snippet)}
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
            onclick={() => (showMobileSearch = true)}
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
      <div class="modal-body" class:scroll-mode={viewMode === "scroll"}>
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
            class:scroll-mode={viewMode === "scroll"}
            class:text-selection-mode={textSelectionMode}
            bind:this={containerElement}
            onwheel={handleWheel}
            onpointerdown={handlePointerDown}
            onpointermove={handlePointerMove}
            onpointerup={handlePointerUp}
            onpointercancel={handlePointerUp}
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
              class:annotation-mode={annotationMode}
              class:dragging={isDragging}
              class:fit-mode={viewMode === "fit"}
            ></canvas>

            {#if textSelectionMode && textLayer}
              <div
                class="selectable-text-layer"
                class:annotation-disabled={annotationMode}
                bind:this={textLayerElement}
                style={textLayerStyle()}
                aria-label="Selectable page text"
                oncopy={handleSelectableTextCopy}
              >
                {#each textLayerColumns as column, columnIndex (columnIndex)}
                  <div
                    class="selectable-column"
                    style={selectableColumnStyle(column)}
                  >
                    {#each column.lines as line, lineIndex (lineIndex)}
                      <div
                        class="selectable-line"
                        style={selectableLineStyle(column, line, lineIndex)}
                      >
                        {#each line.words as word, wordIndex (wordIndex)}{#if wordIndex > 0}<span
                              class="selectable-gap"
                              style={selectableGapStyle(line, wordIndex)}
                              >{" "}</span
                            >{/if}<span
                            class="selectable-word"
                            data-text={word.text}
                            data-column={columnIndex}
                            data-left={word.l}
                            data-right={word.r}
                            data-bottom={word.b}
                            data-top={word.t}
                            style={selectableWordStyle(word, line)}
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
            {#if textSelectionMode && (textLayerLoading || textLayerError)}
              <div class="text-layer-status" role="status">
                {#if textLayerLoading}
                  Preparing selectable text…
                {:else}
                  Could not load selectable text: {textLayerError}
                  <button
                    class="plain-text-btn"
                    onclick={() => (textLayerReload += 1)}
                    type="button"
                  >
                    Retry
                  </button>
                {/if}
              </div>
            {/if}

            {#if !imageLoaded}
              <div class="canvas-loading">
                <div class="canvas-spinner"></div>
              </div>
            {/if}
          </div>
          <!-- Reconstructed Plain Text View Overlay -->
          {#if renderTextSetting.value && pageText && !annotationMode && !textSelectionMode}
            <div class="plain-text-container">
              <div class="plain-text-header">
                <span class="plain-text-title"
                  >Page {modalContent.page} Text</span
                >
                <div class="plain-text-actions">
                  <button
                    class="plain-text-btn"
                    onclick={copyToClipboard}
                    type="button"
                  >
                    {copySuccess ? "Copied" : "Copy Text"}
                  </button>
                  <button
                    class="plain-text-btn plain-text-btn--close"
                    onclick={() => renderTextSetting.set(false)}
                    type="button"
                  >
                    Dismiss
                  </button>
                </div>
              </div>
              <div class="plain-text-content">
                {pageText}
              </div>
            </div>
          {/if}
        {/if}
      </div>

      <!-- ── Mobile HUD ── -->
      {#if imageLoaded && modalContent.num_pages > 1}
        <div
          class="mobile-hud mobile-only"
          role="toolbar"
          aria-label="Page navigation"
        >
          <button
            class="hud-btn"
            onclick={viewPrevPage}
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
                  aria-label="Go to page"
                />
              </form>
            {:else}
              <button class="hud-page" onclick={togglePageInput} type="button">
                {modalContent.page} / {modalContent.num_pages}
              </button>
            {/if}
          </div>

          <div class="hud-actions">
            <button
              class="hud-action"
              class:icon-btn--active={renderTextSetting.value}
              onclick={() => renderTextSetting.set(!renderTextSetting.value)}
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
              onclick={rotateImage}
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
              onclick={toggleViewMode}
              aria-label="Toggle view mode"
              type="button"
            >
              {#if viewMode === "scroll"}
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
            onclick={viewNextPage}
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
