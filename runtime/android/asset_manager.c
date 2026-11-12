// libandroid.so AAssetManager_* / AAsset_* / AAssetDir_*, backed by a host directory or by the
// APK itself (android_set_asset_apk): the APK's `assets/` members, read through its central
// directory. Stored members (all of a typical shipped game's assets) are pread/mmap'd straight
// from the file; deflated ones are refused (open returns NULL, as if absent) unless a later
// version adds zlib, and logs at ANDROID_SHIM_TRACE.
// (Unity 2017.4 itself reads the APK zip directly and does not import these; they are here for
// completeness, for a NativeActivity engine (runtime/host/nativeact.c) and for plugins.)
#define _GNU_SOURCE
#include "android_shims.h"
#include "shim_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct AAssetManager { char root[PATH_MAX]; };
struct AAsset {
    int fd;
    long long length, pos;
    void *map;        // AAsset_getBuffer mapping (whole file), lazily created
    int zip;          // index in the APK's table, or -1
    long long off;    // APK offset of the entry's data
};
struct AAssetDir {
    DIR *dir;
    char path[PATH_MAX];
    char name[NAME_MAX + 1];
    int zip;          // listing the APK's entries below `prefix`
    char prefix[PATH_MAX];
    int idx;
};

static AAssetManager g_mgr;
static pthread_mutex_t g_mgr_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_mgr_set;

// ---- the APK's ZIP central directory ---------------------------------------------------------
struct zipent {
    char name[300];
    uint32_t method, csize, usize, lho;
};

static int g_zip_fd = -1;
static void *g_zip_map;
static size_t g_zip_size;
static struct zipent *g_zip;
static int g_zip_n;
static const char g_zip_prefix[] = "assets/";

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int zipsig(const unsigned char *p, uint32_t sig) { return rd32(p) == sig; }

int android_set_asset_apk(const char *apk_path)
{
    if (!apk_path || !*apk_path) return -1;
    int fd = open(apk_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 22) { close(fd); return -1; }
    size_t size = (size_t)st.st_size;
    // end-of-central-directory record: last 64 KB + 22 bytes at most
    size_t tail = size < 65558 ? size : 65558;
    unsigned char *buf = malloc(tail);
    if (!buf) { close(fd); return -1; }
    if (pread64(fd, buf, tail, (off_t)(size - tail)) != (ssize_t)tail) { free(buf); close(fd); return -1; }
    size_t eo = 0;
    int found = 0;
    for (size_t i = tail - 22 + 1; i-- > 0; )
        if (zipsig(buf + i, 0x06054b50u)) { eo = i; found = 1; break; }
    if (!found) { SHIM_TRACE("asset APK %s: no EOCD", apk_path); free(buf); close(fd); return -1; }
    int n = rd16(buf + eo + 10);
    uint32_t cd_off = rd32(buf + eo + 16);
    if (n == 0xffff || cd_off == 0xffffffffu) {
        SHIM_TRACE("asset APK %s: ZIP64 central directory not supported", apk_path);
        free(buf); close(fd); return -1;
    }
    free(buf);
    size_t eocd_off = size - tail + eo;
    if (eocd_off <= cd_off || eocd_off - cd_off > (size_t)256 << 20) { SHIM_TRACE("asset APK %s: bad central directory", apk_path); close(fd); return -1; }
    size_t cd_size = eocd_off - cd_off;
    unsigned char *cd = malloc(cd_size);
    struct zipent *ents = calloc(n ? (size_t)n : 1, sizeof *ents);
    if (!cd || !ents) { free(cd); free(ents); close(fd); return -1; }
    if (pread64(fd, cd, cd_size, (off_t)cd_off) != (ssize_t)cd_size) { free(cd); free(ents); close(fd); return -1; }
    int k = 0;
    size_t pos = 0;
    for (int i = 0; i < n; i++) {
        if (pos + 46 > cd_size || !zipsig(cd + pos, 0x02014b50u)) break;
        unsigned char *p = cd + pos;
        unsigned nlen = rd16(p + 28), elen = rd16(p + 30), clen = rd16(p + 32);
        if (pos + 46 + nlen + elen + clen > cd_size) break;
        uint32_t method = rd16(p + 10), usize = rd32(p + 24), lho = rd32(p + 42);
        int dir = nlen > 0 && p[46 + nlen - 1] == '/';
        if (nlen && nlen < sizeof ents[k].name && !dir && (method == 0 || method == 8)) {
            memcpy(ents[k].name, p + 46, nlen);
            ents[k].name[nlen] = 0;
            ents[k].method = method;
            ents[k].csize = rd32(p + 20);
            ents[k].usize = usize;
            ents[k].lho = lho;
            k++;
        }
        pos += 46 + nlen + elen + clen;
    }
    free(cd);
    if (k == 0) { free(ents); close(fd); return -1; }
    g_zip_fd = fd;
    g_zip = ents;
    g_zip_n = k;
    g_zip_size = size;
    g_zip_map = NULL;
    fprintf(stderr, "[assets] %s: %d ZIP entries (of %d)\n", apk_path, k, n);
    return 0;
}

// central directory records are variable length: zip_index is the walk inside
// android_set_asset_apk (n records, each 46 bytes + name + extra + comment)
static const struct zipent *zip_find(const char *name)
{
    char full[300];
    snprintf(full, sizeof full, "%s%s", g_zip_prefix, name);
    for (int i = 0; i < g_zip_n; i++)
        if (!strcmp(g_zip[i].name, full)) return &g_zip[i];
    return NULL;
}

static long long zip_data_off(const struct zipent *z)
{
    unsigned char lh[30];
    if (pread64(g_zip_fd, lh, sizeof lh, (off_t)z->lho) != (ssize_t)sizeof lh) return -1;
    if (!zipsig(lh, 0x04034b50u)) return -1;
    return (long long)z->lho + 30 + rd16(lh + 26) + rd16(lh + 28);
}

static void *zip_map_file(void)
{
    if (!g_zip_map && g_zip_fd >= 0 && g_zip_size) {
        g_zip_map = mmap(NULL, g_zip_size, PROT_READ, MAP_PRIVATE, g_zip_fd, 0);
        if (g_zip_map == MAP_FAILED) g_zip_map = NULL;
    }
    return g_zip_map;
}

// ---- directory root ---------------------------------------------------------------------------
void android_set_asset_dir(const char *dir)
{
    pthread_mutex_lock(&g_mgr_lock);
    snprintf(g_mgr.root, sizeof g_mgr.root, "%s", dir && *dir ? dir : ".");
    size_t n = strlen(g_mgr.root);
    while (n > 1 && g_mgr.root[n - 1] == '/') g_mgr.root[--n] = 0;
    g_mgr_set = 1;
    pthread_mutex_unlock(&g_mgr_lock);
}

static AAssetManager *default_mgr(void)
{
    pthread_mutex_lock(&g_mgr_lock);
    int set = g_mgr_set;
    pthread_mutex_unlock(&g_mgr_lock);
    if (!set) {
        const char *e = getenv("ANDROID_ASSET_DIR");
        android_set_asset_dir(e && *e ? e : "./assets");
    }
    return &g_mgr;
}

AAssetManager *AAssetManager_fromJava(void *env, void *assetManager)
{
    (void)env; (void)assetManager;
    return default_mgr();
}

// Asset names are relative ("bin/Data/data.unity3d"); refuse ".." so lookups stay in the root.
static int asset_path(AAssetManager *m, const char *name, char *out, size_t outsz)
{
    if (!m) m = default_mgr();
    if (!name) return -1;
    while (*name == '/') name++;
    for (const char *p = name; *p; ) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) return -1;
        const char *s = strchr(p, '/');
        if (!s) break;
        p = s + 1;
    }
    int n = *name ? snprintf(out, outsz, "%s/%s", m->root, name) : snprintf(out, outsz, "%s", m->root);
    if (n < 0 || (size_t)n >= outsz) return -1;
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
    return 0;
}

static AAsset *zip_open(const char *name)
{
    const struct zipent *z = zip_find(name);
    if (!z) return NULL;
    if (z->method != 0) {
        SHIM_TRACE("AAssetManager_open(%s): deflated member, not supported", name);
        return NULL;
    }
    long long off = zip_data_off(z);
    if (off < 0) return NULL;
    AAsset *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->fd = -1;
    a->zip = (int)(z - g_zip);
    a->off = off;
    a->length = z->usize;
    SHIM_TRACE("AAssetManager_open(%s) -> %p (%lld bytes, APK offset %lld)", name, (void *)a, a->length, off);
    return a;
}

AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode)
{
    (void)mode;
    if (g_zip && filename && *filename) {
        while (*filename == '/') filename++;
        AAsset *a = zip_open(filename);
        if (a) return a;
    }
    char path[PATH_MAX];
    if (asset_path(mgr, filename, path, sizeof path)) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { SHIM_TRACE("AAssetManager_open(%s): %s", filename, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    AAsset *a = calloc(1, sizeof *a);
    if (!a) { close(fd); return NULL; }
    a->fd = fd;
    a->zip = -1;
    a->length = st.st_size;
    SHIM_TRACE("AAssetManager_open(%s) -> %p (%lld bytes)", filename, (void *)a, a->length);
    return a;
}

int AAsset_read(AAsset *a, void *buf, size_t count)
{
    if (!a) return -1;
    long long left = a->length - a->pos;
    if (left <= 0) return 0;
    if ((long long)count > left) count = left;
    if (count > INT_MAX) count = INT_MAX;
    ssize_t r;
    if (a->zip >= 0) r = pread64(g_zip_fd, buf, count, (off_t)(a->off + a->pos));
    else r = pread64(a->fd, buf, count, a->pos);
    if (r < 0) return -1;
    a->pos += r;
    return (int)r;
}

a_off64_t AAsset_seek64(AAsset *a, a_off64_t offset, int whence)
{
    if (!a) return -1;
    long long np;
    switch (whence) {
    case SEEK_SET: np = offset; break;
    case SEEK_CUR: np = a->pos + offset; break;
    case SEEK_END: np = a->length + offset; break;
    default: return -1;
    }
    if (np < 0 || np > a->length) return -1;   // like Android's _FileAsset::seek
    a->pos = np;
    return np;
}

a_off_t AAsset_seek(AAsset *a, a_off_t offset, int whence)
{
    a_off64_t r = AAsset_seek64(a, offset, whence);
    return r > LONG_MAX ? -1 : (a_off_t)r;
}

void AAsset_close(AAsset *a)
{
    if (!a) return;
    if (a->map) munmap(a->map, a->length ? a->length : 1);
    if (a->fd >= 0) close(a->fd);
    free(a);
}

const void *AAsset_getBuffer(AAsset *a)
{
    if (!a) return NULL;
    if (a->zip >= 0) {
        void *m = zip_map_file();
        return m ? (unsigned char *)m + a->off : NULL;
    }
    if (!a->map) {
        if (a->length == 0) {
            a->map = mmap(NULL, 1, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        } else {
            a->map = mmap(NULL, a->length, PROT_READ, MAP_PRIVATE, a->fd, 0);
        }
        if (a->map == MAP_FAILED) { a->map = NULL; return NULL; }
    }
    return a->map;
}

a_off_t AAsset_getLength(AAsset *a) { return a ? (a_off_t)a->length : -1; }
a_off64_t AAsset_getLength64(AAsset *a) { return a ? a->length : -1; }
a_off_t AAsset_getRemainingLength(AAsset *a) { return a ? (a_off_t)(a->length - a->pos) : -1; }
a_off64_t AAsset_getRemainingLength64(AAsset *a) { return a ? a->length - a->pos : -1; }

int AAsset_openFileDescriptor64(AAsset *a, a_off64_t *outStart, a_off64_t *outLength)
{
    if (!a) return -1;
    int fd = fcntl(a->zip >= 0 ? g_zip_fd : a->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (outStart) *outStart = a->zip >= 0 ? a->off : 0;
    if (outLength) *outLength = a->length;
    return fd;
}

int AAsset_openFileDescriptor(AAsset *a, a_off_t *outStart, a_off_t *outLength)
{
    a_off64_t s, l;
    int fd = AAsset_openFileDescriptor64(a, &s, &l);
    if (fd >= 0) {
        if (outStart) *outStart = (a_off_t)s;
        if (outLength) *outLength = (a_off_t)l;
    }
    return fd;
}

int AAsset_isAllocated(AAsset *a) { (void)a; return 0; }   /* Android: 1 only for a malloc'd buffer */

AAssetDir *AAssetManager_openDir(AAssetManager *mgr, const char *dirName)
{
    AAssetDir *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    if (g_zip) {
        char sub[PATH_MAX];
        const char *n = dirName ? dirName : "";
        while (*n == '/') n++;
        if (*n) snprintf(sub, sizeof sub, "%s%s/", g_zip_prefix, n);
        else snprintf(sub, sizeof sub, "%s", g_zip_prefix);
        snprintf(d->prefix, sizeof d->prefix, "%s", sub);
        d->zip = 1;
        return d;
    }
    if (asset_path(mgr, dirName ? dirName : "", d->path, sizeof d->path) == 0)
        d->dir = opendir(d->path);
    // Android returns a (possibly empty) AAssetDir even for missing directories.
    return d;
}

// Only regular files are listed, like the Android implementation (no subdirectories).
const char *AAssetDir_getNextFileName(AAssetDir *d)
{
    if (!d) return NULL;
    if (d->zip) {
        size_t plen = strlen(d->prefix);
        for (int i = d->idx; i < g_zip_n; i++) {
            const char *n = g_zip[i].name;
            if (strncmp(n, d->prefix, plen) != 0) continue;
            const char *rest = n + plen;
            if (!*rest || strchr(rest, '/')) continue;
            if (strcmp(rest, d->name) == 0) continue;
            d->idx = i + 1;
            snprintf(d->name, sizeof d->name, "%s", rest);
            return d->name;
        }
        return NULL;
    }
    if (!d->dir) return NULL;
    struct dirent *e;
    while ((e = readdir(d->dir))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        int isreg = e->d_type == DT_REG;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            char p[PATH_MAX + NAME_MAX + 2];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", d->path, e->d_name);
            isreg = stat(p, &st) == 0 && S_ISREG(st.st_mode);
        }
        if (!isreg) continue;
        snprintf(d->name, sizeof d->name, "%s", e->d_name);
        return d->name;
    }
    return NULL;
}

void AAssetDir_rewind(AAssetDir *d)
{
    if (!d) return;
    if (d->zip) d->idx = 0;
    else if (d->dir) rewinddir(d->dir);
}

void AAssetDir_close(AAssetDir *d)
{
    if (!d) return;
    if (d->dir) closedir(d->dir);
    free(d);
}
