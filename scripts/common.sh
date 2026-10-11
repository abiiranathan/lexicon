# Shared configuration for backup and restore scripts
readonly BACKUP_DIR="${BACKUP_DIR:-${HOME}/.pg_backups}"
readonly DB_NAME="${DB_NAME:-}"
readonly DB_USER="${DB_USER:-postgres}"
readonly DB_HOST="${DB_HOST:-localhost}"
readonly DB_PORT="${DB_PORT:-5432}"
readonly TIMESTAMP="$(date +%Y%m%d_%H%M%S)"