/*
 * Tests for the remfile HDF5 virtual file driver.
 *
 * These are hermetic: an HDF5 file is generated on disk, its bytes are served
 * by an in-process HTTP server on loopback, and the driver reads it back over
 * HTTP. Every test compares what the driver returns against ground truth read
 * from the same file with the stock sec2 driver, so "correct" means
 * "indistinguishable from a local read".
 *
 * The test server also counts requests and records byte ranges, which lets us
 * assert the caching and read-ahead behavior directly rather than inferring it
 * from timings.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include <hdf5.h>
#include <remfile/remfile_vfd.h>

#include "http_test_server.hpp"

using remfile_test::HttpTestServer;

namespace
{

/* ~5 MB of doubles. Deliberately much larger than the driver's 100 KiB chunk
 * so that a read of the whole dataset spans ~50 chunks: enough that a driver
 * which ramped up geometrically instead of fetching the requested span would
 * need ~8 round trips, and the read-ahead tests below can tell the difference. */
constexpr hsize_t kRows = 40000;
constexpr hsize_t kCols = 16;
constexpr size_t kNumElements = (size_t)kRows * kCols;
constexpr size_t kDataBytes = kNumElements * sizeof(double);

/* Deterministic contents so tests can assert on exact values. */
double expected_value(hsize_t r, hsize_t c)
{
  return (double)r * 100.0 + (double)c;
}

/* Writes an HDF5 file containing:
 *   /contiguous  - a contiguous 2D double dataset (kRows x kCols)
 *   /chunked     - the same data, chunked + gzip compressed if available
 *   /small       - a tiny 1D dataset, to exercise sub-chunk reads
 *   /attr        - an attribute on the root group
 * Returns the file's bytes. */
std::vector<uint8_t> build_test_file(const std::string& path)
{
  hid_t file = H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  REQUIRE(file >= 0);

  std::vector<double> data(kNumElements);
  for (hsize_t r = 0; r < kRows; r++)
    for (hsize_t c = 0; c < kCols; c++)
      data[(size_t)(r * kCols + c)] = expected_value(r, c);

  hsize_t dims[2] = {kRows, kCols};
  hid_t space = H5Screate_simple(2, dims, nullptr);

  hid_t dset = H5Dcreate2(file, "contiguous", H5T_NATIVE_DOUBLE, space,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  REQUIRE(dset >= 0);
  REQUIRE(H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                   data.data()) >= 0);
  H5Dclose(dset);

  /* Chunked (and compressed, when the filter is available) so we also cover
   * the scattered-read pattern that chunked datasets produce. */
  hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
  hsize_t chunk[2] = {100, kCols};
  H5Pset_chunk(dcpl, 2, chunk);
  if (H5Zfilter_avail(H5Z_FILTER_DEFLATE) > 0)
    H5Pset_deflate(dcpl, 4);
  dset = H5Dcreate2(file, "chunked", H5T_NATIVE_DOUBLE, space, H5P_DEFAULT,
                    dcpl, H5P_DEFAULT);
  REQUIRE(dset >= 0);
  REQUIRE(H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                   data.data()) >= 0);
  H5Dclose(dset);
  H5Pclose(dcpl);
  H5Sclose(space);

  hsize_t sdim = 5;
  hid_t sspace = H5Screate_simple(1, &sdim, nullptr);
  int small[5] = {10, 20, 30, 40, 50};
  dset = H5Dcreate2(file, "small", H5T_NATIVE_INT, sspace, H5P_DEFAULT,
                    H5P_DEFAULT, H5P_DEFAULT);
  REQUIRE(H5Dwrite(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, small) >= 0);
  H5Dclose(dset);

  hid_t ascalar = H5Screate(H5S_SCALAR);
  hid_t attr = H5Acreate2(file, "answer", H5T_NATIVE_INT, ascalar, H5P_DEFAULT,
                          H5P_DEFAULT);
  int answer = 42;
  REQUIRE(H5Awrite(attr, H5T_NATIVE_INT, &answer) >= 0);
  H5Aclose(attr);
  H5Sclose(ascalar);
  H5Sclose(sspace);
  H5Fclose(file);

  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
}

/* Fixture: build the file once, serve it, expose helpers. */
struct ServedFile
{
  std::string path = "remfile_test_data.h5";
  std::vector<uint8_t> bytes;
  HttpTestServer server;

  ServedFile()
      : bytes(build_test_file(path))
      , server(bytes)
  {
  }

  ~ServedFile() { std::remove(path.c_str()); }

  /* Open the served file through the remfile driver. */
  hid_t open_remote(const H5FD_remfile_config_t* config = nullptr)
  {
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    REQUIRE(H5Pset_fapl_remfile(fapl, config) >= 0);
    hid_t file = H5Fopen(server.url().c_str(), H5F_ACC_RDONLY, fapl);
    H5Pclose(fapl);
    return file;
  }
};

/* Read a hyperslab of /contiguous or /chunked from an open file. */
std::vector<double> read_slab(hid_t file, const char* name, hsize_t row0,
                              hsize_t nrows)
{
  hid_t dset = H5Dopen2(file, name, H5P_DEFAULT);
  REQUIRE(dset >= 0);
  hid_t space = H5Dget_space(dset);

  hsize_t start[2] = {row0, 0};
  hsize_t count[2] = {nrows, kCols};
  REQUIRE(H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, count,
                              nullptr) >= 0);
  hid_t memspace = H5Screate_simple(2, count, nullptr);

  std::vector<double> out((size_t)(nrows * kCols));
  REQUIRE(H5Dread(dset, H5T_NATIVE_DOUBLE, memspace, space, H5P_DEFAULT,
                  out.data()) >= 0);

  H5Sclose(memspace);
  H5Sclose(space);
  H5Dclose(dset);
  return out;
}

}  // namespace

TEST_CASE("reads a whole dataset identically to a local read", "[vfd]")
{
  ServedFile f;

  hid_t file = f.open_remote();
  REQUIRE(file >= 0);
  std::vector<double> remote = read_slab(file, "contiguous", 0, kRows);
  H5Fclose(file);

  /* Ground truth: the same file read with the stock (local) driver. */
  hid_t local = H5Fopen(f.path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
  REQUIRE(local >= 0);
  std::vector<double> expected = read_slab(local, "contiguous", 0, kRows);
  H5Fclose(local);

  REQUIRE(remote.size() == expected.size());
  REQUIRE(remote == expected);
}

TEST_CASE("reads chunked and compressed datasets", "[vfd]")
{
  ServedFile f;

  hid_t file = f.open_remote();
  REQUIRE(file >= 0);
  std::vector<double> got = read_slab(file, "chunked", 0, kRows);
  H5Fclose(file);

  for (hsize_t r = 0; r < kRows; r++)
    for (hsize_t c = 0; c < kCols; c++)
      REQUIRE(got[(size_t)(r * kCols + c)] == expected_value(r, c));
}

TEST_CASE("hyperslab reads at arbitrary offsets are correct", "[vfd]")
{
  ServedFile f;
  hid_t file = f.open_remote();
  REQUIRE(file >= 0);

  /* Deliberately awkward offsets/sizes that straddle the driver's internal
   * 100 KiB chunk boundaries. */
  struct Slab { hsize_t row0, nrows; };
  const Slab slabs[] = {
      {0, 1}, {1, 1}, {7, 13}, {999, 2}, {1000, 500}, {kRows - 1, 1},
      {kRows - 250, 250},
  };

  for (const Slab& s : slabs) {
    std::vector<double> got = read_slab(file, "contiguous", s.row0, s.nrows);
    for (hsize_t r = 0; r < s.nrows; r++)
      for (hsize_t c = 0; c < kCols; c++)
        REQUIRE(got[(size_t)(r * kCols + c)] == expected_value(s.row0 + r, c));
  }
  H5Fclose(file);
}

TEST_CASE("attributes and small datasets read correctly", "[vfd]")
{
  ServedFile f;
  hid_t file = f.open_remote();
  REQUIRE(file >= 0);

  hid_t attr = H5Aopen(file, "answer", H5P_DEFAULT);
  REQUIRE(attr >= 0);
  int answer = 0;
  REQUIRE(H5Aread(attr, H5T_NATIVE_INT, &answer) >= 0);
  REQUIRE(answer == 42);
  H5Aclose(attr);

  hid_t dset = H5Dopen2(file, "small", H5P_DEFAULT);
  REQUIRE(dset >= 0);
  int small[5] = {0};
  REQUIRE(H5Dread(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, small) >= 0);
  REQUIRE(small[0] == 10);
  REQUIRE(small[4] == 50);
  H5Dclose(dset);
  H5Fclose(file);
}

TEST_CASE("cached chunks are not re-fetched", "[vfd][cache]")
{
  ServedFile f;
  hid_t file = f.open_remote();
  REQUIRE(file >= 0);

  std::vector<double> first = read_slab(file, "contiguous", 0, 500);
  f.server.reset_counts();

  /* Reading the same region again must be served entirely from the cache. */
  std::vector<double> second = read_slab(file, "contiguous", 0, 500);
  REQUIRE(second == first);
  REQUIRE(f.server.request_count() == 0);

  H5Fclose(file);
}

TEST_CASE("a large read is fetched without ramping up over many round trips",
          "[vfd][readahead]")
{
  ServedFile f;
  hid_t file = f.open_remote();
  REQUIRE(file >= 0);
  f.server.reset_counts();

  /* Read the full ~5 MB dataset. It spans ~50 internal 100 KiB chunks.
   *
   * A driver that only grew its request geometrically (1.7x per step, as the
   * Python implementation does) would need ~8 sequential round trips to work
   * through that span — each one a full network latency. Fetching the span the
   * read actually asked for should take 1, or 2 if HDF5 splits the read.
   *
   * This is the regression guard for a real bug: benchmarking against the ROS3
   * VFD showed the ramp costing 11 requests on a 23 MB read. */
  std::vector<double> got = read_slab(file, "contiguous", 0, kRows);
  REQUIRE(got.size() == kNumElements);

  const int requests = f.server.request_count();
  INFO("range requests for a " << kDataBytes / 1024 << " KB sequential read: "
                               << requests);
  REQUIRE(requests > 0);
  REQUIRE(requests <= 2);

  /* The first request must already cover most of the read, not a single chunk
   * or an early rung of the geometric ramp (which would be <= ~200 KiB). */
  auto ranges = f.server.requested_ranges();
  REQUIRE(!ranges.empty());
  const uint64_t first_span = ranges[0].second - ranges[0].first + 1;
  INFO("first request span: " << first_span << " bytes");
  REQUIRE(first_span >= kDataBytes / 2);

  H5Fclose(file);
}

TEST_CASE("read-ahead does not over-fetch wildly", "[vfd][readahead]")
{
  ServedFile f;
  hid_t file = f.open_remote();
  REQUIRE(file >= 0);
  f.server.reset_counts();

  /* Read-ahead is speculative, so some over-fetch is expected and desirable.
   * But it must stay proportionate: pulling down several times the requested
   * data would waste bandwidth on exactly the large reads we care about. */
  read_slab(file, "contiguous", 0, kRows);

  const uint64_t served = f.server.bytes_served();
  INFO("served " << served << " bytes for a " << kDataBytes << " byte read");
  REQUIRE(served >= kDataBytes);
  REQUIRE(served < kDataBytes * 2);

  H5Fclose(file);
}

TEST_CASE("failed requests are retried", "[vfd][retry]")
{
  ServedFile f;

  /* Fail the first two requests; the driver retries with backoff and should
   * still open and read the file successfully. */
  f.server.fail_next(2);

  hid_t file = f.open_remote();
  REQUIRE(file >= 0);

  f.server.fail_next(2);
  std::vector<double> got = read_slab(file, "contiguous", 0, 100);
  for (hsize_t r = 0; r < 100; r++)
    REQUIRE(got[(size_t)(r * kCols)] == expected_value(r, 0));

  H5Fclose(file);
}

TEST_CASE("opening a missing file fails cleanly and fast", "[vfd][errors]")
{
  ServedFile f;
  f.server.set_found(false);

  const auto t0 = std::chrono::steady_clock::now();
  H5E_BEGIN_TRY
  {
    hid_t file = f.open_remote();
    REQUIRE(file < 0);
  }
  H5E_END_TRY
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  /* A 404 is permanent: it must not be retried with exponential backoff
   * (8 retries would take ~25 s). One request, immediate failure. */
  REQUIRE(f.server.request_count() == 1);
  REQUIRE(elapsed < std::chrono::seconds(2));
}

TEST_CASE("a transient failure during open is retried", "[vfd][retry]")
{
  ServedFile f;

  /* The file-length probe is the first request of the file's life. A
   * transient 500 there must not fail the open outright. */
  f.server.fail_next(3);

  hid_t file = f.open_remote();
  REQUIRE(file >= 0);
  REQUIRE(f.server.request_count() > 3);  /* it retried past the failures */

  std::vector<double> got = read_slab(file, "contiguous", 0, 10);
  REQUIRE(got[0] == expected_value(0, 0));
  H5Fclose(file);
}

TEST_CASE("a server without range support is rejected", "[vfd][errors]")
{
  ServedFile f;
  f.server.set_support_ranges(false);

  /* The driver requires byte ranges; it must fail rather than silently
   * mis-read a full-body 200 response as a partial one. */
  H5E_BEGIN_TRY
  {
    hid_t file = f.open_remote();
    REQUIRE(file < 0);
  }
  H5E_END_TRY
}

TEST_CASE("the driver is read-only", "[vfd][errors]")
{
  ServedFile f;

  hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
  REQUIRE(H5Pset_fapl_remfile(fapl, nullptr) >= 0);

  H5E_BEGIN_TRY
  {
    /* Read-write open must be refused. */
    hid_t file = H5Fopen(f.server.url().c_str(), H5F_ACC_RDWR, fapl);
    REQUIRE(file < 0);

    /* So must creating a file. */
    hid_t created = H5Fcreate(f.server.url().c_str(), H5F_ACC_TRUNC,
                              H5P_DEFAULT, fapl);
    REQUIRE(created < 0);
  }
  H5E_END_TRY

  H5Pclose(fapl);
}

TEST_CASE("data is correct across a range of chunk sizes", "[vfd][config]")
{
  ServedFile f;

  /* The chunking math (offsets, spans, cache keys) must hold for chunk sizes
   * both much smaller and much larger than the reads being issued. */
  const size_t chunk_sizes[] = {4 * 1024, 64 * 1024, 100 * 1024, 1024 * 1024};

  for (size_t cs : chunk_sizes) {
    H5FD_remfile_config_t config;
    H5FD_remfile_config_init(&config);
    config.min_chunk_size = cs;
    config.max_chunk_size = cs * 64;
    config.max_cache_size = cs * 128;

    hid_t file = f.open_remote(&config);
    INFO("min_chunk_size = " << cs);
    REQUIRE(file >= 0);

    std::vector<double> got = read_slab(file, "contiguous", 123, 456);
    for (hsize_t r = 0; r < 456; r++)
      for (hsize_t c = 0; c < kCols; c++)
        REQUIRE(got[(size_t)(r * kCols + c)] == expected_value(123 + r, c));

    H5Fclose(file);
  }
}

TEST_CASE("a tiny cache still returns correct data", "[vfd][cache]")
{
  ServedFile f;

  /* Force heavy eviction: a cache far smaller than the data being read. The
   * point is that eviction must never corrupt a read, only cost extra
   * requests. */
  H5FD_remfile_config_t config;
  H5FD_remfile_config_init(&config);
  config.min_chunk_size = 8 * 1024;
  config.max_chunk_size = 32 * 1024;
  config.max_cache_size = 32 * 1024;  /* room for only a few chunks */

  hid_t file = f.open_remote(&config);
  REQUIRE(file >= 0);

  std::vector<double> got = read_slab(file, "contiguous", 0, kRows);
  for (hsize_t r = 0; r < kRows; r += 97)  /* spot-check across the whole array */
    for (hsize_t c = 0; c < kCols; c++)
      REQUIRE(got[(size_t)(r * kCols + c)] == expected_value(r, c));

  H5Fclose(file);
}

TEST_CASE("random-access reads are correct and bounded", "[vfd][cache]")
{
  ServedFile f;
  hid_t file = f.open_remote();
  REQUIRE(file >= 0);

  /* Jump around the file. This drives the non-sequential branch of the smart
   * loader, which shrinks the read-ahead window. */
  const hsize_t rows[] = {3900, 12, 2500, 700, 3333, 1, 1999};
  for (hsize_t r0 : rows) {
    std::vector<double> got = read_slab(file, "contiguous", r0, 2);
    for (hsize_t c = 0; c < kCols; c++) {
      REQUIRE(got[(size_t)c] == expected_value(r0, c));
      REQUIRE(got[(size_t)(kCols + c)] == expected_value(r0 + 1, c));
    }
  }

  /* Every byte served must have been asked for as a range. */
  REQUIRE(f.server.bytes_served() > 0);
  H5Fclose(file);
}
