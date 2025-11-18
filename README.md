# Lexicon

A high-performance full-text search service for PDF documents with web-based UI. Built with C, PostgreSQL full-text search and AI integration, and an in-memory LRU cache for fast response times.

## Features

- **Full-text search** across PDF documents using PostgreSQL's `tsvector` indexing
- **Page-level granularity** with text extraction and rendering
- **Fast responses** via in-memory LRU cache (1024 entries, 5-minute TTL)
- **Multi-threaded** request handling with connection pooling
- **REST API** with CORS support
- **PNG rendering** of PDF pages on-demand
- **Pagination** for file listings

## Prerequisites

- **C23-compatible compiler** (GCC 14+ or Clang 16+)
- **PostgreSQL 12+** with `pg_trgm` and full-text search extensions
- **Required libraries:**
  - `libpq` (PostgreSQL client library)
  - `poppler-glib` (PDF rendering, depending on `pdf.h` implementation)
  - `yyjson` (JSON serialization)
  - Custom libraries: `pgconn`, `pulsar`, `solidc` under https://github.com/abiiranathan

## Building

```bash
# Compile the project (adjust compiler flags as needed)
mkdir build && cd build
cmake ..
make
```

## Database Setup

```bash
# Create a PostgreSQL database
createdb -U postgres pdfsearch
```

## Usage

### 1. Index PDF Files

Before running the server, index your PDF collection:

```bash
./lexicon index \
    --pgconn "postgresql://user:password@localhost/pdfsearch" \
    --root /path/to/pdf/directory \
    --min_pages 4

# Options:
#   --pgconn, -c   PostgreSQL connection URI (required)
#   --root, -r     Root directory containing PDFs (required)
#   --min_pages    Minimum pages in a PDF to be considered for indexing (default: 4)
#   --dryrun       Test without committing changes
```

**Example:**
```bash
./lexicon index \
    -c "postgres://localhost/pdfsearch" \
    -r ~/Documents/Books \
    --min_pages 10
```

The indexer will:
- Recursively scan the directory for PDF files
- Extract text from each page
- Store metadata and full-text searchable content
- Skip PDFs with fewer pages than `--min_pages`

### 2. Run the Server

```bash
./lexicon \
    --pgconn "postgresql://user:password@localhost/pdfsearch" \
    --port 8080 \
    --addr 0.0.0.0

# Options:
#   --pgconn, -c   PostgreSQL connection URI (required)
#   --port, -p     Server port (default: 8080)
#   --addr, -a     Bind address (default: 0.0.0.0)
```

**Example:**
```bash
./lexicon \
    -c "postgres://localhost/pdfsearch" \
    -p 3000 \
    -a 127.0.0.1
```

The server will:
- Create 8 database connection pools (one per worker thread)
- Initialize the response cache (1024 entries, 300-second TTL)
- Start serving requests on the specified port

## API Endpoints

### Search PDFs
```http
GET /api/search?q=machine+learning
```
Returns ranked search results with highlighted snippets.

**Response:**
```json
{
  "query": "machine learning",
  "count": 15,
  "results": [
    {
      "file_id": 42,
      "file_name": "AI_Textbook.pdf",
      "page_num": 127,
      "num_pages": 450,
      "snippet": "...introduction to <b>machine</b> <b>learning</b> algorithms...",
      "rank": 0.456
    }
  ]
}
```

### List Files
```http
GET /api/list-files?page=1&limit=25&name=neural
```

**Response:**
```json
{
  "page": 1,
  "limit": 25,
  "total_count": 142,
  "total_pages": 6,
  "has_next": true,
  "has_prev": false,
  "results": [
    {
      "id": 23,
      "name": "Neural_Networks.pdf",
      "path": "/books/ml/Neural_Networks.pdf",
      "num_pages": 320
    }
  ]
}
```

### Get File by ID
```http
GET /api/list-files/23
```

**Response:**
```json
{
  "id": 23,
  "name": "Neural_Networks.pdf",
  "path": "/books/ml/Neural_Networks.pdf",
  "num_pages": 320
}
```

### Get Page Text
```http
GET /api/file/23/page/15
```

**Response:**
```json
{
  "file_id": 23,
  "page_num": 15,
  "text": "Chapter 3: Backpropagation\n\nThe backpropagation algorithm..."
}
```

### Render Page as PNG
```http
GET /api/file/23/render-page/15
```

Returns PNG image with `Content-Type: image/png` and 1-hour cache header.

## Cache Configuration

The service uses an in-memory LRU cache with the following TTLs:

| Endpoint       | TTL   | Reasoning           |
| -------------- | ----- | ------------------- |
| Page text      | 5 min | Stable content      |
| File metadata  | 5 min | Rarely changes      |
| File listings  | 5 min | Moderate volatility |
| Search results | 1 min | Query-dependent     |

Adjust capacity and TTL based on your workload.

## Performance Tuning

### Database Indexes
The following indexes are automatically created:
```sql
-- GIN index for full-text search
CREATE INDEX idx_pages_text_vector ON pages USING GIN(text_vector);

-- B-tree indexes for lookups
CREATE INDEX idx_pages_file_id ON pages(file_id);
CREATE INDEX idx_pages_lookup ON pages(file_id, page_num);
```

### Cache Invalidation
When updating the database externally, clear the cache:
```c
// In your update/delete handlers
cache_clear(g_response_cache);  // Clear all

// Or invalidate specific entries
cache_invalidate(g_response_cache, key);
```

### Worker Threads
Adjust `NUM_WORKERS` in your build configuration (default: 8) based on CPU cores and expected load.

## Web Interface

The server serves a static web UI from `./ui/dist`:
```
http://localhost:8080/
```

Build the frontend separately (Svelte UI):
```bash
cd ui
npm install
npm run build  # Outputs to ./dist
```

## Troubleshooting

### "Connection failed at worker: X"
- Verify PostgreSQL is running: `pg_isready`
- Check connection string format: `postgresql://user:pass@host:port/dbname`
- Ensure user has CREATE/INSERT/SELECT permissions

### "Unable to create table for schema"
- Check PostgreSQL version supports `tsvector` (9.6+)
- Verify database exists: `psql -l | grep pdfsearch`

### Search returns no results
- Verify indexing completed: `SELECT COUNT(*) FROM pages;`
- Check search query format (uses `websearch_to_tsquery`)
- Minimum rank threshold is 0.005 (adjust in `pdf_search` function)

### High memory usage
- Reduce cache capacity: `init_response_cache(512, 300);`
- Check for memory leaks with valgrind: `valgrind --leak-check=full ./lexicon`

## License

MIT
