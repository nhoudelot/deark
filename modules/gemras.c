// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// GEM VDI Bit Image / Gem Raster

#include <deark-config.h>
#include <deark-modules.h>

typedef struct localctx_struct {
	int is_ximg;
	de_int64 w, h;
	de_int64 nplanes;
	de_int64 patlen;
	de_int64 rowspan_per_plane;
	de_int64 rowspan_total;
	de_int64 pixwidth, pixheight;
	de_int64 header_size_in_bytes;
	de_byte *pattern_buf;
	de_uint32 pal[256];
} lctx;

// Caller must initialize *repeat_count.
static void uncompress_line(deark *c, lctx *d, dbuf *unc_line,
	de_int64 pos1, de_int64 rownum,
	de_int64 *bytes_consumed, de_int64 *repeat_count)
{
	de_int64 pos;
	de_byte b0, b1;
	de_byte val;
	de_int64 count;
	de_int64 k;
	de_int64 tmp_repeat_count;
	de_int64 unc_line_len_orig;

	*bytes_consumed = 0;
	pos = pos1;
	unc_line_len_orig = unc_line->len;

	while(1) {
		if(pos >= c->infile->len) break;
		if(unc_line->len - unc_line_len_orig >= d->rowspan_per_plane) break;

		b0 = de_getbyte(pos++);
		
		if(b0==0) { // Pattern run or scanline run
			b1 = de_getbyte(pos++);
			if(b1>0) { // pattern run
				de_read(d->pattern_buf, pos, d->patlen);
				pos += d->patlen;
				count = (de_int64)b1;
				for(k=0; k<count; k++) {
					dbuf_write(unc_line, d->pattern_buf, d->patlen);
				}
			}
			else { // (b1==0) scanline run
				de_byte flagbyte;
				flagbyte = de_getbyte(pos);
				if(flagbyte==0xff) {
					pos++;
					tmp_repeat_count = (de_int64)de_getbyte(pos++);
					if(tmp_repeat_count == 0) {
						de_dbg(c, "row %d: bad repeat count\n", (int)rownum);
					}
					else {
						*repeat_count = tmp_repeat_count;
					}
				}
				else {
					de_dbg(c, "row %d: bad scanline run marker: 0x%02x\n",
						(int)rownum, (unsigned int)flagbyte);
				}
			}
		}
		else if(b0==0x80) { // "Uncompressed bit string"
			count = (de_int64)de_getbyte(pos++);
			dbuf_copy(c->infile, pos, count, unc_line);
			pos += count;
		}
		else { // "solid run"
			val = (b0&0x80) ? 0xff : 0x00;
			count = (de_int64)(b0 & 0x7f);
			dbuf_write_run(unc_line, val, count);
		}
	}

	*bytes_consumed = pos - pos1;
}

static void uncompress_pixels(deark *c, lctx *d, dbuf *unc_pixels,
	de_int64 pos1, de_int64 len)
{
	de_int64 bytes_consumed;
	de_int64 pos;
	de_int64 ypos;
	de_int64 repeat_count;
	de_int64 k;
	de_int64 plane;
	dbuf *unc_line = NULL;

	d->pattern_buf = de_malloc(c, d->patlen);
	unc_line = dbuf_create_membuf(c, d->rowspan_total);

	pos = pos1;

	ypos = 0;
	while(1) {
		if(ypos >= d->h) break;

		repeat_count = 1;

		dbuf_empty(unc_line);
		for(plane=0; plane<d->nplanes; plane++) {
			uncompress_line(c, d, unc_line,
				pos, ypos, &bytes_consumed, &repeat_count);
			pos+=bytes_consumed;
			if(bytes_consumed<1) goto done1;
		}

		for(k=0; k<repeat_count; k++) {
			if(ypos >= d->h) break;
			dbuf_copy(unc_line, 0, d->rowspan_total, unc_pixels);
			ypos++;
		}
	}
done1:
	dbuf_close(unc_line);
	de_free(c, d->pattern_buf);
	d->pattern_buf = NULL;
}

static void set_density(deark *c, lctx *d, struct deark_bitmap *img)
{
	if(d->pixwidth>0 && d->pixheight>0) {
		img->density_code = DE_DENSITY_DPI;
		img->xdens = 25400.0/(double)d->pixwidth;
		img->ydens = 25400.0/(double)d->pixheight;
	}
}

static int do_gem_img(deark *c, lctx *d)
{
	dbuf *unc_pixels = NULL;
	struct deark_bitmap *img = NULL;

	unc_pixels = dbuf_create_membuf(c, d->rowspan_total*d->h);

	uncompress_pixels(c, d, unc_pixels, d->header_size_in_bytes, c->infile->len-d->header_size_in_bytes);

	img = de_bitmap_create(c, d->w, d->h, 1);
	set_density(c, d, img);

	de_convert_image_bilevel(unc_pixels, 0, d->rowspan_per_plane, img, DE_CVTF_WHITEISZERO);
	de_bitmap_write_to_file_finfo(img, NULL);

	de_bitmap_destroy(img);
	dbuf_close(unc_pixels);
	return 1;
}

static de_byte scale_1000_to_255(de_int64 n1)
{
	if(n1<=0) return 0;
	if(n1>=1000) return 255;
	return (de_byte)(0.5+((255.0/1000.0)*(double)n1));
}

static void read_palette(deark *c, lctx *d)
{
	de_int64 pal_entries;
	de_int64 i;
	de_int64 cr1, cg1, cb1;
	de_byte cr, cg, cb;

	pal_entries = (de_int64)(1<<((unsigned int)d->nplanes));
	if(pal_entries>256) pal_entries=256;

	for(i=0; i<pal_entries; i++) {
		cr1 = de_getui16be(22 + 6*i);
		cg1 = de_getui16be(22 + 6*i + 2);
		cb1 = de_getui16be(22 + 6*i + 4);

		cr = scale_1000_to_255(cr1);
		cg = scale_1000_to_255(cg1);
		cb = scale_1000_to_255(cb1);

		de_dbg2(c, "pal[%3d] = (%4d,%4d,%4d) -> (%3d,%3d,%3d)\n", (int)i,
			(int)cr1, (int)cg1, (int)cb1,
			(int)cr, (int)cg, (int)cb);

		d->pal[i] = DE_MAKE_RGB(cr, cg, cb);
	}
}

static int do_gem_ximg(deark *c, lctx *d)
{
	dbuf *unc_pixels = NULL;
	struct deark_bitmap *img = NULL;
	int retval = 0;
	de_int64 i, j, plane;
	unsigned int n;
	de_byte x;

	if(d->nplanes<1 || d->nplanes>8) {
		de_err(c, "%d-plane XIMG images are not supported\n", (int)d->nplanes);
		goto done;
	}

	read_palette(c, d);

	unc_pixels = dbuf_create_membuf(c, d->rowspan_total*d->h);

	uncompress_pixels(c, d, unc_pixels, d->header_size_in_bytes, c->infile->len-d->header_size_in_bytes);

	img = de_bitmap_create(c, d->w, d->h, 3);
	set_density(c, d, img);

	for(j=0; j<d->h; j++) {
		for(i=0; i<d->w; i++) {
			n = 0;
			for(plane=0; plane<d->nplanes; plane++) {
				x = de_get_bits_symbol(unc_pixels, 1, j*d->rowspan_total + plane*d->rowspan_per_plane, i);
				if(x) n |= 1<<plane;
			}

			de_bitmap_setpixel_rgb(img, i, j, d->pal[n]);
		}
	}
	de_bitmap_write_to_file_finfo(img, NULL);

	de_bitmap_destroy(img);
	dbuf_close(unc_pixels);
	retval = 1;

done:
	return retval;
}

static void de_run_gemraster(deark *c, const char *params)
{
	de_int64 header_size_in_words;
	de_int64 ver;
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));
	ver = de_getui16be(0);
	de_dbg(c, "version: %d\n", (int)ver);
	header_size_in_words = de_getui16be(2);
	d->header_size_in_bytes = header_size_in_words*2;
	de_dbg(c, "header size: %d words (%d bytes)\n", (int)header_size_in_words,
		(int)d->header_size_in_bytes);
	d->nplanes = de_getui16be(4);
	de_dbg(c, "planes: %d\n", (int)d->nplanes);

	if(header_size_in_words>=11) {
		d->is_ximg = !dbuf_memcmp(c->infile, 16, "XIMG", 4);
	}

	if(d->is_ximg) {
		;
	}
	else if(header_size_in_words!=0x08 || d->nplanes!=1) {
		de_err(c, "This version of GEM Raster is not supported.\n");
		return;
	}

	d->patlen = de_getui16be(6);
	d->pixwidth = de_getui16be(8);
	d->pixheight = de_getui16be(10);
	de_dbg(c, "pixel size: %dx%d microns\n", (int)d->pixwidth, (int)d->pixheight);
	d->w = de_getui16be(12);
	d->h = de_getui16be(14);
	de_dbg(c, "dimensions: %dx%d\n", (int)d->w, (int)d->h);
	if(!de_good_image_dimensions(c, d->w, d->h)) goto done;

	d->rowspan_per_plane = (d->w+7)/8;
	d->rowspan_total = d->rowspan_per_plane * d->nplanes;

	if(d->is_ximg) {
		de_declare_fmt(c, "GEM VDI Bit Image, XIMG extension");
		do_gem_ximg(c, d);
	}
	else {
		de_declare_fmt(c, "GEM VDI Bit Image");
		do_gem_img(c, d);
	}

done:
	de_free(c, d);
}

static int de_identify_gemraster(deark *c)
{
	de_int64 ver, x2;
	de_int64 nplanes;

	if(!de_input_file_has_ext(c, "img") &&
		!de_input_file_has_ext(c, "ximg"))
	{
		return 0;
	}
	ver = de_getui16be(0);
	if(ver!=1 && ver!=2) return 0;
	x2 = de_getui16be(2);
	if(x2<0x0008 || x2>0x0800) return 0;
	nplanes = de_getui16be(4);
	if(!(nplanes>=1 && nplanes<=8) && nplanes!=16 && nplanes!=24) {
		return 0;
	}
	if(ver==1 && x2==0x08) return 70;
	if(!dbuf_memcmp(c->infile, 16, "XIMG", 4)) {
		return 100;
	}
	if(ver!=1) return 0;
	return 10;
}

void de_module_gemraster(deark *c, struct deark_module_info *mi)
{
	mi->id = "gemraster";
	mi->run_fn = de_run_gemraster;
	mi->identify_fn = de_identify_gemraster;
}
