#ifndef DATABASE_H
#define DATABASE_H

#include <pgpool/pgpool.h>
#include <pgpool/pgtypes.h>
#include <pulsar/pulsar.h>

// Global connection pool created by database.c
extern pgpool_t* pool;

// Connection to database and init connection pool.
void init_connections(const char* conn_string);

// Create all table schema.
void create_schema();

// Close connection pool.
void close_connections();

#endif  // DATABASE
