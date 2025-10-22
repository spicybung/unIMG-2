// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #define path_sep '\\'
  #define fseek64 _fseeki64
  #define ftell64 _ftelli64
  typedef unsigned long long u64;
#else
  #include <sys/stat.h>
  #include <unistd.h>
  #define path_sep '/'
  #define fseek64 fseeko
  #define ftell64 ftello
  typedef unsigned long long u64;
#endif

#include <zlib.h>

typedef struct {
    uint32_t lvz_off;
    uint32_t wrld_type;
    uint32_t total_size;
    uint32_t global0;
    uint32_t global1;
    uint32_t global_count;
    uint32_t continuation;
    uint32_t reserved;
} WrldHeader;

typedef struct {
    WrldHeader* items;
    size_t count;
    size_t cap;
} HeaderList;

static void die(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void* xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) die("Out of memory (alloc %zu)", n);
    return p;
}

static void* xrealloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) die("Out of memory (realloc %zu)", n);
    return q;
}

static uint32_t read_u32le(const uint8_t* b, size_t off) {
    return (uint32_t)b[off] | ((uint32_t)b[off+1] << 8)
         | ((uint32_t)b[off+2] << 16) | ((uint32_t)b[off+3] << 24);
}

static int file_exists(const char* path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
#else
    return access(path, F_OK) == 0;
#endif
}

static void make_dir_if_needed(const char* path) {
#ifdef _WIN32
    CreateDirectoryA(path, NULL); /* ok if exists */
#else
    mkdir(path, 0755);
#endif
}

static void path_dirname(const char* in, char* out, size_t outsz) {
    const char* last = strrchr(in, path_sep);
#ifdef _WIN32
    const char* last2 = strrchr(in, '/'); /* just in case */
    if (!last || (last2 && last2 > last)) last = last2;
#endif
    if (!last) { out[0] = 0; return; }
    size_t n = (size_t)(last - in);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, in, n); out[n] = 0;
}

static void path_join(char* out, size_t outsz, const char* a, const char* b) {
    size_t la = strlen(a);
    snprintf(out, outsz, "%s%s%s", a, (la && a[la-1] != path_sep) ? (const char[]){path_sep,0} : "", b);
}

static void path_stem(const char* in, char* out, size_t outsz) {
    const char* slash = strrchr(in, path_sep);
#ifdef _WIN32
    const char* slash2 = strrchr(in, '/');
    if (!slash || (slash2 && slash2 > slash)) slash = slash2;
#endif
    const char* file = slash ? slash+1 : in;
    const char* dot = strrchr(file, '.');
    size_t len = dot ? (size_t)(dot - file) : strlen(file);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, file, len); out[len] = 0;
}

static void derive_img_path(const char* lvz_path, char* out, size_t outsz) {
    char dir[1024]; dir[0]=0;
    char stem[512]; stem[0]=0;
    path_dirname(lvz_path, dir, sizeof(dir));
    path_stem(lvz_path, stem, sizeof(stem));

    /* try <dir>/<stem>.IMG then .img (case insensitive fallback) */
    if (dir[0]) {
        snprintf(out, outsz, "%s%c%s.IMG", dir, path_sep, stem);
        if (file_exists(out)) return;
        snprintf(out, outsz, "%s%c%s.img", dir, path_sep, stem);
        if (file_exists(out)) return;
    } else {
        snprintf(out, outsz, "%s.IMG", stem);
        if (file_exists(out)) return;
        snprintf(out, outsz, "%s.img", stem);
        if (file_exists(out)) return;
    }
    /* last resort: same folder, any case; we just return .IMG */
    if (dir[0]) snprintf(out, outsz, "%s%c%s.IMG", dir, path_sep, stem);
    else snprintf(out, outsz, "%s.IMG", stem);
}

static void out_dir_default(const char* lvz_path, char* out, size_t outsz) {
    char dir[1024]; dir[0]=0;
    path_dirname(lvz_path, dir, sizeof(dir));
    if (dir[0]) snprintf(out, outsz, "%s%cout_wrld", dir, path_sep);
    else snprintf(out, outsz, "out_wrld");
}

static int looks_like_zlib(const uint8_t* b, size_t n) {
    if (n < 2) return 0;
    return b[0] == 0x78 && (b[1] == 0x01 || b[1] == 0x9C || b[1] == 0xDA);
}

/* inflate with windowBits; return 0 on success */
static int try_inflate(const uint8_t* in, size_t in_len, int window_bits,
                       uint8_t** out_data, size_t* out_len) {
    int ret;
    z_stream strm; memset(&strm, 0, sizeof(strm));
    ret = inflateInit2(&strm, window_bits);
    if (ret != Z_OK) return -1;

    size_t cap = in_len * 3 + 1024;
    if (cap < 4096) cap = 4096;
    uint8_t* out = (uint8_t*)xmalloc(cap);
    size_t total = 0;

    strm.next_in = (Bytef*)in;
    strm.avail_in = (unsigned)in_len;

    for (;;) {
        if (total == cap) {
            cap = cap * 2 + 8192;
            out = (uint8_t*)xrealloc(out, cap);
        }
        strm.next_out = out + total;
        strm.avail_out = (unsigned)(cap - total);

        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            total = cap - strm.avail_out;
            break;
        }
        if (ret != Z_OK) {
            inflateEnd(&strm);
            free(out);
            return -2;
        }
        total = cap - strm.avail_out;
    }

    inflateEnd(&strm);
    *out_data = out; *out_len = total;
    return 0;
}

/* Try zlib, then gzip, then raw; else return copy of input */
static void maybe_decompress_lvz(const uint8_t* in, size_t in_len,
                                 uint8_t** out, size_t* out_len) {
    int ok; uint8_t* d = NULL; size_t n = 0;

    ok = try_inflate(in, in_len, 15, &d, &n);          /* zlib */
    if (ok == 0) { *out = d; *out_len = n; return; }

    ok = try_inflate(in, in_len, 16 + 15, &d, &n);     /* gzip */
    if (ok == 0) { *out = d; *out_len = n; return; }

    ok = try_inflate(in, in_len, -15, &d, &n);         /* raw DEFLATE */
    if (ok == 0) { *out = d; *out_len = n; return; }

    d = (uint8_t*)xmalloc(in_len);
    memcpy(d, in, in_len);
    *out = d; *out_len = in_len;
}

/* dynamic list of headers */
static void header_list_init(HeaderList* hl) {
    hl->items = NULL; hl->count = 0; hl->cap = 0;
}
static void header_list_push(HeaderList* hl, WrldHeader h) {
    if (hl->count == hl->cap) {
        hl->cap = hl->cap ? hl->cap * 2 : 128;
        hl->items = (WrldHeader*)xrealloc(hl->items, hl->cap * sizeof(WrldHeader));
    }
    hl->items[hl->count++] = h;
}
static int cmp_by_lvz_off(const void* a, const void* b) {
    const WrldHeader* x = (const WrldHeader*)a;
    const WrldHeader* y = (const WrldHeader*)b;
    if (x->lvz_off < y->lvz_off) return -1;
    if (x->lvz_off > y->lvz_off) return 1;
    return 0;
}

static void scan_slave_headers(const uint8_t* d, size_t n, HeaderList* out, FILE* log) {
    header_list_init(out);
    size_t i = 0;
    size_t hits = 0;
    while (1) {
        size_t j = i;
        for (; j + 4 <= n; ++j) {
            if (d[j]=='D' && d[j+1]=='L' && d[j+2]=='R' && d[j+3]=='W') break;
        }
        if (j + 32 > n) break;

        uint32_t wrld_type = read_u32le(d, j+0x04);
        uint32_t total     = read_u32le(d, j+0x08);
        uint32_t g0        = read_u32le(d, j+0x0C);
        uint32_t g1        = read_u32le(d, j+0x10);
        uint32_t gcnt      = read_u32le(d, j+0x14);
        uint32_t cont      = read_u32le(d, j+0x18);
        uint32_t resv      = read_u32le(d, j+0x1C);

        if (total >= 32 && cont != 0) {
            WrldHeader h = { (uint32_t)j, wrld_type, total, g0, g1, gcnt, cont, resv };
            header_list_push(out, h);
            if (log && hits < 50) {
                fprintf(log, "[scan] [%zu] @LVZ+0x%08X type=%u size=%u g0=0x%X g1=0x%X gcnt=%u cont=0x%X\n",
                        out->count-1, (unsigned)h.lvz_off, h.wrld_type, h.total_size,
                        h.global0, h.global1, h.global_count, h.continuation);
            }
            hits++;
        }
        i = j + 4;
    }

    /* sort and dedupe by lvz_off */
    qsort(out->items, out->count, sizeof(WrldHeader), cmp_by_lvz_off);
    size_t w = 0;
    for (size_t r = 0; r < out->count; ++r) {
        if (w == 0 || out->items[r].lvz_off != out->items[w-1].lvz_off) {
            out->items[w++] = out->items[r];
        }
    }
    out->count = w;

    if (log) fprintf(log, "[scan] total slave headers: %zu\n", out->count);
}

/* copy IMG slice [start, end) to f, returns bytes written */
static u64 copy_img_slice(FILE* img, u64 start, u64 end, FILE* f) {
    const size_t CHUNK = 1u << 20; /* 1 MiB */
    unsigned char* buf = (unsigned char*)xmalloc(CHUNK);
    u64 left = (end > start) ? (end - start) : 0;
#ifdef _WIN32
    _fseeki64(img, (long long)start, SEEK_SET);
#else
    fseeko(img, (off_t)start, SEEK_SET);
#endif
    u64 total = 0;
    while (left) {
        size_t want = (left > CHUNK) ? CHUNK : (size_t)left;
        size_t got = fread(buf, 1, want, img);
        if (got == 0) break;
        fwrite(buf, 1, got, f);
        left -= got; total += got;
        if (got < want) break; /* reached EOF earlier than expected */
    }
    free(buf);
    return total;
}

static int write_wrld(const WrldHeader* h, const uint8_t* decomp_lvz,
                      FILE* img, u64 img_size,
                      const char* out_path, FILE* log) {
    FILE* f = fopen(out_path, "wb");
    if (!f) {
        if (log) fprintf(log, "[error] cannot write %s (%s)\n", out_path, strerror(errno));
        return -1;
    }
    /* header */
    if (fwrite(decomp_lvz + h->lvz_off, 1, 32, f) != 32) {
        if (log) fprintf(log, "[error] write header failed for %s\n", out_path);
        fclose(f);
        return -2;
    }

    /* body */
    u64 start = (u64)h->continuation;
    u64 need  = (h->total_size >= 32) ? ((u64)h->total_size - 32ull) : 0ull;
    u64 end   = start + need;
    if (start > img_size) {
        if (log) fprintf(log, "[warn] continuation start beyond IMG (%llu > %llu); writing header only\n",
                         (unsigned long long)start, (unsigned long long)img_size);
        fclose(f);
        return 0;
    }
    if (end > img_size) {
        if (log) fprintf(log, "[warn] continuation clipped (%llu -> %llu)\n",
                         (unsigned long long)end, (unsigned long long)img_size);
        end = img_size;
    }

    u64 body = copy_img_slice(img, start, end, f);
    if (log) fprintf(log, "[build] %s header=32 body=%llu total_out=%llu (expected %u)\n",
                     out_path, (unsigned long long)body,
                     (unsigned long long)(32ull + body), h->total_size);
    fclose(f);
    return 0;
}

static void banner(void) {
    fprintf(stderr, "=== unIMG 2 Stories IMG Extractor ===\n");
    fprintf(stderr, "Usage: unimg <path-to>.lvz\n\n");
}

int main(int argc, char** argv) {
    if (argc != 2) {
        banner();
        return 1;
    }

    const char* lvz_path = argv[1];

    /* derive IMG and out_dir */
    char img_path[1024]; derive_img_path(lvz_path, img_path, sizeof(img_path));
    if (!file_exists(img_path)) {
        fprintf(stderr, "ERROR: matching IMG not found for %s (tried: %s)\n", lvz_path, img_path);
        return 2;
    }
    char out_dir[1024]; out_dir_default(lvz_path, out_dir, sizeof(out_dir));
    make_dir_if_needed(out_dir);

    /* open log */
    char log_path[1200];
    path_join(log_path, sizeof(log_path), out_dir, "wrld_import.log");
    FILE* log = fopen(log_path, "w");
    if (!log) die("Cannot open log file: %s", log_path);

    time_t now = time(NULL);
    fprintf(log, "===== unIMG 2 =====\n");
    fprintf(log, "Time: %s", ctime(&now));
    fprintf(log, "LVZ: %s\n", lvz_path);
    fprintf(log, "IMG: %s\n", img_path);
    fprintf(log, "Out: %s\n\n", out_dir);

    /* read LVZ into memory */
    FILE* flvz = fopen(lvz_path, "rb");
    if (!flvz) die("Cannot open LVZ: %s", lvz_path);
    fseek(flvz, 0, SEEK_END);
    long lvz_len_l = ftell(flvz);
    if (lvz_len_l < 0) die("ftell failed on LVZ");
    size_t lvz_len = (size_t)lvz_len_l;
    fseek(flvz, 0, SEEK_SET);
    uint8_t* lvz_raw = (uint8_t*)xmalloc(lvz_len);
    if (fread(lvz_raw, 1, lvz_len, flvz) != lvz_len) die("Failed to read LVZ");
    fclose(flvz);

    /* decompress if possible */
    uint8_t* decomp = NULL; size_t decomp_len = 0;
    maybe_decompress_lvz(lvz_raw, lvz_len, &decomp, &decomp_len);
    free(lvz_raw);
    fprintf(log, "[io] LVZ bytes: %zu; decompressed: %zu\n", lvz_len, decomp_len);
    if (decomp_len < 32) {
        fprintf(log, "[error] decompressed stream too small\n");
        fclose(log);
        free(decomp);
        return 3;
    }
    if (!(decomp[0]=='D'&&decomp[1]=='L'&&decomp[2]=='R'&&decomp[3]=='W')) {
        fprintf(log, "[warn] decompressed data does not start with DLRW, scanning anyway\n");
    }

    /* scan headers */
    HeaderList headers; scan_slave_headers(decomp, decomp_len, &headers, log);
    if (headers.count == 0) {
        fprintf(stderr, "No slave WRLD headers found.\n");
        fprintf(log, "[error] no slave headers\n");
        fclose(log);
        free(decomp);
        return 4;
    }

    /* open IMG for streaming, get size */
    FILE* fimg = fopen(img_path, "rb");
    if (!fimg) {
        fprintf(log, "[error] cannot open IMG\n");
        fclose(log);
        free(decomp);
        return 5;
    }
    fseek64(fimg, 0, SEEK_END);
    u64 img_size = (u64)ftell64(fimg);
    fseek64(fimg, 0, SEEK_SET);
    fprintf(log, "[io] IMG bytes: %llu\n\n", (unsigned long long)img_size);

    /* write each WRLD */
    size_t written = 0;
    for (size_t i = 0; i < headers.count; ++i) {
        char name[256];
        snprintf(name, sizeof(name), "wrld_%04zu.wrld", i);
        char out_path[1400]; path_join(out_path, sizeof(out_path), out_dir, name);
        int rc = write_wrld(&headers.items[i], decomp, fimg, img_size, out_path, log);
        if (rc == 0) ++written;
        else fprintf(log, "[warn] failed to write %s (rc=%d)\n", name, rc);
    }

    fprintf(log, "\n[done] wrote %zu WRLD files to %s\n", written, out_dir);
    fclose(fimg);
    fclose(log);
    free(headers.items);
    free(decomp);

    fprintf(stderr, "unIMG 2: extracted %zu WRLD files to %s\n", written, out_dir);
    fprintf(stderr, "Log: %s\n", log_path);
    return 0;
}
