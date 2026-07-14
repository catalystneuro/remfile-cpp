/*
 * Test program for the remfile VFD — the C equivalent of examples/example1.py.
 *
 * Usage:
 *   ./test_remfile [url [dataset_path]]
 *
 * Defaults to the DANDI file and dataset used in example1.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <hdf5.h>
#include <remfile/remfile_vfd.h>

static const char *default_url =
    "https://dandiarchive.s3.amazonaws.com/blobs/d86/055/d8605573-4639-4b99-a6d9-e0ac13f9a7df";
static const char *default_dataset =
    "/processing/behavior/Whisker_label 1/SpatialSeries/data";

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static herr_t print_link_name(hid_t group, const char *name,
                              const H5L_info2_t *info, void *op_data)
{
    (void)group;
    (void)info;
    (void)op_data;
    printf("  /%s\n", name);
    return 0;
}

int main(int argc, char **argv)
{
    const char *url = argc > 1 ? argv[1] : default_url;
    const char *dataset_path = argc > 2 ? argv[2] : default_dataset;
    int verbose = getenv("REMFILE_VERBOSE") != NULL;

    double t0 = now_seconds();

    H5FD_remfile_config_t config;
    H5FD_remfile_config_init(&config);
    config.verbose = verbose;

    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (H5Pset_fapl_remfile(fapl, &config) < 0) {
        fprintf(stderr, "Failed to set remfile driver on fapl\n");
        return 1;
    }

    hid_t file = H5Fopen(url, H5F_ACC_RDONLY, fapl);
    H5Pclose(fapl);
    if (file < 0) {
        fprintf(stderr, "Failed to open %s\n", url);
        return 1;
    }

    printf("Root group contents:\n");
    H5Literate2(file, H5_INDEX_NAME, H5_ITER_NATIVE, NULL, print_link_name, NULL);

    hid_t dset = H5Dopen2(file, dataset_path, H5P_DEFAULT);
    if (dset < 0) {
        fprintf(stderr, "Failed to open dataset %s\n", dataset_path);
        H5Fclose(file);
        return 1;
    }

    hid_t space = H5Dget_space(dset);
    int ndims = H5Sget_simple_extent_ndims(space);
    hsize_t dims[H5S_MAX_RANK];
    H5Sget_simple_extent_dims(space, dims, NULL);
    printf("Dataset %s shape: (", dataset_path);
    hsize_t num_elements = 1;
    for (int i = 0; i < ndims; i++) {
        printf(i ? ", %llu" : "%llu", (unsigned long long)dims[i]);
        num_elements *= dims[i];
    }
    printf(")\n");

    hid_t dtype = H5Dget_type(dset);
    size_t elem_size = H5Tget_size(dtype);

    printf("Reading data (%.1f MB)...\n",
           (double)(num_elements * elem_size) / 1e6);
    void *data = malloc(num_elements * elem_size);
    if (!data) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    if (H5Dread(dset, dtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data) < 0) {
        fprintf(stderr, "Failed to read dataset\n");
        return 1;
    }
    printf("Done reading data.\n");

    /* Print first few values if the dataset is a float type, as a sanity check */
    if (H5Tget_class(dtype) == H5T_FLOAT && num_elements >= 3) {
        double sample[3];
        hsize_t count = 3;
        hid_t memspace = H5Screate_simple(1, &count, NULL);
        hsize_t start[H5S_MAX_RANK] = {0};
        hsize_t cnt[H5S_MAX_RANK];
        for (int i = 0; i < ndims; i++)
            cnt[i] = 1;
        cnt[0] = 3 <= dims[0] ? 3 : dims[0];
        if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, cnt, NULL) >= 0 &&
            H5Dread(dset, H5T_NATIVE_DOUBLE, memspace, space, H5P_DEFAULT, sample) >= 0)
            printf("First values: %g %g %g\n", sample[0], sample[1], sample[2]);
        H5Sclose(memspace);
    }

    free(data);
    H5Tclose(dtype);
    H5Sclose(space);
    H5Dclose(dset);
    H5Fclose(file);

    printf("Total time: %.2f seconds\n", now_seconds() - t0);
    return 0;
}
