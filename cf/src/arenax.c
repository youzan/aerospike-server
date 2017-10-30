/*
 * arenax.c
 *
 * Copyright (C) 2012-2014 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

//==========================================================
// Includes.
//

#include "arenax.h"
 
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "fault.h"


//==========================================================
// Typedefs & constants.
//

// Limit so stage_size fits in 32 bits.
// (Probably unnecessary - size_t is 64 bits on our systems.)
const uint64_t MAX_STAGE_SIZE = 0xFFFFffff;

// Must be in-sync with cf_arenax_err:
const char* ARENAX_ERR_STRINGS[] = {
	"ok",
	"bad parameter",
	"error creating stage",
	"error attaching stage",
	"error detaching stage",
	"unknown error"
};


//==========================================================
// Public API.
//

// Return persistent memory size needed. Excludes stages, which cf_arenax
// handles internally.
size_t
cf_arenax_sizeof()
{
	return sizeof(cf_arenax);
}

// Convert cf_arenax_err to meaningful string.
const char*
cf_arenax_errstr(cf_arenax_err err)
{
	if (err < 0 || err > CF_ARENAX_ERR_UNKNOWN) {
		err = CF_ARENAX_ERR_UNKNOWN;
	}

	return ARENAX_ERR_STRINGS[err];
}

// Create a cf_arenax object in persistent memory. Also create and attach the
// first arena stage in persistent memory.
void
cf_arenax_init(cf_arenax* arena, key_t key_base, uint32_t element_size,
		uint32_t stage_capacity, uint32_t max_stages, uint32_t flags)
{
	if (stage_capacity == 0) {
		stage_capacity = MAX_STAGE_CAPACITY;
	}
	else if (stage_capacity > MAX_STAGE_CAPACITY) {
		cf_crash(CF_ARENAX, "stage capacity %u too large", stage_capacity);
	}

	if (max_stages == 0) {
		max_stages = CF_ARENAX_MAX_STAGES;
	}
	else if (max_stages > CF_ARENAX_MAX_STAGES) {
		cf_crash(CF_ARENAX, "max stages %u too large", max_stages);
	}

	uint64_t stage_size = (uint64_t)stage_capacity * (uint64_t)element_size;

	if (stage_size > MAX_STAGE_SIZE) {
		cf_crash(CF_ARENAX, "stage size %lu too large", stage_size);
	}

	arena->key_base = key_base;
	arena->element_size = element_size;
	arena->stage_capacity = stage_capacity;
	arena->max_stages = max_stages;
	arena->flags = flags;

	arena->stage_size = (size_t)stage_size;

	arena->free_h = 0;

	// Skip 0:0 so null handle is never used.
	arena->at_stage_id = 0;
	arena->at_element_id = 1;

	if ((flags & CF_ARENAX_BIGLOCK) != 0) {
		pthread_mutex_init(&arena->lock, NULL);
	}

	arena->stage_count = 0;
	memset(arena->stages, 0, sizeof(arena->stages));

	// Add first stage.
	if (cf_arenax_add_stage(arena) != CF_ARENAX_OK) {
		cf_crash(CF_ARENAX, "failed to add first stage");
	}

	// Clear the null element - allocation bypasses it, but it may be read.
	memset(cf_arenax_resolve(arena, 0), 0, element_size);
}

// Allocate an element within the arena.
cf_arenax_handle
cf_arenax_alloc(cf_arenax* arena)
{
	if ((arena->flags & CF_ARENAX_BIGLOCK) != 0) {
		pthread_mutex_lock(&arena->lock);
	}

	cf_arenax_handle h;

	// Check free list first.
	if (arena->free_h != 0) {
		h = arena->free_h;

		free_element* p_free_element = cf_arenax_resolve(arena, h);

		arena->free_h = p_free_element->next_h;
	}
	// Otherwise keep end-allocating.
	else {
		if (arena->at_element_id >= arena->stage_capacity) {
			if (cf_arenax_add_stage(arena) != CF_ARENAX_OK) {
				if ((arena->flags & CF_ARENAX_BIGLOCK) != 0) {
					pthread_mutex_unlock(&arena->lock);
				}

				return 0;
			}

			arena->at_stage_id++;
			arena->at_element_id = 0;
		}

		cf_arenax_set_handle(&h, arena->at_stage_id, arena->at_element_id);

		arena->at_element_id++;
	}

	if ((arena->flags & CF_ARENAX_BIGLOCK) != 0) {
		pthread_mutex_unlock(&arena->lock);
	}

	if ((arena->flags & CF_ARENAX_CALLOC) != 0) {
		memset(cf_arenax_resolve(arena, h), 0, arena->element_size);
	}

	return h;
}

// Free an element.
void
cf_arenax_free(cf_arenax* arena, cf_arenax_handle h)
{
	free_element* p_free_element = cf_arenax_resolve(arena, h);

	if ((arena->flags & CF_ARENAX_BIGLOCK) != 0) {
		pthread_mutex_lock(&arena->lock);
	}

	p_free_element->magic = FREE_MAGIC;
	p_free_element->next_h = arena->free_h;
	arena->free_h = h;

	if ((arena->flags & CF_ARENAX_BIGLOCK) != 0) {
		pthread_mutex_unlock(&arena->lock);
	}
}

// Convert cf_arenax_handle to memory address.
void*
cf_arenax_resolve(cf_arenax* arena, cf_arenax_handle h)
{
	return arena->stages[h >> ELEMENT_ID_NUM_BITS] +
			((h & ELEMENT_ID_MASK) * arena->element_size);
}
