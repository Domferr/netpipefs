#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <poll.h>
#include "../include/netpipe.h"
#include "../include/utils.h"
#include "../include/openfiles.h"
#include "../include/netpipefs_socket.h"
#include "../include/scfiles.h"

#define NOT_OPEN (-1)

/** How many bytes can be sent to the remote host */
#define available_remote(file) ((file)->remotemax - (file)->remotesize)

extern struct netpipefs_socket netpipefs_socket;

/** Linked list of poll handles */
struct poll_handle {
    void *ph;
    struct poll_handle *next;
};

/** Netpipe read or write request */
typedef struct netpipe_req {
    char *buf;
    size_t bytes_processed;
    size_t size;
    int error;
    struct netpipe_req *next;
} netpipe_req_t;

/**
 * Add a new read or write request to the given file.
 *
 * @param file
 * @param buf
 * @param size
 * @param mode if O_RDONLY then the request is a read request. If O_WRONLY then the request is write request
 * @return the request added, NULL on error and it sets errno
 */
static netpipe_req_t *netpipe_add_request(struct netpipe *file, char *buf, size_t size, int mode) {
    netpipe_req_t *new_req = (netpipe_req_t *) malloc(sizeof(netpipe_req_t));
    if (new_req == NULL) return NULL;

    new_req->size = size;
    new_req->buf = buf;
    new_req->bytes_processed = 0;
    new_req->error = 0;

    if (mode == O_RDONLY) {
        new_req->next = file->rd_req;
        file->rd_req = new_req;
    } else {
        new_req->next = file->wr_req;
        file->wr_req = new_req;
    }

    return new_req;
}

struct netpipe *netpipe_alloc(const char *path) {
    int err;
    struct netpipe *file = (struct netpipe *) malloc(sizeof(struct netpipe));
    EQNULL(file, return NULL)

    EQNULL(file->path = strdup(path), free(file); return NULL)

    if ((err = pthread_mutex_init(&(file->mtx), NULL) != 0)) {
        errno = err;
        free((void*) file->path);
        free(file);
        return NULL;
    }

    if ((err = pthread_cond_init(&(file->canopen), NULL)) != 0) {
        errno = err;
        goto error;
    }

    if ((err = pthread_cond_init(&(file->wr), NULL)) != 0) {
        errno = err;
        pthread_cond_destroy(&(file->canopen));
        goto error;
    }

    if ((err = pthread_cond_init(&(file->rd), NULL)) != 0) {
        errno = err;
        pthread_cond_destroy(&(file->canopen));
        pthread_cond_destroy(&(file->wr));
        goto error;
    }

    file->buffer = cbuf_alloc(netpipefs_options.pipecapacity);
    if (file->buffer == NULL) {
        pthread_cond_destroy(&(file->canopen));
        pthread_cond_destroy(&(file->wr));
        pthread_cond_destroy(&(file->rd));
        goto error;
    }

    file->open_mode = NOT_OPEN;
    file->force_exit = 0;
    file->writers = 0;
    file->readers = 0;
    file->remotemax = netpipefs_socket.remotepipecapacity;
    file->remotesize = 0;
    file->poll_handles = NULL;
    file->wr_req = NULL;
    file->rd_req = NULL;

    return file;

error:
    free((void*) file->path);
    pthread_mutex_destroy(&(file->mtx));
    free(file);
    return NULL;
}

int netpipe_free(struct netpipe *file) {
    int ret = 0, err;

    cbuf_free(file->buffer);
    free((void*) file->path);

    struct poll_handle *ph = file->poll_handles;
    struct poll_handle *oldph;
    while(ph != NULL) {
        netpipefs_poll_destroy(ph->ph);
        oldph = ph;
        ph = ph->next;
        free(oldph);
    }

    /* free pending read requests */
    netpipe_req_t *rd_list = file->rd_req;
    netpipe_req_t *oldreq;
    while(rd_list != NULL) {
        oldreq = rd_list;
        rd_list = rd_list->next;
        free(oldreq);
    }

    /* free pending write requests */
    netpipe_req_t *wr_list = file->wr_req;
    while(wr_list != NULL) {
        oldreq = wr_list;
        wr_list = wr_list->next;
        free(oldreq);
    }

    if ((err = pthread_mutex_destroy(&(file->mtx))) != 0) { errno = err; ret = -1; }
    if ((err = pthread_cond_destroy(&(file->canopen))) != 0) { errno = err; ret = -1; }
    if ((err = pthread_cond_destroy(&(file->wr))) != 0) { errno = err; ret = -1; }
    if ((err = pthread_cond_destroy(&(file->rd))) != 0) { errno = err; ret = -1; }

    free(file);

    return ret;
}

int netpipe_lock(struct netpipe *file) {
    int err = pthread_mutex_lock(&(file->mtx));
    if (err != 0) errno = err;
    return err;
}

int netpipe_unlock(struct netpipe *file) {
    int err = pthread_mutex_unlock(&(file->mtx));
    if (err != 0) errno = err;
    return err;
}

struct netpipe *netpipe_open(const char *path, int mode, int nonblock) {
    int err, bytes, just_created = 0;
    struct netpipe *file;

    /* both read and write access is not allowed */
    if (mode == O_RDWR) {
        errno = EPERM;
        return NULL;
    }

    /* get the file struct or create it */
    file = netpipefs_get_or_create_open_file(path, &just_created);
    if (file == NULL) return NULL;

    NOTZERO(netpipe_lock(file), goto error)

    if (file->force_exit) {
        errno = ENOENT;
        goto error;
    }

    if (file->open_mode != -1 && file->open_mode != mode) {
        errno = EPERM;
        goto error;
    }

    /* Update readers and writers */
    if (mode == O_RDONLY) file->readers++;
    else if (mode == O_WRONLY) file->writers++;

    if (nonblock && (file->readers == 0 || file->writers == 0)) {
        errno = EAGAIN;
        goto undo_open;
    }
    DEBUGFILE(file);

    /* Notify who's waiting for readers/writers */
    PTH(err, pthread_cond_broadcast(&(file->canopen)), goto undo_open)

    bytes = send_open_message(&netpipefs_socket, path, mode);
    if (bytes <= 0) { // cannot write over socket
        goto undo_open;
    }

    file->open_mode = mode;
    /* Wait for at least one writer and one reader */
    while (!file->force_exit && (file->readers == 0 || file->writers == 0)) {
        PTH(err, pthread_cond_wait(&(file->canopen), &(file->mtx)), goto undo_open)
    }

    if (file->force_exit) {
        errno = ENOENT;
        goto undo_open;
    }

    NOTZERO(netpipe_unlock(file), goto undo_open)

    return file;

undo_open:
    if (mode == O_RDONLY) {
        file->readers--;
        if (file->readers == 0) file->open_mode = NOT_OPEN; // revert to unopen
    } else if (mode == O_WRONLY) {
        file->writers--;
        if (file->writers == 0) file->open_mode = NOT_OPEN; // revert to unopen
    }

error:
    if (just_created) {
        netpipefs_remove_open_file(path);
        netpipe_unlock(file);
        netpipe_free(file); // for sure there is no poll handle
    }
    return NULL;
}

struct netpipe *netpipe_open_update(const char *path, int mode) {
    int err, just_created = 0;
    struct netpipe *file;

    if (mode == O_RDWR) {
        errno = EPERM;
        return NULL;
    }

    file = netpipefs_get_or_create_open_file(path, &just_created);
    if (file == NULL) return NULL;

    NOTZERO(netpipe_lock(file), goto error)

    if (mode == O_RDONLY) file->readers++;
    else if (mode == O_WRONLY) file->writers++;
    DEBUGFILE(file);
    PTH(err, pthread_cond_broadcast(&(file->canopen)), netpipe_unlock(file); goto unopen)

    NOTZERO(netpipe_unlock(file), goto unopen)

    return file;

unopen:
    if (mode == O_RDONLY) file->readers--;
    else if (mode == O_WRONLY) file->writers--;
error:
    if (just_created) {
        netpipefs_remove_open_file(path);
        netpipe_free(file); // for sure there is no poll handle
    }
    return NULL;
}

/**
 * Send data pointed by the given buffer.
 *
 * @param file the file
 * @param bytes_sent will be set with how many bytes were sent
 * @return 1 on success and it sets datasent, 0 if connection was lost, -1 on error
 */
static int do_send(struct netpipe *file, char *bufptr, size_t size, size_t *bytes_sent) {
    int bytes;

    *bytes_sent = size < available_remote(file) ? size : available_remote(file);
    if (*bytes_sent == 0) return 1;

    bytes = send_write_message(&netpipefs_socket, file->path, bufptr, *bytes_sent);
    if (bytes <= 0) return bytes;

    *bytes_sent = bytes;
    file->remotesize += *bytes_sent;

    return 1;
}

/**
 * Flush data which means that data available from local file buffer is sent to the host.
 *
 * @param file the file to be flushed
 * @param bytes_sent will be set with how many bytes were sent
 * @return 1 on success and it sets datasent, 0 if connection was lost, -1 on error
 */
static int do_flush(struct netpipe *file, size_t *bytes_sent) {
    int bytes;
    size_t available_locally;

    available_locally = cbuf_size(file->buffer);
    *bytes_sent = available_locally < available_remote(file) ? available_locally : available_remote(file);
    if (*bytes_sent == 0) return 1;

    bytes = send_flush_message(&netpipefs_socket, file, *bytes_sent);
    if (bytes <= 0) return bytes;

    *bytes_sent = bytes;
    file->remotesize += *bytes_sent;

    return 1;
}

/**
 * Read "size" bytes from local file buffer and put data to the given pointer.
 *
 * @param file the file
 * @param bufptr where to put bytes read
 * @param size how many bytes read
 * @param bytes_read will be sent with how many bytes were read
 * @return 1 on success, 0 when socket connection is lost, -1 on error
 */
static int do_buffered_read(struct netpipe *file, char *bufptr, size_t size, size_t *bytes_read) {
    int bytes;

    *bytes_read = cbuf_get(file->buffer, bufptr, size);
    if (*bytes_read == 0) return 1;

    bytes = send_read_message(&netpipefs_socket, file->path, *bytes_read);
    if (bytes <= 0) return bytes;

    return 1;
}

/**
 * Notify each poll handle that something is changed
 *
 * @param file file which is changed
 * @param poll_notify function used to notify
 */
static void loop_poll_notify(struct netpipe *file) {
    struct poll_handle *currph = file->poll_handles;
    struct poll_handle *oldph;
    while(currph) {
        netpipefs_poll_notify(currph->ph); // caller should free currph->ph
        oldph = currph;
        currph = currph->next;
        free(oldph);
    }
    file->poll_handles = NULL;
}

ssize_t netpipe_send(struct netpipe *file, const char *buf, size_t size, int nonblock) {
    int err;
    char *bufptr = (char *) buf;
    size_t sent = 0, bytes, remaining;

    NOTZERO(netpipe_lock(file), return -1)

    if (file->force_exit || file->readers == 0) {
        errno = EPIPE;
        netpipe_unlock(file);
        return -1;
    }

    // Flush buffer: send data from buffer
    err = do_flush(file, &bytes);
    if (err <= 0) {
        netpipe_unlock(file);
        return -1;
    }
    if (bytes > 0) {
        DEBUG("flush[%s] %ld bytes\n", file->path, bytes);
        PTH(err, pthread_cond_broadcast(&(file->wr)), netpipe_unlock(file); return -1)
    }

    // If host can still receive data and local buffer is empty
    // Directly send data
    if (available_remote(file) > 0 && cbuf_empty(file->buffer)) {
        DEBUG("netpipe_send and do_send\n");
        err = do_send(file, bufptr, size, &bytes);
        if (err <= 0) {
            netpipe_unlock(file);
            return -1;
        }

        bufptr += bytes;
        sent += bytes;
        DEBUG("send[%s] %ld bytes\n", file->path, bytes);
    }

    // If all the bytes were sent
    if (sent == size) {
        netpipe_unlock(file);
        return sent;
    }

    // If there is space into the buffer and this request need to send data
    // Put data from this request into the buffer (writeahead). Data put will be 0 if the buffer is full
    bytes = cbuf_put(file->buffer, bufptr, size);
    if (bytes > 0) {
        DEBUG("writeahead[%s] %ld bytes\n", file->path, bytes);
    }

    bufptr += bytes;
    sent += bytes;

    if (sent == size || nonblock) {
        if (sent == 0) errno = EAGAIN;
        netpipe_unlock(file);
        return sent;
    }

    remaining = size - sent;
    netpipe_req_t *request = netpipe_add_request(file, bufptr, remaining, O_WRONLY);
    while(!file->force_exit && request->bytes_processed != remaining && !request->error) {
        PTH(err, pthread_cond_wait(&(file->wr), &(file->mtx)), netpipe_unlock(file); return -1)
    }

    if (request->bytes_processed == 0 && (file->force_exit || request->error)) {
        if (request->error) errno = request->error;
        else errno = EPIPE;
        sent = -1;
    } else {
        sent += request->bytes_processed;
    }

    NOTZERO(netpipe_unlock(file), return -1)

    free(request);
    return sent;
}

int netpipe_recv(struct netpipe *file, size_t size) {
    int err, wakeup;
    char *bufptr;
    netpipe_req_t *req;
    size_t remaining, toberead, dataread;

    NOTZERO(netpipe_lock(file), return -1)

    // Move data from buffer to pending requests
    req = file->rd_req;
    while(req != NULL && !cbuf_empty(file->buffer)) {
        bufptr = req->buf + req->bytes_processed;
        remaining = req->size - req->bytes_processed;

        err = do_buffered_read(file, bufptr, remaining, &dataread);
        if (err <= 0) {
            netpipe_unlock(file);
            return err;
        }
        DEBUG("buffered read[%s] %ld bytes\n", file->path, dataread);
        req->bytes_processed += dataread;
        if (req->bytes_processed == req->size) {
            wakeup = 1;
            req = req->next;
        }
    }
    file->rd_req = req;

    // Move data from socket to pending requests
    req = file->rd_req;
    remaining = size;
    while(req != NULL && cbuf_empty(file->buffer) && remaining > 0) {
        bufptr = req->buf + req->bytes_processed;
        toberead = req->size - req->bytes_processed < remaining ? req->size - req->bytes_processed:remaining;

        dataread = readn(netpipefs_socket.fd, bufptr, toberead);
        if (dataread <= 0) {
            netpipe_unlock(file);
            return dataread;
        }
        err = send_read_message(&netpipefs_socket, file->path, dataread);
        if (err <= 0) {
            netpipe_unlock(file);
            return err;
        }
        DEBUG("read[%s] %ld bytes\n", file->path, dataread);
        req->bytes_processed += dataread;
        remaining -= dataread;
        if (req->bytes_processed == req->size) {
            wakeup = 1;
            req = req->next;
        }
    }
    file->rd_req = req;

    // Put remaining data from socket to the buffer (readahead)
    if (remaining > 0) {
        dataread = cbuf_readn(netpipefs_socket.fd, file->buffer, remaining);
        if (dataread <= 0) {
            if (dataread == 0) DEBUG("cannot write locally: Connection lost!\n");
            netpipe_unlock(file);
            return dataread;
        }
        if (dataread != size) DEBUG("cannot write locally: buffer is full. SOMETHING IS WRONG!\n");
        DEBUG("readahead[%s] %ld bytes\n", file->path, dataread);
    }

    if (wakeup)
        PTH(err, pthread_cond_broadcast(&(file->rd)), netpipe_unlock(file); return -1)
    DEBUGFILE(file);
    loop_poll_notify(file);

    NOTZERO(netpipe_unlock(file), return -1)

    return size;
}

ssize_t netpipe_read(struct netpipe *file, char *buf, size_t size, int nonblock) {
    int err;
    char *bufptr = (char *) buf;
    size_t read = 0, remaining;

    NOTZERO(netpipe_lock(file), return -1)

    if (file->force_exit) {
        errno = EPIPE;
        netpipe_unlock(file);
        return -1;
    }

    // Read from buffer (readahead). Bytes read can be zero if the buffer is empty
    err = do_buffered_read(file, bufptr, size, &read);
    if (err <= 0) {
        netpipe_unlock(file);
        return err;
    }
    if (read > 0) {
        DEBUG("buffered read[%s] %ld bytes\n", file->path, read);
        bufptr += read;
    }
    // If all the bytes were read
    if (read == size || nonblock) {
        if (read == 0) errno = EAGAIN;
        netpipe_unlock(file);
        return read;
    }

    if (file->writers == 0) {
        netpipe_unlock(file);
        return 0;
    }

    remaining = size - read;
    netpipe_req_t *request = netpipe_add_request(file, bufptr, remaining, O_RDONLY);
    err = send_read_request_message(&netpipefs_socket, file->path, remaining);
    if (err <= 0) {
        free(request);
        netpipe_unlock(file);
        return err;
    }
    while(!file->force_exit && request->bytes_processed != remaining && !request->error) {
        PTH(err, pthread_cond_wait(&(file->rd), &(file->mtx)), netpipe_unlock(file); return -1)
    }

    if (request->bytes_processed == 0 && (file->force_exit || request->error)) {
        if (request->error == EPIPE) read = 0;
        else if (request->error) errno = request->error;
        read = -1;
    } else {
        read += request->bytes_processed;
    }

    free(request);
    NOTZERO(netpipe_unlock(file), return -1)

    return read;
}

/**
 * Send data to remote host.
 *
 * @param file the file
 * @return number of bytes sent, 0 if connection is lost, -1 on error
 */
static size_t send_data(struct netpipe *file) {
    int err;
    size_t datasent = 0, remaining, bytes;
    netpipe_req_t *req;
    char *bufptr;

    // Flush buffer: send data from buffer
    err = do_flush(file, &bytes);
    if (err <= 0) return -1;

    if (bytes > 0) {
        datasent = bytes;
        DEBUG("flush[%s] %ld bytes\n", file->path, bytes);
    }

    // If host can still receive data
    // Handle requests: send data from pending requests
    req = file->wr_req;
    while(available_remote(file) > 0 && req != NULL) {
        bufptr = req->buf + req->bytes_processed;
        remaining = req->size - req->bytes_processed;

        err = do_send(file, bufptr, remaining, &bytes);
        if (err <= 0) {
            if (err == 0) req->error = ECONNRESET;
            else req->error = errno;
            PTH(err, pthread_cond_broadcast(&(file->wr)), err = -1)
            file->wr_req = req->next;
            return err;
        }
        datasent += bytes;
        DEBUG("send[%s] %ld bytes\n", file->path, bytes);

        req->bytes_processed += bytes;
        if (req->bytes_processed == req->size) req = req->next;
    }
    file->wr_req = req;

    // If there are pending requests and there is space into the buffer
    // Put data from requests into the buffer (Writeahead)
    while (req != NULL && !cbuf_full(file->buffer)) {
        bufptr = req->buf + req->bytes_processed;
        remaining = req->size - req->bytes_processed;

        bytes = cbuf_put(file->buffer, bufptr, remaining);
        DEBUG("writeahead[%s] %ld bytes\n", file->path, bytes);

        datasent += bytes;
        req->bytes_processed += bytes;
        if (req->bytes_processed == req->size) req = req->next;
    }
    file->wr_req = req;

    if (datasent > 0)
        loop_poll_notify(file);

    return datasent;
}

int netpipe_read_request(struct netpipe *file, size_t size) {
    int err;

    NOTZERO(netpipe_lock(file), return -1)

    file->remotemax += size;

    DEBUG("netpipe_read_request and send_data\n");
    err = send_data(file);
    if (err > 0) PTH(err, pthread_cond_broadcast(&(file->wr)), return -1)

    NOTZERO(netpipe_unlock(file), return -1)

    return err;
}

int netpipe_read_update(struct netpipe *file, size_t size) {
    int err;

    NOTZERO(netpipe_lock(file), return -1)

    file->remotemax -= size;
    file->remotesize -= size;

    DEBUG("netpipe_read_update and send_data\n");
    err = send_data(file);
    if (err > 0) PTH(err, pthread_cond_broadcast(&(file->wr)), return -1)

    NOTZERO(netpipe_unlock(file), return -1)

    return err;
}

ssize_t netpipe_flush(struct netpipe *file, int nonblock) {
    int err;
    ssize_t datasent = 0;
    size_t bytes, remaining;

    NOTZERO(netpipe_lock(file), return -1)

    if (file->force_exit || file->readers == 0) {
        errno = EPIPE;
        netpipe_unlock(file);
        return -1;
    }

    // Flush buffer: send data from buffer
    err = do_flush(file, &bytes);
    if (err <= 0) return -1;

    if (err > 0) {
        datasent = bytes;
        DEBUG("flush[%s] %ld bytes\n", file->path, bytes);
        PTH(err, pthread_cond_broadcast(&(file->wr)), netpipe_unlock(file); return -1)
    }

    // If all the bytes were sent or nonclock is 1
    remaining = cbuf_size(file->buffer);
    if (remaining == 0 || nonblock) {
        netpipe_unlock(file);
        return datasent ? datasent: -1;
    }

    char *bufptr = (char *) malloc(sizeof(char) * remaining);
    if (bufptr == NULL) {
        netpipe_unlock(file);
        return datasent;
    }

    netpipe_req_t *request = netpipe_add_request(file, bufptr, remaining, O_WRONLY);
    while(!file->force_exit && request->bytes_processed != remaining && !request->error) {
        PTH(err, pthread_cond_wait(&(file->wr), &(file->mtx)), netpipe_unlock(file); return -1)
    }

    if (request->bytes_processed == 0 && (file->force_exit || request->error)) {
        if (request->error) errno = request->error;
        else errno = EPIPE;
        datasent = -1;
    } else {
        datasent += request->bytes_processed;
    }

    NOTZERO(netpipe_unlock(file), return -1)

    free(request);
    return datasent;
}

int netpipe_poll(struct netpipe *file, void *ph, unsigned int *reventsp) {
    struct poll_handle *newph = (struct poll_handle *) malloc(sizeof(struct poll_handle));
    if (newph == NULL) return -1;
    newph->ph = ph;

    MINUS1(netpipe_lock(file), free(newph); return -1)

    // add poll handle
    newph->next = file->poll_handles;
    file->poll_handles = newph;

    // readable
    if (file->open_mode == O_RDONLY) {
        if (!cbuf_empty(file->buffer) || file->writers > 0) {
            // can readahead because there is data, no matter how many writers there are
            *reventsp |= POLLIN;
        } else if (file->writers == 0) { // no data is available, can't read
            *reventsp |= POLLHUP;
        }
    } else {
        // no readers. cannot write
        if (file->readers == 0) {
            *reventsp |= POLLERR;
        } else if (available_remote(file) + (cbuf_capacity(file->buffer) - cbuf_size(file->buffer)) > 0) { // writable
            // can send directly or can writeahead
            *reventsp |= POLLOUT;
        }
    }

    MINUS1(netpipe_unlock(file), return -1)

    return 0;
}

int netpipe_close(struct netpipe *file, int mode) {
    int bytes, err = 0;

    NOTZERO(netpipe_lock(file), return -1)

    if (mode == O_WRONLY) {
        file->writers--;
        if (file->writers == 0) {
            // there are no writers, then flush all data before closing
            NOTZERO(netpipe_unlock(file), return -1)
            bytes = netpipe_flush(file, 0);
            NOTZERO(netpipe_lock(file), return -1)
            if (bytes <= 0) err = -1;
        }
    } else if (mode == O_RDONLY) {
        file->readers--;
    }

    DEBUGFILE(file);

    bytes = send_close_message(&netpipefs_socket, file->path, mode);
    if (bytes <= 0) err = -1;

    if (file->writers == 0 && file->readers == 0) {
        MINUS1(netpipefs_remove_open_file(file->path), err = -1)
        NOTZERO(netpipe_unlock(file), err = -1)
        MINUS1(netpipe_free(file), err = -1)
    } else {
        NOTZERO(netpipe_unlock(file), err = -1)
    }

    if (err == -1) return -1;

    return bytes; // > 0
}

int netpipe_close_update(struct netpipe *file, int mode) {
    int err;
    netpipe_req_t *req;

    NOTZERO(netpipe_lock(file), return -1)

    if (mode == O_WRONLY) {
        file->writers--;
        if (file->writers == 0) {
            req = file->rd_req;
            while(req != NULL) { // set error = EPIPE to all write requests
                req->error = EPIPE;
                req = req->next;
            }
            file->rd_req = NULL;
            PTH(err, pthread_cond_broadcast(&(file->rd)), netpipe_unlock(file); return -1)
        }
    } else if (mode == O_RDONLY) {
        file->readers--;
        if (file->readers == 0) {
            req = file->wr_req;
            while(req != NULL) { // set error = EPIPE to all read requests
                req->error = EPIPE;
                req = req->next;
            }
            file->wr_req = NULL;
            PTH(err, pthread_cond_broadcast(&(file->wr)), netpipe_unlock(file); return -1)
        }
    }

    DEBUGFILE(file);

    loop_poll_notify(file);

    if (file->writers == 0 && file->readers == 0) {
        err = 0;
        MINUS1(netpipefs_remove_open_file(file->path), err = -1)
        MINUS1(netpipe_unlock(file), err = -1)
        MINUS1(netpipe_free(file), err = -1)

        return err;
    }

    NOTZERO(netpipe_unlock(file), return -1)

    return 0;
}

int netpipe_force_exit(struct netpipe *file) {
    int err;

    MINUS1(netpipe_lock(file), return -1)

    file->force_exit = 1;
    PTH(err, pthread_cond_broadcast(&(file->canopen)), netpipe_unlock(file); return -1)
    PTH(err, pthread_cond_broadcast(&(file->rd)), netpipe_unlock(file); return -1)
    PTH(err, pthread_cond_broadcast(&(file->wr)), netpipe_unlock(file); return -1)

    MINUS1(netpipe_unlock(file), return -1)

    return 0;
}