/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef _WIN32
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#if !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "disc_image.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#include <jni.h>
#endif
#endif

/* XDVDFS: 2048-byte sectors. Sector 32 of the game partition is the volume
   descriptor ("MICROSOFT*XBOX*MEDIA", then the root directory's sector and
   size). A directory is a binary tree of entries, each 4-byte aligned:
   left and right child offsets in 4-byte units (0: none), the first sector
   and size of the file or directory, attributes (0x10: directory), the name's
   length and the name. An entry never crosses a sector; 0xff fills the rest. */
enum {
    SECTOR = 2048u,
    DESCRIPTOR_SECTOR = 32u,
    ATTRIBUTE_DIRECTORY = 0x10u,
    MAX_DEPTH = 64u,
};

static const char kMagic[] = "MICROSOFT*XBOX*MEDIA";

/* Where the game partition can start: an extracted ISO, and full dumps of
   the three disc formats (XGD1, XGD2, XGD3). */
static const uint64_t kPartitions[] = {0u, 0x18300000u, 0xfd90000u, 0x89d80000u};

static FILE *image;
static uint64_t partition;
static uint32_t root_sector, root_size;

#ifdef _WIN32
static SRWLOCK lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)
#else
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t slot_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t readers_cv = PTHREAD_COND_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)

enum {
    CACHE_BLOCK_SHIFT = 19u, /* 512 KB per block */
    CACHE_BLOCK_SIZE = 1u << CACHE_BLOCK_SHIFT,
    CACHE_SLOTS = 32u, /* 16 MB block cache */
};

typedef struct {
    uint64_t block_index;
    uint64_t last_used;
    size_t valid_bytes;
    bool valid;
    bool loading;
    uint8_t *data;
} DiscCacheSlot;

static int image_fd = -1;
static uint32_t active_reads = 0u;
static bool closing_image = false;
static DiscCacheSlot cache_slots[CACHE_SLOTS];
static uint64_t cache_tick = 0u;
static uint64_t last_seq_end = 0u;

static pthread_t prefetch_thread;
static bool prefetch_started = false;
static bool prefetch_stop = false;
static uint64_t prefetch_block = UINT64_MAX;
static pthread_mutex_t prefetch_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_cv = PTHREAD_COND_INITIALIZER;
#endif

static bool seek(FILE *file, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(file, (long long)offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

#ifndef _WIN32
static size_t pread_full(int fd, void *buffer, size_t length, uint64_t offset)
{
    size_t total = 0u;
    uint8_t *dst = (uint8_t *)buffer;
    while (total < length) {
        ssize_t r = pread(fd, dst + total, length - total, (off_t)(offset + total));
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    return total;
}

static size_t read_cached_block(uint64_t block_idx, size_t in_blk, void *dst, size_t max_take)
{
    LOCK();
    for (;;) {
        if (image_fd < 0 || closing_image) {
            UNLOCK();
            return 0u;
        }
        int found = -1;
        int loading_idx = -1;
        for (int i = 0; i < (int)CACHE_SLOTS; ++i) {
            if (cache_slots[i].valid && cache_slots[i].block_index == block_idx) {
                found = i;
                break;
            }
            if (cache_slots[i].loading && cache_slots[i].block_index == block_idx) {
                loading_idx = i;
                break;
            }
        }
        if (found >= 0) {
            cache_slots[found].last_used = ++cache_tick;
            size_t avail = in_blk < cache_slots[found].valid_bytes
                ? cache_slots[found].valid_bytes - in_blk : 0u;
            size_t take = max_take < avail ? max_take : avail;
            if (take > 0u && dst != NULL) {
                memcpy(dst, cache_slots[found].data + in_blk, take);
            }
            UNLOCK();
            return take;
        }
        if (loading_idx >= 0) {
            pthread_cond_wait(&slot_cv, &lock);
            continue;
        }
        /* Pick an unbusy LRU victim slot */
        int victim = -1;
        for (int i = 0; i < (int)CACHE_SLOTS; ++i) {
            if (cache_slots[i].loading) continue;
            if (!cache_slots[i].valid) {
                victim = i;
                break;
            }
            if (victim < 0 || cache_slots[i].last_used < cache_slots[victim].last_used) {
                victim = i;
            }
        }
        if (victim < 0) {
            pthread_cond_wait(&slot_cv, &lock);
            continue;
        }
        if (cache_slots[victim].data == NULL) {
            cache_slots[victim].data = (uint8_t *)malloc(CACHE_BLOCK_SIZE);
            if (cache_slots[victim].data == NULL) {
                UNLOCK();
                return 0u;
            }
        }
        cache_slots[victim].valid = false;
        cache_slots[victim].loading = true;
        cache_slots[victim].block_index = block_idx;
        cache_slots[victim].valid_bytes = 0u;
        uint8_t *slot_buf = cache_slots[victim].data;
        int fd = image_fd;
        ++active_reads;
        UNLOCK();

        const size_t got = pread_full(fd, slot_buf, CACHE_BLOCK_SIZE, block_idx << CACHE_BLOCK_SHIFT);

        LOCK();
        --active_reads;
        if (active_reads == 0u) pthread_cond_broadcast(&readers_cv);
        cache_slots[victim].loading = false;
        size_t take = 0u;
        if (got > 0u && !closing_image) {
            cache_slots[victim].valid_bytes = got;
            cache_slots[victim].last_used = ++cache_tick;
            cache_slots[victim].valid = true;
            if (in_blk < got) {
                size_t avail = got - in_blk;
                take = max_take < avail ? max_take : avail;
                if (take > 0u && dst != NULL) {
                    memcpy(dst, slot_buf + in_blk, take);
                }
            }
        }
        pthread_cond_broadcast(&slot_cv);
        UNLOCK();
        return take;
    }
}

static void *prefetch_worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&prefetch_mu);
        while (!prefetch_stop && prefetch_block == UINT64_MAX) {
            pthread_cond_wait(&prefetch_cv, &prefetch_mu);
        }
        if (prefetch_stop) {
            pthread_mutex_unlock(&prefetch_mu);
            break;
        }
        uint64_t blk = prefetch_block;
        prefetch_block = UINT64_MAX;
        pthread_mutex_unlock(&prefetch_mu);

        /* Warm next 2 blocks (1 MB ahead) */
        (void)read_cached_block(blk, 0u, NULL, 0u);
        (void)read_cached_block(blk + 1u, 0u, NULL, 0u);
    }
    return NULL;
}

static void schedule_prefetch(uint64_t next_block)
{
    pthread_mutex_lock(&prefetch_mu);
    if (prefetch_started && !prefetch_stop) {
        prefetch_block = next_block;
        pthread_cond_signal(&prefetch_cv);
    }
    pthread_mutex_unlock(&prefetch_mu);
}
#endif

static size_t read_at(uint64_t offset, void *buffer, size_t length)
{
#ifdef _WIN32
    LOCK();
    size_t got = image != NULL && seek(image, offset) ? fread(buffer, 1, length, image) : 0u;
    UNLOCK();
    return got;
#else
    if (length == 0u) return 0u;

    /* Large reads (>= 1 MB) bypass the small-block cache to avoid cache
       thrashing while remaining lock-free via pread() and triggering read-ahead. */
    if (length >= (size_t)(CACHE_BLOCK_SIZE * 2u)) {
        LOCK();
        if (image_fd < 0 || closing_image) {
            UNLOCK();
            return 0u;
        }
        int fd = image_fd;
        ++active_reads;
        UNLOCK();

        size_t got = pread_full(fd, buffer, length, offset);

        LOCK();
        --active_reads;
        if (active_reads == 0u) pthread_cond_broadcast(&readers_cv);
        UNLOCK();

        if (got > 0u) {
            schedule_prefetch((offset + got) >> CACHE_BLOCK_SHIFT);
        }
        return got;
    }

    size_t done = 0u;
    uint8_t *dst = (uint8_t *)buffer;
    while (done < length) {
        const uint64_t cur = offset + done;
        const uint64_t blk = cur >> CACHE_BLOCK_SHIFT;
        const size_t in_blk = (size_t)(cur & (CACHE_BLOCK_SIZE - 1u));
        const size_t take = read_cached_block(blk, in_blk, dst + done, length - done);
        if (take == 0u) break;
        done += take;
    }

    if (done > 0u) {
        LOCK();
        const bool sequential = (offset == last_seq_end);
        last_seq_end = offset + done;
        UNLOCK();
        if (sequential) {
            schedule_prefetch(((offset + done) >> CACHE_BLOCK_SHIFT) + 1u);
        }
    }
    return done;
#endif
}

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

void recomp_disc_image_close(void)
{
#ifndef _WIN32
    LOCK();
    closing_image = true;
    pthread_cond_broadcast(&slot_cv);
    UNLOCK();

    pthread_mutex_lock(&prefetch_mu);
    const bool join_prefetch = prefetch_started;
    if (prefetch_started) {
        prefetch_stop = true;
        pthread_cond_signal(&prefetch_cv);
    }
    pthread_mutex_unlock(&prefetch_mu);
    if (join_prefetch) {
        pthread_join(prefetch_thread, NULL);
        pthread_mutex_lock(&prefetch_mu);
        prefetch_started = false;
        prefetch_stop = false;
        prefetch_block = UINT64_MAX;
        pthread_mutex_unlock(&prefetch_mu);
    }

    LOCK();
    while (active_reads > 0u) {
        pthread_cond_wait(&readers_cv, &lock);
    }
    if (image != NULL) fclose(image);
    image = NULL;
    image_fd = -1;
    closing_image = false;
    last_seq_end = 0u;
    for (size_t i = 0u; i < CACHE_SLOTS; ++i) {
        free(cache_slots[i].data);
        cache_slots[i].data = NULL;
        cache_slots[i].valid = false;
        cache_slots[i].loading = false;
        cache_slots[i].valid_bytes = 0u;
    }
    UNLOCK();
#else
    LOCK();
    if (image != NULL) fclose(image);
    image = NULL;
    UNLOCK();
#endif
}

bool recomp_disc_image_active(void)
{
    return image != NULL;
}

#ifdef __ANDROID__
static int open_android_content_fd(const char *path)
{
    typedef void *(*GetJNIEnvFn)(void);
    typedef void *(*GetActivityFn)(void);
    GetJNIEnvFn get_env = (GetJNIEnvFn)dlsym(RTLD_DEFAULT, "SDL_GetAndroidJNIEnv");
    GetActivityFn get_activity = (GetActivityFn)dlsym(RTLD_DEFAULT, "SDL_GetAndroidActivity");
    if (get_env == NULL || get_activity == NULL) return -1;
    JNIEnv *env = (JNIEnv *)get_env();
    jobject activity = (jobject)get_activity();
    if (env == NULL || activity == NULL) return -1;
    jclass cls = (*env)->GetObjectClass(env, activity);
    int fd = -1;
    if (cls != NULL) {
        jmethodID mid = (*env)->GetMethodID(env, cls, "openContentUri", "(Ljava/lang/String;)I");
        if (mid != NULL) {
            jstring jpath = (*env)->NewStringUTF(env, path);
            if (jpath != NULL) {
                fd = (int)(*env)->CallIntMethod(env, activity, mid, jpath);
                (*env)->DeleteLocalRef(env, jpath);
            }
        }
        (*env)->DeleteLocalRef(env, cls);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        fd = -1;
    }
    (*env)->DeleteLocalRef(env, activity);
    return fd;
}
#endif

FILE *recomp_open_host_file(const char *path)
{
    if (path == NULL || path[0] == '\0') return NULL;
#ifndef _WIN32
#ifdef __ANDROID__
    if (strncmp(path, "content://", 10) == 0) {
        int fd = open_android_content_fd(path);
        if (fd < 0) return NULL;
        FILE *f = fdopen(fd, "rb");
        if (f == NULL) {
            close(fd);
            return NULL;
        }
        return f;
    }
#endif
    if (strncmp(path, "fd:", 3) == 0) {
        char *end = NULL;
        long orig_fd = strtol(path + 3, &end, 10);
        if (end != path + 3 && orig_fd >= 0) {
            int dup_fd = dup((int)orig_fd);
            if (dup_fd >= 0) {
                FILE *f = fdopen(dup_fd, "rb");
                if (f != NULL) return f;
                close(dup_fd);
            }
        }
        return NULL;
    }
#endif
    return fopen(path, "rb");
}

bool recomp_disc_image_open(const char *path)
{
    recomp_disc_image_close();
    FILE *file = recomp_open_host_file(path);
    if (file == NULL) {
        fprintf(stderr, "recomp disc image: can't open %s\n", path);
        return false;
    }
    for (size_t i = 0; i < sizeof kPartitions / sizeof kPartitions[0]; ++i) {
        uint8_t descriptor[28];
        if (!seek(file, kPartitions[i] + (uint64_t)DESCRIPTOR_SECTOR * SECTOR) ||
            fread(descriptor, 1, sizeof descriptor, file) != sizeof descriptor ||
            memcmp(descriptor, kMagic, 20) != 0) continue;
        LOCK();
        image = file;
        partition = kPartitions[i];
        root_sector = u32(descriptor + 20);
        root_size = u32(descriptor + 24);
#ifndef _WIN32
        image_fd = fileno(file);
        closing_image = false;
        last_seq_end = 0u;
#endif
        UNLOCK();
#ifndef _WIN32
        pthread_mutex_lock(&prefetch_mu);
        prefetch_stop = false;
        prefetch_block = UINT64_MAX;
        if (pthread_create(&prefetch_thread, NULL, prefetch_worker, NULL) == 0) {
            prefetch_started = true;
        }
        pthread_mutex_unlock(&prefetch_mu);
#endif
        fprintf(stderr, "recomp disc image: %s (game partition at 0x%llx)\n", path,
            (unsigned long long)partition);
        return true;
    }
    fclose(file);
    fprintf(stderr, "recomp disc image: %s is not an Xbox disc image\n", path);
    return false;
}

/* A directory's bytes, read whole (directories are small). */
static uint8_t *read_directory(uint32_t sector, uint32_t size)
{
    if (size == 0u || size > 64u * 1024u * 1024u) return NULL;
    uint8_t *bytes = malloc(size);
    if (bytes == NULL) return NULL;
    if (read_at(partition + (uint64_t)sector * SECTOR, bytes, size) != size) {
        free(bytes);
        return NULL;
    }
    return bytes;
}

static void entry_of(const uint8_t *at, RecompDiscImageEntry *entry)
{
    entry->directory = (at[12] & ATTRIBUTE_DIRECTORY) != 0u;
    entry->offset = partition + (uint64_t)u32(at + 4) * SECTOR;
    entry->size = u32(at + 8);
}

/* The entry at `offset` (bytes) of a directory, if it is a whole one. */
static const uint8_t *entry_at(const uint8_t *bytes, uint32_t size, uint32_t offset)
{
    if ((uint64_t)offset + 14u > size) return NULL;
    const uint8_t *at = bytes + offset;
    if (u16(at) == 0xffffu && u16(at + 2) == 0xffffu) return NULL;
    if ((uint64_t)offset + 14u + at[13] > size || at[13] == 0u) return NULL;
    return at;
}

static int compare_name(const char *a, size_t a_length, const uint8_t *b, size_t b_length)
{
    const size_t n = a_length < b_length ? a_length : b_length;
    for (size_t i = 0; i < n; ++i) {
        const int x = toupper((unsigned char)a[i]), y = toupper(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return a_length == b_length ? 0 : a_length < b_length ? -1 : 1;
}

/* The tree is sorted by name without regard to case: walk it. */
static bool find_in(const uint8_t *bytes, uint32_t size, const char *name, size_t length, RecompDiscImageEntry *entry)
{
    uint32_t offset = 0u;
    for (unsigned depth = 0u; depth < 4096u; ++depth) {
        const uint8_t *at = entry_at(bytes, size, offset);
        if (at == NULL) return false;
        const int order = compare_name(name, length, at + 14, at[13]);
        if (order == 0) {
            entry_of(at, entry);
            return true;
        }
        const uint16_t next = order < 0 ? u16(at) : u16(at + 2);
        if (next == 0u) return false;
        offset = (uint32_t)next * 4u;
    }
    return false;
}

bool recomp_disc_image_lookup(const char *path, RecompDiscImageEntry *entry)
{
    if (image == NULL) return false;
    RecompDiscImageEntry current = {true, partition + (uint64_t)root_sector * SECTOR, root_size};
    const char *at = path;
    while (*at == '\\' || *at == '/') ++at;
    while (*at != '\0') {
        const char *end = at;
        while (*end != '\0' && *end != '\\' && *end != '/') ++end;
        if (!current.directory) return false;
        const uint32_t sector = (uint32_t)((current.offset - partition) / SECTOR);
        uint8_t *bytes = read_directory(sector, (uint32_t)current.size);
        if (bytes == NULL) return false;
        RecompDiscImageEntry next;
        const bool found = find_in(bytes, (uint32_t)current.size, at, (size_t)(end - at), &next);
        free(bytes);
        if (!found) return false;
        current = next;
        at = end;
        while (*at == '\\' || *at == '/') ++at;
    }
    *entry = current;
    return true;
}

static void visit(const uint8_t *bytes, uint32_t size, uint32_t offset, unsigned depth,
    RecompDiscImageVisit each, void *context)
{
    const uint8_t *at = entry_at(bytes, size, offset);
    if (at == NULL || depth > MAX_DEPTH * 64u) return;
    if (u16(at) != 0u) visit(bytes, size, (uint32_t)u16(at) * 4u, depth + 1u, each, context);
    char name[256];
    memcpy(name, at + 14, at[13]);
    name[at[13]] = '\0';
    RecompDiscImageEntry entry;
    entry_of(at, &entry);
    each(context, name, &entry);
    if (u16(at + 2) != 0u) visit(bytes, size, (uint32_t)u16(at + 2) * 4u, depth + 1u, each, context);
}

bool recomp_disc_image_list(const char *path, RecompDiscImageVisit each, void *context)
{
    RecompDiscImageEntry directory;
    if (!recomp_disc_image_lookup(path, &directory) || !directory.directory) return false;
    if (directory.size == 0u) return true;
    uint8_t *bytes = read_directory((uint32_t)((directory.offset - partition) / SECTOR), (uint32_t)directory.size);
    if (bytes == NULL) return false;
    visit(bytes, (uint32_t)directory.size, 0u, 0u, each, context);
    free(bytes);
    return true;
}

size_t recomp_disc_image_read(uint64_t offset, void *buffer, size_t length)
{
    return length == 0u ? 0u : read_at(offset, buffer, length);
}
