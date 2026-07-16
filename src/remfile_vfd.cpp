/*
 * remfile HDF5 Virtual File Driver — implementation.
 *
 * This is a C++ port of the Python remfile package's RemFile class, exposed
 * to the HDF5 library through the VFD (H5FD) layer. All HDF5 low-level reads
 * are answered from an in-memory chunk cache; cache misses are filled with
 * HTTP range requests via libcurl. Sequential access is detected and the
 * request size grows geometrically (factor 1.7), mirroring remfile's
 * "smart loader".
 */

#include "remfile/remfile_vfd.h"

/* The H5FD_class_t struct moved to H5FDdevelop.h and gained new fields in
 * HDF5 1.14; this driver supports both the old (1.10/1.12) and new (>= 1.14)
 * layouts. */
#if H5_VERSION_GE(1, 14, 0)
#  include <H5FDdevelop.h>
#endif
#include <curl/curl.h>

#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kDefaultMinChunkSize = 100 * 1024;
constexpr size_t kDefaultMaxCacheSize = 1000000000; /* 1e9, as in Python */
constexpr size_t kDefaultMaxChunkSize = 100 * 1024 * 1024;
constexpr int    kNumRequestRetries   = 8;

#if H5_VERSION_GE(1, 14, 0)
constexpr H5FD_class_value_t kRemFileVFDValue = static_cast<H5FD_class_value_t>(566);
#endif

hid_t g_driver_id = H5I_INVALID_HID;
std::once_flag g_curl_global_once;

struct RemFile {
    H5FD_t pub{}; /* must be first — HDF5 casts H5FD_t* <-> RemFile* */

    H5FD_remfile_config_t config{};
    std::string url;
    CURL       *curl = nullptr;
    uint64_t    length = 0;
    haddr_t     eoa = 0;

    std::unordered_map<uint64_t, std::vector<uint8_t>> chunks;
    std::deque<uint64_t> chunk_order; /* insertion order, for cache eviction */
    size_t   max_chunks_in_cache = 0;
    int64_t  last_chunk_index_accessed = -99;
    uint64_t chunk_sequence_length = 1;
};

/* ---------------------------------------------------------------------- */
/* HTTP layer (libcurl)                                                    */
/* ---------------------------------------------------------------------- */

size_t write_to_vector(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buf = static_cast<std::vector<uint8_t> *>(userdata);
    buf->insert(buf->end(), reinterpret_cast<uint8_t *>(ptr),
                reinterpret_cast<uint8_t *>(ptr) + size * nmemb);
    return size * nmemb;
}

struct HeaderCapture {
    uint64_t content_range_total = 0;
};

size_t capture_headers(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *cap = static_cast<HeaderCapture *>(userdata);
    std::string line(ptr, size * nmemb);
    /* Look for: Content-Range: bytes 0-0/123456789 */
    static const char prefix[] = "content-range:";
    if (line.size() > sizeof(prefix) - 1) {
        std::string lower = line;
        for (auto &c : lower)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (lower.compare(0, sizeof(prefix) - 1, prefix) == 0) {
            size_t slash = line.rfind('/');
            if (slash != std::string::npos)
                cap->content_range_total = strtoull(line.c_str() + slash + 1, nullptr, 10);
        }
    }
    return size * nmemb;
}

/* 4xx responses mean the request itself is wrong (missing file, bad URL,
 * expired credentials); retrying cannot fix them. Transient failures (network
 * errors, 5xx, throttling) are worth retrying. */
bool is_permanent_http_error(long http_code)
{
    return http_code >= 400 && http_code < 500 && http_code != 429;
}

/* Fetch [start_byte, end_byte] (inclusive) with retries and exponential
 * backoff, matching remfile's retry policy (8 retries, 0.1 * 2^n seconds). */
bool fetch_bytes(RemFile *f, uint64_t start_byte, uint64_t end_byte,
                 std::vector<uint8_t> &out)
{
    char range[64];
    snprintf(range, sizeof(range), "%" PRIu64 "-%" PRIu64, start_byte, end_byte);

    for (int try_num = 0; try_num <= kNumRequestRetries; try_num++) {
        out.clear();
        curl_easy_reset(f->curl);
        curl_easy_setopt(f->curl, CURLOPT_URL, f->url.c_str());
        curl_easy_setopt(f->curl, CURLOPT_RANGE, range);
        curl_easy_setopt(f->curl, CURLOPT_WRITEFUNCTION, write_to_vector);
        curl_easy_setopt(f->curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(f->curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(f->curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(f->curl, CURLOPT_ACCEPT_ENCODING, "identity");

        CURLcode rc = curl_easy_perform(f->curl);
        long http_code = 0;
        curl_easy_getinfo(f->curl, CURLINFO_RESPONSE_CODE, &http_code);

        uint64_t expected = end_byte - start_byte + 1;
        if (rc == CURLE_OK && (http_code == 206 || http_code == 200) &&
            out.size() == expected)
            return true;

        if (is_permanent_http_error(http_code)) {
            fprintf(stderr,
                    "remfile: request for bytes %s failed permanently "
                    "(http %ld)\n",
                    range, http_code);
            return false;
        }

        if (try_num == kNumRequestRetries) {
            fprintf(stderr,
                    "remfile: request for bytes %s failed after %d retries "
                    "(curl: %s, http: %ld, got %zu of %" PRIu64 " bytes)\n",
                    range, kNumRequestRetries, curl_easy_strerror(rc), http_code,
                    out.size(), expected);
            return false;
        }
        double delay = 0.1 * std::pow(2.0, try_num);
        if (f->config.verbose)
            fprintf(stderr, "remfile: retrying bytes %s after failure (waiting %.1f s)\n",
                    range, delay);
        std::this_thread::sleep_for(std::chrono::duration<double>(delay));
    }
    return false;
}

/* Determine the remote file size. Presigned S3 URLs often reject HEAD, so
 * (like Python remfile's aborted GET) we avoid it: a 1-byte range request
 * yields the total size in the Content-Range header. */
bool fetch_content_length(RemFile *f, uint64_t *length_out)
{
    /* Retried with the same policy as fetch_bytes: this is the first request
     * of the file's life, and a transient failure here would otherwise fail
     * the open outright even though every subsequent read would retry. */
    for (int try_num = 0; try_num <= kNumRequestRetries; try_num++) {
        std::vector<uint8_t> body;
        HeaderCapture cap;

        curl_easy_reset(f->curl);
        curl_easy_setopt(f->curl, CURLOPT_URL, f->url.c_str());
        curl_easy_setopt(f->curl, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(f->curl, CURLOPT_WRITEFUNCTION, write_to_vector);
        curl_easy_setopt(f->curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(f->curl, CURLOPT_HEADERFUNCTION, capture_headers);
        curl_easy_setopt(f->curl, CURLOPT_HEADERDATA, &cap);
        curl_easy_setopt(f->curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(f->curl, CURLOPT_FAILONERROR, 1L);

        CURLcode rc = curl_easy_perform(f->curl);
        long http_code = 0;
        curl_easy_getinfo(f->curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (rc == CURLE_OK && http_code == 206 && cap.content_range_total > 0) {
            *length_out = cap.content_range_total;
            return true;
        }

        /* A server that answers but refuses to honor the range is a permanent
         * condition — retrying it would just stall. Fail immediately. */
        if (rc == CURLE_OK && http_code == 200) {
            fprintf(stderr,
                    "remfile: server did not honor range request (http 200) — "
                    "byte-range support is required\n");
            return false;
        }

        /* 4xx means the request itself is wrong (missing file, bad URL, expired
         * credentials). Retrying cannot fix it and would just add latency. */
        if (is_permanent_http_error(http_code)) {
            fprintf(stderr,
                    "remfile: error getting file length: http %ld\n", http_code);
            return false;
        }

        if (try_num == kNumRequestRetries) {
            fprintf(stderr,
                    "remfile: error getting file length after %d retries "
                    "(curl: %s, http: %ld)\n",
                    kNumRequestRetries, curl_easy_strerror(rc), http_code);
            return false;
        }

        double delay = 0.1 * std::pow(2.0, try_num);
        if (f->config.verbose)
            fprintf(stderr,
                    "remfile: retrying file-length request after failure "
                    "(waiting %.1f s)\n",
                    delay);
        std::this_thread::sleep_for(std::chrono::duration<double>(delay));
    }
    return false;
}

/* ---------------------------------------------------------------------- */
/* Smart loader — direct port of RemFile._load_chunk / read                */
/* ---------------------------------------------------------------------- */

/* Python `round(x)` on the smart-loader expressions (x always ends in a
 * +0.5 term, so ordinary llround matches). */
uint64_t py_round(double x)
{
    return static_cast<uint64_t>(std::llround(x));
}

/* Load the chunk at chunk_index, plus read-ahead.
 *
 * chunks_needed is the number of chunks the *current* read still needs
 * starting at chunk_index. The fetch is never smaller than that, so a read
 * spanning many chunks costs one request rather than ramping into it over
 * several round trips. (This is a deliberate improvement over the Python
 * implementation, which only ever grows the request geometrically and so
 * pays a full round trip per step of the ramp on large reads.) Read-ahead
 * beyond the requested range still follows remfile's 1.7x smart loader. */
bool load_chunk(RemFile *f, uint64_t chunk_index, uint64_t chunks_needed)
{
    if (f->chunks.count(chunk_index)) {
        f->last_chunk_index_accessed = static_cast<int64_t>(chunk_index);
        return true;
    }

    const size_t min_chunk = f->config.min_chunk_size;
    const uint64_t max_seq = f->config.max_chunk_size / min_chunk;

    if (static_cast<int64_t>(chunk_index) == f->last_chunk_index_accessed + 1) {
        /* Sequential access: grow the request by a factor of 1.7. */
        f->chunk_sequence_length = py_round(f->chunk_sequence_length * 1.7 + 0.5);
    }
    else {
        /* Non-sequential access: shrink the request. */
        f->chunk_sequence_length = py_round(f->chunk_sequence_length / 1.7 + 0.5);
    }

    /* Never fetch less than what this read already needs. */
    if (f->chunk_sequence_length < chunks_needed)
        f->chunk_sequence_length = chunks_needed;
    if (f->chunk_sequence_length > max_seq)
        f->chunk_sequence_length = max_seq;

    /* Don't re-fetch chunks that are already cached ahead of us. */
    for (uint64_t j = 1; j < f->chunk_sequence_length; j++) {
        if (f->chunks.count(chunk_index + j)) {
            f->chunk_sequence_length = j;
            break;
        }
    }

    uint64_t data_start = chunk_index * min_chunk;
    uint64_t data_end   = data_start + min_chunk * f->chunk_sequence_length - 1;
    if (data_end >= f->length)
        data_end = f->length - 1;

    if (f->config.verbose)
        fprintf(stderr,
                "remfile: loading %" PRIu64 " chunks starting at %" PRIu64
                " (%.3f million bytes)\n",
                f->chunk_sequence_length, chunk_index,
                static_cast<double>(data_end - data_start + 1) / 1e6);

    std::vector<uint8_t> data;
    if (!fetch_bytes(f, data_start, data_end, data))
        return false;

    if (f->chunk_sequence_length == 1) {
        f->chunks[chunk_index] = std::move(data);
        f->chunk_order.push_back(chunk_index);
    }
    else {
        for (uint64_t i = 0; i < f->chunk_sequence_length; i++) {
            if (i * min_chunk >= data.size())
                break;
            size_t piece_start = i * min_chunk;
            size_t piece_end   = std::min(piece_start + min_chunk, data.size());
            f->chunks[chunk_index + i] =
                std::vector<uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(piece_start), data.begin() + static_cast<std::ptrdiff_t>(piece_end));
            f->chunk_order.push_back(chunk_index + i);
        }
    }
    f->last_chunk_index_accessed =
        static_cast<int64_t>(chunk_index + f->chunk_sequence_length - 1);
    return true;
}

void cleanup_cache(RemFile *f)
{
    if (f->chunk_order.size() <= f->max_chunks_in_cache)
        return;
    if (f->config.verbose)
        fprintf(stderr, "remfile: cleaning up cache\n");
    size_t num_to_drop = f->max_chunks_in_cache / 2;
    for (size_t i = 0; i < num_to_drop && !f->chunk_order.empty(); i++) {
        f->chunks.erase(f->chunk_order.front());
        f->chunk_order.pop_front();
    }
}

bool remfile_read_bytes(RemFile *f, uint64_t position, size_t size, void *buf)
{
    if (size == 0)
        return true;
    if (position + size > f->length)
        return false;

    const size_t min_chunk = f->config.min_chunk_size;
    uint64_t chunk_start_index = position / min_chunk;
    uint64_t chunk_end_index   = (position + size - 1) / min_chunk;

    for (uint64_t ci = chunk_start_index; ci <= chunk_end_index; ci++)
        if (!load_chunk(f, ci, chunk_end_index - ci + 1))
            return false;

    auto *out = static_cast<uint8_t *>(buf);
    size_t written = 0;
    for (uint64_t ci = chunk_start_index; ci <= chunk_end_index; ci++) {
        const std::vector<uint8_t> &chunk = f->chunks[ci];
        size_t chunk_offset = (ci == chunk_start_index) ? static_cast<size_t>(position % min_chunk) : 0;
        size_t chunk_length = std::min(chunk.size() - chunk_offset, size - written);
        memcpy(out + written, chunk.data() + chunk_offset, chunk_length);
        written += chunk_length;
    }

    cleanup_cache(f);
    return written == size;
}

/* ---------------------------------------------------------------------- */
/* VFD callbacks                                                           */
/* ---------------------------------------------------------------------- */

H5FD_t *remfile_open(const char *name, unsigned flags, hid_t fapl_id,
                     haddr_t maxaddr)
{
    (void)maxaddr;
    if (!name || !*name)
        return nullptr;
    if (flags & (H5F_ACC_RDWR | H5F_ACC_TRUNC | H5F_ACC_CREAT | H5F_ACC_EXCL)) {
        fprintf(stderr, "remfile: driver is read-only (use H5F_ACC_RDONLY)\n");
        return nullptr;
    }

    std::call_once(g_curl_global_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    auto *f = new (std::nothrow) RemFile();
    if (!f)
        return nullptr;

    /* Driver info is absent when the driver was selected by name with no
     * config (e.g. via HDF5_PLUGIN_PATH); fall back to defaults silently. */
    const void *fa = nullptr;
    H5E_BEGIN_TRY
    {
        fa = H5Pget_driver_info(fapl_id);
    }
    H5E_END_TRY
    if (fa)
        f->config = *static_cast<const H5FD_remfile_config_t *>(fa);
    else
        H5FD_remfile_config_init(&f->config);

    f->url = name;
    f->max_chunks_in_cache = f->config.max_cache_size / f->config.min_chunk_size;

    /* One easy handle per file: connections are reused across requests,
     * which is the equivalent of remfile's requests.Session. */
    f->curl = curl_easy_init();
    if (!f->curl || !fetch_content_length(f, &f->length)) {
        if (f->curl)
            curl_easy_cleanup(f->curl);
        delete f;
        return nullptr;
    }

    return &f->pub;
}

herr_t remfile_close(H5FD_t *_file)
{
    auto *f = reinterpret_cast<RemFile *>(_file);
    if (f->curl)
        curl_easy_cleanup(f->curl);
    delete f;
    return 0;
}

int remfile_cmp(const H5FD_t *_f1, const H5FD_t *_f2)
{
    auto *f1 = reinterpret_cast<const RemFile *>(_f1);
    auto *f2 = reinterpret_cast<const RemFile *>(_f2);
    return f1->url.compare(f2->url);
}

herr_t remfile_query(const H5FD_t *_file, unsigned long *flags)
{
    (void)_file;
    if (flags) {
        /* Same feature set as the ros3 VFD: let HDF5's data sieve buffer and
         * small-data aggregation reduce the number of driver reads. */
        *flags = H5FD_FEAT_DATA_SIEVE | H5FD_FEAT_AGGREGATE_SMALLDATA;
    }
    return 0;
}

haddr_t remfile_get_eoa(const H5FD_t *_file, H5FD_mem_t type)
{
    (void)type;
    return reinterpret_cast<const RemFile *>(_file)->eoa;
}

herr_t remfile_set_eoa(H5FD_t *_file, H5FD_mem_t type, haddr_t addr)
{
    (void)type;
    reinterpret_cast<RemFile *>(_file)->eoa = addr;
    return 0;
}

haddr_t remfile_get_eof(const H5FD_t *_file, H5FD_mem_t type)
{
    (void)type;
    return static_cast<haddr_t>(reinterpret_cast<const RemFile *>(_file)->length);
}

herr_t remfile_read(H5FD_t *_file, H5FD_mem_t type, hid_t dxpl_id, haddr_t addr,
                    size_t size, void *buf)
{
    (void)type;
    (void)dxpl_id;
    auto *f = reinterpret_cast<RemFile *>(_file);
    return remfile_read_bytes(f, static_cast<uint64_t>(addr), size, buf) ? 0 : -1;
}

herr_t remfile_write(H5FD_t *_file, H5FD_mem_t type, hid_t dxpl_id, haddr_t addr,
                     size_t size, const void *buf)
{
    (void)_file;
    (void)type;
    (void)dxpl_id;
    (void)addr;
    (void)size;
    (void)buf;
    fprintf(stderr, "remfile: cannot write — driver is read-only\n");
    return -1;
}

herr_t remfile_flush(H5FD_t *_file, hid_t dxpl_id, hbool_t closing)
{
    (void)_file;
    (void)dxpl_id;
    (void)closing;
    return 0; /* nothing to flush — read-only */
}

herr_t remfile_truncate(H5FD_t *_file, hid_t dxpl_id, hbool_t closing)
{
    (void)_file;
    (void)dxpl_id;
    (void)closing;
    return 0; /* nothing to truncate — read-only */
}

const H5FD_class_t remfile_class = {
#if H5_VERSION_GE(1, 14, 0)
    H5FD_CLASS_VERSION,            /* version          */
    kRemFileVFDValue,              /* value            */
#endif
    "remfile",                     /* name             */
    HADDR_MAX,                     /* maxaddr          */
    H5F_CLOSE_WEAK,                /* fc_degree        */
    nullptr,                       /* terminate        */
    nullptr,                       /* sb_size          */
    nullptr,                       /* sb_encode        */
    nullptr,                       /* sb_decode        */
    sizeof(H5FD_remfile_config_t), /* fapl_size        */
    nullptr,                       /* fapl_get         */
    nullptr,                       /* fapl_copy        */
    nullptr,                       /* fapl_free        */
    0,                             /* dxpl_size        */
    nullptr,                       /* dxpl_copy        */
    nullptr,                       /* dxpl_free        */
    remfile_open,                  /* open             */
    remfile_close,                 /* close            */
    remfile_cmp,                   /* cmp              */
    remfile_query,                 /* query            */
    nullptr,                       /* get_type_map     */
    nullptr,                       /* alloc            */
    nullptr,                       /* free             */
    remfile_get_eoa,               /* get_eoa          */
    remfile_set_eoa,               /* set_eoa          */
    remfile_get_eof,               /* get_eof          */
    nullptr,                       /* get_handle       */
    remfile_read,                  /* read             */
    remfile_write,                 /* write            */
#if H5_VERSION_GE(1, 14, 0)
    nullptr,                       /* read_vector      */
    nullptr,                       /* write_vector     */
    nullptr,                       /* read_selection   */
    nullptr,                       /* write_selection  */
#endif
    remfile_flush,                 /* flush            */
    remfile_truncate,              /* truncate         */
    nullptr,                       /* lock             */
    nullptr,                       /* unlock           */
#if H5_VERSION_GE(1, 14, 0)
    nullptr,                       /* del              */
    nullptr,                       /* ctl              */
#endif
    H5FD_FLMAP_DICHOTOMY           /* fl_map           */
};

} /* anonymous namespace */

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

extern "C" {

void H5FD_remfile_config_init(H5FD_remfile_config_t *config)
{
    if (!config)
        return;
    config->min_chunk_size = kDefaultMinChunkSize;
    config->max_cache_size = kDefaultMaxCacheSize;
    config->max_chunk_size = kDefaultMaxChunkSize;
    config->verbose        = 0;
}

hid_t H5FD_remfile_init(void)
{
    if (g_driver_id == H5I_INVALID_HID || H5Iis_valid(g_driver_id) <= 0)
        g_driver_id = H5FDregister(&remfile_class);
    return g_driver_id;
}

herr_t H5Pset_fapl_remfile(hid_t fapl_id, const H5FD_remfile_config_t *config)
{
    hid_t driver_id = H5FD_remfile_init();
    if (driver_id < 0)
        return -1;

    H5FD_remfile_config_t fa;
    if (config) {
        fa = *config;
        if (fa.min_chunk_size == 0 || fa.max_chunk_size < fa.min_chunk_size ||
            fa.max_cache_size < fa.min_chunk_size)
            return -1;
    }
    else {
        H5FD_remfile_config_init(&fa);
    }
    return H5Pset_driver(fapl_id, driver_id, &fa);
}

} /* extern "C" */

/* ---------------------------------------------------------------------- */
/* Dynamic VFD plugin entry points (HDF5_PLUGIN_PATH /                     */
/* H5Pset_driver_by_name("remfile", ...))                                  */
/* ---------------------------------------------------------------------- */

#if defined(REMFILE_VFD_BUILD_PLUGIN) && H5_VERSION_GE(1, 14, 0)
#include <H5PLextern.h>

extern "C" {

H5PL_type_t H5PLget_plugin_type(void)
{
    return H5PL_TYPE_VFD;
}

const void *H5PLget_plugin_info(void)
{
    return &remfile_class;
}

} /* extern "C" */
#endif /* REMFILE_VFD_BUILD_PLUGIN */
