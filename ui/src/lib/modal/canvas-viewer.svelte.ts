// CanvasViewer owns the page-image canvas: loading, pan/zoom/rotation,
// annotations and all pointer/touch/wheel input. Public members used as
// template callbacks are arrow fields so `this` survives bare references.

import type { ModalPorts } from "./modal-types";

type AnnotationPoint = { x: number; y: number };
type AnnotationStroke = {
  points: AnnotationPoint[];
  width: number;
};

export class CanvasViewer {
  private ports: ModalPorts;
  private isTextSelectionActive: () => boolean;

  canvasElement = $state<HTMLCanvasElement | null>(null);
  containerElement = $state<HTMLDivElement | null>(null);
  canvasSize = $state({ width: 0, height: 0 });
  imageLoaded = $state(false);
  loadedImage = $state<HTMLImageElement | null>(null);
  annotations = $state<Record<string, AnnotationStroke[]>>({});
  annotationMode = $state(false);
  activeAnnotationStroke = $state<AnnotationStroke | null>(null);
  annotationStartY = 0;

  // View mode: default to 'fit' on small screens (< 760px) and 'scroll' on larger screens
  viewMode = $state<"fit" | "scroll">(
    typeof window !== "undefined" && window.innerWidth < 760 ? "fit" : "scroll",
  );

  // Pan and zoom state (only used in 'fit' mode)
  scale = $state(1);
  translateX = $state(0);
  translateY = $state(0);
  isDragging = $state(false);
  lastTouchDistance = $state(0);
  dragStartX = $state(0);
  dragStartY = $state(0);
  rotation = $state(0); // 0, 90, 180, 270

  isLandscapeRotation = $derived(
    this.rotation === 90 || this.rotation === 270,
  );

  constructor(
    ports: ModalPorts,
    opts: { isTextSelectionActive: () => boolean },
  ) {
    this.ports = ports;
    this.isTextSelectionActive = opts.isTextSelectionActive;

    $effect(() => {
      const content = this.ports.content;
      if (
        !content?.imageBlob ||
        !this.canvasElement ||
        !this.containerElement
      ) {
        this.imageLoaded = false;
        this.loadedImage = null;
        return;
      }

      this.scale = 1;
      this.translateX = 0;
      this.translateY = 0;
      this.rotation = 0;
      this.imageLoaded = false;
      this.loadedImage = null;

      const url = URL.createObjectURL(content.imageBlob);
      const img = new Image();

      img.onload = () => {
        this.activeAnnotationStroke = null;
        this.loadedImage = img;
        this.imageLoaded = true;
        URL.revokeObjectURL(url);
      };

      img.onerror = () => {
        URL.revokeObjectURL(url);
        this.imageLoaded = false;
        this.loadedImage = null;
      };

      img.src = url;
    });

    $effect(() => {
      if (!this.imageLoaded || !this.loadedImage) return;
      const _ = [
        this.scale,
        this.translateX,
        this.translateY,
        this.viewMode,
        this.rotation,
      ];
      void _;
      this.renderCanvas();
    });

    $effect(() => {
      if (!this.ports.isOpen || !this.containerElement) return;

      const observer = new ResizeObserver(() => {
        this.renderCanvas();
      });

      observer.observe(this.containerElement);

      return () => observer.disconnect();
    });
  }

  annotationKey = (): string | null => {
    const content = this.ports.content;
    return content ? `${content.file_id}:${content.page}` : null;
  };

  textLayerLayout(): {
    left: number;
    top: number;
    width: number;
    height: number;
    transform: string;
  } | null {
    if (!this.canvasElement || !this.loadedImage || !this.containerElement)
      return null;
    const canvasWidth = this.canvasSize.width || this.canvasElement.clientWidth;
    const canvasHeight =
      this.canvasSize.height || this.canvasElement.clientHeight;
    const imageWidth = this.isLandscapeRotation
      ? this.loadedImage.height
      : this.loadedImage.width;
    const imageHeight = this.isLandscapeRotation
      ? this.loadedImage.width
      : this.loadedImage.height;
    const aspect = imageWidth / imageHeight;
    const containerAspect = canvasWidth / canvasHeight;
    const drawWidth =
      this.viewMode === "scroll"
        ? canvasWidth
        : aspect > containerAspect
          ? canvasWidth
          : canvasHeight * aspect;
    const drawHeight =
      this.viewMode === "scroll"
        ? canvasHeight
        : aspect > containerAspect
          ? canvasWidth / aspect
          : canvasHeight;

    const pageWidth = this.isLandscapeRotation ? drawHeight : drawWidth;
    const pageHeight = this.isLandscapeRotation ? drawWidth : drawHeight;
    const left =
      this.canvasElement.offsetLeft + (canvasWidth - pageWidth) / 2;
    const top =
      this.canvasElement.offsetTop + (canvasHeight - pageHeight) / 2;
    const pageScale = this.viewMode === "fit" ? this.scale : 1;
    const panX = this.viewMode === "fit" ? this.translateX * this.scale : 0;
    const panY = this.viewMode === "fit" ? this.translateY * this.scale : 0;

    return {
      left,
      top,
      width: pageWidth,
      height: pageHeight,
      transform: `translate(${panX}px,${panY}px) scale(${pageScale}) rotate(${this.rotation}deg)`,
    };
  }

  renderCanvas(): void {
    if (!this.canvasElement || !this.containerElement || !this.loadedImage)
      return;

    const canvas = this.canvasElement;
    const container = this.containerElement;
    const img = this.loadedImage;

    const imgW = this.isLandscapeRotation ? img.height : img.width;
    const imgH = this.isLandscapeRotation ? img.width : img.height;
    const imgAspect = imgW / imgH;

    if (this.viewMode === "scroll") {
      const cssWidth = Math.max(container.clientWidth, 600);
      const cssHeight = cssWidth / imgAspect;

      const ctx = this.prepareContext(canvas, container, cssWidth, cssHeight);
      if (!ctx) return;

      ctx.fillStyle = "#ffffff";
      ctx.fillRect(0, 0, cssWidth, cssHeight);

      this.drawRotatedImage(
        ctx,
        img,
        cssWidth / 2,
        cssHeight / 2,
        cssWidth,
        cssHeight,
        this.rotation,
      );
    } else {
      const cssWidth = container.clientWidth;
      const cssHeight = container.clientHeight;

      const ctx = this.prepareContext(canvas, container, cssWidth, cssHeight);
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
      ctx.scale(this.scale, this.scale);
      ctx.translate(-cx + this.translateX, -cy + this.translateY);

      this.drawRotatedImage(ctx, img, cx, cy, drawWidth, drawHeight, this.rotation);

      ctx.restore();
    }
  }

  resetTransform = (): void => {
    this.scale = 1;
    this.translateX = 0;
    this.translateY = 0;
  };

  toggleViewMode = (): void => {
    this.viewMode = this.viewMode === "fit" ? "scroll" : "fit";
    if (this.viewMode === "fit") this.resetTransform();
  };

  rotateImage = (): void => {
    this.rotation = (this.rotation + 90) % 360;
  };

  undoAnnotation = (): void => {
    const key = this.annotationKey();
    if (!key) return;
    this.annotations[key]?.pop();
    this.activeAnnotationStroke = null;
    this.renderCanvas();
  };

  handleWheel = (e: WheelEvent): void => {
    if (this.viewMode !== "fit") return;
    e.preventDefault();
    this.scale = Math.max(0.5, Math.min(5, this.scale - e.deltaY * 0.001));
  };

  handlePointerDown = (e: PointerEvent): void => {
    if (!this.annotationMode || !e.isPrimary || e.button !== 0) return;
    const point = this.getCanvasPoint(e.clientX, e.clientY);
    const key = this.annotationKey();
    const img = this.loadedImage;
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
    this.annotations[key] = [...(this.annotations[key] ?? []), stroke];
    this.activeAnnotationStroke = stroke;
    this.annotationStartY = e.clientY;
    this.renderCanvas();
  };

  handlePointerMove = (e: PointerEvent): void => {
    if (!this.annotationMode || !this.activeAnnotationStroke) return;
    const point = this.getCanvasPoint(
      e.clientX,
      e.clientY,
      e.shiftKey ? this.annotationStartY : undefined,
    );
    const img = this.loadedImage;
    const key = this.annotationKey();
    if (!point || !img || !key) return;

    const strokes = this.annotations[key];
    const stroke = strokes?.[strokes.length - 1];
    if (!stroke) return;
    const last = stroke.points.at(-1);
    const nextPoint = {
      x: Math.max(0, Math.min(img.width, point.imageX)),
      y: Math.max(0, Math.min(img.height, point.imageY)),
    };
    if (!last || Math.hypot(nextPoint.x - last.x, nextPoint.y - last.y) >= 1) {
      stroke.points = [...stroke.points, nextPoint];
      this.renderCanvas();
    }
  };

  handlePointerUp = (e: PointerEvent): void => {
    if (this.activeAnnotationStroke) {
      this.activeAnnotationStroke = null;
      const target = e.currentTarget as HTMLDivElement;
      if (target.hasPointerCapture(e.pointerId)) {
        target.releasePointerCapture(e.pointerId);
      }
    }
  };

  handleMouseDown = (e: MouseEvent): void => {
    // Let the browser handle text selection anywhere inside the text layer,
    // including the gaps between words, instead of starting a pan drag.
    if (
      this.annotationMode ||
      (e.target as HTMLElement).closest(".selectable-text-layer")
    ) {
      return;
    }

    if (this.viewMode !== "fit") return;
    this.isDragging = true;
    this.dragStartX = e.clientX - this.translateX;
    this.dragStartY = e.clientY - this.translateY;
  };

  handleMouseMove = (e: MouseEvent): void => {
    if (this.annotationMode || this.viewMode !== "fit" || !this.isDragging)
      return;
    this.translateX = e.clientX - this.dragStartX;
    this.translateY = e.clientY - this.dragStartY;
  };

  handleMouseUp = (): void => {
    this.isDragging = false;
  };

  handleTouchStart = (e: TouchEvent): void => {
    if (this.annotationMode || this.isTextSelectionActive()) return;
    if (this.viewMode !== "fit") return;

    if (e.touches.length === 2) {
      e.preventDefault();
      this.lastTouchDistance = Math.hypot(
        e.touches[1].clientX - e.touches[0].clientX,
        e.touches[1].clientY - e.touches[0].clientY,
      );
    } else if (e.touches.length === 1) {
      this.isDragging = true;
      this.dragStartX = e.touches[0].clientX - this.translateX;
      this.dragStartY = e.touches[0].clientY - this.translateY;
    }
  };

  handleTouchMove = (e: TouchEvent): void => {
    if (this.annotationMode || this.isTextSelectionActive()) return;
    if (this.viewMode !== "fit") return;

    if (e.touches.length === 2) {
      e.preventDefault();
      const distance = Math.hypot(
        e.touches[1].clientX - e.touches[0].clientX,
        e.touches[1].clientY - e.touches[0].clientY,
      );
      if (this.lastTouchDistance > 0) {
        this.scale = Math.max(
          0.5,
          Math.min(5, this.scale + (distance - this.lastTouchDistance) * 0.01),
        );
      }
      this.lastTouchDistance = distance;
    } else if (e.touches.length === 1 && this.isDragging) {
      e.preventDefault();
      this.translateX = e.touches[0].clientX - this.dragStartX;
      this.translateY = e.touches[0].clientY - this.dragStartY;
    }
  };

  handleTouchEnd = (e: TouchEvent): void => {
    if (e.touches.length < 2) this.lastTouchDistance = 0;
    if (e.touches.length === 0) this.isDragging = false;
  };

  private prepareContext(
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
    this.canvasSize = { width: cssWidth, height: cssHeight };

    const ctx = canvas.getContext("2d", { alpha: false });
    if (!ctx) return null;

    ctx.scale(dpr, dpr);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = "high";

    return ctx;
  }

  private drawRotatedImage(
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

    const key = this.annotationKey();
    const strokes = key ? (this.annotations[key] ?? []) : [];
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

  private getCanvasPoint(
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
    if (!this.canvasElement || !this.containerElement || !this.loadedImage)
      return null;

    const rect = this.canvasElement.getBoundingClientRect();
    const canvasX = clientX - rect.left;
    const canvasY = (lockY ?? clientY) - rect.top;
    const img = this.loadedImage;
    const imgW = this.isLandscapeRotation ? img.height : img.width;
    const imgH = this.isLandscapeRotation ? img.width : img.height;
    const imgAspect = imgW / imgH;
    const cssWidth = rect.width;
    const cssHeight = rect.height;
    const containerAspect = cssWidth / cssHeight;
    const drawWidth =
      this.viewMode === "scroll"
        ? cssWidth
        : imgAspect > containerAspect
          ? cssWidth
          : cssHeight * imgAspect;
    const drawHeight =
      this.viewMode === "scroll"
        ? cssHeight
        : imgAspect > containerAspect
          ? cssWidth / imgAspect
          : cssHeight;
    const dw = this.isLandscapeRotation ? drawHeight : drawWidth;
    const dh = this.isLandscapeRotation ? drawWidth : drawHeight;
    const cx = cssWidth / 2;
    const cy = cssHeight / 2;
    const pointX =
      this.viewMode === "fit"
        ? (canvasX - cx) / this.scale - this.translateX
        : canvasX - cx;
    const pointY =
      this.viewMode === "fit"
        ? (canvasY - cy) / this.scale - this.translateY
        : canvasY - cy;
    const rad = (-this.rotation * Math.PI) / 180;
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
}
