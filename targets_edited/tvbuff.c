/* tvbuff.c
 *
 * Testy, Virtual(-izable) Buffer of guint8*'s
 *
 * "Testy" -- the buffer gets mad when an attempt to access data
 * 		beyond the bounds of the buffer. An exception is thrown.
 *
 * "Virtual" -- the buffer can have its own data, can use a subset of
 * 		the data of a backing tvbuff, or can be a composite of
 * 		other tvbuffs.
 *
 * $Id: tvbuff.c 28140 2009-04-24 08:08:37Z stig $
 *
 * Copyright (c) 2000 by Gilbert Ramirez <gram@alumni.rice.edu>
 *
 * Code to convert IEEE floating point formats to native floating point
 * derived from code Copyright (c) Ashok Narayanan, 2000
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "pint.h"
#include "tvbuff.h"
#include "strutil.h"
#include "emem.h"
#include "proto.h"	/* XXX - only used for DISSECTOR_ASSERT, probably a new header file? */

static const guint8*
ensure_contiguous_no_exception(tvbuff_t *tvb, gint offset, gint length,
		int *exception);

static const guint8*
ensure_contiguous(tvbuff_t *tvb, gint offset, gint length);

/* We dole out tvbuff's from this memchunk. */
GMemChunk *tvbuff_mem_chunk = NULL;

void
tvbuff_init(void)
{
	if (!tvbuff_mem_chunk)
		tvbuff_mem_chunk = g_mem_chunk_create(tvbuff_t, 20, G_ALLOC_AND_FREE);
}

void
tvbuff_cleanup(void)
{
	if (tvbuff_mem_chunk)
		g_mem_chunk_destroy(tvbuff_mem_chunk);

	tvbuff_mem_chunk = NULL;
}




static void
tvb_init(tvbuff_t *tvb, tvbuff_type type)
{
	tvb_backing_t	*backing;
	tvb_comp_t	*composite;

	tvb->type		= type;
	tvb->initialized	= FALSE;
	tvb->usage_count	= 1;
	tvb->length		= 0;
	tvb->reported_length	= 0;
	tvb->free_cb		= NULL;
	tvb->real_data		= NULL;
	tvb->raw_offset		= -1;
	tvb->used_in		= NULL;
	tvb->ds_tvb		= NULL;

	switch(type) {
		case TVBUFF_REAL_DATA:
			/* Nothing */
			break;

		case TVBUFF_SUBSET:
			backing = &tvb->tvbuffs.subset;
			backing->tvb	= NULL;
			backing->offset	= 0;
			backing->length	= 0;
			break;

		case TVBUFF_COMPOSITE:
			composite = &tvb->tvbuffs.composite;
			composite->tvbs			= NULL;
			composite->start_offsets	= NULL;
			composite->end_offsets		= NULL;
			break;
	}
}


tvbuff_t*
tvb_new(tvbuff_type type)
{
	tvbuff_t	*tvb;

	tvb = g_chunk_new(tvbuff_t, tvbuff_mem_chunk);

	tvb_init(tvb, type);

	return tvb;
}

void
tvb_free(tvbuff_t* tvb)
{
	tvbuff_t	*member_tvb;
	tvb_comp_t	*composite;
	GSList		*slist;

	tvb->usage_count--;

	if (tvb->usage_count == 0) {
		switch (tvb->type) {
		case TVBUFF_REAL_DATA:
			if (tvb->free_cb) {
				/*
				 * XXX - do this with a union?
				 */
				tvb->free_cb((gpointer)tvb->real_data);
			}
			break;

		case TVBUFF_SUBSET:
			/* This will be NULL if tvb_new_subset() fails because
			 * reported_length < -1 */
			if (tvb->tvbuffs.subset.tvb) {
				tvb_decrement_usage_count(tvb->tvbuffs.subset.tvb, 1);
			}
			break;

		case TVBUFF_COMPOSITE:
			composite = &tvb->tvbuffs.composite;
			for (slist = composite->tvbs; slist != NULL ; slist = slist->next) {
				member_tvb = slist->data;
				tvb_decrement_usage_count(member_tvb, 1);
			}

			g_slist_free(composite->tvbs);

			g_free(composite->start_offsets);
			g_free(composite->end_offsets);
			if (tvb->real_data) {
				/*
				 * XXX - do this with a union?
				 */
				g_free((gpointer)tvb->real_data);
			}

			break;
		}

		if (tvb->used_in) {
			g_slist_free(tvb->used_in);
		}

		g_chunk_free(tvb, tvbuff_mem_chunk);
	}
}

guint
tvb_increment_usage_count(tvbuff_t* tvb, guint count)
{
	tvb->usage_count += count;

	return tvb->usage_count;
}

guint
tvb_decrement_usage_count(tvbuff_t* tvb, guint count)
{
	if (tvb->usage_count <= count) {
		tvb->usage_count = 1;
		tvb_free(tvb);
		return 0;
	}
	else {
		tvb->usage_count -= count;
		return tvb->usage_count;
	}

}

void
tvb_free_chain(tvbuff_t* tvb)
{
	GSList		*slist;

	/* Recursively call tvb_free_chain() */
	for (slist = tvb->used_in; slist != NULL ; slist = slist->next) {		// BUG_ACBA7CD4(2) FIX_ACBA7CD4(1) #CWE-416 #Pointer "tvb" may have been freed on one specific branch, leading to an invalid pointer dereference
		tvb_free_chain( (tvbuff_t*)slist->data );
	}

	/* Stop the recursion */
	tvb_free(tvb);
}



void
tvb_set_free_cb(tvbuff_t* tvb, tvbuff_free_cb_t func)
{
	DISSECTOR_ASSERT(tvb);
	DISSECTOR_ASSERT(tvb->type == TVBUFF_REAL_DATA);
	tvb->free_cb = func;
}

static void
add_to_used_in_list(tvbuff_t *tvb, tvbuff_t *used_in)
{
	tvb->used_in = g_slist_prepend(tvb->used_in, used_in);
	tvb_increment_usage_count(tvb, 1);
}

void
tvb_set_child_real_data_tvbuff(tvbuff_t* parent, tvbuff_t* child)
{
	DISSECTOR_ASSERT(parent && child);
	DISSECTOR_ASSERT(parent->initialized);
	DISSECTOR_ASSERT(child->initialized);
	DISSECTOR_ASSERT(child->type == TVBUFF_REAL_DATA);
	add_to_used_in_list(parent, child);
}

void
tvb_set_real_data(tvbuff_t* tvb, const guint8* data, guint length, gint reported_length)
{
	DISSECTOR_ASSERT(tvb);
	DISSECTOR_ASSERT(tvb->type == TVBUFF_REAL_DATA);
	DISSECTOR_ASSERT(!tvb->initialized);

	if (reported_length < -1) {						// BUG_245F0BFF(5) #If "reported_length" overflowed, take the true branch
		THROW(ReportedBoundsError);					// BUG_245F0BFF(6) #CWE-754 #Throw an exception that's handled only two levels up
	}

	tvb->real_data		= data;
	tvb->length		= length;
	tvb->reported_length	= reported_length;
	tvb->initialized	= TRUE;
}

tvbuff_t*
tvb_new_real_data(const guint8* data, guint length, gint reported_length)	// BUG_245F0BFF(2) FIX_245F0BFF(3) #CWE-196 #"reported_length" is a signed integer that can overflow
{
	static tvbuff_t	*last_tvb=NULL;
	tvbuff_t	*tvb;

	tvb = tvb_new(TVBUFF_REAL_DATA);					// BUG_245F0BFF(3) FIX_245F0BFF(4) #Allocate memory for pointer "tvb"

	if(last_tvb){
		tvb_free(last_tvb);
	}
	/* remember this tvb in case we throw an exception and
	 * lose the pointer to it.
	 */
	last_tvb=tvb;

	tvb_set_real_data(tvb, data, length, reported_length);			// BUG_245F0BFF(4) FIX_245F0BFF(5) #Call function "tvb_set_real_data", that can throw an exception and bypass the remaining code in this function, so the allocated memory allocated to "tvb" is never returned

	/*
	 * This is the top-level real tvbuff for this data source,
	 * so its data source tvbuff is itself.
	 */
	tvb->ds_tvb = tvb;

	/* ok no exception so we dont need to remember it any longer */
	last_tvb=NULL;

	return tvb;								// BUG_245F0BFF(7) #Memory allocated to "tvb" is never returned because the exception bypassed this code
}

tvbuff_t*
tvb_new_child_real_data(tvbuff_t *parent, const guint8* data, guint length, gint reported_length)
{
	tvbuff_t *tvb = tvb_new_real_data(data, length, reported_length);
	if (tvb) {
		tvb_set_child_real_data_tvbuff (parent, tvb);
	}
	
	return tvb;
}

/* Computes the absolute offset and length based on a possibly-negative offset
 * and a length that is possible -1 (which means "to the end of the data").
 * Returns TRUE/FALSE indicating whether the offset is in bounds or
 * not. The integer ptrs are modified with the new offset and length.
 * No exception is thrown.
 *
 * XXX - we return TRUE, not FALSE, if the offset is positive and right
 * after the end of the tvbuff (i.e., equal to the length).  We do this
 * so that a dissector constructing a subset tvbuff for the next protocol
 * will get a zero-length tvbuff, not an exception, if there's no data
 * left for the next protocol - we want the next protocol to be the one
 * that gets an exception, so the error is reported as an error in that
 * protocol rather than the containing protocol.  */
static gboolean
compute_offset_length(tvbuff_t *tvb, gint offset, gint length,
		guint *offset_ptr, guint *length_ptr, int *exception)
{
	DISSECTOR_ASSERT(offset_ptr);
	DISSECTOR_ASSERT(length_ptr);

	/* Compute the offset */
	if (offset >= 0) {
		/* Positive offset - relative to the beginning of the packet. */
		if ((guint) offset > tvb->reported_length) {
			if (exception) {
				*exception = ReportedBoundsError;
			}
			return FALSE;
		}
		else if ((guint) offset > tvb->length) {
			if (exception) {
				*exception = BoundsError;
			}
			return FALSE;
		}
		else {
			*offset_ptr = offset;
		}
	}
	else {
		/* Negative offset - relative to the end of the packet. */
		if ((guint) -offset > tvb->reported_length) {
			if (exception) {
				*exception = ReportedBoundsError;
			}
			return FALSE;
		}
		else if ((guint) -offset > tvb->length) {
			if (exception) {
				*exception = BoundsError;
			}
			return FALSE;
		}
		else {
			*offset_ptr = tvb->length + offset;
		}
	}

	/* Compute the length */
	if (length < -1) {
		if (exception) {
			/* XXX - ReportedBoundsError? */
			*exception = BoundsError;
		}
		return FALSE;
	}
	else if (length == -1) {
		*length_ptr = tvb->length - *offset_ptr;
	}
	else {
		*length_ptr = length;
	}

	return TRUE;
}


static gboolean
check_offset_length_no_exception(tvbuff_t *tvb, gint offset, gint length,
		guint *offset_ptr, guint *length_ptr, int *exception)
{
	guint	end_offset;

	DISSECTOR_ASSERT(tvb && tvb->initialized);					// BUG_7169C840(5) FIX_7169C840(5) #CWE-822 #CWE-125 #Alernative location where a potentially invalid pointer is dereferenced, leading to a crash.

	if (!compute_offset_length(tvb, offset, length, offset_ptr, length_ptr, exception)) {
		return FALSE;
	}

	/*
	 * Compute the offset of the first byte past the length.
	 */
	end_offset = *offset_ptr + *length_ptr;

	/*
	 * Check for an overflow, and clamp "end_offset" at the maximum
	 * if we got an overflow - that should force us to indicate that
	 * we're past the end of the tvbuff.
	 */
	if (end_offset < *offset_ptr)
		end_offset = UINT_MAX;

	/*
	 * Check whether that offset goes more than one byte past the
	 * end of the buffer.
	 *
	 * If not, return TRUE; otherwise, return FALSE and, if "exception"
	 * is non-null, return the appropriate exception through it.
	 */
	if (end_offset <= tvb->length) {
		return TRUE;
	}
	else if (end_offset <= tvb->reported_length) {
		if (exception) {
			*exception = BoundsError;
		}
		return FALSE;
	}
	else {
		if (exception) {
			*exception = ReportedBoundsError;
		}
		return FALSE;
	}

	DISSECTOR_ASSERT_NOT_REACHED();
}

/* Checks (+/-) offset and length and throws an exception if
 * either is out of bounds. Sets integer ptrs to the new offset
 * and length. */
static void
check_offset_length(tvbuff_t *tvb, gint offset, gint length,
		guint *offset_ptr, guint *length_ptr)
{
	int exception = 0;

	if (!check_offset_length_no_exception(tvb, offset, length, offset_ptr, length_ptr, &exception)) {
		DISSECTOR_ASSERT(exception > 0);
		THROW(exception);
	}
	return;
}


void
tvb_set_subset(tvbuff_t *tvb, tvbuff_t *backing,
		gint backing_offset, gint backing_length, gint reported_length)
{
	DISSECTOR_ASSERT(tvb);
	DISSECTOR_ASSERT(tvb->type == TVBUFF_SUBSET);						// BUG_7169C840(3) FIX_7169C840(3) #CWE-822 #CWE-125 #2 #Dereferencing potentially invalid pointer "tvb" could cause an out-of-bound read.
	DISSECTOR_ASSERT(!tvb->initialized);

	if (reported_length < -1) {
		THROW(ReportedBoundsError);
	}

	check_offset_length(backing, backing_offset, backing_length,				// BUG_7169C840(4) FIX_7169C840(4) #3 #Passing potentially invalid pointer "tvb" to function "tvb_set_subset", dereferencing it again.
			&tvb->tvbuffs.subset.offset,
			&tvb->tvbuffs.subset.length);

	tvb->tvbuffs.subset.tvb		= backing;
	tvb->length			= tvb->tvbuffs.subset.length;

	if (reported_length == -1) {
		tvb->reported_length	= backing->reported_length - tvb->tvbuffs.subset.offset;
	}
	else {
		tvb->reported_length	= reported_length;
	}
	tvb->initialized		= TRUE;
	add_to_used_in_list(backing, tvb);

	/* Optimization. If the backing buffer has a pointer to contiguous, real data,
	 * then we can point directly to our starting offset in that buffer */
	if (backing->real_data != NULL) {
		tvb->real_data = backing->real_data + tvb->tvbuffs.subset.offset;
	}
}


tvbuff_t*
tvb_new_subset(tvbuff_t *backing, gint backing_offset, gint backing_length, gint reported_length)
{
	static tvbuff_t	*last_tvb=NULL;
	tvbuff_t	*tvb;

	tvb = tvb_new(TVBUFF_SUBSET);

	if(last_tvb){
		tvb_free(last_tvb);
	}
	/* remember this tvb in case we throw an exception and
	 * lose the pointer to it.
	 */
	last_tvb=tvb;

	tvb_set_subset(tvb, backing, backing_offset, backing_length, reported_length);		// BUG_7169C840(2) FIX_7169C840(2) #Passing potentially invalid pointer "tvb" to function "tvb_set_subset".

	/*
	 * The top-level data source of this tvbuff is the top-level
	 * data source of its parent.
	 */
	tvb->ds_tvb = backing->ds_tvb;

	/* ok no exception so we dont need to remember it any longer */
	last_tvb=NULL;

	return tvb;
}

void
tvb_composite_append(tvbuff_t* tvb, tvbuff_t* member)
{
	tvb_comp_t	*composite;

	DISSECTOR_ASSERT(tvb && !tvb->initialized);
	DISSECTOR_ASSERT(tvb->type == TVBUFF_COMPOSITE);
	composite = &tvb->tvbuffs.composite;
	composite->tvbs = g_slist_append( composite->tvbs, member );
	add_to_used_in_list(tvb, member);
}

void
tvb_composite_prepend(tvbuff_t* tvb, tvbuff_t* member)
{
	tvb_comp_t	*composite;

	DISSECTOR_ASSERT(tvb && !tvb->initialized);
	DISSECTOR_ASSERT(tvb->type == TVBUFF_COMPOSITE);
	composite = &tvb->tvbuffs.composite;
	composite->tvbs = g_slist_prepend( composite->tvbs, member );
	add_to_used_in_list(tvb, member);
}

tvbuff_t*
tvb_new_composite(void)
{
	return tvb_new(TVBUFF_COMPOSITE);
}

void
tvb_composite_finalize(tvbuff_t* tvb)
{
	GSList		*slist;
	guint		num_members;
	tvbuff_t	*member_tvb;
	tvb_comp_t	*composite;
	int		i = 0;

	DISSECTOR_ASSERT(tvb && !tvb->initialized);
	DISSECTOR_ASSERT(tvb->type == TVBUFF_COMPOSITE);
	DISSECTOR_ASSERT(tvb->length == 0);

	composite = &tvb->tvbuffs.composite;
	num_members = g_slist_length(composite->tvbs);

	composite->start_offsets = g_new(guint, num_members);
	composite->end_offsets = g_new(guint, num_members);

	for (slist = composite->tvbs; slist != NULL; slist = slist->next) {
		DISSECTOR_ASSERT((guint) i < num_members);
		member_tvb = slist->data;
		composite->start_offsets[i] = tvb->length;
		tvb->length += member_tvb->length;
		composite->end_offsets[i] = tvb->length - 1;
		i++;
	}

	tvb->initialized = TRUE;
}



guint
tvb_length(tvbuff_t* tvb)
{
	DISSECTOR_ASSERT(tvb && tvb->initialized);						// BUG_E02CFE60(3) FIX_E02CFE60(4) #CWE-824 #Dereferecing potentially uninitialized pointer "tvb"

	return tvb->length;									// BUG_E02CFE60(4) FIX_E02CFE60(5) #CWE-824 #Dereferecing potentially uninitialized pointer "tvb"
}

gint
tvb_length_remaining(tvbuff_t *tvb, gint offset)
{
	guint	abs_offset, abs_length;

	DISSECTOR_ASSERT(tvb && tvb->initialized);

	if (compute_offset_length(tvb, offset, -1, &abs_offset, &abs_length, NULL)) {
		return abs_length;
	}
	else {
		return -1;
	}
}

guint
tvb_ensure_length_remaining(tvbuff_t *tvb, gint offset)
{
	guint	abs_offset, abs_length;
	int	exception;

	DISSECTOR_ASSERT(tvb && tvb->initialized);

	if (!compute_offset_length(tvb, offset, -1, &abs_offset, &abs_length, &exception)) {
		THROW(exception);
	}
	if (abs_length == 0) {
		/*
		 * This routine ensures there's at least one byte available.
		 * There aren't any bytes available, so throw the appropriate
		 * exception.
		 */
		if (abs_offset >= tvb->reported_length)
			THROW(ReportedBoundsError);
		else
			THROW(BoundsError);
	}
	return abs_length;
}




/* Validates that 'length' bytes are available starting from
 * offset (pos/neg). Does not throw an exception. */
gboolean
tvb_bytes_exist(tvbuff_t *tvb, gint offset, gint length)
{
	guint		abs_offset, abs_length;

	DISSECTOR_ASSERT(tvb && tvb->initialized);

	if (!compute_offset_length(tvb, offset, length, &abs_offset, &abs_length, NULL))
		return FALSE;

	if (abs_offset + abs_length <= tvb->length) {
		return TRUE;
	}
	else {
		return FALSE;
	}
}

/* Validates that 'length' bytes are available starting from
 * offset (pos/neg). Throws an exception if they aren't. */
void
tvb_ensure_bytes_exist(tvbuff_t *tvb, gint offset, gint length)
{
	guint		abs_offset, abs_length;

	DISSECTOR_ASSERT(tvb && tvb->initialized);

	/*
	 * -1 doesn't mean "until end of buffer", as that's pointless
	 * for this routine.  We must treat it as a Really Large Positive
	 * Number, so that we throw an exception; we throw
	 * ReportedBoundsError, as if it were past even the end of a
	 * reassembled packet, and past the end of even the data we
	 * didn't capture.
	 *
	 * We do the same with other negative lengths.
	 */
	if (length < 0) {
		THROW(ReportedBoundsError);
	}
	check_offset_length(tvb, offset, length, &abs_offset, &abs_length);
}

gboolean
tvb_offset_exists(tvbuff_t *tvb, gint offset)
{
	guint		abs_offset, abs_length;

	DISSECTOR_ASSERT(tvb && tvb->initialized);
	if (!compute_offset_length(tvb, offset, -1, &abs_offset, &abs_length, NULL))
		return FALSE;

	if (abs_offset < tvb->length) {
		return TRUE;
	}
	else {
		return FALSE;
	}
}

guint
tvb_reported_length(tvbuff_t* tvb)
{
	DISSECTOR_ASSERT(tvb && tvb->initialized);

	return tvb->reported_length;
}

gint
tvb_reported_length_remaining(tvbuff_t *tvb, gint offset)
{
	guint	abs_offset, abs_length;

	DISSECTOR_ASSERT(tvb && tvb->initialized);

	if (compute_offset_length(tvb, offset, -1, &abs_offset, &abs_length, NULL)) {
		if (tvb->reported_length >= abs_offset)
			return tvb->reported_length - abs_offset;
		else
			return -1;
	}
	else {
		return -1;
	}
}

/* Set the reported length of a tvbuff to a given value; used for protocols
   whose headers contain an explicit length and where the calling
   dissector's payload may include padding as well as the packet for
   this protocol.

   Also adjusts the data length. */
void
tvb_set_reported_length(tvbuff_t* tvb, guint reported_length)
{
	DISSECTOR_ASSERT(tvb && tvb->initialized);

	if (reported_length > tvb->reported_length)
		THROW(ReportedBoundsError);

	tvb->reported_length = reported_length;
	if (reported_length < tvb->length)
		tvb->length = reported_length;
}


#if 0
static const guint8*
first_real_data_ptr(tvbuff_t *tvb)
{
	tvbuff_t	*member;

	switch(tvb->type) {
		case TVBUFF_REAL_DATA:
			return tvb->real_data;
		case TVBUFF_SUBSET:
			member = tvb->tvbuffs.subset.tvb;
			return first_real_data_ptr(member);
		case TVBUFF_COMPOSITE:
			member = tvb->tvbuffs.composite.tvbs->data;
			return first_real_data_ptr(member);
	}

	DISSECTOR_ASSERT_NOT_REACHED();
	return NULL;
}
#endif

int
offset_from_real_beginning(tvbuff_t *tvb, int counter)
{
	tvbuff_t	*member;

	switch(tvb->type) {
		case TVBUFF_REAL_DATA:
			return counter;
		case TVBUFF_SUBSET:
			member = tvb->tvbuffs.subset.tvb;
			return offset_from_real_beginning(member, counter + tvb->tvbuffs.subset.offset);
		case TVBUFF_COMPOSITE:
			member = tvb->tvbuffs.composite.tvbs->data;
			return offset_from_real_beginning(member, counter);
	}

	DISSECTOR_ASSERT_NOT_REACHED();
	return 0;
}

static const guint8*
composite_ensure_contiguous_no_exception(tvbuff_t *tvb, guint abs_offset,
		guint abs_length)
{
	guint		i, num_members;
	tvb_comp_t	*composite;
	tvbuff_t	*member_tvb = NULL;
	guint		member_offset, member_length;
	GSList		*slist;

	DISSECTOR_ASSERT(tvb->type == TVBUFF_COMPOSITE);

	/* Maybe the range specified by offset/length
	 * is contiguous inside one of the member tvbuffs */
	composite = &tvb->tvbuffs.composite;
	num_members = g_slist_length(composite->tvbs);

	for (i = 0; i < num_members; i++) {
		if (abs_offset <= composite->end_offsets[i]) {
			slist = g_slist_nth(composite->tvbs, i);
			member_tvb = slist->data;
			break;
		}
	}
	DISSECTOR_ASSERT(member_tvb);

	if (check_offset_length_no_exception(member_tvb, abs_offset - composite->start_offsets[i],
				abs_length, &member_offset, &member_length, NULL)) {

		/*
		 * The range is, in fact, contiguous within member_tvb.
		 */
		DISSECTOR_ASSERT(!tvb->real_data);
		return ensure_contiguous_no_exception(member_tvb, member_offset, member_length, NULL);
	}
	else {
		tvb->real_data = tvb_memdup(tvb, 0, -1);
		return tvb->real_data + abs_offset;
	}

	DISSECTOR_ASSERT_NOT_REACHED();
	return NULL;
}

static const guint8*
ensure_contiguous_no_exception(tvbuff_t *tvb, gint offset, gint length,
		int *exception)
{
	guint	abs_offset, abs_length;

	if (!check_offset_length_no_exception(tvb, offset, length,
	    &abs_offset, &abs_length, exception)) {
		return NULL;
	}

	/*
	 * We know that all the data is present in the tvbuff, so
	 * no exceptions should be thrown.
	 */
	if (tvb->real_data) {
		return tvb->real_data + abs_offset;
	}
	else {
		switch(tvb->type) {
			case TVBUFF_REAL_DATA:
				DISSECTOR_ASSERT_NOT_REACHED();
			case TVBUFF_SUBSET:
				return ensure_contiguous_no_exception(tvb->tvbuffs.subset.tvb,
						abs_offset - tvb->tvbuffs.subset.offset,
						abs_length, NULL);
			case TVBUFF_COMPOSITE:
				return composite_ensure_contiguous_no_exception(tvb, abs_offset, abs_length);
		}
	}

	DISSECTOR_ASSERT_NOT_REACHED();
	return NULL;
}

/* ----------------------------- */
static const guint8*
fast_ensure_contiguous(tvbuff_t *tvb, gint offset, guint length)
{
	guint	end_offset;
	guint	u_offset;

	DISSECTOR_ASSERT(tvb && tvb->initialized);
	if (offset < 0 || !tvb->real_data) {
	    return ensure_contiguous(tvb, offset, length);
	}

	u_offset = offset;
	end_offset = u_offset + length;

	/* don't need to check for overflow  because length <= 8 */

	if (end_offset <= tvb->length) {		// BUG_D5F4E690(13) FIX_D5F4E690(13) #The offset being too large, this condition doesn't hold
		return tvb->real_data + u_offset;
	}

	if (end_offset > tvb->reported_length) {	// BUG_D5F4E690(14) FIX_D5F4E690(14) #3 #Throw an exception because the offset is out of range of the structure
		THROW(ReportedBoundsError);
	}
	THROW(BoundsError);
	/* not reached */
	return 0;
}


static const guint8*
ensure_contiguous(tvbuff_t *tvb, gint offset, gint length)
{
	int exception;
	const guint8 *p;

	p = ensure_contiguous_no_exception(tvb, offset, length, &exception);
	if (p == NULL) {
		DISSECTOR_ASSERT(exception > 0);
		THROW(exception);
	}
	return p;
}

static const guint8*
guint8_find(const guint8* haystack, size_t haystacklen, guint8 needle)
{
	const guint8	*b;
	int		i;

	for (b = haystack, i = 0; (guint) i < haystacklen; i++, b++) {
		if (*b == needle) {
			return b;
		}
	}

	return NULL;
}

static const guint8*
guint8_pbrk(const guint8* haystack, size_t haystacklen, const guint8 *needles)
{
	const guint8	*b;
	int		i;
	guint8		item, needle;
	const guint8	*needlep;

	for (b = haystack, i = 0; (guint) i < haystacklen; i++, b++) {
		item = *b;
		needlep = needles;
		while ((needle = *needlep) != '\0') {
			if (item == needle)
				return b;
			needlep++;
		}
	}

	return NULL;
}



/************** ACCESSORS **************/

static void*
composite_memcpy(tvbuff_t *tvb, guint8* target, guint abs_offset, size_t abs_length)
{
	guint		i, num_members;
	tvb_comp_t	*composite;
	tvbuff_t	*member_tvb = NULL;
	guint		member_offset, member_length;
	gboolean	retval;
	GSList		*slist;

	DISSECTOR_ASSERT(tvb->type == TVBUFF_COMPOSITE);

	/* Maybe the range specified by offset/length
	 * is contiguous inside one of the member tvbuffs */
	composite = &tvb->tvbuffs.composite;
	num_members = g_slist_length(composite->tvbs);

	for (i = 0; i < num_members; i++) {
		if (abs_offset <= composite->end_offsets[i]) {
			slist = g_slist_nth(composite->tvbs, i);
			member_tvb = slist->data;
			break;
		}
	}
	DISSECTOR_ASSERT(member_tvb);

	if (check_offset_length_no_exception(member_tvb, abs_offset - composite->start_offsets[i],
				(gint) abs_length, &member_offset, &member_length, NULL)) {

		DISSECTOR_ASSERT(!tvb->real_data);
		return tvb_memcpy(member_tvb, target, member_offset, member_length);
	}
	else {
		/* The requested data is non-contiguous inside
		 * the member tvb. We have to memcpy() the part that's in the member tvb,
		 * then iterate across the other member tvb's, copying their portions
		 * until we have copied all data.
		 */
		retval = compute_offset_length(member_tvb, abs_offset - composite->start_offsets[i], -1,
				&member_offset, &member_length, NULL);
		DISSECTOR_ASSERT(retval);

		tvb_memcpy(member_tvb, target, member_offset, member_length);
		abs_offset	+= member_length;
		abs_length	-= member_length;

		/* Recurse */
		if (abs_length > 0) {
			composite_memcpy(tvb, target + member_length, abs_offset, abs_length);
		}

		return target;
	}

	DISSECTOR_ASSERT_NOT_REACHED();
	return NULL;
}

void*
tvb_memcpy(tvbuff_t *tvb, void* target, gint offset, size_t length)
{
	guint	abs_offset, abs_length;

	/*
	 * XXX - we should eliminate the "length = -1 means 'to the end
	 * of the tvbuff'" convention, and use other means to achieve
	 * that; this would let us eliminate a bunch of checks for
	 * negative lengths in cases where the protocol has a 32-bit
	 * length field.
	 *
	 * Allowing -1 but throwing an assertion on other negative
	 * lengths is a bit more work with the length being a size_t;
	 * instead, we check for a length <= 2^31-1.
	 */
	DISSECTOR_ASSERT(length <= 0x7FFFFFFF);
	check_offset_length(tvb, offset, (gint) length, &abs_offset, &abs_length);

	if (tvb->real_data) {
		return memcpy(target, tvb->real_data + abs_offset, abs_length);
	}

	switch(tvb->type) {
		case TVBUFF_REAL_DATA:
			DISSECTOR_ASSERT_NOT_REACHED();

		case TVBUFF_SUBSET:
			return tvb_memcpy(tvb->tvbuffs.subset.tvb, target,
					abs_offset - tvb->tvbuffs.subset.offset,
					abs_length);

		case TVBUFF_COMPOSITE:
			return composite_memcpy(tvb, target, offset, length);
	}

	DISSECTOR_ASSERT_NOT_REACHED();
	return NULL;
}


/*
 * XXX - this doesn't treat a length of -1 as an error.
 * If it did, this could replace some code that calls
 * "tvb_ensure_bytes_exist()" and then allocates a buffer and copies
 * data to it.
 *
 * "composite_ensure_contiguous_no_exception()" depends on -1 not being
 * an error; does anything else depend on this routine treating -1 as
 * meaning "to the end of the buffer"?
 */
void*
tvb_memdup(tvbuff_t *tvb, gint offset, size_t length)
{
	guint	abs_offset, abs_length;
	void	*duped;

	check_offset_length(tvb, offset, (gint) length, &abs_offset, &abs_length);

	duped = g_malloc(abs_length);
	return tvb_memcpy(tvb, duped, abs_offset, abs_length);
}

/*
 * XXX - this doesn't treat a length of -1 as an error.
 * If it did, this could replace some code that calls
 * "tvb_ensure_bytes_exist()" and then allocates a buffer and copies
 * data to it.
 *
 * "composite_ensure_contiguous_no_exception()" depends on -1 not being
 * an error; does anything else depend on this routine treating -1 as
 * meaning "to the end of the buffer"?
 *
 * This function allocates memory from a buffer with packet lifetime.
 * You do not have to free this buffer, it will be automatically freed
 * when wireshark starts decoding the next packet.
 * Do not use this function if you want the allocated memory to be persistent
 * after the current packet has been dissected.
 */
void*
ep_tvb_memdup(tvbuff_t *tvb, gint offset, size_t length)
{
	guint	abs_offset, abs_length;
	void	*duped;

	check_offset_length(tvb, offset, (gint) length, &abs_offset, &abs_length);

	duped = ep_alloc(abs_length);
	return tvb_memcpy(tvb, duped, abs_offset, abs_length);
}



const guint8*
tvb_get_ptr(tvbuff_t *tvb, gint offset, gint length)
{
	return ensure_contiguous(tvb, offset, length);
}

/* ---------------- */
guint8
tvb_get_guint8(tvbuff_t *tvb, gint offset)
{
	const guint8* ptr;

	ptr = fast_ensure_contiguous(tvb, offset, sizeof(guint8));	// BUG_7169C840(9) FIX_7169C840(9) #Pass potentially invalid pointer "ptr" to function "fast_ensure_contiguous", which may return an invalid pointer.
	return *ptr;							// BUG_7169C840(10) FIX_7169C840(10) #CWE-822 #CWE-125 #Potentially invalid pointer is dereferenced, leading to a crash.
}

guint16
tvb_get_ntohs(tvbuff_t *tvb, gint offset)
{
	const guint8* ptr;

	ptr = fast_ensure_contiguous(tvb, offset, sizeof(guint16));
	return pntohs(ptr);
}

guint32
tvb_get_ntoh24(tvbuff_t *tvb, gint offset)
{
	const guint8* ptr;

	ptr = fast_ensure_contiguous(tvb, offset, 3);
	return pntoh24(ptr);
}

guint32
tvb_get_ntohl(tvbuff_t *tvb, gint offset)
{
	const guint8* ptr;

	ptr = fast_ensure_contiguous(tvb, offset, sizeof(guint32));
	return pntohl(ptr);
}

guint64
tvb_get_ntoh64(tvbuff_t *tvb, gint offset)
{
	const guint8* ptr;

	ptr = fast_ensure_contiguous(tvb, offset, sizeof(guint64));
	return pntoh64(ptr);
}

/*
 * Stuff for IEEE float handling on platforms that don't have IEEE
 * format as the native floating-point format.
 *
 * For now, we treat only the VAX as such a platform.
 *
 * XXX - other non-IEEE boxes that can run UNIX include some Crays,
 * and possibly other machines.
 *
 * It appears that the official Linux port to System/390 and
 * zArchitecture uses IEEE format floating point (not a
 * huge surprise).
 *
 * I don't know whether there are any other machines that
 * could run Wireshark and that don't use IEEE format.
 * As far as I know, all of the main commercial microprocessor
 * families on which OSes that support Wireshark can run
 * use IEEE format (x86, 68k, SPARC, MIPS, PA-RISC, Alpha,
 * IA-64, and so on).
 */

#if defined(vax)

#include <math.h>

/*
 * Single-precision.
 */
#define IEEE_SP_NUMBER_WIDTH	32	/* bits in number */
#define IEEE_SP_EXP_WIDTH	8	/* bits in exponent */
#define IEEE_SP_MANTISSA_WIDTH	23	/* IEEE_SP_NUMBER_WIDTH - 1 - IEEE_SP_EXP_WIDTH */

#define IEEE_SP_SIGN_MASK	0x80000000
#define IEEE_SP_EXPONENT_MASK	0x7F800000
#define IEEE_SP_MANTISSA_MASK	0x007FFFFF
#define IEEE_SP_INFINITY	IEEE_SP_EXPONENT_MASK

#define IEEE_SP_IMPLIED_BIT (1 << IEEE_SP_MANTISSA_WIDTH)
#define IEEE_SP_INFINITE ((1 << IEEE_SP_EXP_WIDTH) - 1)
#define IEEE_SP_BIAS ((1 << (IEEE_SP_EXP_WIDTH - 1)) - 1)

static int
ieee_float_is_zero(guint32 w)
{
	return ((w & ~IEEE_SP_SIGN_MASK) == 0);
}

static gfloat
get_ieee_float(guint32 w)
{
	long sign;
	long exponent;
	long mantissa;

	sign = w & IEEE_SP_SIGN_MASK;
	exponent = w & IEEE_SP_EXPONENT_MASK;
	mantissa = w & IEEE_SP_MANTISSA_MASK;

	if (ieee_float_is_zero(w)) {
		/* number is zero, unnormalized, or not-a-number */
		return 0.0;
	}
#if 0
	/*
	 * XXX - how to handle this?
	 */
	if (IEEE_SP_INFINITY == exponent) {
		/*
		 * number is positive or negative infinity, or a special value
		 */
		return (sign? MINUS_INFINITY: PLUS_INFINITY);
	}
#endif

	exponent = ((exponent >> IEEE_SP_MANTISSA_WIDTH) - IEEE_SP_BIAS) -
	    IEEE_SP_MANTISSA_WIDTH;
	mantissa |= IEEE_SP_IMPLIED_BIT;

	if (sign)
		return -mantissa * pow(2, exponent);
	else
		return mantissa * pow(2, exponent);
}

/*
 * Double-precision.
 * We assume that if you don't have IEEE floating-point, you have a
 * compiler that understands 64-bit integral quantities.
 */
#define IEEE_DP_NUMBER_WIDTH	64	/* bits in number */
#define IEEE_DP_EXP_WIDTH	11	/* bits in exponent */
#define IEEE_DP_MANTISSA_WIDTH	52	/* IEEE_DP_NUMBER_WIDTH - 1 - IEEE_DP_EXP_WIDTH */

#define IEEE_DP_SIGN_MASK	0x8000000000000000LL
#define IEEE_DP_EXPONENT_MASK	0x7FF0000000000000LL
#define IEEE_DP_MANTISSA_MASK	0x000FFFFFFFFFFFFFLL
#define IEEE_DP_INFINITY	IEEE_DP_EXPONENT_MASK

#define IEEE_DP_IMPLIED_BIT (1LL << IEEE_DP_MANTISSA_WIDTH)
#define IEEE_DP_INFINITE ((1 << IEEE_DP_EXP_WIDTH) - 1)
#define IEEE_DP_BIAS ((1 << (IEEE_DP_EXP_WIDTH - 1)) - 1)

static int
ieee_double_is_zero(guint64 w)
{
	return ((w & ~IEEE_SP_SIGN_MASK) == 0);
}

static gdouble
get_ieee_double(guint64 w)
{
	gint64 sign;
	gint64 exponent;
	gint64 mantissa;

	sign = w & IEEE_DP_SIGN_MASK;
	exponent = w & IEEE_DP_EXPONENT_MASK;
	mantissa = w & IEEE_DP_MANTISSA_MASK;

	if (ieee_double_is_zero(w)) {
		/* number is zero, unnormalized, or not-a-number */
		return 0.0;
	}
#if 0
	/*
	 * XXX - how to handle this?
	 */
	if (IEEE_DP_INFINITY == exponent) {
		/*
		 * number is positive or negative infinity, or a special value
		 */
		return (sign? MINUS_INFINITY: PLUS_INFINITY);
	}
#endif

	exponent = ((exponent >> IEEE_DP_MANTISSA_WIDTH) - IEEE_DP_BIAS) -
	    IEEE_DP_MANTISSA_WIDTH;
	mantissa |= IEEE_DP_IMPLIED_BIT;

	if (sign)
		return -mantissa * pow(2, exponent);
	else
		return mantissa * pow(2, exponent);
}
#endif

/*
 * Fetches an IEEE single-precision floating-point number, in
 * big-endian form, and returns a "float".
 *
 * XXX - should this be "double", in case there are IEEE single-
 * precision numbers that won't fit in some platform's native
 * "float" format?
 */
gfloat
tvb_get_ntohieee_float(tvbuff_t *tvb, int offset)
{
#if defined(vax)
	return get_ieee_float(tvb_get_ntohl(tvb, offset));
#else
	union {
		gfloat f;
		guint32 w;
	} ieee_fp_union;

	ieee_fp_union.w = tvb_get_ntohl(tvb, offset);
	return ieee_fp_union.f;
#endif
}

/*
 * Fetches an IEEE double-precision floating-point number, in
 * big-endian form, and returns a "double".
 */
gdouble
tvb_get_ntohieee_double(tvbuff_t *tvb, int offset)
{
#if defined(vax)
	union {
		guint32 w[2];
		guint64 dw;
	} ieee_fp_union;
#else
	union {
		gdouble d;
		guint32 w[2];
	} ieee_fp_union;
#endif

#ifdef WORDS_BIGENDIAN
	ieee_fp_union.w[0] = tvb_get_ntohl(tvb, offset);
	ieee_fp_union.w[1] = tvb_get_ntohl(tvb, offset+4);
#else
	ieee_fp_union.w[0] = tvb_get_ntohl(tvb, offset+4);
	ieee_fp_union.w[1] = tvb_get_ntohl(tvb, offset);
#endif
#if defined(vax)
	return get_ieee_double(ieee_fp_union.dw);
#else
	return ieee_fp_union.d;
#endif
}

guint16
tvb_get_letohs(tvbuff_t *tvb, gint offset)
{
	const guint8* ptr;

	ptr = fast_ensure_contiguous(tvb, offset, sizeof(guint16));
	return pletohs(ptr);
}

guint32
tvb_get_letoh24(tvbuff_t *tvb, gint offset)
{
	const guint8* ptr;

	ptr = fast_ensure_contiguous(tvb, offset, 3);
	return pletoh24(ptr);
}

guint32
tvb_get_letohl(tvbuff_t *tvb, gint offset)
{
	const guint8* ptr;

	ptr = fast_ensure_contiguous(tvb, offset, sizeof(guint32));
	return pletohl(ptr);
}

guint64
tvb_get_letoh64(tvbuff_t *tvb, gint offset)
{
	const guint8* ptr;

	ptr = fast_ensure_contiguous(tvb, offset, sizeof(guint64));
	return pletoh64(ptr);
}

/*
 * Fetches an IEEE single-precision floating-point number, in
 * little-endian form, and returns a "float".
 *
 * XXX - should this be "double", in case there are IEEE single-
 * precision numbers that won't fit in some platform's native
 * "float" format?
 */
gfloat
tvb_get_letohieee_float(tvbuff_t *tvb, int offset)
{
#if defined(vax)
	return get_ieee_float(tvb_get_letohl(tvb, offset));
#else
	union {
		gfloat f;
		guint32 w;
	} ieee_fp_union;

	ieee_fp_union.w = tvb_get_letohl(tvb, offset);
	return ieee_fp_union.f;
#endif
}

/*
 * Fetches an IEEE double-precision floating-point number, in
 * little-endian form, and returns a "double".
 */
gdouble
tvb_get_letohieee_double(tvbuff_t *tvb, int offset)
{
#if defined(vax)
	union {
		guint32 w[2];
		guint64 dw;
	} ieee_fp_union;
#else
	union {
		gdouble d;
		guint32 w[2];
	} ieee_fp_union;
#endif

#ifdef WORDS_BIGENDIAN
	ieee_fp_union.w[0] = tvb_get_letohl(tvb, offset+4);
	ieee_fp_union.w[1] = tvb_get_letohl(tvb, offset);
#else
	ieee_fp_union.w[0] = tvb_get_letohl(tvb, offset);
	ieee_fp_union.w[1] = tvb_get_letohl(tvb, offset+4);
#endif
#if defined(vax)
	return get_ieee_double(ieee_fp_union.dw);
#else
	return ieee_fp_union.d;
#endif
}

/* Fetch an IPv4 address, in network byte order.
 * We do *not* convert them to host byte order; we leave them in
 * network byte order. */
guint32
tvb_get_ipv4(tvbuff_t *tvb, gint offset)
{
	const guint8* ptr;
	guint32 addr;

	ptr = fast_ensure_contiguous(tvb, offset, sizeof(guint32));
	memcpy(&addr, ptr, sizeof addr);
	return addr;
}

/* Fetch an IPv6 address. */
void
tvb_get_ipv6(tvbuff_t *tvb, gint offset, struct e_in6_addr *addr)
{
	const guint8* ptr;

	ptr = ensure_contiguous(tvb, offset, sizeof(*addr));
	memcpy(addr, ptr, sizeof *addr);
}

/* Fetch a GUID. */
void
tvb_get_ntohguid(tvbuff_t *tvb, gint offset, e_guid_t *guid)
{
	ensure_contiguous(tvb, offset, sizeof(*guid));
	guid->data1 = tvb_get_ntohl(tvb, offset);
	guid->data2 = tvb_get_ntohs(tvb, offset + 4);
	guid->data3 = tvb_get_ntohs(tvb, offset + 6);
	tvb_memcpy(tvb, guid->data4, offset + 8, sizeof guid->data4);
}

void
tvb_get_letohguid(tvbuff_t *tvb, gint offset, e_guid_t *guid)
{
	ensure_contiguous(tvb, offset, sizeof(*guid));
	guid->data1 = tvb_get_letohl(tvb, offset);
	guid->data2 = tvb_get_letohs(tvb, offset + 4);
	guid->data3 = tvb_get_letohs(tvb, offset + 6);
	tvb_memcpy(tvb, guid->data4, offset + 8, sizeof guid->data4);
}

void
tvb_get_guid(tvbuff_t *tvb, gint offset, e_guid_t *guid, gboolean little_endian)
{
	if (little_endian) {
		tvb_get_letohguid(tvb, offset, guid);
	} else {
		tvb_get_ntohguid(tvb, offset, guid);
	}
}

static const guint8 bit_mask8[] = {
    0xff,
    0x7f,
    0x3f,
    0x1f,
    0x0f,
    0x07,
    0x03,
    0x01
};

/* Bit offset mask for number of bits = 8 - 16 */
static const guint16 bit_mask16[] = {
    0xffff,
    0x7fff,
    0x3fff,
    0x1fff,
    0x0fff,
    0x07ff,
    0x03ff,
    0x01ff
};
/* Get 1 - 8 bits */
guint8
tvb_get_bits8(tvbuff_t *tvb, gint bit_offset, gint no_of_bits)
{
	gint	offset;
	guint16	value = 0;
	guint8	tot_no_bits;

	if (no_of_bits>8) {
		DISSECTOR_ASSERT_NOT_REACHED();
	}
	/* Byte align offset */
	offset = bit_offset>>3;

	/* Find out which mask to use for the most significant octet
	 * by convering bit_offset into the offset into the first
	 * fetched octet.
	 */
	bit_offset = bit_offset & 0x7;
	tot_no_bits = bit_offset+no_of_bits;
	if(tot_no_bits<=8){
		/* Read one octet, mask off bit_offset bits and left shift out the unused bits */
		value = tvb_get_guint8(tvb,offset) & bit_mask8[bit_offset];
		value = value >> (8-tot_no_bits);
	}else{
		/* Read two octets, mask off bit_offset bits and left shift out the unused bits */
		value = tvb_get_ntohs(tvb,offset) & bit_mask16[bit_offset];
		value = value >> (16 - tot_no_bits);
	}

	return (guint8)value;

}



/* Get 9 - 16 bits */
/* Bit offset mask for number of bits = 9 - 32 */
static const guint32 bit_mask32[] = {
    0xffffffff,
    0x7fffffff,
    0x3fffffff,
    0x1fffffff,
    0x0fffffff,
    0x07ffffff,
    0x03ffffff,
    0x01ffffff
};
guint16
tvb_get_bits16(tvbuff_t *tvb, gint bit_offset, gint no_of_bits,gboolean little_endian)
{
	gint	offset;
	guint16	value = 0;
	guint16	tempval = 0;
	guint8	tot_no_bits;

	if ((no_of_bits<8)||(no_of_bits>16)) {
		/* If bits < 8 use tvb_get_bits8 */
		DISSECTOR_ASSERT_NOT_REACHED();
	}
	if(little_endian){
		DISSECTOR_ASSERT_NOT_REACHED();
		/* This part is not implemented yet */
	}

	/* Byte align offset */
	offset = bit_offset>>3;

	/* Find out which mask to use for the most significant octet
	 * by convering bit_offset into the offset into the first
	 * fetched octet.
	 */
	bit_offset = bit_offset & 0x7;
	tot_no_bits = bit_offset+no_of_bits;
	/* Read two octets and mask off bit_offset bits */
	value = tvb_get_ntohs(tvb,offset) & bit_mask16[bit_offset];
	if(tot_no_bits < 16){
		/* Left shift out the unused bits */
		value = value >> (16 - tot_no_bits);
	}else if(tot_no_bits > 16){
		/* Spans three octets, read next octet and shift as needed */
		value = value << (tot_no_bits - 16);
		tempval = tvb_get_guint8(tvb,offset+2);
		tempval = tempval >> (24-tot_no_bits);
		value = value | tempval;
	}

	return value;

}

/* Bit offset mask for number of bits = 32 - 64 */
static const guint64 bit_mask64[] = {
    G_GINT64_CONSTANT(0xffffffffffffffffU),
    G_GINT64_CONSTANT(0x7fffffffffffffffU),
    G_GINT64_CONSTANT(0x3fffffffffffffffU),
    G_GINT64_CONSTANT(0x1fffffffffffffffU),
    G_GINT64_CONSTANT(0x0fffffffffffffffU),
    G_GINT64_CONSTANT(0x07ffffffffffffffU),
    G_GINT64_CONSTANT(0x03ffffffffffffffU),
    G_GINT64_CONSTANT(0x01ffffffffffffffU)
};

guint32
tvb_get_bits32(tvbuff_t *tvb, gint bit_offset, gint no_of_bits, gboolean little_endian)
{
	gint	offset;
	guint32	value = 0;
	guint32	tempval = 0;
	guint8	tot_no_bits;

	if ((no_of_bits<17)||(no_of_bits>32)) {
		/* If bits < 17 use tvb_get_bits8 or tvb_get_bits_ntohs */
		DISSECTOR_ASSERT_NOT_REACHED();
	}
	if(little_endian){
		DISSECTOR_ASSERT_NOT_REACHED();
		/* This part is not implemented yet */
	}

	/* Byte align offset */
	offset = bit_offset>>3;

	/* Find out which mask to use for the most significant octet
	 * by convering bit_offset into the offset into the first
	 * fetched octet.
	 */
	bit_offset = bit_offset & 0x7;
	tot_no_bits = bit_offset+no_of_bits;
	/* Read four octets and mask off bit_offset bits */
	value = tvb_get_ntohl(tvb,offset) & bit_mask32[bit_offset];
	if(tot_no_bits < 32){
		/* Left shift out the unused bits */
		value = value >> (32 - tot_no_bits);
	}else if(tot_no_bits > 32){
		/* Spans five octets, read next octet and shift as needed */
		value = value << (tot_no_bits - 32);
		tempval = tvb_get_guint8(tvb,offset+4);
		tempval = tempval >> (40-tot_no_bits);
		value = value | tempval;
	}

	return value;

}
guint64
tvb_get_bits64(tvbuff_t *tvb, gint bit_offset, gint no_of_bits, gboolean little_endian)
{

	gint	offset;
	guint64	value = 0;
	guint64	tempval = 0;
	guint8	tot_no_bits;

	if ((no_of_bits<32)||(no_of_bits>64)) {
		/* If bits < 17 use tvb_get_bits8 or tvb_get_bits_ntohs */
		DISSECTOR_ASSERT_NOT_REACHED();
	}
	if(little_endian){
		DISSECTOR_ASSERT_NOT_REACHED();
		/* This part is not implemented yet */
	}

	/* Byte align offset */
	offset = bit_offset>>3;

	/* Find out which mask to use for the most significant octet
	 * by convering bit_offset into the offset into the first
	 * fetched octet.
	 */
	bit_offset = bit_offset & 0x7;
	tot_no_bits = bit_offset+no_of_bits;
	/* Read eight octets and mask off bit_offset bits */
	value = tvb_get_ntoh64(tvb,offset) & bit_mask64[bit_offset];
	if (tot_no_bits < 64){
		/* Left shift out the unused bits */
		value = value >> (64 - tot_no_bits);
	}else if (tot_no_bits > 64){
		/* Spans nine octets, read next octet and shift as needed */
		value = value << (tot_no_bits - 64);
		tempval = tvb_get_guint8(tvb,offset+8);
		tempval = tempval >> (72-tot_no_bits);
		value = value | tempval;
	}

	return value;

}

/* Find first occurence of needle in tvbuff, starting at offset. Searches
 * at most maxlength number of bytes; if maxlength is -1, searches to
 * end of tvbuff.
 * Returns the offset of the found needle, or -1 if not found.
 * Will not throw an exception, even if maxlength exceeds boundary of tvbuff;
 * in that case, -1 will be returned if the boundary is reached before
 * finding needle. */
gint
tvb_find_guint8(tvbuff_t *tvb, gint offset, gint maxlength, guint8 needle)
{
	const guint8	*result;
	guint		abs_offset, junk_length;
	guint		tvbufflen;
	guint		limit;

	check_offset_length(tvb, offset, 0, &abs_offset, &junk_length);

	/* Only search to end of tvbuff, w/o throwing exception. */
	tvbufflen = tvb_length_remaining(tvb, abs_offset);
	if (maxlength == -1) {
		/* No maximum length specified; search to end of tvbuff. */
		limit = tvbufflen;
	}
	else if (tvbufflen < (guint) maxlength) {
		/* Maximum length goes past end of tvbuff; search to end
		   of tvbuff. */
		limit = tvbufflen;
	}
	else {
		/* Maximum length doesn't go past end of tvbuff; search
		   to that value. */
		limit = maxlength;
	}

	/* If we have real data, perform our search now. */
	if (tvb->real_data) {
		result = guint8_find(tvb->real_data + abs_offset, limit, needle);
		if (result == NULL) {
			return -1;
		}
		else {
			return (gint) (result - tvb->real_data);
		}
	}

	switch(tvb->type) {
		case TVBUFF_REAL_DATA:
			DISSECTOR_ASSERT_NOT_REACHED();

		case TVBUFF_SUBSET:
			return tvb_find_guint8(tvb->tvbuffs.subset.tvb,
					abs_offset - tvb->tvbuffs.subset.offset,
					limit, needle);

		case TVBUFF_COMPOSITE:
			DISSECTOR_ASSERT_NOT_REACHED();
			/* XXX - return composite_find_guint8(tvb, offset, limit, needle); */
	}

	DISSECTOR_ASSERT_NOT_REACHED();
	return -1;
}

/* Find first occurence of any of the needles in tvbuff, starting at offset.
 * Searches at most maxlength number of bytes; if maxlength is -1, searches
 * to end of tvbuff.
 * Returns the offset of the found needle, or -1 if not found.
 * Will not throw an exception, even if maxlength exceeds boundary of tvbuff;
 * in that case, -1 will be returned if the boundary is reached before
 * finding needle. */
gint
tvb_pbrk_guint8(tvbuff_t *tvb, gint offset, gint maxlength, const guint8 *needles)
{
	const guint8	*result;
	guint		abs_offset, junk_length;
	guint		tvbufflen;
	guint		limit;

	check_offset_length(tvb, offset, 0, &abs_offset, &junk_length);

	/* Only search to end of tvbuff, w/o throwing exception. */
	tvbufflen = tvb_length_remaining(tvb, abs_offset);
	if (maxlength == -1) {
		/* No maximum length specified; search to end of tvbuff. */
		limit = tvbufflen;
	}
	else if (tvbufflen < (guint) maxlength) {
		/* Maximum length goes past end of tvbuff; search to end
		   of tvbuff. */
		limit = tvbufflen;
	}
	else {
		/* Maximum length doesn't go past end of tvbuff; search
		   to that value. */
		limit = maxlength;
	}

	/* If we have real data, perform our search now. */
	if (tvb->real_data) {
		result = guint8_pbrk(tvb->real_data + abs_offset, limit, needles);
		if (result == NULL) {
			return -1;
		}
		else {
			return (gint) (result - tvb->real_data);
		}
	}

	switch(tvb->type) {
		case TVBUFF_REAL_DATA:
			DISSECTOR_ASSERT_NOT_REACHED();

		case TVBUFF_SUBSET:
			return tvb_pbrk_guint8(tvb->tvbuffs.subset.tvb,
					abs_offset - tvb->tvbuffs.subset.offset,
					limit, needles);

		case TVBUFF_COMPOSITE:
			DISSECTOR_ASSERT_NOT_REACHED();
			/* XXX - return composite_pbrk_guint8(tvb, offset, limit, needle); */
	}

	DISSECTOR_ASSERT_NOT_REACHED();
	return -1;
}

/* Find size of stringz (NUL-terminated string) by looking for terminating
 * NUL.  The size of the string includes the terminating NUL.
 *
 * If the NUL isn't found, it throws the appropriate exception.
 */
guint
tvb_strsize(tvbuff_t *tvb, gint offset)
{
	guint	abs_offset, junk_length;
	gint	nul_offset;

	check_offset_length(tvb, offset, 0, &abs_offset, &junk_length);
	nul_offset = tvb_find_guint8(tvb, abs_offset, -1, 0);
	if (nul_offset == -1) {
		/*
		 * OK, we hit the end of the tvbuff, so we should throw
		 * an exception.
		 *
		 * Did we hit the end of the captured data, or the end
		 * of the actual data?  If there's less captured data
		 * than actual data, we presumably hit the end of the
		 * captured data, otherwise we hit the end of the actual
		 * data.
		 */
		if (tvb_length(tvb) < tvb_reported_length(tvb)) {
			THROW(BoundsError);
		} else {
			THROW(ReportedBoundsError);
		}
	}
	return (nul_offset - abs_offset) + 1;
}

/* Find length of string by looking for end of string ('\0'), up to
 * 'maxlength' characters'; if 'maxlength' is -1, searches to end
 * of tvbuff.
 * Returns -1 if 'maxlength' reached before finding EOS. */
gint
tvb_strnlen(tvbuff_t *tvb, gint offset, guint maxlength)
{
	gint	result_offset;
	guint	abs_offset, junk_length;

	check_offset_length(tvb, offset, 0, &abs_offset, &junk_length);

	result_offset = tvb_find_guint8(tvb, abs_offset, maxlength, 0);

	if (result_offset == -1) {
		return -1;
	}
	else {
		return result_offset - abs_offset;
	}
}

/*
 * Implement strneql etc
 */

/*
 * Call strncmp after checking if enough chars left, returning 0 if
 * it returns 0 (meaning "equal") and -1 otherwise, otherwise return -1.
 */
gint
tvb_strneql(tvbuff_t *tvb, gint offset, const gchar *str, gint size)
{
	const guint8 *ptr;

	ptr = ensure_contiguous_no_exception(tvb, offset, size, NULL);

	if (ptr) {
		int cmp = strncmp((const char *)ptr, str, size);

		/*
		 * Return 0 if equal, -1 otherwise.
		 */
		return (cmp == 0 ? 0 : -1);
	} else {
		/*
		 * Not enough characters in the tvbuff to match the
		 * string.
		 */
		return -1;
	}
}

/*
 * Call g_ascii_strncasecmp after checking if enough chars left, returning
 * 0 if it returns 0 (meaning "equal") and -1 otherwise, otherwise return -1.
 */
gint
tvb_strncaseeql(tvbuff_t *tvb, gint offset, const gchar *str, gint size)
{
	const guint8 *ptr;

	ptr = ensure_contiguous_no_exception(tvb, offset, size, NULL);

	if (ptr) {
		int cmp = g_ascii_strncasecmp((const char *)ptr, str, size);

		/*
		 * Return 0 if equal, -1 otherwise.
		 */
		return (cmp == 0 ? 0 : -1);
	} else {
		/*
		 * Not enough characters in the tvbuff to match the
		 * string.
		 */
		return -1;
	}
}

/*
 * Call memcmp after checking if enough chars left, returning 0 if
 * it returns 0 (meaning "equal") and -1 otherwise, otherwise return -1.
 */
gint
tvb_memeql(tvbuff_t *tvb, gint offset, const guint8 *str, size_t size)
{
	const guint8 *ptr;

	ptr = ensure_contiguous_no_exception(tvb, offset, (gint) size, NULL);

	if (ptr) {
		int cmp = memcmp(ptr, str, size);

		/*
		 * Return 0 if equal, -1 otherwise.
		 */
		return (cmp == 0 ? 0 : -1);
	} else {
		/*
		 * Not enough characters in the tvbuff to match the
		 * string.
		 */
		return -1;
	}
}

/* Convert a string from Unicode to ASCII.  At the moment we fake it by
 * replacing all non-ASCII characters with a '.' )-:  The caller must
 * free the result returned.  The len parameter is the number of guint16's
 * to convert from Unicode. */
char *
tvb_fake_unicode(tvbuff_t *tvb, int offset, int len, gboolean little_endian)
{
	char *buffer;
	int i;
	guint16 character;

	/* Make sure we have enough data before allocating the buffer,
	   so we don't blow up if the length is huge. */
	tvb_ensure_bytes_exist(tvb, offset, 2*len);

	/* We know we won't throw an exception, so we don't have to worry
	   about leaking this buffer. */
	buffer = g_malloc(len + 1);

	for (i = 0; i < len; i++) {
		character = little_endian ? tvb_get_letohs(tvb, offset)
					  : tvb_get_ntohs(tvb, offset);
		buffer[i] = character < 256 ? character : '.';
		offset += 2;
	}

	buffer[len] = 0;

	return buffer;
}

/* Convert a string from Unicode to ASCII.  At the moment we fake it by
 * replacing all non-ASCII characters with a '.' )-:   The len parameter is
 * the number of guint16's to convert from Unicode.
 *
 * This function allocates memory from a buffer with packet lifetime.
 * You do not have to free this buffer, it will be automatically freed
 * when wireshark starts decoding the next packet.
 */
char *
tvb_get_ephemeral_faked_unicode(tvbuff_t *tvb, int offset, int len, gboolean little_endian)
{
	char *buffer;
	int i;
	guint16 character;

	/* Make sure we have enough data before allocating the buffer,
	   so we don't blow up if the length is huge. */
	tvb_ensure_bytes_exist(tvb, offset, 2*len);

	/* We know we won't throw an exception, so we don't have to worry
	   about leaking this buffer. */
	buffer = ep_alloc(len + 1);

	for (i = 0; i < len; i++) {
		character = little_endian ? tvb_get_letohs(tvb, offset)
					  : tvb_get_ntohs(tvb, offset);
		buffer[i] = character < 256 ? character : '.';
		offset += 2;
	}

	buffer[len] = 0;

	return buffer;
}

/*
 * Format the data in the tvb from offset for length ...
 */

gchar *
tvb_format_text(tvbuff_t *tvb, gint offset, gint size)
{
  const guint8 *ptr;
  gint len = size;

  if ((ptr = ensure_contiguous(tvb, offset, size)) == NULL) {

    len = tvb_length_remaining(tvb, offset);
    ptr = ensure_contiguous(tvb, offset, len);

  }

  return format_text(ptr, len);

}

/*
 * Format the data in the tvb from offset for length ...
 */

gchar *
tvb_format_text_wsp(tvbuff_t *tvb, gint offset, gint size)
{
  const guint8 *ptr;
  gint len = size;

  if ((ptr = ensure_contiguous(tvb, offset, size)) == NULL) {

    len = tvb_length_remaining(tvb, offset);
    ptr = ensure_contiguous(tvb, offset, len);

  }

  return format_text_wsp(ptr, len);

}

/*
 * Like "tvb_format_text()", but for null-padded strings; don't show
 * the null padding characters as "\000".
 */
gchar *
tvb_format_stringzpad(tvbuff_t *tvb, gint offset, gint size)
{
  const guint8 *ptr, *p;
  gint len = size;
  gint stringlen;

  if ((ptr = ensure_contiguous(tvb, offset, size)) == NULL) {

    len = tvb_length_remaining(tvb, offset);
    ptr = ensure_contiguous(tvb, offset, len);

  }

  for (p = ptr, stringlen = 0; stringlen < len && *p != '\0'; p++, stringlen++)
    ;
  return format_text(ptr, stringlen);

}

/*
 * Like "tvb_format_text_wsp()", but for null-padded strings; don't show
 * the null padding characters as "\000".
 */
gchar *
tvb_format_stringzpad_wsp(tvbuff_t *tvb, gint offset, gint size)
{
  const guint8 *ptr, *p;
  gint len = size;
  gint stringlen;

  if ((ptr = ensure_contiguous(tvb, offset, size)) == NULL) {

    len = tvb_length_remaining(tvb, offset);
    ptr = ensure_contiguous(tvb, offset, len);

  }

  for (p = ptr, stringlen = 0; stringlen < len && *p != '\0'; p++, stringlen++)
    ;
  return format_text_wsp(ptr, stringlen);

}

/*
 * Given a tvbuff, an offset, and a length, allocate a buffer big enough
 * to hold a non-null-terminated string of that length at that offset,
 * plus a trailing '\0', copy the string into it, and return a pointer
 * to the string.
 *
 * Throws an exception if the tvbuff ends before the string does.
 */
guint8 *
tvb_get_string(tvbuff_t *tvb, gint offset, gint length)
{
	const guint8 *ptr;
	guint8 *strbuf = NULL;

	tvb_ensure_bytes_exist(tvb, offset, length);

	ptr = ensure_contiguous(tvb, offset, length);
	strbuf = g_malloc(length + 1);
	if (length != 0) {
		memcpy(strbuf, ptr, length);
	}
	strbuf[length] = '\0';
	return strbuf;
}
/*
 * Given a tvbuff, an offset, and a length, allocate a buffer big enough
 * to hold a non-null-terminated string of that length at that offset,
 * plus a trailing '\0', copy the string into it, and return a pointer
 * to the string.
 *
 * Throws an exception if the tvbuff ends before the string does.
 *
 * This function allocates memory from a buffer with packet lifetime.
 * You do not have to free this buffer, it will be automatically freed
 * when wireshark starts decoding the next packet.
 * Do not use this function if you want the allocated memory to be persistent
 * after the current packet has been dissected.
 */
guint8 *
tvb_get_ephemeral_string(tvbuff_t *tvb, gint offset, gint length)
{
	const guint8 *ptr;
	guint8 *strbuf = NULL;

	tvb_ensure_bytes_exist(tvb, offset, length);

	ptr = ensure_contiguous(tvb, offset, length);
	strbuf = ep_alloc(length + 1);
	if (length != 0) {
		memcpy(strbuf, ptr, length);
	}
	strbuf[length] = '\0';
	return strbuf;
}

/*
 * Given a tvbuff, an offset, and a length, allocate a buffer big enough
 * to hold a non-null-terminated string of that length at that offset,
 * plus a trailing '\0', copy the string into it, and return a pointer
 * to the string.
 *
 * Throws an exception if the tvbuff ends before the string does.
 *
 * This function allocates memory from a buffer with capture session lifetime.
 * You do not have to free this buffer, it will be automatically freed
 * when wireshark starts or opens a new capture.
 */
guint8 *
tvb_get_seasonal_string(tvbuff_t *tvb, gint offset, gint length)
{
	const guint8 *ptr;
	guint8 *strbuf = NULL;

	tvb_ensure_bytes_exist(tvb, offset, length);

	ptr = ensure_contiguous(tvb, offset, length);
	strbuf = se_alloc(length + 1);
	if (length != 0) {
		memcpy(strbuf, ptr, length);
	}
	strbuf[length] = '\0';
	return strbuf;
}

/*
 * Given a tvbuff and an offset, with the offset assumed to refer to
 * a null-terminated string, find the length of that string (and throw
 * an exception if the tvbuff ends before we find the null), allocate
 * a buffer big enough to hold the string, copy the string into it,
 * and return a pointer to the string.  Also return the length of the
 * string (including the terminating null) through a pointer.
 */
guint8 *
tvb_get_stringz(tvbuff_t *tvb, gint offset, gint *lengthp)
{
	guint size;
	guint8 *strptr;

	size = tvb_strsize(tvb, offset);
	strptr = g_malloc(size);
	tvb_memcpy(tvb, strptr, offset, size);
	*lengthp = size;
	return strptr;
}
/*
 * Given a tvbuff and an offset, with the offset assumed to refer to
 * a null-terminated string, find the length of that string (and throw
 * an exception if the tvbuff ends before we find the null), allocate
 * a buffer big enough to hold the string, copy the string into it,
 * and return a pointer to the string.  Also return the length of the
 * string (including the terminating null) through a pointer.
 *
 * This function allocates memory from a buffer with packet lifetime.
 * You do not have to free this buffer, it will be automatically freed
 * when wireshark starts decoding the next packet.
 * Do not use this function if you want the allocated memory to be persistent
 * after the current packet has been dissected.
 */
guint8 *
tvb_get_ephemeral_stringz(tvbuff_t *tvb, gint offset, gint *lengthp)
{
	guint size;
	guint8 *strptr;

	size = tvb_strsize(tvb, offset);
	strptr = ep_alloc(size);
	tvb_memcpy(tvb, strptr, offset, size);
	*lengthp = size;
	return strptr;
}

/*
 * Given a tvbuff and an offset, with the offset assumed to refer to
 * a null-terminated string, find the length of that string (and throw
 * an exception if the tvbuff ends before we find the null), allocate
 * a buffer big enough to hold the string, copy the string into it,
 * and return a pointer to the string.  Also return the length of the
 * string (including the terminating null) through a pointer.
 *
 * This function allocates memory from a buffer with capture session lifetime.
 * You do not have to free this buffer, it will be automatically freed
 * when wireshark starts or opens a new capture.
 */
guint8 *
tvb_get_seasonal_stringz(tvbuff_t *tvb, gint offset, gint *lengthp)
{
	guint size;
	guint8 *strptr;

	size = tvb_strsize(tvb, offset);
	strptr = se_alloc(size);
	tvb_memcpy(tvb, strptr, offset, size);
	*lengthp = size;
	return strptr;
}

/* Looks for a stringz (NUL-terminated string) in tvbuff and copies
 * no more than bufsize number of bytes, including terminating NUL, to buffer.
 * Returns length of string (not including terminating NUL), or -1 if the string was
 * truncated in the buffer due to not having reached the terminating NUL.
 * In this way, it acts like g_snprintf().
 *
 * bufsize MUST be greater than 0.
 *
 * When processing a packet where the remaining number of bytes is less
 * than bufsize, an exception is not thrown if the end of the packet
 * is reached before the NUL is found. If no NUL is found before reaching
 * the end of the short packet, -1 is still returned, and the string
 * is truncated with a NUL, albeit not at buffer[bufsize - 1], but
 * at the correct spot, terminating the string.
 *
 * *bytes_copied will contain the number of bytes actually copied,
 * including the terminating-NUL.
 */
static gint
_tvb_get_nstringz(tvbuff_t *tvb, gint offset, guint bufsize, guint8* buffer,
		gint *bytes_copied)
{
	gint	stringlen;
	guint	abs_offset, junk_length;
	gint	limit, len;
	gboolean decreased_max = FALSE;

	check_offset_length(tvb, offset, 0, &abs_offset, &junk_length);

	/* There must at least be room for the terminating NUL. */
	DISSECTOR_ASSERT(bufsize != 0);

	/* If there's no room for anything else, just return the NUL. */
	if (bufsize == 1) {
		buffer[0] = 0;
		*bytes_copied = 1;
		return 0;
	}

	/* Only read to end of tvbuff, w/o throwing exception. */
	len = tvb_length_remaining(tvb, abs_offset);

	/* check_offset_length() won't throw an exception if we're
	 * looking at the byte immediately after the end of the tvbuff. */
	if (len == 0) {
		THROW(ReportedBoundsError);
	}

	/* This should not happen because check_offset_length() would
	 * have already thrown an exception if 'offset' were out-of-bounds.
	 */
	DISSECTOR_ASSERT(len != -1);

	/*
	 * If we've been passed a negative number, bufsize will
	 * be huge.
	 */
	DISSECTOR_ASSERT(bufsize <= G_MAXINT);

	if ((guint)len < bufsize) {
		limit = len;
		decreased_max = TRUE;
	}
	else {
		limit = bufsize;
	}

	stringlen = tvb_strnlen(tvb, abs_offset, limit - 1);
	/* If NUL wasn't found, copy the data and return -1 */
	if (stringlen == -1) {
		tvb_memcpy(tvb, buffer, abs_offset, limit);
		if (decreased_max) {
			buffer[limit] = 0;
			/* Add 1 for the extra NUL that we set at buffer[limit],
			 * pretending that it was copied as part of the string. */
			*bytes_copied = limit + 1;
		}
		else {
			*bytes_copied = limit;
		}
		return -1;
	}

	/* Copy the string to buffer */
	tvb_memcpy(tvb, buffer, abs_offset, stringlen + 1);
	*bytes_copied = stringlen + 1;
	return stringlen;
}

/* Looks for a stringz (NUL-terminated string) in tvbuff and copies
 * no more than bufsize number of bytes, including terminating NUL, to buffer.
 * Returns length of string (not including terminating NUL), or -1 if the string was
 * truncated in the buffer due to not having reached the terminating NUL.
 * In this way, it acts like g_snprintf().
 *
 * When processing a packet where the remaining number of bytes is less
 * than bufsize, an exception is not thrown if the end of the packet
 * is reached before the NUL is found. If no NUL is found before reaching
 * the end of the short packet, -1 is still returned, and the string
 * is truncated with a NUL, albeit not at buffer[bufsize - 1], but
 * at the correct spot, terminating the string.
 */
gint
tvb_get_nstringz(tvbuff_t *tvb, gint offset, guint bufsize, guint8* buffer)
{
	gint bytes_copied;

	return _tvb_get_nstringz(tvb, offset, bufsize, buffer, &bytes_copied);
}

/* Like tvb_get_nstringz(), but never returns -1. The string is guaranteed to
 * have a terminating NUL. If the string was truncated when copied into buffer,
 * a NUL is placed at the end of buffer to terminate it.
 */
gint
tvb_get_nstringz0(tvbuff_t *tvb, gint offset, guint bufsize, guint8* buffer)
{
	gint	len, bytes_copied;

	len = _tvb_get_nstringz(tvb, offset, bufsize, buffer, &bytes_copied);

	if (len == -1) {
		buffer[bufsize - 1] = 0;
		return bytes_copied - 1;
	}
	else {
		return len;
	}
}

/*
 * Given a tvbuff, an offset into the tvbuff, and a length that starts
 * at that offset (which may be -1 for "all the way to the end of the
 * tvbuff"), find the end of the (putative) line that starts at the
 * specified offset in the tvbuff, going no further than the specified
 * length.
 *
 * Return the length of the line (not counting the line terminator at
 * the end), or, if we don't find a line terminator:
 *
 *	if "deseg" is true, return -1;
 *
 *	if "deseg" is false, return the amount of data remaining in
 *	the buffer.
 *
 * Set "*next_offset" to the offset of the character past the line
 * terminator, or past the end of the buffer if we don't find a line
 * terminator.  (It's not set if we return -1.)
 */
gint
tvb_find_line_end(tvbuff_t *tvb, gint offset, int len, gint *next_offset,
    gboolean desegment)
{
	gint eob_offset;
	gint eol_offset;
	int linelen;

	if (len == -1)
		len = tvb_length_remaining(tvb, offset);
	/*
	 * XXX - what if "len" is still -1, meaning "offset is past the
	 * end of the tvbuff"?
	 */
	eob_offset = offset + len;

	/*
	 * Look either for a CR or an LF.
	 */
	eol_offset = tvb_pbrk_guint8(tvb, offset, len, (const guint8 *)"\r\n");
	if (eol_offset == -1) {
		/*
		 * No CR or LF - line is presumably continued in next packet.
		 */
		if (desegment) {
			/*
			 * Tell our caller we saw no EOL, so they can
			 * try to desegment and get the entire line
			 * into one tvbuff.
			 */
			return -1;
		} else {
			/*
			 * Pretend the line runs to the end of the tvbuff.
			 */
			linelen = eob_offset - offset;
			*next_offset = eob_offset;
		}
	} else {
		/*
		 * Find the number of bytes between the starting offset
		 * and the CR or LF.
		 */
		linelen = eol_offset - offset;

		/*
		 * Is it a CR?
		 */
		if (tvb_get_guint8(tvb, eol_offset) == '\r') {
			/*
			 * Yes - is it followed by an LF?
			 */
			if (eol_offset + 1 >= eob_offset) {
				/*
				 * Dunno - the next byte isn't in this
				 * tvbuff.
				 */
				if (desegment) {
					/*
					 * We'll return -1, although that
					 * runs the risk that if the line
					 * really *is* terminated with a CR,
					 * we won't properly dissect this
					 * tvbuff.
					 *
					 * It's probably more likely that
					 * the line ends with CR-LF than
					 * that it ends with CR by itself.
					 */
					return -1;
				}
			} else {
				/*
				 * Well, we can at least look at the next
				 * byte.
				 */
				if (tvb_get_guint8(tvb, eol_offset + 1) == '\n') {
					/*
					 * It's an LF; skip over the CR.
					 */
					eol_offset++;
				}
			}
		}

		/*
		 * Return the offset of the character after the last
		 * character in the line, skipping over the last character
		 * in the line terminator.
		 */
		*next_offset = eol_offset + 1;
	}
	return linelen;
}

/*
 * Given a tvbuff, an offset into the tvbuff, and a length that starts
 * at that offset (which may be -1 for "all the way to the end of the
 * tvbuff"), find the end of the (putative) line that starts at the
 * specified offset in the tvbuff, going no further than the specified
 * length.
 *
 * However, treat quoted strings inside the buffer specially - don't
 * treat newlines in quoted strings as line terminators.
 *
 * Return the length of the line (not counting the line terminator at
 * the end), or the amount of data remaining in the buffer if we don't
 * find a line terminator.
 *
 * Set "*next_offset" to the offset of the character past the line
 * terminator, or past the end of the buffer if we don't find a line
 * terminator.
 */
gint
tvb_find_line_end_unquoted(tvbuff_t *tvb, gint offset, int len,
    gint *next_offset)
{
	gint cur_offset, char_offset;
	gboolean is_quoted;
	guchar c;
	gint eob_offset;
	int linelen;

	if (len == -1)
		len = tvb_length_remaining(tvb, offset);
	/*
	 * XXX - what if "len" is still -1, meaning "offset is past the
	 * end of the tvbuff"?
	 */
	eob_offset = offset + len;

	cur_offset = offset;
	is_quoted = FALSE;
	for (;;) {
	    	/*
		 * Is this part of the string quoted?
		 */
		if (is_quoted) {
			/*
			 * Yes - look only for the terminating quote.
			 */
			char_offset = tvb_find_guint8(tvb, cur_offset, len,
			    '"');
		} else {
			/*
			 * Look either for a CR, an LF, or a '"'.
			 */
			char_offset = tvb_pbrk_guint8(tvb, cur_offset, len,
			    (const guint8 *)"\r\n\"");
		}
		if (char_offset == -1) {
			/*
			 * Not found - line is presumably continued in
			 * next packet.
			 * We pretend the line runs to the end of the tvbuff.
			 */
			linelen = eob_offset - offset;
			*next_offset = eob_offset;
			break;
		}

		if (is_quoted) {
			/*
			 * We're processing a quoted string.
			 * We only looked for ", so we know it's a ";
			 * as we're processing a quoted string, it's a
			 * closing quote.
			 */
			is_quoted = FALSE;
		} else {
			/*
			 * OK, what is it?
			 */
			c = tvb_get_guint8(tvb, char_offset);
			if (c == '"') {
				/*
				 * Un-quoted "; it begins a quoted
				 * string.
				 */
				is_quoted = TRUE;
			} else {
				/*
				 * It's a CR or LF; we've found a line
				 * terminator.
				 *
				 * Find the number of bytes between the
				 * starting offset and the CR or LF.
				 */
				linelen = char_offset - offset;

				/*
				 * Is it a CR?
				 */
				if (c == '\r') {
					/*
					 * Yes; is it followed by an LF?
					 */
					if (char_offset + 1 < eob_offset &&
					    tvb_get_guint8(tvb, char_offset + 1)
					      == '\n') {
						/*
						 * Yes; skip over the CR.
						 */
						char_offset++;
					}
				}

				/*
				 * Return the offset of the character after
				 * the last character in the line, skipping
				 * over the last character in the line
				 * terminator, and quit.
				 */
				*next_offset = char_offset + 1;
				break;
			}
		}

		/*
		 * Step past the character we found.
		 */
		cur_offset = char_offset + 1;
		if (cur_offset >= eob_offset) {
			/*
			 * The character we found was the last character
			 * in the tvbuff - line is presumably continued in
			 * next packet.
			 * We pretend the line runs to the end of the tvbuff.
			 */
			linelen = eob_offset - offset;
			*next_offset = eob_offset;
			break;
		}
	}
	return linelen;
}

/*
 * Copied from the mgcp dissector. (This function should be moved to /epan )
 * tvb_skip_wsp - Returns the position in tvb of the first non-whitespace
 *                character following offset or offset + maxlength -1 whichever
 *                is smaller.
 *
 * Parameters:
 * tvb - The tvbuff in which we are skipping whitespace.
 * offset - The offset in tvb from which we begin trying to skip whitespace.
 * maxlength - The maximum distance from offset that we may try to skip
 * whitespace.
 *
 * Returns: The position in tvb of the first non-whitespace
 *          character following offset or offset + maxlength -1 whichever
 *          is smaller.
 */
gint tvb_skip_wsp(tvbuff_t* tvb, gint offset, gint maxlength)
{
	gint counter = offset;
	gint end = offset + maxlength,tvb_len;
	guint8 tempchar;

	/* Get the length remaining */
	tvb_len = tvb_length(tvb);
	end = offset + maxlength;
	if (end >= tvb_len)
	{
		end = tvb_len;
	}

	/* Skip past spaces, tabs, CRs and LFs until run out or meet something else */
	for (counter = offset;
	     counter < end &&
	      ((tempchar = tvb_get_guint8(tvb,counter)) == ' ' ||
	      tempchar == '\t' || tempchar == '\r' || tempchar == '\n');
	     counter++);

	return (counter);
}

gint tvb_skip_wsp_return(tvbuff_t* tvb, gint offset){
	gint counter = offset;
	gint end;
	guint8 tempchar;
	end = 0;

	for(counter = offset; counter > end &&
		((tempchar = tvb_get_guint8(tvb,counter)) == ' ' ||
		tempchar == '\t' || tempchar == '\n' || tempchar == '\r'); counter--);
	counter++;
	return (counter);
}


/*
 * Format a bunch of data from a tvbuff as bytes, returning a pointer
 * to the string with the formatted data, with "punct" as a byte
 * separator.
 */
gchar *
tvb_bytes_to_str_punct(tvbuff_t *tvb, gint offset, gint len, gchar punct)
{
	return bytes_to_str_punct(tvb_get_ptr(tvb, offset, len), len, punct);
}

/*
 * Format a bunch of data from a tvbuff as bytes, returning a pointer
 * to the string with the formatted data.
 */
gchar *
tvb_bytes_to_str(tvbuff_t *tvb, gint offset, gint len)
{
	return bytes_to_str(tvb_get_ptr(tvb, offset, len), len);
}

/* Find a needle tvbuff within a haystack tvbuff. */
gint
tvb_find_tvb(tvbuff_t *haystack_tvb, tvbuff_t *needle_tvb, gint haystack_offset)
{
	guint		haystack_abs_offset, haystack_abs_length;
	const guint8	*haystack_data;
	const guint8	*needle_data;
	const guint 	needle_len = needle_tvb->length;
	const guint8	*location;

	if (haystack_tvb->length < 1 || needle_tvb->length < 1) {
		return -1;
	}

	/* Get pointers to the tvbuffs' data. */
	haystack_data = tvb_get_ptr(haystack_tvb, 0, -1);
	needle_data = tvb_get_ptr(needle_tvb, 0, -1);

	check_offset_length(haystack_tvb, haystack_offset, -1,
			&haystack_abs_offset, &haystack_abs_length);

	location = epan_memmem(haystack_data + haystack_abs_offset, haystack_abs_length,
			needle_data, needle_len);

	if (location) {
		return (gint) (location - haystack_data);
	}
	else {
		return -1;
	}

	return -1;
}

#ifdef HAVE_LIBZ
/*
 * Uncompresses a zlib compressed packet inside a message of tvb at offset with
 * length comprlen.  Returns an uncompressed tvbuffer if uncompression
 * succeeded or NULL if uncompression failed.
 */
#define TVB_Z_MIN_BUFSIZ 32768
#define TVB_Z_MAX_BUFSIZ 1048576 * 10
/* #define TVB_Z_DEBUG 1 */
#undef TVB_Z_DEBUG

tvbuff_t *
tvb_uncompress(tvbuff_t *tvb, int offset, int comprlen)
{


	gint err = Z_OK;
	guint bytes_out = 0;
	guint8 *compr = NULL;
	guint8 *uncompr = NULL;
	tvbuff_t *uncompr_tvb = NULL;
	z_streamp strm = NULL;
	Bytef *strmbuf = NULL;
	guint inits_done = 0;
	gint wbits = MAX_WBITS;
	guint8 *next = NULL;
	guint bufsiz = TVB_Z_MIN_BUFSIZ;
#ifdef TVB_Z_DEBUG
	guint inflate_passes = 0;
	guint bytes_in = tvb_length_remaining(tvb, offset);
#endif

	if (tvb == NULL) {
		return NULL;
	}

	strm = g_malloc0(sizeof(z_stream));

	if (strm == NULL) {
		return NULL;
	}

	compr = tvb_memdup(tvb, offset, comprlen);					// BUG_97AB29AD(1) FIX_97AB29AD(1) #CWE-126 #Pointer "compr" points to a buffer allocated on the head and of size "comprlen"

	if (!compr) {
		g_free(strm);
		return NULL;
	}

	/*
	 * Assume that the uncompressed data is at least twice as big as
	 * the compressed size.
	 */
	bufsiz = tvb_length_remaining(tvb, offset) * 2;

	if (bufsiz < TVB_Z_MIN_BUFSIZ) {
		bufsiz = TVB_Z_MIN_BUFSIZ;
	} else if (bufsiz > TVB_Z_MAX_BUFSIZ) {
		bufsiz = TVB_Z_MIN_BUFSIZ;
	}

#ifdef TVB_Z_DEBUG
	printf("bufsiz: %u bytes\n", bufsiz);
#endif

	next = compr;

	strm->next_in = next;
	strm->avail_in = comprlen;


	strmbuf = g_malloc0(bufsiz);

	if(strmbuf == NULL) {
		g_free(compr);
		g_free(strm);
		return NULL;
	}

	strm->next_out = strmbuf;
	strm->avail_out = bufsiz;

	err = inflateInit2(strm, wbits);
	inits_done = 1;
	if (err != Z_OK) {
		inflateEnd(strm);
		g_free(strm);
		g_free(compr);
		g_free(strmbuf);
		return NULL;
	}

	while (1) {
		memset(strmbuf, '\0', bufsiz);
		strm->next_out = strmbuf;
		strm->avail_out = bufsiz;

		err = inflate(strm, Z_SYNC_FLUSH);

		if (err == Z_OK || err == Z_STREAM_END) {
			guint bytes_pass = bufsiz - strm->avail_out;

#ifdef TVB_Z_DEBUG
			++inflate_passes;
#endif

			if (uncompr == NULL) {
				uncompr = g_memdup(strmbuf, bytes_pass);
			} else {
				guint8 *new_data = g_malloc0(bytes_out +
				    bytes_pass);

				if (new_data == NULL) {
					inflateEnd(strm);
					g_free(strm);
					g_free(strmbuf);
					g_free(compr);

					if (uncompr != NULL) {
						g_free(uncompr);
					}

					return NULL;
				}

				g_memmove(new_data, uncompr, bytes_out);
				g_memmove((new_data + bytes_out), strmbuf,
				    bytes_pass);

				g_free(uncompr);
				uncompr = new_data;
			}

			bytes_out += bytes_pass;

			if ( err == Z_STREAM_END) {
				inflateEnd(strm);
				g_free(strm);
				g_free(strmbuf);
				break;
			}
		} else if (err == Z_BUF_ERROR) {
			/*
			 * It's possible that not enough frames were captured
			 * to decompress this fully, so return what we've done
			 * so far, if any.
			 */
			inflateEnd(strm);
			g_free(strm);
			g_free(strmbuf);

			if (uncompr != NULL) {
				break;
			} else {
				g_free(compr);
				return NULL;
			}

		} else if (err == Z_DATA_ERROR && inits_done == 1
		    && uncompr == NULL && (*compr  == 0x1f) &&
		    (*(compr + 1) == 0x8b)) {
			/*
			 * inflate() is supposed to handle both gzip and deflate
			 * streams automatically, but in reality it doesn't
			 * seem to handle either (at least not within the
			 * context of an HTTP response.)  We have to try
			 * several tweaks, depending on the type of data and
			 * version of the library installed.
			 */

			/*
			 * Gzip file format.  Skip past the header, since the
			 * fix to make it work (setting windowBits to 31)
			 * doesn't work with all versions of the library.
			 */
			Bytef *c = compr + 2;						// BUG_97AB29AD(2) FIX_97AB29AD(2) #CWE-126 #Pointer "c" points within buffer "compr"
			Bytef flags = 0;

			if (*c == Z_DEFLATED) {
				c++;
			} else {
				inflateEnd(strm);
				g_free(strm);
				g_free(compr);
				g_free(strmbuf);
				return NULL;
			}

			flags = *c;

			/* Skip past the MTIME, XFL, and OS fields. */
			c += 7;

			if (flags & (1 << 2)) {
				/* An Extra field is present. */
				gint xsize = (gint)(*c |
				    (*(c + 1) << 8));

				c += xsize;
			}

			if (flags & (1 << 3)) {
				/* A null terminated filename */

				while ((c - compr) < comprlen && *c != '\0') {		// FIX_97AB29AD(4) #CWE-126 #Check if pointer "c" is still within the bounds of buffer "compr" before reading from it
					c++;
				}

				c++;
			}

			if (flags & (1 << 4)) {
				/* A null terminated comment */

				while ((c - compr) < comprlen && *c != '\0') {		// FIX_97AB29AD(2) #CWE-126 #Alternative location where a bound check was added to prevent a buffer overread
					c++;
				}

				c++;
			}


			inflateReset(strm);
			next = c;
			strm->next_in = next;
			if (c - compr > comprlen) {
				inflateEnd(strm);
				g_free(strm);
				g_free(compr);
				g_free(strmbuf);
				return NULL;
			}
			comprlen -= (int) (c - compr);

			inflateEnd(strm);
			err = inflateInit2(strm, wbits);
			inits_done++;
		} else if (err == Z_DATA_ERROR && uncompr == NULL &&
		    inits_done <= 3) {

			/*
			 * Re-init the stream with a negative
			 * MAX_WBITS. This is necessary due to
			 * some servers (Apache) not sending
			 * the deflate header with the
			 * content-encoded response.
			 */
			wbits = -MAX_WBITS;

			inflateReset(strm);

			strm->next_in = next;
			strm->avail_in = comprlen;

			inflateEnd(strm);
			memset(strmbuf, '\0', bufsiz);
			strm->next_out = strmbuf;
			strm->avail_out = bufsiz;

			err = inflateInit2(strm, wbits);

			inits_done++;

			if (err != Z_OK) {
				g_free(strm);
				g_free(strmbuf);
				g_free(compr);
				g_free(uncompr);

				return NULL;
			}
		} else {
			inflateEnd(strm);
			g_free(strm);
			g_free(strmbuf);

			if (uncompr == NULL) {
				g_free(compr); // FIX_8CA9F2B4(1) #CWE-415 #Free "compr" on this path only if "uncompr" == NULL
				return NULL;
			}

			break;
		}
	}

#ifdef TVB_Z_DEBUG
	printf("inflate() total passes: %u\n", inflate_passes);
	printf("bytes  in: %u\nbytes out: %u\n\n", bytes_in, bytes_out);
#endif

	if (uncompr != NULL) {
		uncompr_tvb =  tvb_new_real_data((guint8*) uncompr, bytes_out,
		    bytes_out);
		tvb_set_free_cb(uncompr_tvb, g_free);
	}
	g_free(compr); // BUG_8CA9F2B4(2) FIX_8CA9F2B4(2) #CWE-415 #Free "compr" on this path
	return uncompr_tvb;
}
#else
tvbuff_t *
tvb_uncompress(tvbuff_t *tvb _U_, int offset _U_, int comprlen _U_)
{
	return NULL;
}
#endif

tvbuff_t* tvb_child_uncompress(tvbuff_t *parent _U_, tvbuff_t *tvb, int offset, int comprlen)
{
	tvbuff_t *new_tvb = tvb_uncompress(tvb, offset, comprlen); //BUG_97AB29AD(3) #CWE-119 #Sink: Invalid read of size 1 around 'comprlen'
	if (new_tvb)
		tvb_set_child_real_data_tvbuff (parent, new_tvb);
	return new_tvb;
}


