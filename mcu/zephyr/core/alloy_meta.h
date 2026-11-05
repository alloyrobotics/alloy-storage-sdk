/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 *
 * The run's semantics sidecar (POST /v1/meta): device, firmware, mission label, session start and
 * the describe() entries. Platform independent.
 */

#ifndef ALLOY_CORE_META_H_
#define ALLOY_CORE_META_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct alloy_desc {
	char chan[24];
	char field[20];
	char unit[12];
	char about[48];
	float lo; /* NAN = not given */
	float hi;
};

/* Fills d from the arguments, truncating each string to its field. */
void alloy_desc_set(struct alloy_desc *d, const char *chan, const char *field, const char *unit,
		    float lo, float hi, const char *about);

/* Writes the JSON object into out. Returns its length, or -ENOSPC if it does not fit.
 * firmware and mission may be NULL or empty (then omitted).
 */
int alloy_meta_json(char *out, size_t cap, const char *device, const char *firmware,
		    const char *mission, uint32_t session, const struct alloy_desc *descs,
		    size_t n_descs);

#ifdef __cplusplus
}
#endif

#endif /* ALLOY_CORE_META_H_ */
