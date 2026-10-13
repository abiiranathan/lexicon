const baseURL = import.meta.env.VITE_SERVER_URL;

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

export async function loadAllBooks(): Promise<BookListResult> {
    const res = await fetch(`${baseURL}/api/list-books`);
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

export type AuthUser = {
    id: number;
    email: string;
    provider: string;
    is_admin: boolean;
};

export type AuthProviders = {
    google: boolean;
    github: boolean;
};

export type UploadJob = {
    job_id: number;
    status: string;
    filename: string;
    total_bytes: number;
    received_bytes: number;
    file_id?: number;
    error?: string;
};

// Session cookies require credentials on auth/upload calls. Public search
// and render endpoints keep plain fetch so CORS wildcard keeps working.
function authed(path: string, init?: RequestInit): Promise<Response> {
    return fetch(`${baseURL}${path}`, { ...init, credentials: "include" });
}

async function authedJson<T>(path: string, init?: RequestInit): Promise<T> {
    const res = await authed(path, init);
    if (!res.ok) throw await extractError(res);
    return res.json() as Promise<T>;
}

export function authProviders(): Promise<AuthProviders> {
    return authedJson<AuthProviders>("/api/auth/providers");
}

export function authWhoami(): Promise<AuthUser> {
    return authedJson<AuthUser>("/api/auth/whoami");
}

export function authLogout(): Promise<{ ok: boolean }> {
    return authedJson<{ ok: boolean }>("/api/auth/logout", { method: "POST" });
}

export function authLoginUrl(provider: "google" | "github"): string {
    return `${baseURL}/api/auth/login/${provider}`;
}

export function uploadInit(
    filename: string,
    size_bytes: number,
): Promise<UploadJob> {
    return authedJson<UploadJob>("/api/admin/upload/init", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ filename, size_bytes }),
    });
}

export function uploadChunkUrl(job_id: number, offset: number): string {
    return `${baseURL}/api/admin/upload/chunk?job=${job_id}&offset=${offset}`;
}

export function uploadComplete(job_id: number): Promise<UploadJob> {
    return authedJson<UploadJob>(`/api/admin/upload/complete?job=${job_id}`, {
        method: "POST",
    });
}

export function uploadJobStatus(job_id: number): Promise<UploadJob> {
    return authedJson<UploadJob>(`/api/admin/upload/jobs/${job_id}`);
}

export function deleteFile(
    fileId: number,
): Promise<{ ok: boolean; file_id: number }> {
    return authedJson<{ ok: boolean; file_id: number }>(
        `/api/admin/files/${fileId}`,
        { method: "DELETE" },
    );
}
