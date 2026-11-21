#pragma once
#include <stdint.h>
#include <stddef.h>

/* Baseline JPEG to planes, for the frames of a MIAV (.mjp) movie (#7):
 * tables from `hdr` (SOI.. up to SOS, already byte-swapped back), the scan
 * from `scan` (the SOS segment and its entropy data, not byte-stuffed).
 * 4:2:0: y is w*h, cb and cr (w/2)*(h/2). 0 on success. */
int mjp_decode(const uint8_t *hdr, size_t hdr_len, const uint8_t *scan, size_t scan_len,
               uint8_t *y, uint8_t *cb, uint8_t *cr, int *w, int *h, int max_w, int max_h);
