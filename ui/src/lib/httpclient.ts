const baseURL = (import.meta.env.VITE_SERVER_URL as string);

/**
 * Reads the response body once as text, then attempts JSON parsing to extract
 * a structured error message. Falls back to the raw text, then to the HTTP
 * status line if the body is empty.
 */
async function extractError(response: Response): Promise<Error> {
    const body = await response.text();
    try {
        const json = JSON.parse(body);
        return new Error(json.error ?? json.message ?? body);
    } catch {
        return new Error(body || `HTTP ${response.status} ${response.statusText}`);
    }
}

export async function loadAllFiles(params: FileSearchParams): Promise<FileListResult> {
    const searchParams = new URLSearchParams();

    if (params.limit) {
        searchParams.append("limit", params.limit.toString());
    }
    if (params.name) {
        searchParams.append("name", params.name);
    }
    if (params.page) {
        searchParams.append("page", params.page.toString());
    }

    const url = `${baseURL}/api/list-files?${searchParams.toString()}`;
    const res = await fetch(url);
    if (!res.ok) throw await extractError(res);
    return res.json();
}

export async function getFileByID(fileId: number): Promise<FileType> {
    const res = await fetch(`${baseURL}/api/list-files/${fileId}`);
    if (!res.ok) throw await extractError(res);
    return res.json();
}

export async function searchAPI(
    query: string,
    args?: { fileId?: number },
): Promise<{ results: SearchResult[] }> {
    const params = new URLSearchParams();
    params.set("q", query);

    if (args?.fileId) {
        params.set("file_id", args.fileId.toString());
    }

    const res = await fetch(`${baseURL}/api/search?${params.toString()}`);
    if (!res.ok) throw await extractError(res);
    return res.json();
}

export async function fetchPage(
    fileId: number,
    pageNum: number,
    renderParams: RenderParams,
): Promise<Blob> {
    const params = new URLSearchParams({
        format: renderParams.format,
        scale: renderParams.scale.toString(),
    });
    const res = await fetch(`${baseURL}/api/file/${fileId}/render-page/${pageNum}?${params.toString()}`);
    if (!res.ok) throw await extractError(res);
    return res.blob();
}

export async function fetchTextLayer(
    fileId: number,
    pageNum: number,
): Promise<TextLayerResult> {
    const res = await fetch(`${baseURL}/api/file/${fileId}/text-layer/${pageNum}`);
    if (!res.ok) throw await extractError(res);
    return res.json();
}

export async function fetchPageText(
    fileId: number,
    pageNum: number,
): Promise<string> {
    const res = await fetch(`${baseURL}/api/file/${fileId}/page/${pageNum}`);
    if (!res.ok) throw await extractError(res);
    const data: Page = await res.json();
    return data.text.replace(/^\uFEFF/, "").replace(/\uFFFE/g, "");
}
