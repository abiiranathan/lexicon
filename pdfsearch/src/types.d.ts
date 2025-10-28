type FileType = {
    id: number;
    name: string;
    path: string;
    num_pages: number;
}

type SearchResult = {
    file_id: number;
    file_name: string;
    page_num: number;
    snippet: string;
    rank: number;
    num_pages: number;
}

type ModalContentType = {
    title: string;
    imageBlob?: Blob;
    error?: string;
    page: number;
    filename: string,
    file_id: number,
    num_pages: number;
};
