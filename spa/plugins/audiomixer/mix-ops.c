/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "mix-ops.h"

static void
clear_s16(void *dst, int n_bytes)
{
	memset(dst, 0, n_bytes);
}

static void
clear_f32(void *dst, int n_bytes)
{
	memset(dst, 0, n_bytes);
}

static void
copy_s16(void *dst, const void *src, int n_bytes)
{
	memcpy(dst, src, n_bytes);
}

static void
copy_f32(void *dst, const void *src, int n_bytes)
{
	memcpy(dst, src, n_bytes);
}

static void
add_s16(void *dst, const void *src, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;
	int32_t t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = *d + *s;
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d++;
		s++;
	}
}

static void
add_f32(void *dst, const void *src, int n_bytes)
{
	const float *s = src;
	float *d = dst;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d += *s;
		d++;
		s++;
	}
}

static void
copy_scale_s16(void *dst, const void *src, const double scale, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;;
	int32_t v = scale * (1 << 11), t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = (*s * v) >> 11;
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d++;
		s++;
	}
}

static void
copy_scale_f32(void *dst, const void *src, const double scale, int n_bytes)
{
	const float *s = src;
	float *d = dst;
	float v = scale;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d = *s * v;
		d++;
		s++;
	}
}

static void
add_scale_s16(void *dst, const void *src, const double scale, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;
	int32_t v = scale * (1 << 11), t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = *d + ((*s * v) >> 11);
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d++;
		s++;
	}
}

static void
add_scale_f32(void *dst, const void *src, const double scale, int n_bytes)
{
	const float *s = src;
	float *d = dst;
	float v = scale;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d += *s * v;
		d++;
		s++;
	}
}

static void
copy_s16_i(void *dst, int dst_stride, const void *src, int src_stride, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		*d = *s;
		d += dst_stride;
		s += src_stride;
	}
}

static void
copy_f32_i(void *dst, int dst_stride, const void *src, int src_stride, int n_bytes)
{
	const float *s = src;
	float *d = dst;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d = *s;
		d += dst_stride;
		s += src_stride;
	}
}

static void
add_s16_i(void *dst, int dst_stride, const void *src, int src_stride, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;
	int32_t t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = *d + *s;
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d += dst_stride;
		s += src_stride;
	}
}

static void
add_f32_i(void *dst, int dst_stride, const void *src, int src_stride, int n_bytes)
{
	const float *s = src;
	float *d = dst;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d += *s;
		d += dst_stride;
		s += src_stride;
	}
}

static void
copy_scale_s16_i(void *dst, int dst_stride, const void *src, int src_stride, const double scale, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;
	int32_t v = scale * (1 << 11), t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = (*s * v) >> 11;
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d += dst_stride;
		s += src_stride;
	}
}

static void
copy_scale_f32_i(void *dst, int dst_stride, const void *src, int src_stride, const double scale, int n_bytes)
{
	const float *s = src;
	float *d = dst;
	float v = scale;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d = *s * v;
		d += dst_stride;
		s += src_stride;
	}
}

static void
add_scale_s16_i(void *dst, int dst_stride, const void *src, int src_stride, const double scale, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;
	int32_t v = scale * (1 << 11), t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = *d + ((*s * v) >> 11);
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d += dst_stride;
		s += src_stride;
	}
}

static void
add_scale_f32_i(void *dst, int dst_stride, const void *src, int src_stride, const double scale, int n_bytes)
{
	const float *s = src;
	float *d = dst;
	float v = scale;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d += *s * v;
		d += dst_stride;
		s += src_stride;
	}
}

void spa_audiomixer_get_ops(struct spa_audiomixer_ops *ops)
{
	ops->clear[FMT_S16] = clear_s16;
	ops->clear[FMT_F32] = clear_f32;
	ops->copy[FMT_S16] = copy_s16;
	ops->copy[FMT_F32] = copy_f32;
        ops->add[FMT_S16] = add_s16;
        ops->add[FMT_F32] = add_f32;
        ops->copy_scale[FMT_S16] = copy_scale_s16;
        ops->copy_scale[FMT_F32] = copy_scale_f32;
        ops->add_scale[FMT_S16] = add_scale_s16;
        ops->add_scale[FMT_F32] = add_scale_f32;
        ops->copy_i[FMT_S16] = copy_s16_i;
        ops->copy_i[FMT_F32] = copy_f32_i;
        ops->add_i[FMT_S16] = add_s16_i;
        ops->add_i[FMT_F32] = add_f32_i;
        ops->copy_scale_i[FMT_S16] = copy_scale_s16_i;
        ops->copy_scale_i[FMT_F32] = copy_scale_f32_i;
        ops->add_scale_i[FMT_S16] = add_scale_s16_i;
        ops->add_scale_i[FMT_F32] = add_scale_f32_i;
}
