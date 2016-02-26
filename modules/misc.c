// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// This file is for miscellaneous formats that are easy to support.
// Combining them in one file speeds up compilation and development time.

#include <deark-config.h>
#include <deark-modules.h>
#include "fmtutil.h"

// **************************************************************************
// "copy" module
//
// This is a trivial module that makes a copy of the input file.
// **************************************************************************

static void de_run_copy(deark *c, de_module_params *mparams)
{
	dbuf_create_file_from_slice(c->infile, 0, c->infile->len, "bin", NULL);
}

void de_module_copy(deark *c, struct deark_module_info *mi)
{
	mi->id = "copy";
	mi->desc = "Copy the file unchanged";
	mi->run_fn = de_run_copy;
	mi->identify_fn = de_identify_none;
}

// **************************************************************************
// CRC-32
// Prints the CRC-32. Does not create any files.
// (Currently intended for development/debugging use, but might be improved
// and documented in the future.)
// **************************************************************************

static void de_run_crc32(deark *c, de_module_params *mparams)
{
	de_byte *buf = NULL;
	de_uint32 crc;

	// TODO: Make this work for arbitrarily large files.
	buf = de_malloc(c, c->infile->len);
	de_read(buf, 0, c->infile->len);
	crc = de_crc32(buf, c->infile->len);
	de_printf(c, DE_MSGTYPE_MESSAGE, "CRC-32: 0x%08x\n", (unsigned int)crc);
	de_free(c, buf);
}

void de_module_crc32(deark *c, struct deark_module_info *mi)
{
	mi->id = "crc32";
	mi->desc = "Print the IEEE CRC-32 of the file";
	mi->run_fn = de_run_crc32;
	mi->identify_fn = de_identify_none;
	mi->flags |= DE_MODFLAG_HIDDEN;
}

// **************************************************************************
// zlib module
//
// This module is for decompressing zlib-compressed files.
// It uses the deark-miniz.c utilities, which in turn use miniz.c.
// **************************************************************************

static void de_run_zlib(deark *c, de_module_params *mparams)
{
	dbuf *f = NULL;

	f = dbuf_create_output_file(c, "unc", NULL);
	de_uncompress_zlib(c->infile, 0, c->infile->len, f);
	dbuf_close(f);
}

static int de_identify_zlib(deark *c)
{
	de_byte b[2];
	de_read(b, 0, 2);

	if((b[0]&0x0f) != 8)
		return 0;

	if(b[0]<0x08 || b[0]>0x78)
		return 0;

	if(((((unsigned int)b[0])<<8)|b[1])%31 != 0)
		return 0;

	return 50;
}

void de_module_zlib(deark *c, struct deark_module_info *mi)
{
	mi->id = "zlib";
	mi->desc = "Raw zlib compressed data";
	mi->run_fn = de_run_zlib;
	mi->identify_fn = de_identify_zlib;
}

// **************************************************************************
// SAUCE
// Special module that reads SAUCE metadata for other modules to use,
// and handles files with SAUCE records if they aren't otherwise handled.
// **************************************************************************

static void de_run_sauce(deark *c, de_module_params *mparams)
{
	struct de_SAUCE_info *si = NULL;

	si = de_malloc(c, sizeof(struct de_SAUCE_info));
	if(de_read_SAUCE(c, c->infile, si)) {
		de_err(c, "This file has a SAUCE metadata record that identifies it as "
			"DataType %d, FileType %d, but it is not a supported format.\n",
			(int)si->data_type, (int)si->file_type);
	}
	de_free_SAUCE(c, si);
}

static int de_identify_sauce(deark *c)
{
	if(de_detect_SAUCE(c, c->infile, &c->detection_data.sauce)) {
		// This module should have a very low priority, but other modules can use
		// the results of its detection.
		return 2;
	}
	return 0;
}

void de_module_sauce(deark *c, struct deark_module_info *mi)
{
	mi->id = "sauce";
	mi->desc = "SAUCE metadata";
	mi->run_fn = de_run_sauce;
	mi->identify_fn = de_identify_sauce;
	mi->flags |= DE_MODFLAG_HIDDEN;
}

// **************************************************************************
// HP 100LX / HP 200LX .ICN icon format
// **************************************************************************

static void de_run_hpicn(deark *c, de_module_params *mparams)
{
	de_int64 width, height;

	width = de_getui16le(4);
	height = de_getui16le(6);
	de_convert_and_write_image_bilevel(c->infile, 8, width, height, (width+7)/8,
		DE_CVTF_WHITEISZERO, NULL);
}

static int de_identify_hpicn(deark *c)
{
	de_byte b[8];
	de_read(b, 0, 8);
	if(!de_memcmp(b, "\x01\x00\x01\x00\x2c\x00\x20\x00", 8))
		return 100;
	if(!de_memcmp(b, "\x01\x00\x01\x00", 4))
		return 60;
	return 0;
}

void de_module_hpicn(deark *c, struct deark_module_info *mi)
{
	mi->id = "hpicn";
	mi->desc = "HP 100LX/200LX .ICN icon";
	mi->run_fn = de_run_hpicn;
	mi->identify_fn = de_identify_hpicn;
}

// **************************************************************************
// X11 "puzzle" format
// ftp://ftp.x.org/pub/unsupported/programs/puzzle/
// This is the format generated by Netpbm's ppmtopuzz utility.
// **************************************************************************

struct xpuzzctx {
	de_int64 w, h;
	de_int64 palentries;
};

static int xpuzz_read_header(deark *c, struct xpuzzctx *d)
{
	d->w = de_getui32be(0);
	d->h = de_getui32be(4);
	d->palentries = (de_int64)de_getbyte(8);
	if(!de_good_image_dimensions_noerr(c, d->w, d->h)) return 0;
	if(d->palentries==0) d->palentries = 256;
	return 1;
}

static void de_run_xpuzzle(deark *c, de_module_params *mparams)
{
	struct xpuzzctx *d = NULL;
	struct deark_bitmap *img = NULL;
	de_int64 k;
	de_uint32 pal[256];
	de_int64 p;

	d = de_malloc(c, sizeof(struct xpuzzctx));
	if(!xpuzz_read_header(c, d)) goto done;
	if(!de_good_image_dimensions(c, d->w, d->h)) goto done;

	img = de_bitmap_create(c, d->w, d->h, 3);

	// Read the palette
	de_memset(pal, 0, sizeof(pal));
	p = 9;
	for(k=0; k<d->palentries; k++) {
		pal[k] = dbuf_getRGB(c->infile, p, 0);
		de_dbg_pal_entry(c, k, pal[k]);
		p+=3;
	}

	// Read the bitmap
	de_convert_image_paletted(c->infile, p, 8, d->w, pal, img, 0);

	de_bitmap_write_to_file(img, NULL);

done:
	de_bitmap_destroy(img);
	de_free(c, d);
}

static int de_identify_xpuzzle(deark *c)
{
	struct xpuzzctx *d = NULL;
	int retval = 0;

	d = de_malloc(c, sizeof(struct xpuzzctx));

	if(!xpuzz_read_header(c, d)) goto done;

	if(d->w * d->h + 3*d->palentries + 9 == c->infile->len) {
		retval = 20;
	}

done:
	de_free(c, d);
	return retval;
}

void de_module_xpuzzle(deark *c, struct deark_module_info *mi)
{
	mi->id = "xpuzzle";
	mi->desc = "X11 \"puzzle\" image";
	mi->run_fn = de_run_xpuzzle;
	mi->identify_fn = de_identify_xpuzzle;
}

// **************************************************************************
// Winzle! puzzle image
// **************************************************************************

static void de_run_winzle(deark *c, de_module_params *mparams)
{
	de_byte buf[256];
	de_int64 xorsize;
	de_int64 i;
	dbuf *f = NULL;

	xorsize = c->infile->len >= 256 ? 256 : c->infile->len;
	de_read(buf, 0, xorsize);
	for(i=0; i<xorsize; i++) {
		buf[i] ^= 0x0d;
	}

	f = dbuf_create_output_file(c, "bmp", NULL);
	dbuf_write(f, buf, xorsize);
	if(c->infile->len > 256) {
		dbuf_copy(c->infile, 256, c->infile->len - 256, f);
	}
	dbuf_close(f);
}

static int de_identify_winzle(deark *c)
{
	de_byte b[18];
	de_read(b, 0, sizeof(b));

	if(b[0]==0x4f && b[1]==0x40) {
		if(b[14]==0x25 && b[15]==0x0d && b[16]==0x0d && b[17]==0x0d) {
			return 95;
		}
		return 40;
	}
	return 0;
}

void de_module_winzle(deark *c, struct deark_module_info *mi)
{
	mi->id = "winzle";
	mi->desc = "Winzle! puzzle image";
	mi->run_fn = de_run_winzle;
	mi->identify_fn = de_identify_winzle;
}

// **************************************************************************
// Minolta RAW (MRW)
// **************************************************************************

static void do_mrw_seg_list(deark *c, de_int64 pos1, de_int64 len)
{
	de_int64 pos;
	de_byte seg_id[4];
	de_int64 data_len;

	pos = pos1;
	while(pos < pos1+len) {
		de_read(seg_id, pos, 4);
		data_len = de_getui32be(pos+4);
		pos+=8;
		if(pos+data_len > pos1+len) break;
		if(!de_memcmp(seg_id, "\0TTW", 4)) { // Exif
			de_fmtutil_handle_exif(c, pos, data_len);
		}
		pos+=data_len;
	}
}

static void de_run_mrw(deark *c, de_module_params *mparams)
{
	de_int64 mrw_seg_size;

	mrw_seg_size = de_getui32be(4);
	do_mrw_seg_list(c, 8, mrw_seg_size);
}

static int de_identify_mrw(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\x00\x4d\x52\x4d", 4))
		return 100;
	return 0;
}

void de_module_mrw(deark *c, struct deark_module_info *mi)
{
	mi->id = "mrw";
	mi->desc = "Minolta RAW (resources only)";
	mi->run_fn = de_run_mrw;
	mi->identify_fn = de_identify_mrw;
}

// **************************************************************************
// "Bob" bitmap image
// Used by the Bob ray tracer.
// **************************************************************************

static void de_run_bob(deark *c, de_module_params *mparams)
{
	struct deark_bitmap *img = NULL;
	de_int64 w, h;
	de_int64 k;
	de_uint32 pal[256];
	de_int64 p;

	w = de_getui16le(0);
	h = de_getui16le(2);
	if(!de_good_image_dimensions(c, w, h)) goto done;
	img = de_bitmap_create(c, w, h, 3);

	// Read the palette
	de_memset(pal, 0, sizeof(pal));
	p = 4;
	for(k=0; k<256; k++) {
		pal[k] = dbuf_getRGB(c->infile, p, 0);
		de_dbg_pal_entry(c, k, pal[k]);
		p+=3;
	}

	// Read the bitmap
	de_convert_image_paletted(c->infile, p, 8, w, pal, img, 0);

	de_bitmap_write_to_file(img, NULL);

done:
	de_bitmap_destroy(img);
}

static int de_identify_bob(deark *c)
{
	de_int64 w, h;

	if(!de_input_file_has_ext(c, "bob")) return 0;

	w = de_getui16le(0);
	h = de_getui16le(2);
	if(c->infile->len == 4 + 768 + w*h) {
		return 100;
	}
	return 0;
}

void de_module_bob(deark *c, struct deark_module_info *mi)
{
	mi->id = "bob";
	mi->desc = "Bob Ray Tracer bitmap image";
	mi->run_fn = de_run_bob;
	mi->identify_fn = de_identify_bob;
}

// **************************************************************************
// Alias PIX bitmap image.
// Also used by the Vivid ray tracer.
// **************************************************************************

static void de_run_alias_pix(deark *c, de_module_params *mparams)
{
	struct deark_bitmap *img = NULL;
	de_int64 w, h;
	de_int64 i;
	de_int64 pos;
	de_int64 firstline;
	de_int64 depth;
	de_int64 xpos, ypos;
	de_int64 runlen;
	de_uint32 clr;

	w = de_getui16be(0);
	h = de_getui16be(2);
	firstline = de_getui16be(4);
	depth = de_getui16be(8);

	if(!de_good_image_dimensions(c, w, h)) goto done;
	if(firstline >= h) goto done;
	if(depth!=24) {
		de_err(c, "Unsupported image type\n");
		goto done;
	}

	img = de_bitmap_create(c, w, h, 3);

	pos = 10;
	xpos = 0;
	// I don't know for sure what to do with the "first scanline" field, in the
	// unlikely event it is not 0. The documentation doesn't say.
	ypos = firstline;
	while(1) {
		if(pos+4 > c->infile->len) {
			break; // EOF
		}
		runlen = (de_int64)de_getbyte(pos);
		clr = dbuf_getRGB(c->infile, pos+1, DE_GETRGBFLAG_BGR);
		pos+=4;

		for(i=0; i<runlen; i++) {
			de_bitmap_setpixel_rgb(img, xpos, ypos, clr);
			xpos++; // Runs are not allowed to span rows
		}

		if(xpos >= w) {
			xpos=0;
			ypos++;
		}
	}

	de_bitmap_write_to_file(img, NULL);
done:
	de_bitmap_destroy(img);
}

static int de_identify_alias_pix(deark *c)
{
	de_int64 w, h, firstline, lastline, depth;

	if(!de_input_file_has_ext(c, "img") &&
		!de_input_file_has_ext(c, "als") &&
		!de_input_file_has_ext(c, "pix"))
	{
		return 0;
	}

	w = de_getui16be(0);
	h = de_getui16be(2);
	firstline = de_getui16be(4);
	lastline = de_getui16be(6);
	depth = de_getui16be(8);

	if(depth!=24) return 0;
	if(firstline>lastline) return 0;
	// 'lastline' should usually be h-1, but XnView apparently sets it to h.
	if(firstline>h-1 || lastline>h) return 0;
	if(!de_good_image_dimensions_noerr(c, w, h)) return 0;
	return 30;
}

void de_module_alias_pix(deark *c, struct deark_module_info *mi)
{
	mi->id = "alias_pix";
	mi->id_alias[0] = "vivid";
	mi->desc = "Alias PIX image, Vivid .IMG";
	mi->run_fn = de_run_alias_pix;
	mi->identify_fn = de_identify_alias_pix;
}

// **************************************************************************
// Apple volume label image
// Written by netpbm: ppmtoapplevol
// **************************************************************************

static de_byte applevol_get_gray_shade(de_byte clr)
{
	switch(clr) {
		// TODO: These gray shades may not be quite right. I can't find good
		// information about them.
	case 0x00: return 0xff;
	case 0xf6: return 0xee;
	case 0xf7: return 0xdd;
	case 0x2a: return 0xcc;
	case 0xf8: return 0xbb;
	case 0xf9: return 0xaa;
	case 0x55: return 0x99;
	case 0xfa: return 0x88;
	case 0xfb: return 0x77;
	case 0x80: return 0x66;
	case 0xfc: return 0x55;
	case 0xfd: return 0x44;
	case 0xab: return 0x33;
	case 0xfe: return 0x22;
	case 0xff: return 0x11;
	case 0xd6: return 0x00;
	}
	return 0xff;
}

static void de_run_applevol(deark *c, de_module_params *mparams)
{
	struct deark_bitmap *img = NULL;
	de_int64 w, h;
	de_int64 i, j;
	de_int64 p;
	de_byte palent;

	w = de_getui16be(1);
	h = de_getui16be(3);
	if(!de_good_image_dimensions(c, w, h)) goto done;
	img = de_bitmap_create(c, w, h, 1);

	p = 5;
	for(j=0; j<h; j++) {
		for(i=0; i<w; i++) {
			palent = de_getbyte(p+w*j+i);
			de_bitmap_setpixel_gray(img, i, j, applevol_get_gray_shade(palent));
		}
	}

	de_bitmap_write_to_file(img, NULL);

done:
	de_bitmap_destroy(img);
}

static int de_identify_applevol(deark *c)
{
	de_byte buf[5];

	de_read(buf, 0, sizeof(buf));

	if(buf[0]==0x01 && buf[3]==0x00 && buf[4]==0x0c)
		return 20;
	return 0;
}

void de_module_applevol(deark *c, struct deark_module_info *mi)
{
	mi->id = "applevol";
	mi->desc = "Apple volume label image";
	mi->run_fn = de_run_applevol;
	mi->identify_fn = de_identify_applevol;
}

// **************************************************************************
// TRS-80 "HR" ("High Resolution") image
// **************************************************************************

static void de_run_hr(deark *c, de_module_params *mparams)
{
	struct deark_bitmap *img = NULL;

	img = de_bitmap_create(c, 640, 240, 1);
	img->density_code = DE_DENSITY_UNK_UNITS;
	img->xdens = 2;
	img->ydens = 1;
	de_convert_image_bilevel(c->infile, 0, 640/8, img, 0);
	de_bitmap_write_to_file_finfo(img, NULL);
	de_bitmap_destroy(img);
}

static int de_identify_hr(deark *c)
{
	if(de_input_file_has_ext(c, "hr")) {
		if(c->infile->len==19200) return 70;
		if(c->infile->len>19200 && c->infile->len<=19456) return 30;
	}
	return 0;
}

void de_module_hr(deark *c, struct deark_module_info *mi)
{
	mi->id = "hr";
	mi->desc = "TRS-80 HR (High Resolution) image";
	mi->run_fn = de_run_hr;
	mi->identify_fn = de_identify_hr;
}

// **************************************************************************
// RIPterm icon (.ICN)
// **************************************************************************

static void de_run_ripicon(deark *c, de_module_params *mparams)
{
	struct deark_bitmap *img = NULL;
	de_int64 width, height;
	de_int64 chunk_span;
	de_int64 src_rowspan;
	de_int64 i, j, k;
	de_byte x;
	de_uint32 palent;

	width = 1 + de_getui16le(0);
	height = 1 + de_getui16le(2);
	de_dbg(c, "dimensions: %dx%d\n", (int)width, (int)height);
	if(!de_good_image_dimensions(c, width, height)) goto done;

	img = de_bitmap_create(c, width, height, 3);
	chunk_span = (width+7)/8;
	src_rowspan = 4*chunk_span;

	for(j=0; j<height; j++) {
		for(i=0; i<width; i++) {
			palent = 0;
			for(k=0; k<4; k++) {
				x = de_get_bits_symbol(c->infile, 1, 4 + j*src_rowspan + k*chunk_span, i);
				palent = (palent<<1)|x;
			}
			de_bitmap_setpixel_rgb(img, i, j, de_palette_pc16(palent));
		}
	}

	de_bitmap_write_to_file(img, NULL);
done:
	de_bitmap_destroy(img);
}

static int de_identify_ripicon(deark *c)
{
	de_byte buf[4];
	de_int64 expected_size;
	de_int64 width, height;

	if(!de_input_file_has_ext(c, "icn")) return 0;
	de_read(buf, 0, sizeof(buf));
	width = 1 + de_getui16le(0);
	height = 1 + de_getui16le(2);
	expected_size = 4 + height*(4*((width+7)/8)) + 1;
	if(c->infile->len >= expected_size && c->infile->len <= expected_size+1) {
		return 50;
	}
	return 0;
}

void de_module_ripicon(deark *c, struct deark_module_info *mi)
{
	mi->id = "ripicon";
	mi->desc = "RIP/RIPscrip/RIPterm Icon";
	mi->run_fn = de_run_ripicon;
	mi->identify_fn = de_identify_ripicon;
}

// **************************************************************************
// LSS16 image (Used by SYSLINUX)
// **************************************************************************

struct lss16ctx {
	de_int64 pos;
	int nextnibble_valid;
	de_byte nextnibble;
};

static de_byte lss16_get_nibble(deark *c, struct lss16ctx *d)
{
	de_byte n;
	if(d->nextnibble_valid) {
		d->nextnibble_valid = 0;
		return d->nextnibble;
	}
	n = de_getbyte(d->pos);
	d->pos++;
	// The low nibble of each byte is interpreted first.
	// Record the high nibble, and return the low nibble.
	d->nextnibble = (n&0xf0)>>4;
	d->nextnibble_valid = 1;
	return n&0x0f;
}

static void de_run_lss16(deark *c, de_module_params *mparams)
{
	struct lss16ctx *d = NULL;
	struct deark_bitmap *img = NULL;
	de_int64 width, height;
	de_int64 i;
	de_int64 xpos, ypos;
	de_byte n;
	de_byte prev;
	de_int64 run_len;
	de_byte cr1, cg1, cb1;
	de_byte cr2, cg2, cb2;
	de_uint32 pal[16];

	d = de_malloc(c, sizeof(struct lss16ctx));

	d->pos = 4;
	width = de_getui16le(d->pos);
	height = de_getui16le(d->pos+2);
	de_dbg(c, "dimensions: %dx%d\n", (int)width, (int)height);
	if(!de_good_image_dimensions(c, width, height)) goto done;

	d->pos += 4;
	for(i=0; i<16; i++) {
		cr1 = de_getbyte(d->pos);
		cg1 = de_getbyte(d->pos+1);
		cb1 = de_getbyte(d->pos+2);
		// Palette samples are from [0 to 63]. Convert to [0 to 255].
		cr2 = de_palette_sample_6_to_8bit(cr1);
		cg2 = de_palette_sample_6_to_8bit(cg1);
		cb2 = de_palette_sample_6_to_8bit(cb1);
		de_dbg2(c, "pal[%2d] = (%2d,%2d,%2d) -> (%3d,%3d,%3d)\n", (int)i,
			(int)cr1, (int)cg1, (int)cb1,
			(int)cr2, (int)cg2, (int)cb2);
		pal[i] = DE_MAKE_RGB(cr2, cg2, cb2);
		d->pos+=3;
	}

	img = de_bitmap_create(c, width, height, 3);

	xpos=0; ypos=0;
	prev=0;
	while(d->pos<c->infile->len && ypos<height) {
		n = lss16_get_nibble(c, d);

		if(n == prev) {
			// A run of pixels
			run_len = (de_int64)lss16_get_nibble(c, d);
			if(run_len==0) {
				run_len = lss16_get_nibble(c, d) | (lss16_get_nibble(c, d)<<4);
				run_len += 16;
			}
			for(i=0; i<run_len; i++) {
				de_bitmap_setpixel_rgb(img, xpos, ypos, pal[prev]);
				xpos++;
			}
		}
		else {
			// An uncompressed pixel
			de_bitmap_setpixel_rgb(img, xpos, ypos, pal[n]);
			xpos++;
			prev = n;
		}

		// End of row reached?
		if(xpos>=width) {
			xpos=0;
			ypos++;
			d->nextnibble_valid = 0;
			prev = 0;
		}
	}

	de_bitmap_write_to_file(img, NULL);
done:
	de_bitmap_destroy(img);
	de_free(c, d);
}

static int de_identify_lss16(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\x3d\xf3\x13\x14", 4))
		return 100;
	return 0;
}

void de_module_lss16(deark *c, struct deark_module_info *mi)
{
	mi->id = "lss16";
	mi->desc = "SYSLINUX LSS16 image";
	mi->run_fn = de_run_lss16;
	mi->identify_fn = de_identify_lss16;
}

// **************************************************************************
// VBM (VDC BitMap)
// **************************************************************************

static void de_run_vbm(deark *c, de_module_params *mparams)
{
	de_int64 width, height;
	de_byte ver;

	ver = de_getbyte(3);
	if(ver!=2) {
		// TODO: Support VBM v3.
		de_err(c, "Unsupported VBM version (%d)\n", (int)ver);
		return;
	}
	width = de_getui16be(4);
	height = de_getui16be(6);
	de_convert_and_write_image_bilevel(c->infile, 8, width, height, (width+7)/8,
		DE_CVTF_WHITEISZERO, NULL);
}

// Note that this function must work together with de_identify_bmp().
static int de_identify_vbm(deark *c)
{
	de_byte b[4];
	de_read(b, 0, 4);
	if(de_memcmp(b, "BM\xcb", 3)) return 0;
	if(b[3]!=2 && b[3]!=3) return 0;
	if(de_input_file_has_ext(c, "vbm")) return 100;
	return 80;
}

void de_module_vbm(deark *c, struct deark_module_info *mi)
{
	mi->id = "vbm";
	mi->desc = "C64/128 VBM (VDC BitMap)";
	mi->run_fn = de_run_vbm;
	mi->identify_fn = de_identify_vbm;
}

// **************************************************************************
// PFS: 1st Publisher clip art (.ART)
// **************************************************************************

static void de_run_fp_art(deark *c, de_module_params *mparams)
{
	de_int64 width, height;
	de_int64 rowspan;

	width = de_getui16le(2);
	height = de_getui16le(6);
	rowspan = ((width+15)/16)*2;
	de_convert_and_write_image_bilevel(c->infile, 8, width, height, rowspan, 0, NULL);
}

static int de_identify_fp_art(deark *c)
{
	de_int64 width, height;
	de_int64 rowspan;

	if(!de_input_file_has_ext(c, "art")) return 0;

	width = de_getui16le(2);
	height = de_getui16le(6);
	rowspan = ((width+15)/16)*2;
	if(8 + rowspan*height == c->infile->len) {
		return 100;
	}

	return 0;
}

void de_module_fp_art(deark *c, struct deark_module_info *mi)
{
	mi->id = "fp_art";
	mi->desc = "PFS: 1st Publisher clip art (.ART)";
	mi->run_fn = de_run_fp_art;
	mi->identify_fn = de_identify_fp_art;
}

// **************************************************************************
// PNG
// **************************************************************************

static void do_png_iccp(deark *c, de_int64 pos, de_int64 len)
{
	de_byte prof_name[81];
	de_int64 prof_name_len;
	de_byte cmpr_type;
	dbuf *f = NULL;
	de_finfo *fi = NULL;

	de_read(prof_name, pos, 80); // One of the next 80 bytes should be a NUL.
	prof_name[80] = '\0';
	prof_name_len = de_strlen((const char*)prof_name);
	if(prof_name_len > 79) return;
	cmpr_type = de_getbyte(pos + prof_name_len + 1);
	if(cmpr_type!=0) return;

	fi = de_finfo_create(c);
	if(c->filenames_from_file)
		de_finfo_set_name_from_sz(c, fi, (const char*)prof_name, DE_ENCODING_LATIN1);
	f = dbuf_create_output_file(c, "icc", fi);
	de_uncompress_zlib(c->infile, pos + prof_name_len + 2,
		len - (pos + prof_name_len + 2), f);
	dbuf_close(f);
	de_finfo_destroy(c, fi);
}

#define PNGID_IDAT 0x49444154
#define PNGID_iCCP 0x69434350

static void de_run_png(deark *c, de_module_params *mparams)
{
	de_int64 pos;
	de_int64 chunk_data_len;
	de_int64 chunk_id;
	de_int64 prev_chunk_id = 0;
	int suppress_idat_dbg = 0;
	de_byte buf[4];
	char chunk_id_printable[8];

	pos = 8;
	while(pos < c->infile->len) {
		chunk_data_len = de_getui32be(pos);
		if(pos + 8 + chunk_data_len + 4 > c->infile->len) break;
		de_read(buf, pos+4, 4);
		de_make_printable_ascii(buf, 4, chunk_id_printable, sizeof(chunk_id_printable), 0);
		chunk_id = de_getui32be_direct(buf);

		if(chunk_id==PNGID_IDAT && suppress_idat_dbg) {
			;
		}
		else if(chunk_id==PNGID_IDAT && prev_chunk_id==PNGID_IDAT && c->debug_level<2) {
			de_dbg(c, "(more IDAT chunks follow)\n");
			suppress_idat_dbg = 1;
		}
		else {
			de_dbg(c, "'%s' chunk at %d\n", chunk_id_printable, (int)pos);
			if(chunk_id!=PNGID_IDAT) suppress_idat_dbg = 0;
		}

		if(chunk_id==PNGID_iCCP) { // iCCP
			do_png_iccp(c, pos+8, chunk_data_len);
		}
		pos += 8 + chunk_data_len + 4;
		prev_chunk_id = chunk_id;
	}
}

static int de_identify_png(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a", 8))
		return 100;
	return 0;
}

void de_module_png(deark *c, struct deark_module_info *mi)
{
	mi->id = "png";
	mi->desc = "PNG image (resources only)";
	mi->run_fn = de_run_png;
	mi->identify_fn = de_identify_png;
}

// **************************************************************************
// YBM
// **************************************************************************

static void de_run_ybm(deark *c, de_module_params *mparams)
{
	struct deark_bitmap *img = NULL;
	de_int64 width, height;
	de_int64 i, j;
	de_int64 rowspan;
	de_byte x;

	width = de_getui16be(2);
	height = de_getui16be(4);
	if(!de_good_image_dimensions(c, width, height)) goto done;;
	rowspan = ((width+15)/16)*2;

	img = de_bitmap_create(c, width, height, 1);

	for(j=0; j<height; j++) {
		for(i=0; i<width; i++) {
			// This encoding is unusual: LSB-first 16-bit integers.
			x = de_get_bits_symbol(c->infile, 1, 6 + j*rowspan,
				(i-i%16) + (15-i%16));
			de_bitmap_setpixel_gray(img, i, j, x ? 0 : 255);
		}
	}
	de_bitmap_write_to_file(img, NULL);

done:
	de_bitmap_destroy(img);
}

static int de_identify_ybm(deark *c)
{
	de_int64 width, height;
	de_int64 rowspan;

	if(dbuf_memcmp(c->infile, 0, "!!", 2))
		return 0;
	width = de_getui16be(2);
	height = de_getui16be(4);
	rowspan = ((width+15)/16)*2;
	if(6+height*rowspan == c->infile->len)
		return 100;
	return 0;
}

void de_module_ybm(deark *c, struct deark_module_info *mi)
{
	mi->id = "ybm";
	mi->desc = "Bennet Yee's face format, a.k.a. YBM";
	mi->run_fn = de_run_ybm;
	mi->identify_fn = de_identify_ybm;
}

// **************************************************************************
// OLPC .565 firmware icon
// **************************************************************************

static void de_run_olpc565(deark *c, de_module_params *mparams)
{
	struct deark_bitmap *img = NULL;
	de_int64 width, height;
	de_int64 i, j;
	de_int64 rowspan;
	de_byte b0, b1;
	de_uint32 clr;

	width = de_getui16le(4);
	height = de_getui16le(6);
	if(!de_good_image_dimensions(c, width, height)) goto done;
	rowspan = width*2;

	img = de_bitmap_create(c, width, height, 3);

	for(j=0; j<height; j++) {
		for(i=0; i<width; i++) {
			b0 = de_getbyte(8 + j*rowspan + i*2);
			b1 = de_getbyte(8 + j*rowspan + i*2 + 1);
			clr = (((de_uint32)b1)<<8) | b0;
			clr = de_rgb565_to_888(clr);
			de_bitmap_setpixel_rgb(img, i, j, clr);
		}
	}
	de_bitmap_write_to_file(img, NULL);

done:
	de_bitmap_destroy(img);
}

static int de_identify_olpc565(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "C565", 4))
		return 100;
	return 0;
}

void de_module_olpc565(deark *c, struct deark_module_info *mi)
{
	mi->id = "olpc565";
	mi->desc = "OLPC .565 firmware icon";
	mi->run_fn = de_run_olpc565;
	mi->identify_fn = de_identify_olpc565;
}

// **************************************************************************
// InShape .IIM
// **************************************************************************

static void de_run_iim(deark *c, de_module_params *mparams)
{
	struct deark_bitmap *img = NULL;
	de_int64 width, height;
	de_int64 i, j;
	de_int64 n, bpp;
	de_int64 rowspan;
	de_uint32 clr;

	// This code is based on reverse engineering, and may be incorrect.

	n = de_getui16be(8); // Unknown field
	bpp = de_getui16be(10);
	if(n!=4 || bpp!=24) {
		de_dbg(c, "This type of IIM image is not supported\n");
		goto done;
	}
	width = de_getui16be(12);
	height = de_getui16be(14);
	if(!de_good_image_dimensions(c, width, height)) goto done;
	rowspan = width*3;

	img = de_bitmap_create(c, width, height, 3);

	for(j=0; j<height; j++) {
		for(i=0; i<width; i++) {
			clr = dbuf_getRGB(c->infile, 16+j*rowspan+i*3, 0);
			de_bitmap_setpixel_rgb(img, i, j, clr);
		}
	}
	de_bitmap_write_to_file(img, NULL);

done:
	de_bitmap_destroy(img);
}

static int de_identify_iim(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "IS_IMAGE", 8))
		return 100;
	return 0;
}

void de_module_iim(deark *c, struct deark_module_info *mi)
{
	mi->id = "iim";
	mi->desc = "InShape IIM";
	mi->run_fn = de_run_iim;
	mi->identify_fn = de_identify_iim;
}

// **************************************************************************
// PM (format supported by the XV image viewer)
// **************************************************************************

static void de_run_pm_xv(deark *c, de_module_params *mparams)
{
	struct deark_bitmap *img = NULL;
	int is_le;
	de_int64 width, height;
	de_int64 nplanes;
	de_int64 nbands;
	de_int64 pixelformat;
	de_int64 commentsize;
	de_int64 i, j;
	de_int64 plane;
	de_int64 rowspan;
	de_int64 planespan;
	de_int64 pos;
	de_byte b;

	if(!dbuf_memcmp(c->infile, 0, "WEIV", 4))
		is_le = 1;
	else
		is_le = 0;

	nplanes = dbuf_geti32x(c->infile, 4, is_le);
	de_dbg(c, "planes: %d\n", (int)nplanes);

	height = dbuf_geti32x(c->infile, 8, is_le);
	width = dbuf_geti32x(c->infile, 12, is_le);
	de_dbg(c, "dimensions: %dx%d\n", (int)width, (int)height);
	if(!de_good_image_dimensions(c, width, height)) goto done;

	nbands = dbuf_geti32x(c->infile, 16, is_le);
	de_dbg(c, "bands: %d\n", (int)nbands);

	pixelformat = dbuf_geti32x(c->infile, 20, is_le);
	de_dbg(c, "pixel format: 0x%04x\n", (unsigned int)pixelformat);

	commentsize = dbuf_geti32x(c->infile, 24, is_le);
	de_dbg(c, "comment size: %d\n", (int)commentsize);

	pos = 28;

	if((pixelformat==0x8001 && nplanes==3 && nbands==1) ||
		(pixelformat==0x8001 && nplanes==1 && nbands==1))
	{
		;
	}
	else {
		de_err(c, "Unsupported image type (pixel format=0x%04x, "
			"planes=%d, bands=%d)\n", (unsigned int)pixelformat,
			(int)nplanes, (int)nbands);
		goto done;
	}

	rowspan = width;
	planespan = rowspan*height;

	img = de_bitmap_create(c, width, height, (int)nplanes);

	for(plane=0; plane<nplanes; plane++) {
		for(j=0; j<height; j++) {
			for(i=0; i<width; i++) {
				b = de_getbyte(pos + plane*planespan + j*rowspan + i);
				if(nplanes==3) {
					de_bitmap_setsample(img, i, j, plane, b);
				}
				else {
					de_bitmap_setpixel_gray(img, i, j, b);
				}
			}
		}
	}
	de_bitmap_write_to_file(img, NULL);

done:
	de_bitmap_destroy(img);
}

static int de_identify_pm_xv(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "VIEW", 4))
		return 15;
	if(!dbuf_memcmp(c->infile, 0, "WEIV", 4))
		return 15;
	return 0;
}

void de_module_pm_xv(deark *c, struct deark_module_info *mi)
{
	mi->id = "pm_xv";
	mi->desc = "PM (XV)";
	mi->run_fn = de_run_pm_xv;
	mi->identify_fn = de_identify_pm_xv;
}

// **************************************************************************
// Calamus Raster Graphic - CRG
// **************************************************************************

// Warning: The CRG decoder is based on reverse engineering, may not be
// correct, and is definitely incomplete.

static void de_run_crg(deark *c, de_module_params *mparams)
{
	de_int64 width, height;
	de_int64 rowspan;
	de_int64 pos;
	de_byte b1, b2;
	de_int64 count;
	de_int64 cmpr_img_start;
	de_int64 num_cmpr_bytes;
	dbuf *unc_pixels = NULL;

	width = de_getui32be(20);
	height = de_getui32be(24);
	de_dbg(c, "dimensions: %dx%d\n", (int)width, (int)height);
	if(!de_good_image_dimensions(c, width, height)) goto done;

	b1 = de_getbyte(32);
	if(b1!=0x01) {
		de_err(c, "Unsupported CRG format\n");
		goto done;
	}

	num_cmpr_bytes = de_getui32be(38);
	de_dbg(c, "compressed data size: %d\n", (int)num_cmpr_bytes);
	cmpr_img_start = 42;

	if(cmpr_img_start + num_cmpr_bytes > c->infile->len) {
		num_cmpr_bytes = c->infile->len - cmpr_img_start;
	}

	// Uncompress the image
	rowspan = (width+7)/8;
	unc_pixels = dbuf_create_membuf(c, height*rowspan, 1);

	pos = cmpr_img_start;
	while(pos < cmpr_img_start + num_cmpr_bytes) {
		b1 = de_getbyte(pos++);
		if(b1<=0x7f) { // Uncompressed bytes
			count = 1+(de_int64)b1;
			dbuf_copy(c->infile, pos, count, unc_pixels);
			pos += count;
		}
		else { // A compressed run
			b2 = de_getbyte(pos++);
			count = (de_int64)(b1-127);
			dbuf_write_run(unc_pixels, b2, count);
		}
	}
	de_dbg(c, "decompressed to %d bytes\n", (int)unc_pixels->len);

	de_convert_and_write_image_bilevel(unc_pixels, 0, width, height, rowspan,
		DE_CVTF_WHITEISZERO, NULL);

done:
	dbuf_close(unc_pixels);
}

static int de_identify_crg(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "CALAMUSCRG", 10))
		return 100;
	return 0;
}

void de_module_crg(deark *c, struct deark_module_info *mi)
{
	mi->id = "crg";
	mi->desc = "Calamus Raster Graphic";
	mi->run_fn = de_run_crg;
	mi->identify_fn = de_identify_crg;
}

// **************************************************************************
// farbfeld
// **************************************************************************

static void de_run_farbfeld(deark *c, de_module_params *mparams)
{
	struct deark_bitmap *img = NULL;
	de_int64 width, height;
	de_int64 i, j, k;
	de_int64 ppos;
	de_byte s[4];

	width = de_getui32be(8);
	height = de_getui32be(12);
	de_dbg(c, "dimensions: %dx%d\n", (int)width, (int)height);
	if(!de_good_image_dimensions(c, width, height)) return;

	img = de_bitmap_create(c, width, height, 4);

	for(j=0; j<height; j++) {
		for(i=0; i<width; i++) {
			ppos = 16 + 8*(width*j + i);
			for(k=0; k<4; k++) {
				s[k] = de_getbyte(ppos+2*k);
			}
			de_bitmap_setpixel_rgba(img, i, j,
				DE_MAKE_RGBA(s[0],s[1],s[2],s[3]));
		}
	}
	de_bitmap_write_to_file(img, NULL);
	de_bitmap_destroy(img);
}

static int de_identify_farbfeld(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "farbfeld", 8))
		return 100;
	return 0;
}

void de_module_farbfeld(deark *c, struct deark_module_info *mi)
{
	mi->id = "farbfeld";
	mi->desc = "farbfeld image";
	mi->run_fn = de_run_farbfeld;
	mi->identify_fn = de_identify_farbfeld;
}

// **************************************************************************
// VGA font (intended for development/debugging use)
// **************************************************************************

static void de_run_vgafont(deark *c, de_module_params *mparams)
{
	de_byte *fontdata = NULL;
	struct de_bitmap_font *font = NULL;
	de_int64 i;

	if(c->infile->len!=4096) {
		de_err(c, "Bad file size\n");
		goto done;
	}

	fontdata = de_malloc(c, 4096);
	de_read(fontdata, 0, 4096);

	if(de_get_ext_option(c, "vgafont:c")) {
		dbuf *ff;
		ff = dbuf_create_output_file(c, "h", NULL);
		for(i=0; i<4096; i++) {
			if(i%16==0) dbuf_puts(ff, "\t");
			dbuf_printf(ff, "%d", (int)fontdata[i]);
			if(i!=4095) dbuf_puts(ff, ",");
			if(i%16==15) dbuf_puts(ff, "\n");
		}
		dbuf_close(ff);
		goto done;
	}

	font = de_create_bitmap_font(c);
	font->num_chars = 256;
	font->has_nonunicode_codepoints = 1;
	font->has_unicode_codepoints = 0;
	font->prefer_unicode = 0;
	font->nominal_width = 8;
	font->nominal_height = 16;
	font->char_array = de_malloc(c, font->num_chars * sizeof(struct de_bitmap_font_char));

	for(i=0; i<font->num_chars; i++) {
		font->char_array[i].codepoint_nonunicode = (de_int32)i;
		font->char_array[i].width = font->nominal_width;
		font->char_array[i].height = font->nominal_height;
		font->char_array[i].rowspan = 1;
		font->char_array[i].bitmap = &fontdata[i*font->nominal_height];
	}

	de_font_bitmap_font_to_image(c, font, NULL);

done:
	if(font) {
		de_free(c, font->char_array);
		de_destroy_bitmap_font(c, font);
	}
	de_free(c, fontdata);
}

void de_module_vgafont(deark *c, struct deark_module_info *mi)
{
	mi->id = "vgafont";
	mi->desc = "Raw 8x16 VGA font";
	mi->run_fn = de_run_vgafont;
	mi->identify_fn = de_identify_none;
	mi->flags |= DE_MODFLAG_HIDDEN;
}
