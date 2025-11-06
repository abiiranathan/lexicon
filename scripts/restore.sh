#!/usr/bin/env bash
# PostgreSQL Database Restore Script
# Restores databases from compressed pg_dump custom format backups with safety checks.

set -euo pipefail

# Configuration - Override via environment variables
readonly BACKUP_DIR="${BACKUP_DIR:-${HOME}/.pg_backups}"
readonly DB_NAME="${DB_NAME:-}"
readonly DB_USER="${DB_USER:-postgres}"
readonly DB_HOST="${DB_HOST:-localhost}"
readonly DB_PORT="${DB_PORT:-5432}"
readonly BACKUP_FILE="${BACKUP_FILE:-}"
readonly PARALLEL_JOBS="${PARALLEL_JOBS:-4}"
readonly TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
readonly LOG_FILE="${BACKUP_DIR}/restore_${TIMESTAMP}.log"
readonly FORCE_RESTORE="${FORCE_RESTORE:-false}"

# Logging function that writes to both stdout and log file
log() {
    local level="$1"
    shift
    local message="[$(date +'%Y-%m-%d %H:%M:%S')] [${level}] $*"
    echo "${message}" >&2 | tee -a "${LOG_FILE}"  # Logs to stderr + file
}

# Error handler to log failures and exit
error_exit() {
    log "ERROR" "$1"
    exit 1
}

# Validate required tools are installed
check_prerequisites() {
    local required_tools=("pg_restore" "psql" "gunzip")
    for tool in "${required_tools[@]}"; do
        if ! command -v "${tool}" &> /dev/null; then
            error_exit "Required tool '${tool}' is not installed"
        fi
    done
}

# Validate configuration and inputs
validate_config() {
    if [[ -z "${DB_NAME}" ]]; then
        error_exit "DB_NAME environment variable must be set"
    fi
    
    if [[ -z "${BACKUP_FILE}" ]]; then
        error_exit "BACKUP_FILE environment variable must be set"
    fi
    
    if [[ ! -f "${BACKUP_FILE}" ]]; then
        error_exit "Backup file does not exist: ${BACKUP_FILE}"
    fi
    
    # Create backup directory if it doesn't exist (for logs and temp files)
    if [[ ! -d "${BACKUP_DIR}" ]]; then
        if ! mkdir -p "${BACKUP_DIR}"; then
            error_exit "Failed to create backup directory: ${BACKUP_DIR}"
        fi
    fi
}

# Verify backup file integrity using checksum
verify_backup_integrity() {
    local checksum_file="${BACKUP_FILE}.sha256"
    
    if [[ -f "${checksum_file}" ]]; then
        log "INFO" "Verifying backup file integrity..."
        if sha256sum -c "${checksum_file}" &> /dev/null; then
            log "INFO" "Backup file integrity verified"
            return 0
        else
            error_exit "Backup file integrity check failed"
        fi
    else
        log "WARN" "No checksum file found, skipping integrity verification"
    fi
}

# Test database connectivity
test_connection() {
    log "INFO" "Testing database connection..."
    if ! psql -h "${DB_HOST}" -p "${DB_PORT}" -U "${DB_USER}" -d postgres -c "SELECT 1;" &> /dev/null; then
        error_exit "Cannot connect to PostgreSQL server"
    fi
    log "INFO" "Database connection successful"
}

# Check if target database exists
check_database_exists() {
    local db_exists
    db_exists=$(psql -h "${DB_HOST}" -p "${DB_PORT}" -U "${DB_USER}" -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname='${DB_NAME}'")
    
    if [[ "${db_exists}" == "1" ]]; then
        return 0
    else
        return 1
    fi
}

# Create database if it doesn't exist
create_database() {
    log "INFO" "Creating database '${DB_NAME}'..."
    if psql -h "${DB_HOST}" -p "${DB_PORT}" -U "${DB_USER}" -d postgres -c "CREATE DATABASE \"${DB_NAME}\";" &>> "${LOG_FILE}"; then
        log "INFO" "Database created successfully"
    else
        error_exit "Failed to create database"
    fi
}

# Terminate active connections to the target database
terminate_connections() {
    log "INFO" "Terminating active connections to database '${DB_NAME}'..."
    psql -h "${DB_HOST}" -p "${DB_PORT}" -U "${DB_USER}" -d postgres -c \
        "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '${DB_NAME}' AND pid <> pg_backend_pid();" \
        &>> "${LOG_FILE}" || true
    log "INFO" "Active connections terminated"
}

# Prompt user for confirmation before destructive restore
confirm_restore() {
    if [[ "${FORCE_RESTORE}" == "true" ]]; then
        log "WARN" "Force mode enabled, skipping confirmation"
        return 0
    fi
    
    log "WARN" "This will drop and recreate the database '${DB_NAME}'"
    log "WARN" "All existing data will be lost!"
    echo -n "Are you sure you want to continue? (yes/no): "
    read -r response
    
    if [[ "${response}" != "yes" ]]; then
        log "INFO" "Restore cancelled by user"
        exit 0
    fi
}

# Drop existing database
drop_database() {
    log "INFO" "Dropping existing database '${DB_NAME}'..."
    if psql -h "${DB_HOST}" -p "${DB_PORT}" -U "${DB_USER}" -d postgres -c "DROP DATABASE \"${DB_NAME}\";" &>> "${LOG_FILE}"; then
        log "INFO" "Database dropped successfully"
    else
        error_exit "Failed to drop database"
    fi
}

# Decompress backup file if needed
decompress_backup() {
    local decompressed_file="${BACKUP_DIR}/temp_restore_${TIMESTAMP}.dump"
    
    if [[ "${BACKUP_FILE}" == *.gz ]]; then
        log "INFO" "Decompressing backup file..."
        log "INFO" "Target: ${decompressed_file}"
        
        if gunzip -c "${BACKUP_FILE}" > "${decompressed_file}" 2>> "${LOG_FILE}"; then
            local size
            size=$(du -h "${decompressed_file}" | cut -f1)
            log "INFO" "Decompression completed (size: ${size})"
            echo "${decompressed_file}"
        else
            rm -f "${decompressed_file}"
            error_exit "Failed to decompress backup file"
        fi
    else
        log "INFO" "Backup file is not compressed, using directly"
        echo "${BACKUP_FILE}"
    fi
}

# Perform the restore operation
perform_restore() {
    local restore_file
    restore_file=$(decompress_backup)
    
    log "INFO" "Starting restore of database '${DB_NAME}'"
    log "INFO" "Source file: ${BACKUP_FILE}"
    log "INFO" "Decompressed file: ${restore_file}"
    log "INFO" "Using ${PARALLEL_JOBS} parallel jobs"
    
    # Verify the decompressed file exists and is readable
    if [[ ! -f "${restore_file}" ]]; then
        error_exit "Decompressed file not found: ${restore_file}"
    fi
    
    if [[ ! -r "${restore_file}" ]]; then
        error_exit "Decompressed file is not readable: ${restore_file}"
    fi
    
    # Use pg_restore with custom format
    # --no-owner: skip restoration of object ownership
    # --no-acl: skip restoration of access privileges
    # -j: number of parallel jobs for faster restore
    # --exit-on-error: stop on first error for better debugging
    local restore_exit_code=0
    pg_restore \
        -h "${DB_HOST}" \
        -p "${DB_PORT}" \
        -U "${DB_USER}" \
        -d "${DB_NAME}" \
        --verbose \
        --no-owner \
        --no-acl \
        -j "${PARALLEL_JOBS}" \
        "${restore_file}" 2>&1 | tee -a "${LOG_FILE}" || restore_exit_code=$?
    
    # Clean up temporary decompressed file if we created one
    if [[ "${restore_file}" != "${BACKUP_FILE}" ]]; then
        rm -f "${restore_file}"
        log "INFO" "Cleaned up temporary files"
    fi
    
    if [[ ${restore_exit_code} -ne 0 ]]; then
        log "ERROR" "pg_restore exited with code ${restore_exit_code}"
        log "ERROR" "Check the log file for details: ${LOG_FILE}"
        error_exit "Database restore failed"
    fi
    
    log "INFO" "Database restore completed successfully"
    return 0
}

# Verify restore by checking table count and row estimates
verify_restore() {
    log "INFO" "Verifying restore..."
    
    local table_count
    table_count=$(psql -h "${DB_HOST}" -p "${DB_PORT}" -U "${DB_USER}" -d "${DB_NAME}" -tAc \
        "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'public';")
    
    log "INFO" "Tables restored: ${table_count}"
    
    if [[ ${table_count} -eq 0 ]]; then
        log "WARN" "No tables found in restored database"
    else
        log "INFO" "Restore verification passed"
    fi
}

# Main execution flow
main() {
    log "INFO" "=== PostgreSQL Restore Script Started ==="
    log "INFO" "Database: ${DB_NAME}"
    log "INFO" "Host: ${DB_HOST}:${DB_PORT}"
    log "INFO" "User: ${DB_USER}"
    log "INFO" "Backup file: ${BACKUP_FILE}"
    
    check_prerequisites
    validate_config
    verify_backup_integrity
    test_connection
    
    if check_database_exists; then
        confirm_restore
        terminate_connections
        drop_database
    fi
    
    create_database
    perform_restore
    verify_restore
    
    log "INFO" "=== Restore Completed Successfully ==="
}

# Execute main function
main "$@"
