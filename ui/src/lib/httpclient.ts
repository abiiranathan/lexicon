const baseURL = import.meta.env.VITE_SERVER_URL as string;


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
    const query = searchParams.toString();
    const url = `${baseURL}/api/list-files?${query}`
    const res = await fetch(url)
    const data = await res.json()
    return data
}

export async function getFileByID(fileId: number): Promise<FileType> {
    const response = await fetch(`${baseURL}/api/list-files/${fileId}`);
    const data = await response.json();
    return data
}

export async function searchAPI(query: string): Promise<SearchResult[]> {
    const url = `${baseURL}/api/search?q=${encodeURIComponent(query)}`
    const res = await fetch(url)
    const data = await res.json()
    return data
}

export async function fetchPage(fileId: number, pageNum: number) {
    const response = await fetch(`${baseURL}/api/file/${fileId}/render-page/${pageNum}`);
    if (response.ok) {
        const imageBlob = await response.blob();
        return imageBlob;
    } else {
        const error = await response.json();
        throw new Error(error.error || error);
    }
}