#!/usr/bin/env bash
# PostgreSQL Database Backup Script
# Creates compressed backups using pg_dump with custom format for optimal compression and restore flexibility.

set -euo pipefail

# Configuration - Override via environment variables
readonly BACKUP_DIR="${BACKUP_DIR:-${HOME}/.pg_backups}"
readonly DB_NAME="${DB_NAME:-}"
readonly DB_USER="${DB_USER:-postgres}"
readonly DB_HOST="${DB_HOST:-localhost}"
readonly DB_PORT="${DB_PORT:-5432}"
readonly RETENTION_DAYS="${RETENTION_DAYS:-30}"
readonly TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
readonly LOG_FILE="${BACKUP_DIR}/backup_${TIMESTAMP}.log"

mkdir -p "$BACKUP_DIR"

# Logging function that writes to both stdout and log file
log() {
    local level="$1"
    shift
    local message="[$(date +'%Y-%m-%d %H:%M:%S')] [${level}] $*"
    echo "${message}" | tee -a "${LOG_FILE}"
}

# Error handler to log failures and exit
error_exit() {
    log "ERROR" "$1"
    exit 1
}

# Validate required tools are installed
check_prerequisites() {
    local required_tools=("pg_dump" "gzip")
    for tool in "${required_tools[@]}"; do
        if ! command -v "${tool}" &> /dev/null; then
            error_exit "Required tool '${tool}' is not installed"
        fi
    done
}

# Validate configuration parameters
validate_config() {
    if [[ -z "${DB_NAME}" ]]; then
        error_exit "DB_NAME environment variable must be set"
    fi
    
    # Create backup directory if it doesn't exist
    if [[ ! -d "${BACKUP_DIR}" ]]; then
        log "INFO" "Creating backup directory: ${BACKUP_DIR}"
        if ! mkdir -p "${BACKUP_DIR}"; then
            error_exit "Failed to create backup directory: ${BACKUP_DIR}"
        fi
        log "INFO" "Backup directory created successfully"
    fi
    
    # Verify directory is writable
    if [[ ! -w "${BACKUP_DIR}" ]]; then
        error_exit "Backup directory ${BACKUP_DIR} is not writable"
    fi
}

# Test database connectivity before attempting backup
test_connection() {
    log "INFO" "Testing database connection..."
    if ! psql -h "${DB_HOST}" -p "${DB_PORT}" -U "${DB_USER}" -d "${DB_NAME}" -c "SELECT 1;" &> /dev/null; then
        error_exit "Cannot connect to database ${DB_NAME}"
    fi
    log "INFO" "Database connection successful"
}

# Perform the backup operation
perform_backup() {
    local backup_file="${BACKUP_DIR}/${DB_NAME}_${TIMESTAMP}.dump"
    local compressed_file="${backup_file}.gz"
    
    log "INFO" "Starting backup of database '${DB_NAME}'"
    log "INFO" "Backup file: ${compressed_file}"
    
    # Use custom format (-Fc) for best compression and parallel restore capability
    # Add --verbose for detailed logging, --no-owner and --no-acl for portability
    if pg_dump \
        -h "${DB_HOST}" \
        -p "${DB_PORT}" \
        -U "${DB_USER}" \
        -d "${DB_NAME}" \
        -Fc \
        --verbose \
        --no-owner \
        --no-acl \
        --file="${backup_file}" 2>> "${LOG_FILE}"; then
        
        log "INFO" "Database dump completed successfully"
        
        # Compress the backup file
        log "INFO" "Compressing backup file..."
        if gzip -9 "${backup_file}"; then
            log "INFO" "Compression completed"
            
            # Calculate and log file size
            local file_size
            file_size=$(du -h "${compressed_file}" | cut -f1)
            log "INFO" "Backup size: ${file_size}"
            
            # Calculate checksum for integrity verification
            local checksum
            checksum=$(sha256sum "${compressed_file}" | cut -d' ' -f1)
            echo "${checksum}  ${compressed_file}" > "${compressed_file}.sha256"
            log "INFO" "Checksum: ${checksum}"
            
            return 0
        else
            error_exit "Failed to compress backup file"
        fi
    else
        error_exit "Database dump failed"
    fi
}

# Clean up old backups based on retention policy
cleanup_old_backups() {
    log "INFO" "Cleaning up backups older than ${RETENTION_DAYS} days..."
    
    local deleted_count=0
    while IFS= read -r -d '' file; do
        rm -f "${file}" "${file}.sha256"
        log "INFO" "Deleted old backup: $(basename "${file}")"
        ((deleted_count++))
    done < <(find "${BACKUP_DIR}" -name "${DB_NAME}_*.dump.gz" -type f -mtime "+${RETENTION_DAYS}" -print0)
    
    if [[ ${deleted_count} -gt 0 ]]; then
        log "INFO" "Removed ${deleted_count} old backup(s)"
    else
        log "INFO" "No old backups to remove"
    fi
}

# Main execution flow
main() {
    log "INFO" "=== PostgreSQL Backup Script Started ==="
    log "INFO" "Database: ${DB_NAME}"
    log "INFO" "Host: ${DB_HOST}:${DB_PORT}"
    log "INFO" "User: ${DB_USER}"
    
    check_prerequisites
    validate_config
    test_connection
    perform_backup
    cleanup_old_backups
    
    log "INFO" "=== Backup Completed Successfully ==="
}

# Execute main function
main "$@"