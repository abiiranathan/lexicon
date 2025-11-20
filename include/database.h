#ifndef DATABASE_H
#define DATABASE_H

#include <pgconn/pgconn.h>
#include <pulsar/pulsar.h>

void init_connections(const char* conn_string);
void close_connections();
void create_schema();

// Returns a thread-safe pgconn for the current worker.
pgconn_t* get_pgconn(PulsarConn* conn);

#endif  // DATABASE
