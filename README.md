# Lexicon — fast PDF indexer and semantic search engine

Lexicon indexes a PDF library into PostgreSQL (full-text search via the RUM
index), serves a Svelte single-page app, renders pages on demand, and exposes
word-level text layers (native PDF text, with Tesseract OCR fallback).

## Prerequisites

- CMake 3.20+, GCC/Clang, pkg-config, Node.js 18+
- PostgreSQL with the `rum` extension
- Tesseract + Leptonica dev packages and language data (`tesseract-ocr-eng`)
- libcurl and OpenSSL dev packages (OAuth token exchange, PKCE/session hashes)

Debian/Ubuntu:

```sh
sudo apt install cmake pkg-config postgresql postgresql-contrib \
  libtesseract-dev libleptonica-dev tesseract-ocr tesseract-ocr-eng \
  libcurl4-openssl-dev libssl-dev nodejs npm
```

## Configure

```sh
cp .env.example .env
# edit PGCONN (required) and, for sign-in/upload, the OAuth + admin values
```

Key variables (see `.env.example` for the full list):
| Variable | Purpose |
|---|---|
| `PGCONN` | Postgres connection string (required) |
| `GOOGLE_CLIENT_ID` / `GOOGLE_CLIENT_SECRET` | Enable Sign in with Google |
| `GITHUB_CLIENT_ID` / `GITHUB_CLIENT_SECRET` | Enable Sign in with GitHub |
| `APP_BASE_URL` | External origin, e.g. `https://lexicon.example.com` (required for OAuth) |
| `ADMIN_EMAILS` | Comma-separated login emails that become admins; everyone else gets 403 |
| `UPLOAD_DIR` | Where uploaded PDFs land (defaults to the library `--root`) |
| `UPLOAD_MAX_MB` | Per-file upload cap (default 200) |

## Build and run

```sh
# Backend
cmake -S . -B build && cmake --build build

# Frontend (VITE_SERVER_URL=http://localhost:8080 for dev, "" for same-origin prod)
cd ui && npm install && npm run build && cd ..

# Index a library, then serve
./build/lexicon index --root /path/to/pdfs
./build/lexicon --root /path/to/pdfs
```

The UI is served from `./ui/dist`. Search stays public; PDF upload, job
status, and file deletion require an admin session.

Notes:

- Values in `.env` take precedence over same-named environment variables.
  To point the server at a different database, pass `--pgconn` explicitly.
- The database schema is created automatically on first connect (also by a
  single `index` run), including the `users`, `sessions`, `oauth_states`
  and `upload_jobs` tables.

## OAuth setup

Redirect URIs must be registered exactly as
`<APP_BASE_URL>/api/auth/callback/<google|github>`:

- **Google**: Google Cloud Console → APIs & Services → Credentials → OAuth
  client ID (Web application) → Authorized redirect URIs.
- **GitHub**: Settings → Developer settings → OAuth Apps → New OAuth App →
  Authorization callback URL. Requested scopes: `read:user user:email`.

Apple Sign In is intentionally not supported yet (paid Developer account,
Service ID + domain verification, key ceremony).

## Admin upload flow

Admins (emails in `ADMIN_EMAILS`) sign in from the app header, then use the
upload card in the library tab: the file streams in 4 MiB chunks, is
validated (`%PDF` magic), and queued for background indexing
(`fork`+`exec` of the `index` subcommand, so indexer failures fail only the
job). Poll `GET /api/admin/upload/jobs/{job_id}` for status.

When serving from a VFS image (`--vfs`), uploads are imported into the image
at a root-level unique path (`vfs add` semantics via `vfs_import_fd`) and the
server's read handle is swapped lock-free afterwards — no restart needed.
`UPLOAD_DIR` is then only staging space (defaults to `./uploads`).

## Admin file deletion

`DELETE /api/admin/files/{file_id}` (admin only) removes the file record —
`pages` and `ocr_pages` rows follow via `ON DELETE CASCADE` — and deletes
the underlying PDF: from disk in host mode, from the image in VFS mode (with
a read-handle swap so subsequent renders stop seeing it). Stored paths
containing `..` are rejected before the database write; filesystem unlink
failures are logged but still report success since the database is the source
of truth for listings.

In the library tab, admins get a trash button per file (two-click confirm);
deleting a file closes its open reader, clears the search cache, and reloads
the file list.

## Layout

- `src/`, `include/` — C backend (`routes.c`, `pdf_indexer.c`, `ocr.c`,
  `auth.c`, `upload.c`, …). New endpoints go in topic modules with tests
  where logic is pure.
- `ui/src/` — Svelte 5 frontend. Component state lives in
  `ui/src/lib/*.svelte.ts` classes (`lib/modal/`, `lib/auth.svelte.ts`);
  `.svelte` files stay presentational.
- `scripts/` — backup/restore, DB init SQL, helpers.
- `docs/ocr.md` — selectable text + OCR details.
