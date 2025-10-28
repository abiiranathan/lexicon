const baseURL = import.meta.env.VITE_SERVER_URL as string;

export async function loadAllFiles(): Promise<FileType[]> {
    const url = `${baseURL}/api/list-files`
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