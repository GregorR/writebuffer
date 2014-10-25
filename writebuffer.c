/*
 * Copyright (c) 2014 Gregor Richards
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "helpers.h"

#define MB    *1024*1024
#ifndef FILE_BUFFER
#define BUFSZ (16 MB)
#define MAXBUFS 128
#else
#define BUFSZ (512 MB)
#endif

#define ANSI_UP         "\x1B[A"
#define ANSI_DOWN_BACK  "\x1B[B\r"
#define ANSI_CLEAR      "\x1B[K"

#define BUF_TYPE_NORMAL     0
#define BUF_TYPE_END        1
#define BUF_TYPE_HEAD       2
#define BUF_TYPE_TAIL       3

typedef struct Buffer_ {
    struct Buffer_ *next;
    pthread_mutex_t lock;
    unsigned char type;
#ifndef FILE_BUFFER
    unsigned long length;
    unsigned char *buf;
#else
    char file[256];
#endif
} Buffer;

/* the in buffer */
static Buffer inBuffer, *inBufferTail;
static pthread_mutex_t inBufferTailLock = PTHREAD_MUTEX_INITIALIZER;
#ifndef FILE_BUFFER
static sem_t inBufferSem;
#endif

/* and the out buffer */
static Buffer outBuffer, *outBufferTail;
static pthread_mutex_t outBufferTailLock = PTHREAD_MUTEX_INITIALIZER;
static sem_t outBufferSem;

/* and our buffer count */
static unsigned long bufCt = 0;

/* the writer thread */
static void *writer(void *ignore)
{
    Buffer *cur, *tail;
    unsigned long written = 0;
#ifdef FILE_BUFFER
    ssize_t rd;
    char *buf;
    int fd;

    SF(buf, malloc, NULL, (BUFSZ));
#endif

    while (1) {
        sem_wait(&outBufferSem);

        /* lock the first item */
        pthread_mutex_lock(&outBuffer.lock);
        cur = outBuffer.next;
        pthread_mutex_lock(&cur->lock);

        /* remove it from the list */
        outBuffer.next = cur->next;
        pthread_mutex_unlock(&outBuffer.lock);

#ifdef FILE_BUFFER
        pthread_mutex_unlock(&cur->lock);
        /* read it in */
        SF(fd, open, -1, (cur->file, O_RDONLY));
        rd = read(fd, buf, (BUFSZ));
        if (rd == -1) {
            perror("read");
            exit(1);
        }
        close(fd);
        unlink(cur->file);
#endif

        /* write it out */
#ifndef FILE_BUFFER
        if (cur->type == BUF_TYPE_NORMAL) {
            write(1, cur->buf, BUFSZ);
        } else if (cur->type == BUF_TYPE_END) {
            write(1, cur->buf, cur->length);
            pthread_mutex_unlock(&cur->lock);
            free(cur->buf);
            free(cur);
            break;
        }
#else
        write(1, buf, rd);
#endif

#ifndef FILE_BUFFER
        /* return it to the bufferstream */
        pthread_mutex_lock(&inBufferTailLock);
        tail = inBufferTail;
        pthread_mutex_lock(&tail->lock);
        tail->buf = cur->buf;
        tail->type = BUF_TYPE_NORMAL;
        tail->next = cur;
        cur->next = NULL;
        cur->type = BUF_TYPE_TAIL;
        cur->buf = NULL;
        inBufferTail = cur;
        pthread_mutex_unlock(&cur->lock);
        pthread_mutex_unlock(&tail->lock);
        pthread_mutex_unlock(&inBufferTailLock);
        sem_post(&inBufferSem);
#else
        /* and free it up */
        if (cur->type == BUF_TYPE_END) {
            free(cur);
            break;
        }
        free(cur);
#endif

        /* display our status */
        written++;
#ifndef FILE_BUFFER
        fprintf(stderr, ANSI_UP "(mem)  buffer: %luMB    written: %luMB" ANSI_CLEAR ANSI_DOWN_BACK,
                bufCt * BUFSZ / (1 MB),
                written * BUFSZ / (1 MB));
#else
        fprintf(stderr, ANSI_UP ANSI_UP "(file) buffer: %luMB    written: %luMB" ANSI_CLEAR ANSI_DOWN_BACK ANSI_DOWN_BACK,
                (bufCt - written) * BUFSZ / (1 MB),
                written * BUFSZ / (1 MB));
#endif
    }

    return NULL;
}

/* create a new buffer or get it from inBuffers */
static Buffer *newBuffer()
{
    static Buffer *cur;

#ifndef FILE_BUFFER
retryNew:
    pthread_mutex_lock(&inBuffer.lock);
    cur = inBuffer.next;
    pthread_mutex_lock(&cur->lock);
    if (cur->type == BUF_TYPE_NORMAL) {
        /* ready for writing! */
        inBuffer.next = cur->next;
        pthread_mutex_unlock(&cur->lock);
        pthread_mutex_unlock(&inBuffer.lock);
        sem_wait(&inBufferSem);
        cur->length = 0;
        return cur;
    }
    pthread_mutex_unlock(&cur->lock);
    pthread_mutex_unlock(&inBuffer.lock);

    if (bufCt >= MAXBUFS) {
        /* too many! */
        goto waitRetryNew;
    }

    /* we need to allocate a new one */
    cur = malloc(sizeof(Buffer));
    if (cur == NULL) goto waitRetryNew;
    cur->buf = malloc(BUFSZ);
    if (cur->buf == NULL) {
        free(cur);
        goto waitRetryNew;
    }
    bufCt++;
    cur->length = 0;
#else
    SF(cur, malloc, NULL, (sizeof(Buffer)));
    sprintf(cur->file, ".buf.%lu", bufCt++);
#endif

    pthread_mutex_init(&cur->lock, NULL);
    cur->type = BUF_TYPE_NORMAL;
    return cur;

#ifndef FILE_BUFFER
waitRetryNew:
    sem_wait(&inBufferSem);
    sem_post(&inBufferSem);
    goto retryNew;
#endif
}

int main(int argc, char **argv)
{
    int tmpi;
    pthread_t writerTh;
    ssize_t rd;
    Buffer *cur, *tail;
#ifdef FILE_BUFFER
    char *buf;
    size_t len;
#endif

    if (argc > 1) {
#ifndef FILE_BUFFER
        fprintf(stderr, "Use: command | writebuffer > file\n");
#else
        fprintf(stderr, "Use: command | writebuffer | fwritebuffer > file\n");
#endif
        return 1;
    }

#ifdef FILE_BUFFER
    SF(buf, malloc, NULL, (BUFSZ));
    len = 0;
#endif

    inBuffer.type = outBuffer.type = BUF_TYPE_HEAD;

#ifndef FILE_BUFFER
    sem_init(&inBufferSem, 0, 0);
#endif
    sem_init(&outBufferSem, 0, 0);

    /* make our in and out buffer tails */
    pthread_mutex_init(&inBuffer.lock, NULL);
    SF(inBufferTail, malloc, NULL, (sizeof(Buffer)));
    inBuffer.next = inBufferTail;
    pthread_mutex_init(&inBufferTail->lock, NULL);
    inBufferTail->type = BUF_TYPE_TAIL;
    pthread_mutex_init(&outBuffer.lock, NULL);
    SF(outBufferTail, malloc, NULL, (sizeof(Buffer)));
    outBuffer.next = outBufferTail;
    pthread_mutex_init(&outBufferTail->lock, NULL);
    outBufferTail->type = BUF_TYPE_TAIL;

    /* start our writing thread */
    tmpi = pthread_create(&writerTh, NULL, writer, NULL);
    if (tmpi != 0) {
        fprintf(stderr, "Error creating writer thread!\n");
        exit(1);
    }

    /* get a fresh buffer */
    cur = newBuffer();

    /* and perform the reading */
#ifndef FILE_BUFFER
    while ((rd = read(0, cur->buf + cur->length, BUFSZ - cur->length)) >= 0) {
        cur->length += rd;
        if (cur->length == BUFSZ || rd == 0) {

#else
    while ((rd = read(0, buf + len, BUFSZ - len)) >= 0) {
        len += rd;
        if (len == BUFSZ || rd == 0) {
            int fd;

            /* write it to the file */
            SF(fd, open, -1, (cur->file, O_CREAT|O_WRONLY, 0600));
            if (write(fd, buf, len) != len) {
                perror("write");
                exit(1);
            }
            close(fd);
#endif

            /* read all we can for now, add it to the write queue */
            pthread_mutex_lock(&outBufferTailLock);
            tail = outBufferTail;
            pthread_mutex_lock(&tail->lock);

            tail->next = cur;
            tail->type = (rd == 0) ? BUF_TYPE_END : BUF_TYPE_NORMAL;
#ifndef FILE_BUFFER
            tail->length = cur->length;
            tail->buf = cur->buf;
#else
            strcpy(tail->file, cur->file);
#endif

            cur->next = NULL;
            cur->type = BUF_TYPE_TAIL;
#ifndef FILE_BUFFER
            cur->buf = NULL;
#else
            len = 0;
#endif

            outBufferTail = cur;

            pthread_mutex_unlock(&tail->lock);
            pthread_mutex_unlock(&outBufferTailLock);
            sem_post(&outBufferSem);

            cur = newBuffer();
        }

        if (rd == 0) break;
    }

    pthread_join(writerTh, NULL);

    return 0;
}
