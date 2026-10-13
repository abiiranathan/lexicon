// TextLayerState owns selectable page text: fetching, line/column layout,
// super/subscript filtering, overlay styles, copy handling and the
// drag-selection tracking shim. Layout math reads CanvasViewer.

import { fetchPageText, fetchTextLayer } from "../httpclient";
import { useLocalStorage } from "../localstorage.svelte";
import type { CanvasViewer } from "./canvas-viewer.svelte";
import type { ModalPorts } from "./modal-types";

export type LayerWord = TextLayerResult["words"][number];
export type TextLine = {
  top: number;
  bottom: number;
  words: TextLayerResult["words"];
};
export type TextColumn = {
  left: number;
  right: number;
  lines: TextLine[];
};

const BASE_RATIO = 0.25;
const NARROW_RATIO = 0.25;
const CITE_RE = /^[\d,;:.·\-–—\[\]()]*\d[\d,;:.·\-–—\[\]()]*$/;

function lowerMedian(values: number[]): number {
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[(sorted.length - 1) >> 1];
}

export class TextLayerState {
  private ports: ModalPorts;
  private canvas: CanvasViewer;
  private measureCtx: CanvasRenderingContext2D | null = null;

  textSelectionMode = $state(false);
  textLayer = $state<TextLayerResult | null>(null);
  textLayerElement = $state<HTMLDivElement | null>(null);
  textLayerLoading = $state(false);
  textLayerError = $state<string | null>(null);
  textLayerReload = $state(0);

  renderTextSetting = useLocalStorage("render-text", true);
  pageText = $state<string>("");
  copySuccess = $state(false);

  textLayerColumns = $derived.by((): TextColumn[] => {
    const textLayer = this.textLayer;
    if (!textLayer) return [];
    const pageWidth = textLayer.width;

    const words = [...textLayer.words].sort((a, b) => {
      const centerDifference = (b.t + b.b - a.t - a.b) * 0.5;
      return centerDifference || a.l - b.l;
    });

    const lines: TextLine[] = [];

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
    const statsOf = (
      ws: LayerWord[],
    ): {
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

    const columns: TextColumn[] = Array.from(
      { length: columnCuts.length + 1 },
      (_, index) => {
        const left = index === 0 ? 0 : columnCuts[index - 1];
        const right =
          index === columnCuts.length ? pageWidth : columnCuts[index];
        return { left, right, lines: [] as TextLine[] };
      },
    );

    for (const line of lines) {
      const wordsByColumn = columns.map(() => [] as LayerWord[]);
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

  constructor(ports: ModalPorts, canvas: CanvasViewer) {
    this.ports = ports;
    this.canvas = canvas;

    $effect(() => {
      const reload = this.textLayerReload;
      void reload;
      if (
        !this.ports.isOpen ||
        !this.canvas.imageLoaded ||
        !this.textSelectionMode ||
        !this.ports.content
      ) {
        this.textLayer = null;
        this.textLayerLoading = false;
        this.textLayerError = null;
        return;
      }

      let active = true;
      this.textLayer = null;
      this.textLayerLoading = true;
      this.textLayerError = null;

      fetchTextLayer(this.ports.content.file_id, this.ports.content.page)
        .then((result) => {
          if (active) this.textLayer = result;
        })
        .catch((error: unknown) => {
          if (active) {
            this.textLayerError =
              error instanceof Error
                ? error.message
                : "Unable to load page text";
          }
        })
        .finally(() => {
          if (active) this.textLayerLoading = false;
        });

      return () => {
        active = false;
      };
    });

    $effect(() => {
      const layer = this.textLayerElement;
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
        span.parentElement?.insertBefore(
          end,
          forward ? span.nextSibling : span,
        );
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

    // Fetch reconstructed page text asynchronously
    $effect(() => {
      if (!this.ports.isOpen || !this.ports.content) {
        this.pageText = "";
        return;
      }

      const fileId = this.ports.content.file_id;
      const pageNum = this.ports.content.page;

      if (!this.renderTextSetting.value) {
        this.pageText = "";
        return;
      }

      let active = true;

      fetchPageText(fileId, pageNum)
        .then((data) => {
          if (active) {
            this.pageText = data;
          }
        })
        .catch(() => {
          if (active) {
            this.pageText = "";
          }
        });

      return () => {
        active = false;
      };
    });
  }

  textLayerStyle = (): string => {
    const layout = this.canvas.textLayerLayout();
    if (!layout) return "";
    return `left:${layout.left}px;top:${layout.top}px;width:${layout.width}px;height:${layout.height}px;transform:${layout.transform}`;
  };

  selectableWordStyle = (word: LayerWord, line: TextLine): string => {
    const textLayer = this.textLayer;
    if (!textLayer) return "";
    const layout = this.canvas.textLayerLayout();
    if (!layout) return "";

    const boxHeight = ((word.t - word.b) / textLayer.height) * layout.height;
    const boxWidth = ((word.r - word.l) / textLayer.width) * layout.width;
    const fontSize = Math.max(1, boxHeight);
    this.measureCtx ??= document.createElement("canvas").getContext("2d");
    let letterSpacing = 0;
    if (this.measureCtx) {
      this.measureCtx.font = `${fontSize}px Arial`;
      // Stretch (or compress) the text so its advance matches the PDF word
      // box exactly. Divide by the character count (not count - 1) because
      // browsers add letter-spacing after every glyph including the last one;
      // dividing by count - 1 overflows the box and the overlapping
      // translucent highlights paint darker seams between words.
      const naturalWidth = this.measureCtx.measureText(word.text).width;
      const characterCount = [...word.text].length;
      if (characterCount > 0 && naturalWidth > 0) {
        letterSpacing = (boxWidth - naturalWidth) / characterCount;
      }
    }

    const verticalOffset =
      ((line.top - word.t) / textLayer.height) * layout.height;
    return `top:${verticalOffset}px;width:${boxWidth}px;height:${boxHeight}px;font-size:${fontSize}px;line-height:${boxHeight}px;letter-spacing:${letterSpacing}px;`;
  };

  selectableLineStyle = (
    column: TextColumn,
    line: TextLine,
    index: number,
  ): string => {
    const textLayer = this.textLayer;
    if (!textLayer) return "";
    const layout = this.canvas.textLayerLayout();
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
  };

  selectableWordGapWidth = (line: TextLine, index: number): number => {
    const textLayer = this.textLayer;
    if (!textLayer || index === 0) return 0;
    const layout = this.canvas.textLayerLayout();
    if (!layout) return 0;
    const gap = line.words[index].l - line.words[index - 1].r;
    return Math.max(0, (gap / textLayer.width) * layout.width);
  };

  // Inter-word gaps are real selectable spacers, not margins: native
  // ::selection never paints margins, so margin gaps stay unpainted (white)
  // while words get the custom color. A single space stretched with
  // letter-spacing to exactly the PDF gap width paints continuously with the
  // same color as the words.
  selectableGapStyle = (line: TextLine, index: number): string => {
    const textLayer = this.textLayer;
    if (!textLayer) return "";
    const layout = this.canvas.textLayerLayout();
    if (!layout) return "";
    const gapWidth = this.selectableWordGapWidth(line, index);
    if (gapWidth <= 0) return "display:none;";
    const height =
      ((line.top - line.bottom) / textLayer.height) * layout.height;
    const fontSize = Math.max(1, height);
    this.measureCtx ??= document.createElement("canvas").getContext("2d");
    let letterSpacing = 0;
    if (this.measureCtx) {
      this.measureCtx.font = `${fontSize}px Arial`;
      letterSpacing = gapWidth - this.measureCtx.measureText(" ").width;
    }
    return `width:${gapWidth}px;height:${height}px;font-size:${fontSize}px;line-height:${height}px;letter-spacing:${letterSpacing}px;`;
  };

  selectableColumnStyle = (column: TextColumn): string => {
    const textLayer = this.textLayer;
    if (!textLayer) return "";
    const layout = this.canvas.textLayerLayout();
    if (!layout) return "";
    const left = (column.left / textLayer.width) * layout.width;
    const width =
      ((column.right - column.left) / textLayer.width) * layout.width;
    return `left:${left}px;width:${width}px;`;
  };

  handleSelectableTextCopy = (event: ClipboardEvent): void => {
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
  };

  copyToClipboard = (): void => {
    if (!this.pageText) return;

    navigator.clipboard
      .writeText(this.pageText)
      .then(() => {
        this.copySuccess = true;
        setTimeout(() => {
          this.copySuccess = false;
        }, 2000);
      })
      .catch((err) => {
        console.error("Failed to copy text:", err);
      });
  };
}
