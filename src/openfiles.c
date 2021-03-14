#include <errno.h>
#include <unistd.h>
#include "../include/openfiles.h"
#include "../include/utils.h"
#include "../include/icl_hash.h"

#define NBUCKETS 128 // number of buckets used for the open files hash table

extern struct netpipefs_socket netpipefs_socket;

static icl_hash_t *open_files_table = NULL; // hash table with all the open files. Each file has its path as key
static pthread_mutex_t open_files_mtx = PTHREAD_MUTEX_INITIALIZER;

int netpipefs_open_files_table_init(void) {
    // destroys the table if it already exists
    if (open_files_table != NULL) MINUS1(netpipefs_open_files_table_destroy(), return -1)

    open_files_table = icl_hash_create(NBUCKETS, NULL, NULL);
    if (open_files_table == NULL) return -1;

    return 0;
}

int netpipefs_open_files_table_destroy(void) {
    if (icl_hash_destroy(open_files_table, NULL, (void (*)(void *)) &netpipefs_file_free) == -1)
        return -1;
    open_files_table = NULL;
    return 0;
}

struct netpipefs_file *netpipefs_get_open_file(const char *path) {
    int err;
    struct netpipefs_file *file = NULL;

    PTH(err, pthread_mutex_lock(&open_files_mtx), return NULL)

    if (open_files_table == NULL) {
        errno = EPERM;
    } else {
        file = icl_hash_find(open_files_table, (char *) path);
    }

    PTH(err, pthread_mutex_unlock(&open_files_mtx), return NULL)

    return file;
}

int netpipefs_remove_open_file(const char *path) {
    int deleted, err;
    PTH(err, pthread_mutex_lock(&open_files_mtx), return -1)

    if (open_files_table == NULL) {
        errno = EPERM;
    } else {
        deleted = icl_hash_delete(open_files_table, (char *) path, NULL, NULL);
    }

    PTH(err, pthread_mutex_unlock(&open_files_mtx), return -1)
    return deleted;
}

struct netpipefs_file *netpipefs_get_or_create_open_file(const char *path, size_t max_capacity, int *just_created) {
    int err;
    struct netpipefs_file *file;
    *just_created = 0;

    PTH(err, pthread_mutex_lock(&open_files_mtx), return NULL)
    if (open_files_table == NULL) {
        errno = EPERM;
        pthread_mutex_unlock(&open_files_mtx);
        return NULL;
    }

    file = icl_hash_find(open_files_table, (char*) path);
    EQNULL(file, file = netpipefs_file_alloc(path, max_capacity); *just_created = 1)

    if (file != NULL && *just_created) {
        if (icl_hash_insert(open_files_table, (void*) file->path, file) == NULL) {
            netpipefs_file_free(file);
            file = NULL;
        }
    }

    PTH(err, pthread_mutex_unlock(&open_files_mtx), return NULL)

    return file;
}