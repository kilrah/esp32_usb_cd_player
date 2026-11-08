/*
 * Metadata cache export / import as a tar archive (see meta_io.c)
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    int imported;   /* files written (new or replaced) */
    int unchanged;  /* identical to the stored file, not rewritten */
    int skipped;    /* regular files that aren't cache files, or with invalid content */
    int failed;     /* write errors (storage full) */
} meta_import_result_t;

/** Send the whole cache as a tar download (chunked response) */
esp_err_t meta_export_tar(httpd_req_t *req);

/**
 * Read a tar archive from the request body into the cache: adds new files, replaces existing ones.
 * Doesn't send a response. Returns an error if the body isn't a valid tar archive or was cut off;
 * files before that point are kept.
 */
esp_err_t meta_import_tar(httpd_req_t *req, meta_import_result_t *res);
