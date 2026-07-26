// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// Unpacks Liberty City Stories and Vice City Stories LVZ+IMG pairs.
// Rebuilds continuation files and extracts IMG-side resources by their real ids.

#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32
  #define _FILE_OFFSET_BITS 64
  #define _LARGEFILE_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

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

#define STORIES_MAX_RESOURCE_ROWS 200000u
#define STORIES_MAX_SECTOR_RESOURCES 4096u
#define STORIES_MAX_DECOMPRESSED_BYTES (256u * 1024u * 1024u)
#define STORIES_UNPACK_WORD 0x6C018000u
#define STORIES_MAX_RESOURCE_EXTRACT_BYTES (64u * 1024u * 1024u)

typedef enum {
    GAME_AUTO = 0,
    GAME_LCS = 1,
    GAME_VCS = 2
} GameMode;

typedef struct {
    int enabled;
    int* ids;
    size_t count;
    size_t cap;
} IdSet;

typedef struct {
    const char* lvz_path;
    const char* img_path_override;
    const char* out_dir_override;
    GameMode game;
    int write_continuations;
    int write_resources;
    int all_pointer_forms;
    IdSet wanted_ids;
} Options;

typedef struct {
    uint32_t lvz_off;
    char tag[5];
    uint32_t type_or_header_size;
    uint32_t total_size;
    uint32_t data_size;
    uint32_t reloc_table;
    uint32_t reloc_count;
    uint32_t continuation;
    uint32_t reserved;
} ContinuationHeader;

typedef struct {
    ContinuationHeader* items;
    size_t count;
    size_t cap;
} HeaderList;

typedef struct {
    uint32_t magic;
    uint32_t type;
    uint32_t global0;
    uint32_t global1;
    uint32_t global1_dup;
    uint32_t count_like;
    uint32_t res_table_addr;
} MasterInfo;

typedef struct {
    int row_index;
    uint32_t header_addr;
    uint32_t start_x;
} SectorRowDirectory;

typedef struct {
    SectorRowDirectory* items;
    size_t count;
    size_t cap;
} SectorRowDirectoryList;

typedef struct {
    int sector_index;
    int row_index;
    int header_index;
    int sector_x;
    int sector_y;
    char game[8];
    uint32_t header_addr;
    uint32_t file_size;
    uint32_t data_size;
    uint32_t reloc_table;
    uint32_t reloc_count;
    uint32_t cont;
    uint32_t end;
} SectorContainer;

typedef struct {
    SectorContainer* items;
    size_t count;
    size_t cap;
} SectorContainerList;

typedef struct {
    char source[24];
    char layout[40];
    char pointer_form[32];
    char kind[16];
    int sector_index;
    int row_index;
    int resource_index;
    int res_id;
    int stride;
    uint32_t cont;
    uint32_t sector_end;
    uint32_t row_off;
    uint32_t raw_ptr;
    uint32_t raw_off;
    uint32_t resource_end;
    int alternate_pointer;
} ResourceRecord;

typedef struct {
    ResourceRecord* items;
    size_t count;
    size_t cap;
} ResourceRecordList;

static void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void* xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) die("Out of memory while allocating %zu bytes", n);
    return p;
}

static void* xrealloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) die("Out of memory while reallocating %zu bytes", n);
    return q;
}

static uint16_t read_u16le(const uint8_t* b, size_t off) {
    return (uint16_t)b[off] | ((uint16_t)b[off + 1] << 8);
}

static int32_t read_i32le(const uint8_t* b, size_t off) {
    uint32_t v = (uint32_t)b[off]
        | ((uint32_t)b[off + 1] << 8)
        | ((uint32_t)b[off + 2] << 16)
        | ((uint32_t)b[off + 3] << 24);
    return (int32_t)v;
}

static uint32_t read_u32le(const uint8_t* b, size_t off) {
    return (uint32_t)b[off]
        | ((uint32_t)b[off + 1] << 8)
        | ((uint32_t)b[off + 2] << 16)
        | ((uint32_t)b[off + 3] << 24);
}

static int add_u32(uint32_t a, uint32_t b, uint32_t* out) {
    if (a > UINT32_MAX - b) return 0;
    *out = a + b;
    return 1;
}

static int file_exists(const char* path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
#else
    return access(path, F_OK) == 0;
#endif
}

static int dir_exists(const char* path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void make_one_dir(const char* path) {
    if (!path || !path[0] || dir_exists(path)) return;
#ifdef _WIN32
    if (!CreateDirectoryA(path, NULL) && !dir_exists(path)) {
        die("Cannot create directory: %s", path);
    }
#else
    if (mkdir(path, 0755) != 0 && !dir_exists(path)) {
        die("Cannot create directory: %s", path);
    }
#endif
}

static void make_dirs(const char* path) {
    char tmp[2048];
    size_t len;
    if (!path || !path[0]) return;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[len - 1] = 0;
        len--;
    }
#ifdef _WIN32
    if (len >= 2 && tmp[1] == ':') {
        for (size_t i = 3; tmp[i]; ++i) {
            if (tmp[i] == '/' || tmp[i] == '\\') {
                char c = tmp[i];
                tmp[i] = 0;
                make_one_dir(tmp);
                tmp[i] = c;
            }
        }
    } else
#endif
    {
        for (size_t i = 1; tmp[i]; ++i) {
            if (tmp[i] == '/' || tmp[i] == '\\') {
                char c = tmp[i];
                tmp[i] = 0;
                make_one_dir(tmp);
                tmp[i] = c;
            }
        }
    }
    make_one_dir(tmp);
}

static const char* find_last_slash(const char* in) {
    const char* a = strrchr(in, '/');
    const char* b = strrchr(in, '\\');
    if (!a) return b;
    if (!b) return a;
    return a > b ? a : b;
}

static void path_dirname(const char* in, char* out, size_t outsz) {
    const char* slash = find_last_slash(in);
    if (!slash) {
        out[0] = 0;
        return;
    }
    size_t n = (size_t)(slash - in);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, in, n);
    out[n] = 0;
}

static void path_join(char* out, size_t outsz, const char* a, const char* b) {
    size_t la = strlen(a);
    char sep = path_sep;
    if (la > 0 && (a[la - 1] == '/' || a[la - 1] == '\\')) sep = 0;
    if (sep) snprintf(out, outsz, "%s%c%s", a, sep, b);
    else snprintf(out, outsz, "%s%s", a, b);
}

static void path_stem(const char* in, char* out, size_t outsz) {
    const char* slash = find_last_slash(in);
    const char* file = slash ? slash + 1 : in;
    const char* dot = strrchr(file, '.');
    size_t len = dot ? (size_t)(dot - file) : strlen(file);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, file, len);
    out[len] = 0;
}

static void derive_img_path(const char* lvz_path, char* out, size_t outsz) {
    char dir[1024];
    char stem[512];
    path_dirname(lvz_path, dir, sizeof(dir));
    path_stem(lvz_path, stem, sizeof(stem));

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

    if (dir[0]) snprintf(out, outsz, "%s%c%s.IMG", dir, path_sep, stem);
    else snprintf(out, outsz, "%s.IMG", stem);
}

static void default_out_dir(const char* lvz_path, char* out, size_t outsz) {
    char dir[1024];
    char stem[512];
    path_dirname(lvz_path, dir, sizeof(dir));
    path_stem(lvz_path, stem, sizeof(stem));
    if (dir[0]) snprintf(out, outsz, "%s%c%s_unpacked", dir, path_sep, stem);
    else snprintf(out, outsz, "%s_unpacked", stem);
}

static char lower_char(char c) {
    return (char)tolower((unsigned char)c);
}

static int string_equals_ignore_case(const char* a, const char* b) {
    while (*a && *b) {
        if (lower_char(*a) != lower_char(*b)) return 0;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static GameMode parse_game_mode(const char* s) {
    if (string_equals_ignore_case(s, "auto")) return GAME_AUTO;
    if (string_equals_ignore_case(s, "lcs")) return GAME_LCS;
    if (string_equals_ignore_case(s, "vcs")) return GAME_VCS;
    die("Unknown game mode: %s", s);
    return GAME_AUTO;
}

static const char* game_mode_name(GameMode mode) {
    if (mode == GAME_LCS) return "lcs";
    if (mode == GAME_VCS) return "vcs";
    return "auto";
}

static void idset_add(IdSet* set, int id) {
    if (id < 0) return;
    for (size_t i = 0; i < set->count; ++i) {
        if (set->ids[i] == id) return;
    }
    if (set->count == set->cap) {
        set->cap = set->cap ? set->cap * 2 : 32;
        set->ids = (int*)xrealloc(set->ids, set->cap * sizeof(int));
    }
    set->ids[set->count++] = id;
}

static void parse_wanted_ids(IdSet* set, const char* text) {
    char* copy = (char*)xmalloc(strlen(text) + 1);
    strcpy(copy, text);
    char* p = copy;
    set->enabled = 1;

    while (*p) {
        while (*p == ',' || *p == ';' || isspace((unsigned char)*p)) ++p;
        if (!*p) break;

        char* end = p;
        while (*end && *end != ',' && *end != ';' && !isspace((unsigned char)*end)) ++end;
        char saved = *end;
        *end = 0;

        errno = 0;
        char* parse_end = NULL;
        long id = strtol(p, &parse_end, 0);
        if (errno == ERANGE || parse_end == p || *parse_end != 0 || id < 0 || id > INT_MAX) {
            die("Bad resource id in --wanted: %s", p);
        }
        idset_add(set, (int)id);

        *end = saved;
        p = end;
    }

    free(copy);
    if (set->count == 0) die("--wanted needs at least one resource id");
}

static int idset_contains(const IdSet* set, int id) {
    if (!set->enabled) return 1;
    for (size_t i = 0; i < set->count; ++i) {
        if (set->ids[i] == id) return 1;
    }
    return 0;
}

static int is_requested_id(const IdSet* set, int id) {
    if (!set->enabled) return 0;
    return idset_contains(set, id);
}

static void header_list_push(HeaderList* list, ContinuationHeader h) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 256;
        list->items = (ContinuationHeader*)xrealloc(list->items, list->cap * sizeof(ContinuationHeader));
    }
    list->items[list->count++] = h;
}

static void sector_row_list_push(SectorRowDirectoryList* list, SectorRowDirectory row) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 128;
        list->items = (SectorRowDirectory*)xrealloc(list->items, list->cap * sizeof(SectorRowDirectory));
    }
    list->items[list->count++] = row;
}

static void sector_container_list_push(SectorContainerList* list, SectorContainer row) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 512;
        list->items = (SectorContainer*)xrealloc(list->items, list->cap * sizeof(SectorContainer));
    }
    list->items[list->count++] = row;
}

static void resource_record_list_push(ResourceRecordList* list, ResourceRecord row) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 2048;
        list->items = (ResourceRecord*)xrealloc(list->items, list->cap * sizeof(ResourceRecord));
    }
    list->items[list->count++] = row;
}

static int compare_headers_by_lvz_off(const void* a, const void* b) {
    const ContinuationHeader* x = (const ContinuationHeader*)a;
    const ContinuationHeader* y = (const ContinuationHeader*)b;
    if (x->lvz_off < y->lvz_off) return -1;
    if (x->lvz_off > y->lvz_off) return 1;
    return 0;
}

static int compare_resources_by_cont_off(const void* a, const void* b) {
    const ResourceRecord* x = (const ResourceRecord*)a;
    const ResourceRecord* y = (const ResourceRecord*)b;
    if (x->cont < y->cont) return -1;
    if (x->cont > y->cont) return 1;
    if (x->raw_off < y->raw_off) return -1;
    if (x->raw_off > y->raw_off) return 1;
    if (x->res_id < y->res_id) return -1;
    if (x->res_id > y->res_id) return 1;
    if (x->resource_index < y->resource_index) return -1;
    if (x->resource_index > y->resource_index) return 1;
    return strcmp(x->layout, y->layout);
}

static int is_known_preface_tag(const uint8_t* d, size_t n, size_t off) {
    if (off + 4 > n) return 0;
    if (d[off + 0] == 'D' && d[off + 1] == 'L' && d[off + 2] == 'R' && d[off + 3] == 'W') return 1;
    if (d[off + 0] == 'x' && d[off + 1] == 'e' && d[off + 2] == 't' && d[off + 3] == 0) return 1;
    return 0;
}

static void tag_to_string(const uint8_t* d, size_t off, char out[5]) {
    out[0] = (char)d[off + 0];
    out[1] = (char)d[off + 1];
    out[2] = (char)d[off + 2];
    out[3] = (char)d[off + 3];
    out[4] = 0;
    for (int i = 0; i < 4; ++i) {
        if (out[i] == 0 || !isprint((unsigned char)out[i])) out[i] = '_';
    }
}

static int valid_continuation_header(const uint8_t* d, size_t n, size_t off, u64 img_size) {
    if (!is_known_preface_tag(d, n, off)) return 0;
    if (off + 0x20 > n) return 0;
    uint32_t total = read_u32le(d, off + 0x08);
    uint32_t cont = read_u32le(d, off + 0x18);
    if (total < 0x20 || total > 0x4000000u) return 0;
    if ((u64)cont >= img_size) return 0;
    return 1;
}

static void scan_continuation_headers(const uint8_t* d, size_t n, u64 img_size, HeaderList* out) {
    memset(out, 0, sizeof(*out));
    for (size_t off = 0; off + 0x20 <= n; off += 4) {
        if (!valid_continuation_header(d, n, off, img_size)) continue;
        ContinuationHeader h;
        memset(&h, 0, sizeof(h));
        h.lvz_off = (uint32_t)off;
        tag_to_string(d, off, h.tag);
        h.type_or_header_size = read_u32le(d, off + 0x04);
        h.total_size = read_u32le(d, off + 0x08);
        h.data_size = read_u32le(d, off + 0x0C);
        h.reloc_table = read_u32le(d, off + 0x10);
        h.reloc_count = read_u32le(d, off + 0x14);
        h.continuation = read_u32le(d, off + 0x18);
        h.reserved = read_u32le(d, off + 0x1C);
        header_list_push(out, h);
    }

    if (out->count > 1) {
        qsort(out->items, out->count, sizeof(ContinuationHeader), compare_headers_by_lvz_off);
    }
    size_t w = 0;
    for (size_t r = 0; r < out->count; ++r) {
        if (w == 0 || out->items[r].lvz_off != out->items[w - 1].lvz_off) {
            out->items[w++] = out->items[r];
        }
    }
    out->count = w;
}

static int try_inflate(const uint8_t* in, size_t in_len, int window_bits, uint8_t** out_data, size_t* out_len) {
    if (in_len > UINT_MAX) return -4;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int ret = inflateInit2(&strm, window_bits);
    if (ret != Z_OK) return -1;

    size_t cap;
    if (in_len > (STORIES_MAX_DECOMPRESSED_BYTES - 4096u) / 3u) cap = STORIES_MAX_DECOMPRESSED_BYTES;
    else cap = in_len * 3u + 4096u;
    if (cap < 8192u) cap = 8192u;
    if (cap > STORIES_MAX_DECOMPRESSED_BYTES) cap = STORIES_MAX_DECOMPRESSED_BYTES;
    uint8_t* out = (uint8_t*)xmalloc(cap);
    size_t total = 0;

    strm.next_in = (Bytef*)in;
    strm.avail_in = (unsigned)in_len;

    for (;;) {
        if (total == cap) {
            if (cap >= STORIES_MAX_DECOMPRESSED_BYTES) {
                inflateEnd(&strm);
                free(out);
                return -3;
            }
            size_t next_cap;
            if (cap > (STORIES_MAX_DECOMPRESSED_BYTES - 8192u) / 2u) next_cap = STORIES_MAX_DECOMPRESSED_BYTES;
            else next_cap = cap * 2u + 8192u;
            if (next_cap > STORIES_MAX_DECOMPRESSED_BYTES) next_cap = STORIES_MAX_DECOMPRESSED_BYTES;
            out = (uint8_t*)xrealloc(out, next_cap);
            cap = next_cap;
        }

        strm.next_out = out + total;
        strm.avail_out = (unsigned)(cap - total);
        ret = inflate(&strm, Z_NO_FLUSH);
        total = cap - strm.avail_out;

        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) {
            inflateEnd(&strm);
            free(out);
            return -2;
        }
    }

    inflateEnd(&strm);
    *out_data = out;
    *out_len = total;
    return 0;
}

static void maybe_decompress_lvz(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len, int* decompressed) {
    uint8_t* d = NULL;
    size_t n = 0;
    int attempts[3] = { 15, 16 + 15, -15 };
    for (int i = 0; i < 3; ++i) {
        if (try_inflate(in, in_len, attempts[i], &d, &n) == 0) {
            *out = d;
            *out_len = n;
            *decompressed = 1;
            return;
        }
    }

    d = (uint8_t*)xmalloc(in_len);
    memcpy(d, in, in_len);
    *out = d;
    *out_len = in_len;
    *decompressed = 0;
}

static uint8_t* read_entire_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) die("Cannot open file: %s", path);
    if (fseek64(f, 0, SEEK_END) != 0) die("Cannot seek file: %s", path);
    long long len_ll = (long long)ftell64(f);
    if (len_ll < 0) die("Cannot tell file size: %s", path);
    if ((u64)len_ll > (u64)((size_t)-1)) die("File too large for this build: %s", path);
    size_t len = (size_t)len_ll;
    if (fseek64(f, 0, SEEK_SET) != 0) die("Cannot seek file: %s", path);
    uint8_t* data = (uint8_t*)xmalloc(len);
    if (len && fread(data, 1, len, f) != len) die("Cannot read file: %s", path);
    fclose(f);
    *out_len = len;
    return data;
}

static u64 file_size_of_open(FILE* f) {
    if (fseek64(f, 0, SEEK_END) != 0) die("Failed to seek IMG");
    long long v = (long long)ftell64(f);
    if (v < 0) die("Failed to tell IMG size");
    if (fseek64(f, 0, SEEK_SET) != 0) die("Failed to rewind IMG");
    return (u64)v;
}

static int copy_file_slice(FILE* in, u64 start, u64 end, FILE* out, u64* copied_out) {
    const size_t chunk_size = 1u << 20;
    uint8_t* buf = (uint8_t*)xmalloc(chunk_size);
    u64 left = end > start ? end - start : 0;
    u64 total = 0;

    if (fseek64(in, (long long)start, SEEK_SET) != 0) {
        free(buf);
        return 0;
    }

    while (left) {
        size_t want = left > chunk_size ? chunk_size : (size_t)left;
        size_t got = fread(buf, 1, want, in);
        if (got == 0) {
            free(buf);
            return 0;
        }
        if (fwrite(buf, 1, got, out) != got) {
            free(buf);
            return 0;
        }
        total += got;
        left -= got;
        if (got != want) {
            free(buf);
            return 0;
        }
    }

    free(buf);
    *copied_out = total;
    return 1;
}

static int write_binary_file(const char* path, const uint8_t* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return 0;
    }
    if (fclose(f) != 0) return 0;
    return 1;
}

static void parse_master_info(const uint8_t* lvz, size_t n, MasterInfo* info) {
    memset(info, 0, sizeof(*info));
    if (n < 0x24) return;
    info->magic = read_u32le(lvz, 0x00);
    info->type = read_u32le(lvz, 0x04);
    info->global0 = read_u32le(lvz, 0x08);
    info->global1 = read_u32le(lvz, 0x0C);
    info->global1_dup = read_u32le(lvz, 0x10);
    info->count_like = read_u32le(lvz, 0x14);
    info->res_table_addr = read_u32le(lvz, 0x20);
}

static void find_sector_row_directories(const uint8_t* lvz, size_t n, SectorRowDirectoryList* rows) {
    memset(rows, 0, sizeof(*rows));
    if (n < 0x24 || !is_known_preface_tag(lvz, n, 0)) return;

    size_t cursor = 0x24;
    int row_index = 0;
    while (cursor + 8 <= n) {
        uint32_t header_addr = read_u32le(lvz, cursor + 0);
        uint32_t start_x = read_u32le(lvz, cursor + 4);
        if (!(header_addr > 0 && header_addr + 0x20 <= n && (header_addr & 3) == 0)) break;
        if (!is_known_preface_tag(lvz, n, header_addr)) break;
        SectorRowDirectory row;
        row.row_index = row_index;
        row.header_addr = header_addr;
        row.start_x = start_x;
        sector_row_list_push(rows, row);
        row_index++;
        cursor += 8;
    }
}

static int game_from_row_count(size_t row_count, GameMode requested, int master_stride) {
    if (requested == GAME_LCS) return GAME_LCS;
    if (requested == GAME_VCS) return GAME_VCS;
    if (row_count == 47) return GAME_LCS;
    if (row_count == 37) return GAME_VCS;
    if (master_stride == 12) return GAME_VCS;
    if (master_stride == 8) return GAME_LCS;
    return GAME_VCS;
}

static const char* game_name_from_int(int game) {
    return game == GAME_LCS ? "lcs" : "vcs";
}

static int get_pass_count_for_game(int game) {
    return game == GAME_LCS ? 8 : 9;
}

static int rate_resource_entry_stride(const uint8_t* lvz, size_t n, uint32_t base, uint32_t res_count, int stride, uint32_t table_limit) {
    if (!(stride == 8 || stride == 12)) return -1000000;
    if (base == 0 || base >= n || res_count == 0) return -1000000;
    if ((u64)base + (u64)res_count * (u64)stride > (u64)table_limit) return -1000000;

    uint32_t sample_count = res_count < 1024 ? res_count : 1024;
    int rating = 0;
    int invalid = 0;
    int supported = 0;
    int id_like = 0;

    for (uint32_t i = 0; i < sample_count; ++i) {
        size_t off = (size_t)base + (size_t)i * (size_t)stride;
        uint32_t raw_ptr = read_u32le(lvz, off + 0);
        uint32_t aux_ptr = read_u32le(lvz, off + 4);
        if (stride == 12) {
            uint32_t id = read_u32le(lvz, off + 8);
            if (id == 0xFFFFFFFFu) id_like += 2;
            else if (id <= res_count + 4096u || id <= 0xFFFFu) id_like += 1;
            else id_like -= 4;
        }

        if (aux_ptr == 0 || (aux_ptr >= 0x40 && aux_ptr < n && (aux_ptr & 3) == 0)) rating += 1;
        else rating -= 1;

        if (raw_ptr == 0 || raw_ptr == 0xFFFFFFFFu) {
            rating += 1;
            continue;
        }

        if (!(raw_ptr >= 0x40 && raw_ptr + 4 <= n && (raw_ptr & 3) == 0)) {
            invalid++;
            rating -= 8;
            continue;
        }

        rating += 2;
        if (is_known_preface_tag(lvz, n, raw_ptr)) {
            supported++;
            rating += 8;
        } else {
            rating += 1;
        }
    }

    if (stride == 12) rating += id_like;
    rating += supported;
    rating -= invalid * 2;
    return rating;
}

static int detect_master_resource_stride(const uint8_t* lvz, size_t n, uint32_t base, uint32_t res_count, uint32_t first_group_addr) {
    if (base == 0 || base >= n || res_count == 0) return 8;
    uint32_t table_limit = (uint32_t)n;
    if (first_group_addr > base) table_limit = first_group_addr;
    if (table_limit <= base) return 8;
    uint32_t space = table_limit - base;

    int best_stride = 8;
    int best_rating = -1000000;
    if (space >= res_count * 12u) {
        int rating = rate_resource_entry_stride(lvz, n, base, res_count, 12, table_limit);
        if (rating > best_rating) {
            best_rating = rating;
            best_stride = 12;
        }
    }
    if (space >= res_count * 8u) {
        int rating = rate_resource_entry_stride(lvz, n, base, res_count, 8, table_limit);
        if (rating > best_rating) {
            best_rating = rating;
            best_stride = 8;
        }
    }
    return best_stride;
}

static uint32_t detect_master_resource_count(const uint8_t* lvz, size_t n, const MasterInfo* master, const SectorRowDirectoryList* rows) {
    uint32_t count = 0;
    size_t cursor = 0x24 + rows->count * 8;
    if (rows->count > 0 && cursor + 4 <= n) {
        count = read_u32le(lvz, cursor);
    }
    if (count == 0 || count > STORIES_MAX_RESOURCE_ROWS) count = master->count_like;
    if (count > STORIES_MAX_RESOURCE_ROWS) count = STORIES_MAX_RESOURCE_ROWS;
    return count;
}

static void collect_sector_containers(
    const uint8_t* lvz,
    size_t n,
    u64 img_size,
    const SectorRowDirectoryList* rows,
    int game,
    SectorContainerList* containers
) {
    memset(containers, 0, sizeof(*containers));
    if (rows->count < 2) return;

    int sector_index = 0;
    for (size_t row_index = 0; row_index + 1 < rows->count; ++row_index) {
        const SectorRowDirectory* row = &rows->items[row_index];
        const SectorRowDirectory* next_row = &rows->items[row_index + 1];
        if (next_row->header_addr <= row->header_addr) continue;
        uint32_t header_count = (next_row->header_addr - row->header_addr) / 0x20u;
        for (uint32_t header_index = 0; header_index < header_count; ++header_index) {
            uint32_t header_addr = row->header_addr + header_index * 0x20u;
            if (header_addr + 0x20u > n) break;
            if (!is_known_preface_tag(lvz, n, header_addr)) break;
            uint32_t file_size = read_u32le(lvz, header_addr + 0x08);
            uint32_t data_size = read_u32le(lvz, header_addr + 0x0C);
            uint32_t reloc_table = read_u32le(lvz, header_addr + 0x10);
            uint32_t reloc_count = read_u32le(lvz, header_addr + 0x14);
            uint32_t cont = read_u32le(lvz, header_addr + 0x18);
            if (file_size < 0x20u) continue;
            if ((u64)cont >= img_size) continue;
            u64 end64 = (u64)cont + (u64)file_size - 0x20ull;
            if (end64 > img_size) end64 = img_size;
            SectorContainer item;
            memset(&item, 0, sizeof(item));
            item.sector_index = sector_index;
            item.row_index = (int)row_index;
            item.header_index = (int)header_index;
            item.sector_x = (int)row->start_x + (int)header_index;
            item.sector_y = (int)row_index;
            snprintf(item.game, sizeof(item.game), "%s", game_name_from_int(game));
            item.header_addr = header_addr;
            item.file_size = file_size;
            item.data_size = data_size;
            item.reloc_table = reloc_table;
            item.reloc_count = reloc_count;
            item.cont = cont;
            item.end = (uint32_t)end64;
            sector_container_list_push(containers, item);
            sector_index++;
        }
    }
}

static void classify_resource_kind(const uint8_t* data, size_t n, uint32_t off, uint32_t end, char out[16]) {
    snprintf(out, 16, "bin");
    if (off + 4 > n) {
        snprintf(out, 16, "invalid");
        return;
    }
    if (is_known_preface_tag(data, n, off)) {
        if (data[off] == 'D') snprintf(out, 16, "wrld");
        else snprintf(out, 16, "tex");
        return;
    }

    uint32_t scan_end = end;
    if (scan_end > n) scan_end = (uint32_t)n;
    if (scan_end < off || scan_end - off > 0x400u) scan_end = off + 0x400u < n ? off + 0x400u : (uint32_t)n;
    for (uint32_t p = off; p + 4 <= scan_end; p += 4) {
        if (read_u32le(data, p) == STORIES_UNPACK_WORD) {
            snprintf(out, 16, "mdl");
            return;
        }
    }
}

static void add_resource_record(
    ResourceRecordList* records,
    const uint8_t* img,
    size_t img_size,
    const Options* options,
    const SectorContainer* sector,
    int resource_index,
    int res_id,
    uint32_t raw_ptr,
    uint32_t resource_off,
    uint32_t row_off,
    int stride,
    const char* layout,
    const char* pointer_form,
    int alternate_pointer,
    uint32_t expanded_end
) {
    if (res_id < 0) return;
    if (!idset_contains(&options->wanted_ids, res_id)) return;

    int requested = is_requested_id(&options->wanted_ids, res_id);
    if (alternate_pointer && !requested && !options->all_pointer_forms) return;

    if ((resource_off & 3u) != 0) return;
    if ((u64)resource_off + 4ull > (u64)img_size) return;

    if (!alternate_pointer && !(sector->cont <= resource_off && resource_off < expanded_end)) return;

    ResourceRecord rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.source, sizeof(rec.source), "sector_img");
    snprintf(rec.layout, sizeof(rec.layout), "%s", layout);
    snprintf(rec.pointer_form, sizeof(rec.pointer_form), "%s", pointer_form);
    rec.sector_index = sector->sector_index;
    rec.row_index = sector->row_index;
    rec.resource_index = resource_index;
    rec.res_id = res_id;
    rec.stride = stride;
    rec.cont = sector->cont;
    rec.sector_end = expanded_end;
    rec.row_off = row_off;
    rec.raw_ptr = raw_ptr;
    rec.raw_off = resource_off;
    rec.resource_end = expanded_end;
    rec.alternate_pointer = alternate_pointer;
    classify_resource_kind(img, img_size, resource_off, expanded_end, rec.kind);
    resource_record_list_push(records, rec);
}

static void add_sector_resource_pointer_forms(
    ResourceRecordList* records,
    const uint8_t* img,
    size_t img_size,
    const Options* options,
    const SectorContainer* sector,
    int resource_index,
    int res_id,
    uint32_t raw_ptr,
    uint32_t default_off,
    uint32_t row_off,
    int stride,
    const char* layout,
    uint32_t expanded_end
) {
    add_resource_record(records, img, img_size, options, sector, resource_index, res_id, raw_ptr, default_off, row_off, stride, layout, "default", 0, expanded_end);

    int requested = is_requested_id(&options->wanted_ids, res_id);
    if (!requested && !options->all_pointer_forms) return;

    uint32_t offset;
    if (raw_ptr >= 0x20u && add_u32(sector->cont, raw_ptr - 0x20u, &offset)) {
        add_resource_record(records, img, img_size, options, sector, resource_index, res_id, raw_ptr, offset, row_off, stride, layout, "cont_plus_ptr_minus_20", 1, expanded_end);
    }
    if (add_u32(sector->cont, raw_ptr, &offset)) {
        add_resource_record(records, img, img_size, options, sector, resource_index, res_id, raw_ptr, offset, row_off, stride, layout, "cont_plus_ptr", 1, expanded_end);
    }
    add_resource_record(records, img, img_size, options, sector, resource_index, res_id, raw_ptr, raw_ptr, row_off, stride, layout, "absolute_ptr", 1, expanded_end);
    if (raw_ptr >= 0x20u) {
        add_resource_record(records, img, img_size, options, sector, resource_index, res_id, raw_ptr, raw_ptr - 0x20u, row_off, stride, layout, "absolute_ptr_minus_20", 1, expanded_end);
    }
}

static uint32_t expanded_sector_end_for_index(const SectorContainerList* containers, size_t index, size_t img_size) {
    uint32_t cont = containers->items[index].cont;
    uint32_t declared = containers->items[index].end;
    uint32_t next_cont = (uint32_t)img_size;
    for (size_t i = 0; i < containers->count; ++i) {
        uint32_t other = containers->items[i].cont;
        if (other > cont && other < next_cont) next_cont = other;
    }
    uint32_t hard = next_cont > cont ? next_cont : (uint32_t)img_size;
    uint32_t end = declared;
    if (hard > end) end = hard;
    if (end > img_size) end = (uint32_t)img_size;
    return end;
}

static void collect_sector_resources(
    const uint8_t* img,
    size_t img_size,
    const Options* options,
    const SectorContainerList* containers,
    ResourceRecordList* records
) {
    memset(records, 0, sizeof(*records));
    for (size_t ci = 0; ci < containers->count; ++ci) {
        const SectorContainer* sector = &containers->items[ci];
        uint32_t cont = sector->cont;
        uint32_t end = expanded_sector_end_for_index(containers, ci, img_size);
        if (cont + 8u > img_size || end <= cont) continue;

        uint32_t resources_ptr = read_u32le(img, cont + 0x00);
        uint32_t num_resources = read_u16le(img, cont + 0x04);
        if (num_resources == 0 || num_resources > STORIES_MAX_SECTOR_RESOURCES) continue;
        if (resources_ptr < 0x20u) continue;
        uint32_t list_start;
        if (!add_u32(cont, resources_ptr - 0x20u, &list_start)) continue;
        if (list_start < cont || (u64)list_start + 8ull > end || (u64)list_start + 8ull > img_size) continue;

        if ((u64)list_start + (u64)num_resources * 8ull <= end) {
            for (uint32_t ri = 0; ri < num_resources; ++ri) {
                uint32_t row_off = list_start + ri * 8u;
                int res_id = (int)read_i32le(img, row_off + 0x00);
                uint32_t raw_ptr = read_u32le(img, row_off + 0x04);
                if (raw_ptr < 0x20u) continue;
                uint32_t raw_off;
                if (!add_u32(cont, raw_ptr - 0x20u, &raw_off)) continue;
                add_sector_resource_pointer_forms(records, img, img_size, options, sector, (int)ri, res_id, raw_ptr, raw_off, row_off, 8, "id_ptr", end);
            }
        }

        if ((u64)list_start + (u64)num_resources * 12ull <= end) {
            for (uint32_t ri = 0; ri < num_resources; ++ri) {
                uint32_t row_off = list_start + ri * 12u;
                uint32_t a = read_u32le(img, row_off + 0x00);
                uint32_t b = read_u32le(img, row_off + 0x04);
                uint32_t c = read_u32le(img, row_off + 0x08);

                uint32_t row_resource_off;
                if (a >= 0x20u && add_u32(cont, a - 0x20u, &row_resource_off)) {
                    add_sector_resource_pointer_forms(records, img, img_size, options, sector, (int)ri, (int)c, a, row_resource_off, row_off, 12, "ptr_unused_id", end);
                }

                int allow_alt = options->all_pointer_forms || is_requested_id(&options->wanted_ids, (int)a) || is_requested_id(&options->wanted_ids, (int)b) || is_requested_id(&options->wanted_ids, (int)c);
                if (allow_alt) {
                    if (a >= 0x20u && add_u32(cont, a - 0x20u, &row_resource_off)) {
                        add_sector_resource_pointer_forms(records, img, img_size, options, sector, (int)ri, (int)b, a, row_resource_off, row_off, 12, "ptr_id_unused", end);
                    }
                    if (c >= 0x20u && add_u32(cont, c - 0x20u, &row_resource_off)) {
                        add_sector_resource_pointer_forms(records, img, img_size, options, sector, (int)ri, (int)a, c, row_resource_off, row_off, 12, "id_unused_ptr", end);
                    }
                    if (b >= 0x20u && add_u32(cont, b - 0x20u, &row_resource_off)) {
                        add_sector_resource_pointer_forms(records, img, img_size, options, sector, (int)ri, (int)a, b, row_resource_off, row_off, 12, "id_ptr_unused", end);
                    }
                }
            }
        }
    }

    if (records->count > 1) {
        qsort(records->items, records->count, sizeof(ResourceRecord), compare_resources_by_cont_off);
    }

    size_t w = 0;
    for (size_t r = 0; r < records->count; ++r) {
        ResourceRecord* cur = &records->items[r];
        int dup = 0;
        for (size_t p = w; p > 0 && p + 16 > w; --p) {
            ResourceRecord* prev = &records->items[p - 1];
            if (prev->cont != cur->cont) break;
            if (prev->raw_off == cur->raw_off && prev->res_id == cur->res_id && strcmp(prev->layout, cur->layout) == 0 && strcmp(prev->pointer_form, cur->pointer_form) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) records->items[w++] = *cur;
    }
    records->count = w;

    for (size_t i = 0; i < records->count; ++i) {
        uint32_t resource_end = records->items[i].sector_end;
        for (size_t j = i + 1; j < records->count; ++j) {
            if (records->items[j].cont != records->items[i].cont) break;
            if (strcmp(records->items[j].layout, records->items[i].layout) != 0) continue;
            if (strcmp(records->items[j].pointer_form, records->items[i].pointer_form) != 0) continue;
            if (records->items[j].raw_off > records->items[i].raw_off) {
                resource_end = records->items[j].raw_off;
                break;
            }
        }
        if (resource_end <= records->items[i].raw_off) resource_end = records->items[i].raw_off + 4u;
        if (resource_end > img_size) resource_end = (uint32_t)img_size;
        records->items[i].resource_end = resource_end;
    }
}

static const char* extension_for_header(const ContinuationHeader* h) {
    if (strcmp(h->tag, "xet_") == 0 || h->tag[0] == 'x') return "tex";
    return "wrld";
}

static const char* extension_for_resource(const ResourceRecord* rec) {
    if (strcmp(rec->kind, "wrld") == 0) return "wrld";
    if (strcmp(rec->kind, "tex") == 0) return "tex";
    if (strcmp(rec->kind, "mdl") == 0) return "mdl";
    return "bin";
}

static int write_continuation_file(
    FILE* img_file,
    u64 img_size,
    const uint8_t* lvz,
    size_t lvz_size,
    const ContinuationHeader* h,
    const char* out_path,
    FILE* log
) {
    if ((u64)h->lvz_off + 0x20ull > (u64)lvz_size) return 0;

    u64 start = h->continuation;
    u64 need = h->total_size >= 0x20u ? (u64)h->total_size - 0x20ull : 0;
    if (start > img_size || need > img_size - start) {
        fprintf(log, "[skip] bad continuation range: lvz=0x%08X cont=0x%08X size=0x%08X img=0x%llX\n",
            h->lvz_off, h->continuation, h->total_size, (unsigned long long)img_size);
        return 0;
    }
    u64 end = start + need;

    FILE* out = fopen(out_path, "wb");
    if (!out) {
        fprintf(log, "[error] cannot write %s (%s)\n", out_path, strerror(errno));
        return 0;
    }

    if (fwrite(lvz + h->lvz_off, 1, 0x20, out) != 0x20) {
        fprintf(log, "[error] failed to write header: %s\n", out_path);
        fclose(out);
        remove(out_path);
        return 0;
    }

    u64 copied = 0;
    if (!copy_file_slice(img_file, start, end, out, &copied)) {
        fprintf(log, "[error] failed to copy continuation data: %s\n", out_path);
        fclose(out);
        remove(out_path);
        return 0;
    }
    if (fclose(out) != 0) {
        fprintf(log, "[error] failed to finish %s\n", out_path);
        remove(out_path);
        return 0;
    }

    fprintf(log, "[continuation] %s lvz=0x%08X tag=%s cont=0x%08X body=0x%llX total=0x%llX\n",
        out_path, h->lvz_off, h->tag, h->continuation,
        (unsigned long long)copied, (unsigned long long)(copied + 0x20ull));
    return 1;
}

static void write_continuation_outputs(
    FILE* img_file,
    u64 img_size,
    const uint8_t* lvz,
    size_t lvz_size,
    const HeaderList* headers,
    const char* out_dir,
    FILE* log,
    size_t* written_count
) {
    char dir[2048];
    path_join(dir, sizeof(dir), out_dir, "continuations");
    make_dirs(dir);
    *written_count = 0;
    for (size_t i = 0; i < headers->count; ++i) {
        const ContinuationHeader* h = &headers->items[i];
        char name[256];
        snprintf(name, sizeof(name), "%s_%04zu_lvz_%08X_cont_%08X.%s", h->tag, i, h->lvz_off, h->continuation, extension_for_header(h));
        char path[2300];
        path_join(path, sizeof(path), dir, name);
        if (write_continuation_file(img_file, img_size, lvz, lvz_size, h, path, log)) (*written_count)++;
    }
}

static void write_master_resources_csv(
    const uint8_t* lvz,
    size_t n,
    const MasterInfo* master,
    uint32_t res_count,
    int stride,
    const char* out_dir,
    FILE* log
) {
    char path[2048];
    path_join(path, sizeof(path), out_dir, "master_resources.csv");
    FILE* csv = fopen(path, "w");
    if (!csv) {
        fprintf(log, "[error] cannot write %s\n", path);
        return;
    }
    fprintf(csv, "resource_index,res_id,stride,row_off,raw_ptr,aux_or_dma,id_field,kind\n");

    if (master->res_table_addr == 0 || master->res_table_addr >= n || res_count == 0) {
        fclose(csv);
        return;
    }

    uint64_t table_end = (uint64_t)master->res_table_addr + (uint64_t)res_count * (uint64_t)stride;
    if (table_end > n) table_end = n;

    for (uint32_t i = 0; i < res_count; ++i) {
        uint32_t row_off = master->res_table_addr + i * (uint32_t)stride;
        if ((uint64_t)row_off + (uint64_t)stride > table_end) break;
        uint32_t raw_ptr = read_u32le(lvz, row_off + 0);
        uint32_t aux = read_u32le(lvz, row_off + 4);
        uint32_t id_field = stride == 12 ? read_u32le(lvz, row_off + 8) : i;
        int res_id = stride == 12 && id_field != 0xFFFFFFFFu ? (int)id_field : (int)i;
        char kind[16];
        snprintf(kind, sizeof(kind), "empty");
        if (raw_ptr != 0 && raw_ptr != 0xFFFFFFFFu) {
            if (raw_ptr + 4 <= n && is_known_preface_tag(lvz, n, raw_ptr)) {
                if (lvz[raw_ptr] == 'D') snprintf(kind, sizeof(kind), "wrld");
                else snprintf(kind, sizeof(kind), "tex");
            } else if (raw_ptr + 4 <= n) {
                snprintf(kind, sizeof(kind), "lvz_data");
            } else {
                snprintf(kind, sizeof(kind), "invalid");
            }
        }
        fprintf(csv, "%u,%d,%d,0x%08X,0x%08X,0x%08X,0x%08X,%s\n", i, res_id, stride, row_off, raw_ptr, aux, id_field, kind);
    }
    fclose(csv);
    fprintf(log, "[csv] %s\n", path);
}

static void write_sector_containers_csv(const SectorContainerList* containers, const char* out_dir, FILE* log) {
    char path[2048];
    path_join(path, sizeof(path), out_dir, "sector_containers.csv");
    FILE* csv = fopen(path, "w");
    if (!csv) {
        fprintf(log, "[error] cannot write %s\n", path);
        return;
    }
    fprintf(csv, "sector_index,row_index,header_index,game,sector_x,sector_y,lvz_header,file_size,data_size,reloc_table,reloc_count,cont,end\n");
    for (size_t i = 0; i < containers->count; ++i) {
        const SectorContainer* c = &containers->items[i];
        fprintf(csv, "%d,%d,%d,%s,%d,%d,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
            c->sector_index,
            c->row_index,
            c->header_index,
            c->game,
            c->sector_x,
            c->sector_y,
            c->header_addr,
            c->file_size,
            c->data_size,
            c->reloc_table,
            c->reloc_count,
            c->cont,
            c->end);
    }
    fclose(csv);
    fprintf(log, "[csv] %s\n", path);
}

static void write_sector_resources_csv(const ResourceRecordList* resources, const char* out_dir, FILE* log) {
    char path[2048];
    path_join(path, sizeof(path), out_dir, "sector_resources.csv");
    FILE* csv = fopen(path, "w");
    if (!csv) {
        fprintf(log, "[error] cannot write %s\n", path);
        return;
    }
    fprintf(csv, "source,res_id,resource_index,sector_index,row_index,stride,layout,pointer_form,kind,cont,sector_end,row_off,raw_ptr,raw_off,resource_end,size,alternate_pointer\n");
    for (size_t i = 0; i < resources->count; ++i) {
        const ResourceRecord* r = &resources->items[i];
        uint32_t size = r->resource_end > r->raw_off ? r->resource_end - r->raw_off : 0;
        fprintf(csv, "%s,%d,%d,%d,%d,%d,%s,%s,%s,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%X,%d\n",
            r->source,
            r->res_id,
            r->resource_index,
            r->sector_index,
            r->row_index,
            r->stride,
            r->layout,
            r->pointer_form,
            r->kind,
            r->cont,
            r->sector_end,
            r->row_off,
            r->raw_ptr,
            r->raw_off,
            r->resource_end,
            size,
            r->alternate_pointer);
    }
    fclose(csv);
    fprintf(log, "[csv] %s\n", path);
}

static void write_resource_outputs(
    const uint8_t* img,
    size_t img_size,
    const ResourceRecordList* resources,
    const char* out_dir,
    FILE* log,
    size_t* written_count
) {
    char dir[2048];
    path_join(dir, sizeof(dir), out_dir, "resources");
    make_dirs(dir);
    *written_count = 0;

    for (size_t i = 0; i < resources->count; ++i) {
        const ResourceRecord* r = &resources->items[i];
        if (r->resource_end <= r->raw_off) continue;
        uint32_t size = r->resource_end - r->raw_off;
        if (size < 4) continue;
        if (size > STORIES_MAX_RESOURCE_EXTRACT_BYTES) {
            fprintf(log, "[skip] resource too large: res=%d off=0x%08X size=0x%08X\n", r->res_id, r->raw_off, size);
            continue;
        }
        if ((u64)r->raw_off + (u64)size > (u64)img_size) continue;

        char name[512];
        snprintf(name, sizeof(name), "res_%06d_sector_%04d_row_%03d_ri_%04d_off_%08X_%s_%s.%s",
            r->res_id,
            r->sector_index,
            r->row_index,
            r->resource_index,
            r->raw_off,
            r->layout,
            r->pointer_form,
            extension_for_resource(r));
        for (char* p = name; *p; ++p) {
            if (*p == ':' || *p == '/' || *p == '\\' || *p == ' ') *p = '_';
        }

        char path[2600];
        path_join(path, sizeof(path), dir, name);
        if (write_binary_file(path, img + r->raw_off, size)) {
            (*written_count)++;
            fprintf(log, "[resource] %s res=%d kind=%s off=0x%08X size=0x%08X layout=%s pointer=%s\n",
                path,
                r->res_id,
                r->kind,
                r->raw_off,
                size,
                r->layout,
                r->pointer_form);
        } else {
            fprintf(log, "[error] cannot write resource file: %s\n", path);
        }
    }
}

static void write_summary_txt(
    const char* out_dir,
    const Options* options,
    const char* img_path,
    size_t lvz_raw_len,
    size_t lvz_len,
    int decompressed,
    u64 img_size,
    int master_stride,
    uint32_t resource_count,
    int game,
    size_t continuation_count,
    size_t continuation_written,
    size_t sector_count,
    size_t sector_resource_count,
    size_t resources_written
) {
    char path[2048];
    path_join(path, sizeof(path), out_dir, "summary.txt");
    FILE* f = fopen(path, "w");
    if (!f) return;
    time_t now = time(NULL);
    fprintf(f, "Stories LVZ+IMG unpack summary\n");
    fprintf(f, "Time: %s", ctime(&now));
    fprintf(f, "LVZ: %s\n", options->lvz_path);
    fprintf(f, "IMG: %s\n", img_path);
    fprintf(f, "Output: %s\n", out_dir);
    fprintf(f, "Requested game: %s\n", game_mode_name(options->game));
    fprintf(f, "Resolved game: %s\n", game_name_from_int(game));
    fprintf(f, "LVZ raw bytes: %zu\n", lvz_raw_len);
    fprintf(f, "LVZ working bytes: %zu\n", lvz_len);
    fprintf(f, "LVZ decompressed: %s\n", decompressed ? "yes" : "no");
    fprintf(f, "IMG bytes: %llu\n", (unsigned long long)img_size);
    fprintf(f, "Master resource count: %u\n", resource_count);
    fprintf(f, "Master resource stride: %d\n", master_stride);
    fprintf(f, "Continuation headers found: %zu\n", continuation_count);
    fprintf(f, "Continuation files written: %zu\n", continuation_written);
    fprintf(f, "Sector containers: %zu\n", sector_count);
    fprintf(f, "Sector resource records: %zu\n", sector_resource_count);
    fprintf(f, "Resource files written: %zu\n", resources_written);
    fprintf(f, "Wanted id filter: %s\n", options->wanted_ids.enabled ? "enabled" : "disabled");
    if (options->wanted_ids.enabled) {
        fprintf(f, "Wanted ids:");
        for (size_t i = 0; i < options->wanted_ids.count; ++i) fprintf(f, " %d", options->wanted_ids.ids[i]);
        fprintf(f, "\n");
    }
    fprintf(f, "Pointer forms: %s\n", options->all_pointer_forms ? "all" : "requested ids only");
    fclose(f);
}

static void print_usage(void) {
    fprintf(stderr,
        "Stories LVZ+IMG unpacker\n"
        "\n"
        "Usage:\n"
        "  unimg <file.lvz> [options]\n"
        "\n"
        "Options:\n"
        "  --img <file.img>            Use an explicit companion IMG path.\n"
        "  --out <folder>              Use an explicit output folder.\n"
        "  --game auto|lcs|vcs         Set the game layout. Default: auto.\n"
        "  --wanted <ids>              Only write and list these resource ids, comma separated.\n"
        "  --all-pointer-variants      Check every supported pointer form for every resource id.\n"
        "  --no-continuations          Do not rebuild DLRW/xet continuation files.\n"
        "  --no-resources              Do not write IMG resource payload files. CSVs are still written.\n"
        "  --resources-only            Skip continuation files, keep resource payload output.\n"
        "\n"
        "Examples:\n"
        "  unimg beach.lvz --game vcs\n"
        "  unimg indust.lvz --game lcs --wanted 1881,1828\n"
    );
}

static Options parse_options(int argc, char** argv) {
    Options options;
    memset(&options, 0, sizeof(options));
    options.game = GAME_AUTO;
    options.write_continuations = 1;
    options.write_resources = 1;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strcmp(arg, "--img") == 0) {
            if (i + 1 >= argc) die("--img needs a path");
            options.img_path_override = argv[++i];
        } else if (strcmp(arg, "--out") == 0) {
            if (i + 1 >= argc) die("--out needs a folder");
            options.out_dir_override = argv[++i];
        } else if (strcmp(arg, "--game") == 0) {
            if (i + 1 >= argc) die("--game needs auto, lcs, or vcs");
            options.game = parse_game_mode(argv[++i]);
        } else if (strcmp(arg, "--wanted") == 0) {
            if (i + 1 >= argc) die("--wanted needs a comma-separated id list");
            parse_wanted_ids(&options.wanted_ids, argv[++i]);
        } else if (strcmp(arg, "--all-pointer-variants") == 0) {
            options.all_pointer_forms = 1;
        } else if (strcmp(arg, "--no-continuations") == 0) {
            options.write_continuations = 0;
        } else if (strcmp(arg, "--no-resources") == 0) {
            options.write_resources = 0;
        } else if (strcmp(arg, "--resources-only") == 0) {
            options.write_continuations = 0;
            options.write_resources = 1;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0 || strcmp(arg, "/?") == 0) {
            print_usage();
            exit(0);
        } else if (arg[0] == '-') {
            die("Unknown option: %s", arg);
        } else if (!options.lvz_path) {
            options.lvz_path = arg;
        } else {
            die("Unexpected extra argument: %s", arg);
        }
    }

    if (!options.lvz_path) {
        print_usage();
        exit(1);
    }
    return options;
}

int main(int argc, char** argv) {
    Options options = parse_options(argc, argv);

    char img_path[2048];
    if (options.img_path_override) snprintf(img_path, sizeof(img_path), "%s", options.img_path_override);
    else derive_img_path(options.lvz_path, img_path, sizeof(img_path));
    if (!file_exists(img_path)) die("Matching IMG not found. Tried: %s", img_path);

    char out_dir[2048];
    if (options.out_dir_override) snprintf(out_dir, sizeof(out_dir), "%s", options.out_dir_override);
    else default_out_dir(options.lvz_path, out_dir, sizeof(out_dir));
    make_dirs(out_dir);

    char log_path[2300];
    path_join(log_path, sizeof(log_path), out_dir, "unpack.log");
    FILE* log = fopen(log_path, "w");
    if (!log) die("Cannot open log: %s", log_path);

    size_t lvz_raw_len = 0;
    uint8_t* lvz_raw = read_entire_file(options.lvz_path, &lvz_raw_len);
    uint8_t* lvz = NULL;
    size_t lvz_len = 0;
    int decompressed = 0;
    maybe_decompress_lvz(lvz_raw, lvz_raw_len, &lvz, &lvz_len, &decompressed);
    free(lvz_raw);

    if (lvz_len > UINT32_MAX) die("LVZ is larger than the 32-bit format can address: %s", options.lvz_path);

    size_t img_len = 0;
    uint8_t* img_bytes = read_entire_file(img_path, &img_len);
    if (img_len > UINT32_MAX) die("IMG is larger than the 32-bit format can address: %s", img_path);

    FILE* img_file = fopen(img_path, "rb");
    if (!img_file) die("Cannot open IMG: %s", img_path);
    u64 img_size = file_size_of_open(img_file);

    fprintf(log, "Stories LVZ+IMG unpacker\n");
    fprintf(log, "LVZ: %s\n", options.lvz_path);
    fprintf(log, "IMG: %s\n", img_path);
    fprintf(log, "Out: %s\n", out_dir);
    fprintf(log, "LVZ raw bytes: %zu\n", lvz_raw_len);
    fprintf(log, "LVZ working bytes: %zu\n", lvz_len);
    fprintf(log, "LVZ decompressed: %s\n", decompressed ? "yes" : "no");
    fprintf(log, "IMG bytes: %llu\n", (unsigned long long)img_size);

    MasterInfo master;
    parse_master_info(lvz, lvz_len, &master);

    SectorRowDirectoryList sector_rows;
    find_sector_row_directories(lvz, lvz_len, &sector_rows);

    uint32_t resource_count = detect_master_resource_count(lvz, lvz_len, &master, &sector_rows);
    uint32_t first_group_addr = sector_rows.count ? sector_rows.items[0].header_addr : 0;
    int master_stride = detect_master_resource_stride(lvz, lvz_len, master.res_table_addr, resource_count, first_group_addr);
    int game = game_from_row_count(sector_rows.count, options.game, master_stride);

    fprintf(log, "Master magic: 0x%08X\n", master.magic);
    fprintf(log, "Master Resource[] table: 0x%08X\n", master.res_table_addr);
    fprintf(log, "Master Resource[] count: %u\n", resource_count);
    fprintf(log, "Master Resource[] stride: %d\n", master_stride);
    fprintf(log, "Sector row directories: %zu\n", sector_rows.count);
    fprintf(log, "Resolved game: %s\n", game_name_from_int(game));
    fprintf(log, "Sector pass pointer count: %d\n", get_pass_count_for_game(game));

    HeaderList headers;
    scan_continuation_headers(lvz, lvz_len, img_size, &headers);
    fprintf(log, "Continuation headers: %zu\n", headers.count);

    SectorContainerList containers;
    collect_sector_containers(lvz, lvz_len, img_size, &sector_rows, game, &containers);
    fprintf(log, "Sector containers: %zu\n", containers.count);

    ResourceRecordList sector_resources;
    collect_sector_resources(img_bytes, img_len, &options, &containers, &sector_resources);
    fprintf(log, "Sector resource records: %zu\n", sector_resources.count);

    write_master_resources_csv(lvz, lvz_len, &master, resource_count, master_stride, out_dir, log);
    write_sector_containers_csv(&containers, out_dir, log);
    write_sector_resources_csv(&sector_resources, out_dir, log);

    size_t continuation_written = 0;
    if (options.write_continuations) {
        write_continuation_outputs(img_file, img_size, lvz, lvz_len, &headers, out_dir, log, &continuation_written);
    }

    size_t resources_written = 0;
    if (options.write_resources) {
        write_resource_outputs(img_bytes, img_len, &sector_resources, out_dir, log, &resources_written);
    }

    write_summary_txt(
        out_dir,
        &options,
        img_path,
        lvz_raw_len,
        lvz_len,
        decompressed,
        img_size,
        master_stride,
        resource_count,
        game,
        headers.count,
        continuation_written,
        containers.count,
        sector_resources.count,
        resources_written
    );

    fprintf(log, "Done. continuation_written=%zu resources_written=%zu\n", continuation_written, resources_written);
    fclose(img_file);
    fclose(log);

    fprintf(stderr, "Stories LVZ+IMG unpack complete.\n");
    fprintf(stderr, "Output: %s\n", out_dir);
    fprintf(stderr, "Log: %s\n", log_path);
    fprintf(stderr, "Continuations: %zu/%zu\n", continuation_written, headers.count);
    fprintf(stderr, "Sector containers: %zu\n", containers.count);
    fprintf(stderr, "Sector resources: %zu\n", sector_resources.count);
    fprintf(stderr, "Resource files: %zu\n", resources_written);

    free(headers.items);
    free(sector_rows.items);
    free(containers.items);
    free(sector_resources.items);
    free(options.wanted_ids.ids);
    free(lvz);
    free(img_bytes);
    return 0;
}