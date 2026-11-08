/*
 * MusicBrainz disc ID and TOC string, computed from the CD's TOC
 * (https://musicbrainz.org/doc/Disc_ID_Calculation)
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stddef.h>
#include "usb/atapi_host.h"

#define DISCID_LEN      28              /* without terminator */
#define DISCID_TOC_MAX  (4 * 4 + 100 * 7) /* "first last leadout off1 ... offN", space separated */

/**
 * Compute the disc ID (28 chars + NUL into @p id) and the TOC string used for fuzzy lookups
 * (into @p toc_str, "+"-separated for URLs). A trailing data track (Enhanced CD) is excluded,
 * with the lead-out moved to its start - 11400, as libdiscid does.
 * @return number of audio tracks in the ID, 0 on error
 */
int discid_compute(const atapi_toc_t *toc, char id[DISCID_LEN + 1], char *toc_str, size_t toc_str_size);
