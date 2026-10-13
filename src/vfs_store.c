#include <pthread.h>

#include "../include/vfs_store.h"

extern vfs_t* vfs; // Instance of VFS defined in main.c.

static pthread_rwlock_t vfs_lock = PTHREAD_RWLOCK_INITIALIZER;

void* vfs_store_read_file(const char* path, size_t* out_size) {
    pthread_rwlock_rdlock(&vfs_lock);
    void* data = vfs ? vfs_read_file(vfs, path, out_size) : NULL;
    if (!data && out_size) *out_size = 0;
    pthread_rwlock_unlock(&vfs_lock);
    return data;
}

bool vfs_store_active(void) {
    pthread_rwlock_rdlock(&vfs_lock);
    const bool active = vfs != NULL;
    pthread_rwlock_unlock(&vfs_lock);
    return active;
}

bool vfs_store_replace(vfs_t* fresh) {
    if (!fresh) return false;
    pthread_rwlock_wrlock(&vfs_lock);
    vfs_t* old = vfs;
    vfs = fresh;
    pthread_rwlock_unlock(&vfs_lock);
    // The old handle is read-only: close only releases memory, and no
    // reader can hold it anymore past the write lock.
    vfs_close(old);
    return true;
}

void vfs_store_close(void) {
    pthread_rwlock_wrlock(&vfs_lock);
    vfs_t* old = vfs;
    vfs = NULL;
    pthread_rwlock_unlock(&vfs_lock);
    vfs_close(old); // vfs_close(NULL) is a no-op.
}
