/* save as jpeg2000
 *
 * 18/3/20
 * 	- from jp2kload.c
 */

/*

    This file is part of VIPS.
    
    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/*
#define DEBUG_VERBOSE
#define DEBUG
 */

/* TODO
 *
 * - could support tiff-like depth parameter
 *
 * - could support png-like bitdepth parameter
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vips/vips.h>
#include <vips/internal.h>

#ifdef HAVE_LIBOPENJP2

#include <openjpeg.h>

#include "pforeign.h"

/* Surely enough ... does anyone do multispectral imaging with jp2k?
 */
#define MAX_BANDS (100)

typedef struct _VipsForeignSaveJp2k {
	VipsForeignSave parent_object;

	/* Where to write (set by subclasses).
	 */
	VipsTarget *target;

	int tile_width;
	int tile_height;

	/* Lossless mode.
	 */
	gboolean lossless;

	/* Quality factor.
	 */
	int Q;

	/* Chroma subsample mode.
	 */
	VipsForeignSubsample subsample_mode;

	/* Encoder state.
	 */
	opj_stream_t *stream;
	opj_codec_t *codec;
	opj_cparameters_t parameters;
	opj_image_t *image;

	/* The line of tiles we are building, and the buffer we
	 * unpack to for output.
	 */
	VipsRegion *strip;
	VipsPel *tile_buffer;

	/* If we need to subsample during unpacking.
	 */
	gboolean subsample;

	/* If we converto RGB to YCC during save.
	 */
	gboolean save_as_ycc;

	/* Accumulate a line of sums here during chroma subsample.
	 */
	VipsPel *accumulate;
} VipsForeignSaveJp2k;

typedef VipsForeignSaveClass VipsForeignSaveJp2kClass;

G_DEFINE_ABSTRACT_TYPE( VipsForeignSaveJp2k, vips_foreign_save_jp2k, 
	VIPS_TYPE_FOREIGN_SAVE );

static void
vips_foreign_save_jp2k_dispose( GObject *gobject )
{
	VipsForeignSaveJp2k *jp2k = (VipsForeignSaveJp2k *) gobject;

	VIPS_FREEF( opj_destroy_codec, jp2k->codec );
	VIPS_FREEF( opj_stream_destroy, jp2k->stream );
	VIPS_FREEF( opj_image_destroy, jp2k->image );

	VIPS_UNREF( jp2k->target );
	VIPS_UNREF( jp2k->strip );

	VIPS_FREE( jp2k->tile_buffer );
	VIPS_FREE( jp2k->accumulate );

	G_OBJECT_CLASS( vips_foreign_save_jp2k_parent_class )->
		dispose( gobject );
}

static OPJ_SIZE_T
vips_foreign_save_jp2k_write_target( void *buffer, size_t length, void *client )
{
	VipsTarget *target = VIPS_TARGET( client );

	if( vips_target_write( target, buffer, length ) )
		return( 0 );

	return( length );
}

/* Make a libopenjp2 output stream that wraps a VipsTarget.
 */
static opj_stream_t *
vips_foreign_save_jp2k_target( VipsTarget *target )
{
	opj_stream_t *stream;

	/* FALSE means a write stream.
	 */
	if( !(stream = opj_stream_create( OPJ_J2K_STREAM_CHUNK_SIZE, FALSE )) ) 
		return( NULL );

	opj_stream_set_user_data( stream, target, NULL );
	opj_stream_set_write_function( stream, 
		vips_foreign_save_jp2k_write_target );

	return( stream );
}

static void 
vips_foreign_save_jp2k_error_callback( const char *msg, void *client )
{
	vips_error( "jp2ksave", "%s", msg ); 
}

/* The openjpeg info and warning callbacks are incredibly chatty.
 */
static void 
vips_foreign_save_jp2k_warning_callback( const char *msg, void *client )
{
#ifdef DEBUG
	g_warning( "jp2ksave: %s",  class->nickname, msg );
#endif /*DEBUG*/
}

/* The openjpeg info and warning callbacks are incredibly chatty.
 */
static void 
vips_foreign_save_jp2k_info_callback( const char *msg, void *client )
{
#ifdef DEBUG
	g_info( "jp2ksave: %s",  class->nickname, msg );
#endif /*DEBUG*/
}

static void
vips_foreign_save_jp2k_attach_handlers( opj_codec_t *codec )
{
	opj_set_info_handler( codec,
		vips_foreign_save_jp2k_info_callback, NULL );
	opj_set_warning_handler( codec, 
		vips_foreign_save_jp2k_warning_callback, NULL );
	opj_set_error_handler( codec, 
		vips_foreign_save_jp2k_error_callback, NULL );
}

/* See also https://en.wikipedia.org/wiki/YCbCr#JPEG_conversion
 */
#define RGB_TO_YCC( TYPE ) { \
	TYPE *tq = (TYPE *) q; \
	\
	for( x = 0; x < tile->width; x++ ) { \
		int r = tq[0]; \
		int g = tq[1]; \
		int b = tq[2]; \
		\
		int y, cb, cr; \
		\
		y = 0.299 * r + 0.587 * g + 0.114 * b; \
		tq[0] = VIPS_CLIP( 0, y, upb ); \
		\
		cb = offset - (int)(0.168736 * r + 0.331264 * g - 0.5 * b); \
		tq[1] = VIPS_CLIP( 0, cb, upb ); \
		\
		cr = offset - (int)(-0.5 * r + 0.418688 * g + 0.081312 * b); \
		tq[2] = VIPS_CLIP( 0, cr, upb ); \
		\
		tq += 3; \
	} \
}

/* In-place RGB->YCC for a line of pels.
 */
static void
vips_foreign_save_jp2k_rgb_to_ycc( VipsRegion *region, 
	VipsRect *tile, int prec ) 
{
	VipsImage *im = region->im;
	int offset = 1 << (prec - 1);
	int upb = (1 << prec) - 1;

	int x, y;

	g_assert( im->Bands == 3 );

	for( y = 0; y < tile->height; y++ ) {
		VipsPel *q = VIPS_REGION_ADDR( region, 
			tile->left, tile->top + y );

		switch( im->BandFmt ) {
		case VIPS_FORMAT_CHAR:
		case VIPS_FORMAT_UCHAR:
			RGB_TO_YCC( unsigned char );
			break;

		case VIPS_FORMAT_SHORT:
		case VIPS_FORMAT_USHORT:
			RGB_TO_YCC( unsigned short );
			break;

		case VIPS_FORMAT_INT:
		case VIPS_FORMAT_UINT:
			RGB_TO_YCC( unsigned int );
			break;

		default:
			g_assert_not_reached();
			break;
		}
	}
}

/* Shrink in three stages:
 *   1. copy the first line of input pels to acc
 *   2. add subsequent lines in comp.dy.
 *   3. horizontal average to output line
 */
#define SHRINK( ACC_TYPE, PIXEL_TYPE ) { \
	ACC_TYPE *acc = (ACC_TYPE *) accumulate; \
	PIXEL_TYPE *tq = (PIXEL_TYPE *) q; \
	const int n_pels = comp->dx * comp->dy; \
	\
	PIXEL_TYPE *tp; \
	ACC_TYPE *ap; \
	\
	tp = (PIXEL_TYPE *) p; \
	for( x = 0; x < tile->width; x++ ) { \
		acc[x] = *tp; \
		tp += n_bands; \
	} \
	\
	for( z = 1; z < comp->dy; z++ ) { \
		tp = (PIXEL_TYPE *) (p + z * lskip); \
		for( x = 0; x < tile->width; x++ ) { \
			acc[x] += *tp; \
			tp += n_bands; \
		} \
	} \
	\
	ap = acc; \
	for( x = 0; x < output_width; x++ ) { \
		ACC_TYPE sum; \
		\
		sum = 0; \
		for( z = 0; z < comp->dx; z++ ) \
			sum += ap[z]; \
		\
		tq[x] = (sum + n_pels / 2) / n_pels; \
		ap += comp->dx; \
	} \
}

static void
vips_foreign_save_jp2k_unpack_subsample( VipsRegion *region, VipsRect *tile,
	opj_image_t *image, VipsPel *tile_buffer, VipsPel *accumulate )
{
	VipsImage *im = region->im;
	size_t sizeof_element = VIPS_REGION_SIZEOF_ELEMENT( region );
	size_t lskip = VIPS_REGION_LSKIP( region );
	int n_bands = im->Bands;

	VipsPel *q;
	int x, y, z, i;

	q = tile_buffer;
	for( i = 0; i < n_bands; i++ ) {
		opj_image_comp_t *comp = &image->comps[i];

		/* The number of pixels we write for this component. Round to
		 * nearest, and we may have to write half-pixels at the edges.
		 */
		int output_width = VIPS_ROUND_UINT( 
			(double) tile->width / comp->dx );
		int output_height = VIPS_ROUND_UINT( 
			(double) tile->height / comp->dy );;

		for( y = 0; y < output_height; y++ ) {
			VipsPel *p = i * sizeof_element + 
				VIPS_REGION_ADDR( region, 
					tile->left, tile->top + y * comp->dy );

			/* Shrink a line of pels to q.
			 */
			switch( im->BandFmt ) {
			case VIPS_FORMAT_CHAR:
				SHRINK( int, signed char );
				break;

			case VIPS_FORMAT_UCHAR:
				SHRINK( int, unsigned char );
				break;

			case VIPS_FORMAT_SHORT:
				SHRINK( int, signed short );
				break;

			case VIPS_FORMAT_USHORT:
				SHRINK( int, unsigned short );
				break;

			case VIPS_FORMAT_INT:
				SHRINK( gint64, signed int );
				break;

			case VIPS_FORMAT_UINT:
				SHRINK( gint64, unsigned int );
				break;

			default:
				g_assert_not_reached();
				break;
			}

			q += sizeof_element * output_width;
		}
	}
}

#define UNPACK( TYPE ) { \
	TYPE **tplanes = (TYPE **) planes; \
	TYPE *tp = (TYPE *) p; \
	\
	for( i = 0; i < b; i++ ) { \
		TYPE *q = tplanes[i]; \
		TYPE *tp1 = tp + i; \
		\
		for( x = 0; x < tile->width; x++ ) { \
			q[x] = *tp1; \
			tp1 += b; \
		} \
		\
		tplanes[i] += tile->width; \
	} \
}

static void
vips_foreign_save_jp2k_unpack( VipsRegion *region, VipsRect *tile,
	opj_image_t *image, VipsPel *tile_buffer )
{
	VipsImage *im = region->im;
	size_t sizeof_element = VIPS_REGION_SIZEOF_ELEMENT( region );
	int b = im->Bands;

	VipsPel *planes[MAX_BANDS];
	int x, y, i;

	for( i = 0; i < b; i++ )
		planes[i] = tile_buffer +
			i * sizeof_element * tile->width * tile->height;

	for( y = 0; y < tile->height; y++ ) {
		VipsPel *p = VIPS_REGION_ADDR( region, 
			tile->left, tile->top + y );

		switch( im->BandFmt ) {
		case VIPS_FORMAT_CHAR:
		case VIPS_FORMAT_UCHAR:
			UNPACK( unsigned char );
			break;

		case VIPS_FORMAT_SHORT:
		case VIPS_FORMAT_USHORT:
			UNPACK( unsigned short );
			break;

		case VIPS_FORMAT_INT:
		case VIPS_FORMAT_UINT:
			UNPACK( unsigned int );
			break;

		default:
			g_assert_not_reached();
			break;
		}
	}
}

static size_t
vips_foreign_save_jp2k_sizeof_tile( VipsForeignSaveJp2k *jp2k, VipsRect *tile )
{
	VipsForeignSave *save = (VipsForeignSave *) jp2k;
	size_t sizeof_element = VIPS_IMAGE_SIZEOF_ELEMENT( save->ready );

	size_t size;
	int i;

	size = 0;
	for( i = 0; i < jp2k->image->numcomps; i++ ) {
		opj_image_comp_t *comp = &jp2k->image->comps[i];

		/* The number of pixels we write for this component. Round to
		 * nearest, and we may have to write half-pixels at the edges.
		 */
		int output_width = VIPS_ROUND_UINT( 
			(double) tile->width / comp->dx );
		int output_height = VIPS_ROUND_UINT( 
			(double) tile->height / comp->dy );;

		size += output_width * output_height * sizeof_element;
	}

	return( size );
}

static int
vips_foreign_save_jp2k_write_tiles( VipsForeignSaveJp2k *jp2k )
{
	VipsForeignSave *save = (VipsForeignSave *) jp2k;
	VipsImage *im = save->ready;
	int tiles_across = VIPS_ROUND_UP( im->Xsize, jp2k->tile_width ) /
		jp2k->tile_width;

	int x;

	for( x = 0; x < im->Xsize; x += jp2k->tile_width ) {
		VipsRect tile;
		size_t sizeof_tile;
		int tile_index;

		tile.left = x;
		tile.top = jp2k->strip->valid.top;
		tile.width = jp2k->tile_width;
		tile.height = jp2k->tile_height;
		vips_rect_intersectrect( &tile, &jp2k->strip->valid, &tile );

		if( jp2k->save_as_ycc ) 
			vips_foreign_save_jp2k_rgb_to_ycc( jp2k->strip, 
				&tile, jp2k->image->comps[0].prec ); 

		if( jp2k->subsample )
			vips_foreign_save_jp2k_unpack_subsample( jp2k->strip, 
				&tile, jp2k->image, 
				jp2k->tile_buffer, jp2k->accumulate ); 
		else
			vips_foreign_save_jp2k_unpack( jp2k->strip, 
				&tile, jp2k->image, 
				jp2k->tile_buffer );

		sizeof_tile = 
			vips_foreign_save_jp2k_sizeof_tile( jp2k, &tile );
		tile_index = tiles_across * tile.top / jp2k->tile_height +
			x / jp2k->tile_width;
		if( !opj_write_tile( jp2k->codec, tile_index, 
			(VipsPel *) jp2k->tile_buffer, sizeof_tile, 
			jp2k->stream ) )
			return( -1 );
	}

	return( 0 );
}

static int
vips_foreign_save_jp2k_write_block( VipsRegion *region, VipsRect *area, 
	void *a )
{
	VipsForeignSaveJp2k *jp2k = (VipsForeignSaveJp2k *) a;
	VipsForeignSave *save = (VipsForeignSave *) jp2k;

#ifdef DEBUG_VERBOSE
	printf( "vips_foreign_save_jp2k_write_block: y = %d, nlines = %d\n", 
		area->top, area->height );
#endif /*DEBUG_VERBOSE*/

	for(;;) {
		VipsRect hit;
		int y;
		VipsRect strip_position;

		/* The intersection with the strip is the fresh pixels we
		 * have. 
		 */
		vips_rect_intersectrect( area, &(jp2k->strip->valid), &hit );

		/* Copy the new pixels into the strip.
		 */
		for( y = 0; y < hit.height; y++ ) {
			VipsPel *p = VIPS_REGION_ADDR( region, 
				0, hit.top + y );
			VipsPel *q = VIPS_REGION_ADDR( jp2k->strip, 
				0, hit.top + y );

			memcpy( q, p, VIPS_IMAGE_SIZEOF_LINE( region->im ) );
		}

		/* Have we failed to reach the bottom of the strip? We must
		 * have run out of fresh pixels, so we are done.
		 */
		if( VIPS_RECT_BOTTOM( &hit ) != 
			VIPS_RECT_BOTTOM( &jp2k->strip->valid ) ) 
			break;

		/* We have reached the bottom of the strip. Write this line of
		 * pixels and move the strip down.
		 */
		if( vips_foreign_save_jp2k_write_tiles( jp2k ) )
			return( -1 );

		strip_position.left = 0;
		strip_position.top = jp2k->strip->valid.top + jp2k->tile_height;
		strip_position.width = save->ready->Xsize;
		strip_position.height = jp2k->tile_height;
		if( vips_region_buffer( jp2k->strip, &strip_position ) )
			return( -1 );
	}

	return( 0 );
}

static opj_image_t *
vips_foreign_save_jp2k_new_image( VipsImage *im, 
	int width, int height, gboolean subsample, gboolean save_as_ycc )
{
	OPJ_COLOR_SPACE color_space;
	int expected_bands;
	int bits_per_pixel;
	opj_image_cmptparm_t comps[MAX_BANDS];
	opj_image_t *image;
	int i;

	if( im->Bands > MAX_BANDS )
		return( NULL );

	/* CIELAB etc. do not seem to be well documented.
	 */
	switch( im->Type ) {
	case VIPS_INTERPRETATION_B_W:
	case VIPS_INTERPRETATION_GREY16:
		color_space = OPJ_CLRSPC_GRAY;
		expected_bands = 1;
		break;

	case VIPS_INTERPRETATION_sRGB:
	case VIPS_INTERPRETATION_RGB16:
		color_space = save_as_ycc ? OPJ_CLRSPC_SYCC : OPJ_CLRSPC_SRGB;
		expected_bands = 3;
		break;

	case VIPS_INTERPRETATION_CMYK:
		color_space = OPJ_CLRSPC_CMYK;
		expected_bands = 4;
		break;

	default:
		color_space = OPJ_CLRSPC_UNSPECIFIED;
		expected_bands = im->Bands;
		break;
	}

	switch( im->BandFmt ) {
	case VIPS_FORMAT_CHAR:
	case VIPS_FORMAT_UCHAR:
		bits_per_pixel = 8;
		break;

	case VIPS_FORMAT_SHORT:
	case VIPS_FORMAT_USHORT:
		bits_per_pixel = 16;
		break;

	case VIPS_FORMAT_INT:
	case VIPS_FORMAT_UINT:
		/* OpenJPEG only supports up to 31.
		 */
		bits_per_pixel = 31;
		break;

	default:
		g_assert_not_reached();
		break;
	}

	for( i = 0; i < im->Bands; i++ ) {
		comps[i].dx = (subsample && i > 0) ? 2 : 1;
		comps[i].dy = (subsample && i > 0) ? 2 : 1;
		comps[i].w = width;
		comps[i].h = height;
		comps[i].x0 = 0;
		comps[i].y0 = 0;
		comps[i].prec = bits_per_pixel;
		comps[i].bpp = bits_per_pixel;
		comps[i].sgnd = !vips_band_format_isuint( im->BandFmt );
	}

	image = opj_image_create( im->Bands, comps, color_space );
	image->x1 = width;
	image->y1 = height;

	/* Tag alpha channels.
	 */
	for( i = 0; i < im->Bands; i++ )
		image->comps[i].alpha = i >= expected_bands;

	return( image );
}

static int
vips_foreign_save_jp2k_build( VipsObject *object )
{
	VipsObjectClass *class = VIPS_OBJECT_GET_CLASS( object );
	VipsForeignSave *save = (VipsForeignSave *) object;
	VipsForeignSaveJp2k *jp2k = (VipsForeignSaveJp2k *) object;

	size_t sizeof_tile;
	size_t sizeof_line;
	VipsRect strip_position;

	if( VIPS_OBJECT_CLASS( vips_foreign_save_jp2k_parent_class )->
		build( object ) )
		return( -1 );

	/* Analyze our arguments.
	 */

	if( !vips_band_format_isint( save->ready->BandFmt ) ) {
		vips_error( class->nickname,
			"%s", _( "not an integer format" ) );
		return( -1 );
	}

	switch( jp2k->subsample_mode ) {
	case VIPS_FOREIGN_SUBSAMPLE_AUTO:
		jp2k->subsample =
			!jp2k->lossless &&
			jp2k->Q < 90 &&
			save->ready->Xsize % 2 == 0 &&
			save->ready->Ysize % 2 == 0 &&
			(save->ready->Type == VIPS_INTERPRETATION_sRGB ||
			 save->ready->Type == VIPS_INTERPRETATION_RGB16) &&
			save->ready->Bands == 3;
		break;

	case VIPS_FOREIGN_SUBSAMPLE_ON:
		jp2k->subsample = TRUE;
		break;

	case VIPS_FOREIGN_SUBSAMPLE_OFF:
		jp2k->subsample = FALSE;
		break;

	default:
		g_assert_not_reached();
		break;
	}

	if( jp2k->subsample ) 
		jp2k->save_as_ycc = TRUE;

	/* Set parameters for compressor.
	 */ 
	opj_set_default_encoder_parameters( &jp2k->parameters );

	/* Always tile.
	 */
	jp2k->parameters.tile_size_on = OPJ_TRUE;
	jp2k->parameters.cp_tdx = jp2k->tile_width;
	jp2k->parameters.cp_tdy = jp2k->tile_height;

	/* Number of layers to write. Smallest layer is c. 2^5 on the smallest
	 * axis.
	 */
	jp2k->parameters.numresolution = VIPS_MAX( 1, 
		log( VIPS_MIN( save->ready->Xsize, save->ready->Ysize ) ) / 
		log( 2 ) - 4 );
#ifdef DEBUG
	printf( "vips_foreign_save_jp2k_build: numresolutions = %d\n", 
		jp2k->parameters.numresolution );
#endif /*DEBUG*/

	/* Makes three band images smaller, somehow.
	 */
	jp2k->parameters.tcp_mct = 
		(save->ready->Bands >= 3 && !jp2k->subsample) ? 1 : 0;

	/* Lossy mode.
	 */
	if( !jp2k->lossless ) {
		jp2k->parameters.irreversible = TRUE;

		/* Map Q to allowed distortion.
		 */
		jp2k->parameters.cp_disto_alloc = 1;
		jp2k->parameters.cp_fixed_quality = TRUE;
		jp2k->parameters.tcp_distoratio[0] = jp2k->Q;
		jp2k->parameters.tcp_numlayers = 1;
	}

	/* Set up compressor.
	 */

	jp2k->codec = opj_create_compress( OPJ_CODEC_J2K );
	vips_foreign_save_jp2k_attach_handlers( jp2k->codec );
	if( !(jp2k->image = vips_foreign_save_jp2k_new_image( save->ready,
		save->ready->Xsize, save->ready->Ysize, 
		jp2k->subsample, jp2k->save_as_ycc )) )
		return( -1 );
        if( !opj_setup_encoder( jp2k->codec, 
		&jp2k->parameters, jp2k->image ) ) 
		return( -1 );

#ifdef HAVE_LIBOPENJP2_THREADING
	/* Use eg. VIPS_CONCURRENCY etc. to set n-cpus, if this openjpeg has
	 * stable support. 
	 */
	opj_codec_set_threads( jp2k->codec, vips_concurrency_get() );
#endif /*HAVE_LIBOPENJP2_THREADING*/

	if( !(jp2k->stream = vips_foreign_save_jp2k_target( jp2k->target )) )
		return( -1 );

	if( !opj_start_compress( jp2k->codec, jp2k->image, jp2k->stream ) )
		return( -1 );

	/* The buffer we repack tiles to for write. Large enough for one
	 * complete tile.
	 */
	sizeof_tile = VIPS_IMAGE_SIZEOF_PEL( save->ready ) *
		jp2k->tile_width * jp2k->tile_height;
	if( !(jp2k->tile_buffer = VIPS_ARRAY( NULL, sizeof_tile, VipsPel )) )
		return( -1 );

	/* We need a line of sums for chroma subsample. At worst, gint64.
	 */
	sizeof_line = sizeof( gint64 ) * jp2k->tile_width;
	if( !(jp2k->accumulate = VIPS_ARRAY( NULL, sizeof_line, VipsPel )) )
		return( -1 );

	/* The line of tiles we are building.
	 */
	jp2k->strip = vips_region_new( save->ready );

	/* Position strip at the top of the image, the height of a row of
	 * tiles.
	 */
	strip_position.left = 0;
	strip_position.top = 0;
	strip_position.width = save->ready->Xsize;
	strip_position.height = jp2k->tile_height;
	if( vips_region_buffer( jp2k->strip, &strip_position ) ) 
		return( -1 );

	/* Write data. 
	 */
	if( vips_sink_disc( save->ready,
		vips_foreign_save_jp2k_write_block, jp2k ) )
		return( -1 );

	opj_end_compress( jp2k->codec, jp2k->stream );

	vips_target_finish( jp2k->target );

	return( 0 );
}

static void
vips_foreign_save_jp2k_class_init( VipsForeignSaveJp2kClass *class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( class );
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsForeignClass *foreign_class = (VipsForeignClass *) class;
	VipsForeignSaveClass *save_class = (VipsForeignSaveClass *) class;

	gobject_class->dispose = vips_foreign_save_jp2k_dispose;
	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "jp2ksave_base";
	object_class->description = _( "save image in HEIF format" );
	object_class->build = vips_foreign_save_jp2k_build;

	foreign_class->suffs = vips__jp2k_suffs;

	save_class->saveable = VIPS_SAVEABLE_ANY;

	VIPS_ARG_INT( class, "tile_width", 11, 
		_( "Tile width" ), 
		_( "Tile width in pixels" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsForeignSaveJp2k, tile_width ),
		1, 32768, 512 );

	VIPS_ARG_INT( class, "tile_height", 12, 
		_( "Tile height" ), 
		_( "Tile height in pixels" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsForeignSaveJp2k, tile_height ),
		1, 32768, 512 );

	VIPS_ARG_BOOL( class, "lossless", 13, 
		_( "Lossless" ), 
		_( "Enable lossless compression" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsForeignSaveJp2k, lossless ),
		FALSE ); 

	VIPS_ARG_ENUM( class, "subsample_mode", 19,
		_( "Subsample mode" ),
		_( "Select chroma subsample operation mode" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsForeignSaveJp2k, subsample_mode ),
		VIPS_TYPE_FOREIGN_SUBSAMPLE,
		VIPS_FOREIGN_SUBSAMPLE_AUTO );

	VIPS_ARG_INT( class, "Q", 14, 
		_( "Q" ), 
		_( "Q factor" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsForeignSaveJp2k, Q ),
		1, 100, 45 );

}

static void
vips_foreign_save_jp2k_init( VipsForeignSaveJp2k *jp2k )
{
	jp2k->tile_width = 512;
	jp2k->tile_height = 512;

	/* 45 gives about the same filesize as default regular jpg.
	 */
	jp2k->Q = 45;

	jp2k->subsample_mode = VIPS_FOREIGN_SUBSAMPLE_AUTO;
}

typedef struct _VipsForeignSaveJp2kFile {
	VipsForeignSaveJp2k parent_object;

	/* Filename for save.
	 */
	char *filename; 

} VipsForeignSaveJp2kFile;

typedef VipsForeignSaveJp2kClass VipsForeignSaveJp2kFileClass;

G_DEFINE_TYPE( VipsForeignSaveJp2kFile, vips_foreign_save_jp2k_file, 
	vips_foreign_save_jp2k_get_type() );

static int
vips_foreign_save_jp2k_file_build( VipsObject *object )
{
	VipsForeignSaveJp2k *jp2k = (VipsForeignSaveJp2k *) object;
	VipsForeignSaveJp2kFile *file = (VipsForeignSaveJp2kFile *) object;

	if( !(jp2k->target = vips_target_new_to_file( file->filename )) )
		return( -1 );

	if( VIPS_OBJECT_CLASS( vips_foreign_save_jp2k_file_parent_class )->
		build( object ) )
		return( -1 );

	return( 0 );
}

static void
vips_foreign_save_jp2k_file_class_init( VipsForeignSaveJp2kFileClass *class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( class );
	VipsObjectClass *object_class = (VipsObjectClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "jp2ksave";
	object_class->build = vips_foreign_save_jp2k_file_build;

	VIPS_ARG_STRING( class, "filename", 1, 
		_( "Filename" ),
		_( "Filename to load from" ),
		VIPS_ARGUMENT_REQUIRED_INPUT, 
		G_STRUCT_OFFSET( VipsForeignSaveJp2kFile, filename ),
		NULL );

}

static void
vips_foreign_save_jp2k_file_init( VipsForeignSaveJp2kFile *file )
{
}

typedef struct _VipsForeignSaveJp2kBuffer {
	VipsForeignSaveJp2k parent_object;

	/* Save to a buffer.
	 */
	VipsArea *buf;

} VipsForeignSaveJp2kBuffer;

typedef VipsForeignSaveJp2kClass VipsForeignSaveJp2kBufferClass;

G_DEFINE_TYPE( VipsForeignSaveJp2kBuffer, vips_foreign_save_jp2k_buffer, 
	vips_foreign_save_jp2k_get_type() );

static int
vips_foreign_save_jp2k_buffer_build( VipsObject *object )
{
	VipsForeignSaveJp2k *jp2k = (VipsForeignSaveJp2k *) object;
	VipsForeignSaveJp2kBuffer *buffer = 
		(VipsForeignSaveJp2kBuffer *) object;

	VipsBlob *blob;

	if( !(jp2k->target = vips_target_new_to_memory()) )
		return( -1 );

	if( VIPS_OBJECT_CLASS( vips_foreign_save_jp2k_buffer_parent_class )->
		build( object ) )
		return( -1 );

	g_object_get( jp2k->target, "blob", &blob, NULL );
	g_object_set( buffer, "buffer", blob, NULL );
	vips_area_unref( VIPS_AREA( blob ) );

	return( 0 );
}

static void
vips_foreign_save_jp2k_buffer_class_init( 
	VipsForeignSaveJp2kBufferClass *class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( class );
	VipsObjectClass *object_class = (VipsObjectClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "jp2ksave_buffer";
	object_class->build = vips_foreign_save_jp2k_buffer_build;

	VIPS_ARG_BOXED( class, "buffer", 1, 
		_( "Buffer" ),
		_( "Buffer to save to" ),
		VIPS_ARGUMENT_REQUIRED_OUTPUT, 
		G_STRUCT_OFFSET( VipsForeignSaveJp2kBuffer, buf ),
		VIPS_TYPE_BLOB );

}

static void
vips_foreign_save_jp2k_buffer_init( VipsForeignSaveJp2kBuffer *buffer )
{
}

typedef struct _VipsForeignSaveJp2kTarget {
	VipsForeignSaveJp2k parent_object;

	VipsTarget *target;
} VipsForeignSaveJp2kTarget;

typedef VipsForeignSaveJp2kClass VipsForeignSaveJp2kTargetClass;

G_DEFINE_TYPE( VipsForeignSaveJp2kTarget, vips_foreign_save_jp2k_target, 
	vips_foreign_save_jp2k_get_type() );

static int
vips_foreign_save_jp2k_target_build( VipsObject *object )
{
	VipsForeignSaveJp2k *jp2k = (VipsForeignSaveJp2k *) object;
	VipsForeignSaveJp2kTarget *target = 
		(VipsForeignSaveJp2kTarget *) object;

	if( target->target ) {
		jp2k->target = target->target;
		g_object_ref( jp2k->target );
	}

	if( VIPS_OBJECT_CLASS( vips_foreign_save_jp2k_target_parent_class )->
		build( object ) )
		return( -1 );

	return( 0 );
}

static void
vips_foreign_save_jp2k_target_class_init( 
	VipsForeignSaveJp2kTargetClass *class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( class );
	VipsObjectClass *object_class = (VipsObjectClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "jp2ksave_target";
	object_class->build = vips_foreign_save_jp2k_target_build;

	VIPS_ARG_OBJECT( class, "target", 1,
		_( "Target" ),
		_( "Target to save to" ),
		VIPS_ARGUMENT_REQUIRED_INPUT, 
		G_STRUCT_OFFSET( VipsForeignSaveJp2kTarget, target ),
		VIPS_TYPE_TARGET );

}

static void
vips_foreign_save_jp2k_target_init( VipsForeignSaveJp2kTarget *target )
{
}

/* Stuff we track during tile compress.
 */
typedef struct _TileCompress {
	VipsImage *im;

	opj_cparameters_t parameters;
        opj_codec_t *codec;
	opj_image_t *image;
	VipsPel *accumulate;
	unsigned char *tile_buffer;
        gboolean save_as_ycc;
       	gboolean subsample;
       	size_t sizeof_element;
       	size_t sizeof_pel;
} TileCompress;

void
vips__foreign_load_jp2k_compress_free( void *handle )
{
	TileCompress *compress  = (TileCompress *) handle;

	VIPS_FREEF( opj_destroy_codec, compress->codec );
	VIPS_FREEF( opj_image_destroy, compress->image );
	VIPS_FREE( compress->accumulate );
	VIPS_FREE( compress->tile_buffer );
	VIPS_FREE( compress );
}

/* Get ready to compress a set of tiles. Tiles are up to width * height
 * pixels.
 */
void *
vips__foreign_load_jp2k_compress_create( VipsImage *im,
        int width, int height, gboolean save_as_ycc, gboolean subsample,
        gboolean lossless, int Q )
{
	TileCompress *compress;
	size_t sizeof_line;
	size_t sizeof_tile;

	compress = g_new0( TileCompress, 1 );

	/* Set compression params.
	 */
	opj_set_default_encoder_parameters( &compress->parameters );
	compress->im = im;
	compress->parameters.numresolution = 1;
	compress->save_as_ycc = save_as_ycc && im->Bands == 3;
	compress->subsample = subsample && save_as_ycc;
	compress->sizeof_element = VIPS_IMAGE_SIZEOF_ELEMENT( im );;
	compress->sizeof_pel = VIPS_IMAGE_SIZEOF_PEL( im );;

	/* Makes three band images smaller, somehow.
	 */
	compress->parameters.tcp_mct = (im->Bands >= 3 && !subsample) ? 1 : 0;

	/* Lossy mode.
	 */
	if( !lossless ) {
		compress->parameters.irreversible = TRUE;

		/* Map Q to allowed distortion.
		 */
		compress->parameters.cp_disto_alloc = 1;
		compress->parameters.cp_fixed_quality = TRUE;
		compress->parameters.tcp_distoratio[0] = Q;
		compress->parameters.tcp_numlayers = 1;
	}

	/* Create output image.
	 */
	if( !(compress->image = vips_foreign_save_jp2k_new_image( im,
		width, height, subsample, save_as_ycc )) ) {
		vips__foreign_load_jp2k_compress_free( compress );
		return( NULL );
	}

	/* We need a line of sums for chroma subsample. At worst, gint64.
	 */
	sizeof_line = sizeof( gint64 ) * width;
	if( !(compress->accumulate = 
		VIPS_ARRAY( NULL, sizeof_line, VipsPel )) ) {
		vips__foreign_load_jp2k_compress_free( compress );
		return( NULL );
	}

	/* Need to split tile to separate subsampled planes in @unpack. 
	 */
	sizeof_tile = VIPS_IMAGE_SIZEOF_PEL( im ) * width * height;
	if( !(compress->tile_buffer = 
		VIPS_ARRAY( NULL, sizeof_tile, VipsPel )) ) {
		vips__foreign_load_jp2k_compress_free( compress );
		return( NULL );
	}

	compress->codec = opj_create_compress( OPJ_CODEC_J2K );
	vips_foreign_save_jp2k_attach_handlers( compress->codec );
        if( !opj_setup_encoder( compress->codec, 
		&compress->parameters, compress->image ) ) {
		vips__foreign_load_jp2k_compress_free( compress );
		return( NULL );
	}

#ifdef HAVE_LIBOPENJP2_THREADING
	/* Use eg. VIPS_CONCURRENCY etc. to set n-cpus, if this openjpeg has
	 * stable support. 
	 */
	opj_codec_set_threads( compress->codec, vips_concurrency_get() );
#endif /*HAVE_LIBOPENJP2_THREADING*/

	return( (void *) compress );
}

/* Called from eg. tiffsave to compress a tile of pixels. The return pointer
 * must be freed with g_free().
 */
unsigned char *
vips__foreign_load_jp2k_compress( void *handle, 
	VipsRegion *region, VipsRect *tile, size_t *length )
{
	TileCompress *compress  = (TileCompress *) handle;

	opj_stream_t *stream;
	VipsTarget *target;
	int i;
	unsigned char *buffer;

	/* Set the size for this tile.
	 */
	compress->image->x1 = tile->width;
	compress->image->y1 = tile->height;
	for( i = 0; i < compress->im->Bands; i++ ) {
		compress->image->comps[i].w = tile->width;
		compress->image->comps[i].h = tile->height;
	}

	if( compress->save_as_ycc ) 
		vips_foreign_save_jp2k_rgb_to_ycc( region, 
			tile, compress->image->comps[0].prec );

	if( compress->subsample )
		vips_foreign_save_jp2k_unpack_subsample( region,
			tile, compress->image, 
			compress->tile_buffer, compress->accumulate ); 
	else
		vips_foreign_save_jp2k_unpack( region,
			tile, compress->image, 
			compress->tile_buffer );

	target = vips_target_new_to_memory();
	if( !(stream = vips_foreign_save_jp2k_target( target )) ) {
		VIPS_UNREF( target );
		return( NULL );
	}

	if( !opj_start_compress( compress->codec, compress->image, stream ) ) {
		VIPS_FREEF( opj_stream_destroy, stream );
		VIPS_UNREF( target );
		return( NULL );
	}

	if( !opj_encode( compress->codec, stream ) ) {
		VIPS_FREEF( opj_stream_destroy, stream );
		VIPS_UNREF( target );
		return( NULL );
	}

	opj_end_compress( compress->codec, stream );
	vips_target_finish( target );
	buffer = vips_target_steal( target, length );

	VIPS_FREEF( opj_stream_destroy, stream );
	VIPS_UNREF( target );

	return( buffer );
}

#endif /*HAVE_LIBOPENJP2*/

/**
 * vips_jp2ksave: (method)
 * @in: image to save 
 * @filename: file to write to 
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @Q: %gint, quality factor
 * * @lossless: %gboolean, enables lossless compression
 * * @tile_width: %gint for tile size
 * * @tile_height: %gint for tile size
 * * @subsample_mode: #VipsForeignSubsample, chroma subsampling mode
 *
 * Write a VIPS image to a file in JPEG2000 format. 
 * The saver supports 8, 16 and 32-bit int pixel
 * values, signed and unsigned. It supports greyscale, RGB, CMYK and
 * multispectral images. 
 *
 * Use @Q to set the compression quality factor. The default value of 45
 * produces file with approximately the same size as regular JPEG Q 75.
 *
 * Set @lossless to enable lossless compresion.
 *
 * Use @tile_width and @tile_height to set the tile size. The default is 512.
 *
 * Chroma subsampling is normally automatically disabled for Q >= 90. You can
 * force the subsampling mode with @subsample_mode.
 *
 * This operation always writes a pyramid.
 *
 * See also: vips_image_write_to_file(), vips_jp2kload().
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_jp2ksave( VipsImage *in, const char *filename, ... )
{
	va_list ap;
	int result;

	va_start( ap, filename );
	result = vips_call_split( "jp2ksave", ap, in, filename );
	va_end( ap );

	return( result );
}

/**
 * vips_jp2ksave_buffer: (method)
 * @in: image to save 
 * @buf: (array length=len) (element-type guint8): return output buffer here
 * @len: (type gsize): return output length here
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @Q: %gint, quality factor
 * * @lossless: %gboolean, enables lossless compression
 * * @tile_width: %gint for tile size
 * * @tile_height: %gint for tile size
 * * @subsample_mode: #VipsForeignSubsample, chroma subsampling mode
 *
 * As vips_jp2ksave(), but save to a target.
 *
 * See also: vips_jp2ksave(), vips_image_write_to_target().
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_jp2ksave_buffer( VipsImage *in, void **buf, size_t *len, ... )
{
	va_list ap;
	VipsArea *area;
	int result;

	area = NULL; 

	va_start( ap, len );
	result = vips_call_split( "jp2ksave_buffer", ap, in, &area );
	va_end( ap );

	if( !result &&
		area ) { 
		if( buf ) {
			*buf = area->data;
			area->free_fn = NULL;
		}
		if( len ) 
			*len = area->length;

		vips_area_unref( area );
	}

	return( result );
}

/**
 * vips_jp2ksave_target: (method)
 * @in: image to save 
 * @target: save image to this target
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @Q: %gint, quality factor
 * * @lossless: %gboolean, enables lossless compression
 * * @tile_width: %gint for tile size
 * * @tile_height: %gint for tile size
 * * @subsample_mode: #VipsForeignSubsample, chroma subsampling mode
 *
 * As vips_jp2ksave(), but save to a target.
 *
 * See also: vips_jp2ksave(), vips_image_write_to_target().
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_jp2ksave_target( VipsImage *in, VipsTarget *target, ... )
{
	va_list ap;
	int result;

	va_start( ap, target );
	result = vips_call_split( "jp2ksave_target", ap, in, target );
	va_end( ap );

	return( result );
}
