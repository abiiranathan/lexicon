# Lexicon Docker Setup

This document describes how to build and run the Lexicon PDF indexing service using Docker.

## Prerequisites

- Docker Engine 20.10+ (with BuildKit support)
- Docker Compose 2.0+
- Your PDF directory accessible on the host machine

## Quick Start

### 1. Build the UI (one-time setup)

Before building the Docker image, ensure the Svelte frontend is built:

```bash
cd ui
npm install
npm run build
cd ..
```

### 2. Build and Run with Docker Compose

The easiest way to run the entire stack:

```bash
# Start everything (PostgreSQL + Lexicon)
docker-compose up -d

# View logs
docker-compose logs -f lexicon

# Stop everything
docker-compose down
```

### 3. Mount Your PDF Directory

Edit `docker-compose.yml` and update the volumes section under `lexicon` service:

```yaml
volumes:
  # Change ./pdfs to your actual PDF directory path
  - /path/to/your/pdfs:/pdfs:ro
```

The `:ro` flag mounts the directory as read-only for safety.

### 4. Build the Index

Once the services are running, build the search index:

```bash
docker-compose exec lexicon /app/lexicon \
  --pgconn "postgresql://lexicon:lexicon_secret@postgres:5432/lexicon?sslmode=disable" \
  index --root /pdfs --min_pages 4
```

To perform a dry-run first:

```bash
docker-compose exec lexicon /app/lexicon \
  --pgconn "postgresql://lexicon:lexicon_secret@postgres:5432/lexicon?sslmode=disable" \
  index --root /pdfs --min_pages 4 --dryrun

```

## Manual Docker Build

If you prefer to build without Docker Compose:

```bash
# Build the image
docker build -t lexicon:latest .

# Run PostgreSQL
docker run -d \
  --name lexicon-db \
  -e POSTGRES_DB=lexicon \
  -e POSTGRES_USER=lexicon \
  -e POSTGRES_PASSWORD=lexicon_secret \
  -v lexicon-data:/var/lib/postgresql/data \
  -p 5432:5432 \
  postgres:16-alpine

# Wait for PostgreSQL to be ready
sleep 5

# Run Lexicon
docker run -d \
  --name lexicon-app \
  --link lexicon-db:postgres \
  -p 8080:8080 \
  -v /path/to/your/pdfs:/pdfs:ro \
  lexicon:latest \
  --port 8080 \
  --addr 0.0.0.0 \
  --pgconn postgresql://lexicon:lexicon_secret@postgres:5432/lexicon
```

## Configuration

### Environment Variables

The following connection string can be customized via the `--pgconn` flag:

```
postgresql://[user]:[password]@[host]:[port]/[database]?sslmode=disable
```

### Ports

- **8080**: Lexicon web interface and API (configurable via `--port`)
- **5432**: PostgreSQL database

### Volumes

- `/pdfs`: Mount point for your PDF directory (read-only recommended)
- `postgres_data`: Persistent storage for PostgreSQL data

## Security Considerations

1. **Non-root User**: The container runs as user `lexicon` (UID 1000)
2. **Read-only PDFs**: Mount PDF directory as `:ro` to prevent accidental modifications
3. **Database Credentials**: Change the default PostgreSQL password in production
4. **Network Isolation**: Services communicate via a dedicated bridge network

## Health Checks

The container includes a built-in health check that verifies the web server is responding:

```bash
# Check container health
docker-compose ps

# Manual health check
curl http://localhost:8080/
```

## Troubleshooting

### Container won't start

Check logs for detailed error messages:
```bash
docker-compose logs lexicon
```

### Database connection issues

Verify PostgreSQL is healthy:
```bash
docker-compose exec postgres pg_isready -U lexicon
```

### Permission errors with PDFs

Ensure the mounted directory is readable by UID 1000:
```bash
# On host
ls -la /path/to/pdfs
# Files should be readable by 'other' or owned by UID 1000
```

### Memory issues with large PDFs

Increase Docker memory limits in Docker Desktop settings or via `docker-compose.yml`:

```yaml
services:
  lexicon:
    deploy:
      resources:
        limits:
          memory: 4G
```

## Production Deployment

For production use:

1. Change database credentials
2. Use Docker secrets for sensitive data
3. Enable SSL/TLS for PostgreSQL connections
4. Configure proper backup strategy for `postgres_data` volume
5. Use a reverse proxy (nginx, Traefik) for HTTPS termination
6. Set up monitoring and log aggregation
7. Consider using a managed PostgreSQL service

## Multi-Architecture Builds

To build for multiple architectures (e.g., ARM64 for Apple Silicon):

```bash
docker buildx build --platform linux/amd64,linux/arm64 -t lexicon:latest .
```

## Rebuilding After Code Changes

```bash
# Rebuild and restart
docker-compose up -d --build

# Or with no cache
docker-compose build --no-cache
docker-compose up -d
```
