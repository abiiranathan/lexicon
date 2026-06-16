type RenderFormat = "png" | "jpg";

type RenderParams = {
    format: RenderFormat; // Default is JPEG because it is x2 smaller.
    scale: number; // 1.0 – 4.0 (default is 2.0 -> Good balance between quality and size)
};

type FileType = {
    id: number;
    name: string;
    path: string;
    num_pages: number;
}

type FileListResult = {
    page: number;
    limit: number;
    total_count: number;
    has_next: boolean;
    has_prev: boolean;
    total_pages: number;
    results: FileType[];
}

type FileSearchParams = {
    page: number;
    limit?: number;
    name?: string;
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
    filename: string;
    file_id: number;
    num_pages: number;
    renderParams: RenderParams;
};

type TextChar = {
    c: number; // codepoint
    l: number; // left
    r: number; // right
    b: number; // bottom
    t: number; // top
};

type TextLayerResult = {
    width: number;
    height: number;
    chars: TextChar[];
};


type Page = {
    file_id: number;
    page_num: number;
    text: string;
}