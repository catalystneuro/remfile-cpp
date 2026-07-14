# remfile-cpp — an HDF5 Virtual File Driver

A C++ port of the Python [remfile](https://github.com/magland/remfile)
package, implemented as a read-only HDF5 [Virtual File Driver
(VFD)](https://support.hdfgroup.org/documentation/hdf5/latest/_v_f_l.html).
It lets any HDF5-based C/C++ application open a remote HDF5 file directly
from an HTTP(S) URL:

```c
#include <hdf5.h>
#include <remfile/remfile_vfd.h>

hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
H5Pset_fapl_remfile(fapl, NULL); /* NULL -> default configuration */
hid_t file = H5Fopen("https://example.com/file.nwb", H5F_ACC_RDONLY, fapl);
/* ... use the ordinary HDF5 API ... */
```

Reads are served from an in-memory chunk cache; cache misses are filled with
HTTP range requests via libcurl. Sequential access is detected and the
request size grows geometrically (remfile's "smart loader"), which greatly
reduces the number of round trips for streaming reads.

Compared to the ROS3 VFD that ships with HDF5:

- works with **any** HDF5 build (1.10 – 2.x); does not require HDF5 to be
  compiled with ROS3 support
- works with any HTTP(S) server that supports byte-range requests, not just
  S3 (including presigned URLs, which reject HEAD requests)
- adaptive read-ahead: sequential reads are coalesced into geometrically
  growing range requests (100 KiB up to 100 MiB), typically several times
  faster than ROS3 for large sequential reads

## Requirements

- CMake >= 3.15, a C++17 compiler
- libhdf5 (C library, version 1.10 or later)
- libcurl

## Building and installing

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /path/to/install
```

## Using from CMake

```cmake
find_package(remfile REQUIRED)
target_link_libraries(my_app PRIVATE remfile::remfile)
```

Or via FetchContent (no install step needed):

```cmake
include(FetchContent)
FetchContent_Declare(remfile
    GIT_REPOSITORY https://github.com/catalystneuro/remfile-cpp.git
    GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(remfile)
target_link_libraries(my_app PRIVATE remfile::remfile)
```

## Configuration

`H5Pset_fapl_remfile` accepts an optional `H5FD_remfile_config_t`:

```c
H5FD_remfile_config_t config;
H5FD_remfile_config_init(&config);  /* fill with defaults */
config.min_chunk_size = 100 * 1024;        /* fetch granularity */
config.max_cache_size = 1000000000;        /* in-memory cache bound */
config.max_chunk_size = 100 * 1024 * 1024; /* largest single request */
config.verbose = 1;                        /* log requests to stderr */
H5Pset_fapl_remfile(fapl, &config);
```

The defaults match the Python remfile package.

## Test program

```bash
cd build
./test_remfile                # reads a small DANDI NWB file over HTTPS
./test_remfile <url> <dataset_path>
```

## Dynamic VFD plugin (HDF5 >= 1.14)

Build with `-DREMFILE_BUILD_PLUGIN=ON` to also produce a dynamically
loadable VFD plugin. Applications can then use the driver without linking
against this library:

```bash
export HDF5_PLUGIN_PATH=/path/containing/libremfile_vfd_plugin
```

```c
H5Pset_driver_by_name(fapl, "remfile", NULL);
```
