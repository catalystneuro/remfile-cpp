/*
 * remfile HDF5 Virtual File Driver (VFD)
 *
 * A read-only HDF5 file driver that reads a remote file over HTTP(S) using
 * range requests, with the same adaptive chunk caching ("smart loader")
 * strategy as the Python remfile package.
 *
 * Usage:
 *     hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
 *     H5Pset_fapl_remfile(fapl, NULL);  // NULL -> default config
 *     hid_t file = H5Fopen("https://example.com/file.h5", H5F_ACC_RDONLY, fapl);
 */

#ifndef REMFILE_VFD_H
#define REMFILE_VFD_H

#include <hdf5.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct H5FD_remfile_config_t {
    size_t min_chunk_size;  /* chunks are fetched in multiples of this size (default 100 KiB) */
    size_t max_cache_size;  /* max bytes held in the in-memory chunk cache (default 1 GB) */
    size_t max_chunk_size;  /* upper bound on a single range request (default 100 MiB) */
    int    verbose;         /* print fetch info to stderr for debugging */
} H5FD_remfile_config_t;

/* Fill a config struct with the default values (same defaults as Python remfile). */
void H5FD_remfile_config_init(H5FD_remfile_config_t *config);

/* Register the driver with the HDF5 library (idempotent) and return its hid_t. */
hid_t H5FD_remfile_init(void);

/* Set the remfile driver on a file access property list.
 * config may be NULL, in which case defaults are used. */
herr_t H5Pset_fapl_remfile(hid_t fapl_id, const H5FD_remfile_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* REMFILE_VFD_H */
