// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// Amiga Workbench icons, including "Newicons" and "Glowicons" formats

#include <deark-config.h>
#include <deark-modules.h>

typedef struct localctx_struct {
	de_int64 main_width;
	de_int64 main_height;
	de_int64 icon_revision;
	de_byte icon_type;
	int has_drawerdata;
	int has_toolwindow;
	int has_defaulttool;
	int has_tooltypes;

	// Newicons-specific data
	de_byte pending_data;
	int pending_data_bits_used;
	int newicons_bits_per_pixel;
	int newicons_line_count;

	// Glowicons-specific data
	de_int64 glowicons_width, glowicons_height;
	de_uint32 glowicons_palette[256];
} lctx;

static const de_uint32 rev1pal[4] = { 0x55aaff,0x000000,0xffffff,0xff8800 }; // http://krashan.ppa.pl/articles/amigaicons/
//static const de_uint32 rev1pal[4] = { 0x0055aa,0x000020,0xffffff,0xff8a00 }; // Netpbm

static const de_uint32 rev2pal[4] = { 0x959595,0xffffff,0x000000,0x3b67a2 }; // http://krashan.ppa.pl/articles/amigaicons/
//static const de_uint32 rev2pal[4] = { 0xaaaaaa,0xffffff,0x000000,0x556699 }; // XnView

static const de_uint32 magicwbpal[8] = {
	0x959595,0x7b7b7b,0xffffff,0xaa907c,0x000000,0xafafaf,0x3b67a2,0xffa997 // http://krashan.ppa.pl/articles/amigaicons/ fixed? (& Wikipedia)
	//0xaaaaaa,0x999999,0xffffff,0xbbaa99,0x000000,0xbbbbbb,0x556699,0xffbbaa // XnView
};

static void do_newicons_append_bit(deark *c, lctx *d, dbuf *f, de_byte b)
{
	if(d->pending_data_bits_used==0) {
		d->pending_data = 0;
	}
	d->pending_data = (d->pending_data<<1) | b;
	d->pending_data_bits_used++;

	if(d->newicons_line_count==0) {
		// We're still reading palette samples, which are always 8 bits.
		if(d->pending_data_bits_used==8) {
			dbuf_writebyte(f, d->pending_data);
			d->pending_data_bits_used=0;
		}
		return;
	}

	if(d->pending_data_bits_used >= d->newicons_bits_per_pixel) {
		dbuf_writebyte(f, d->pending_data);
		d->pending_data_bits_used=0;
	}
}

// Decode one NewIcons image. (There are usually two such images per file.)
// The raw data from the relevant ToolTypes table items must first be
// written to 'f'.
static void do_decode_newicons(deark *c, lctx *d,
	dbuf *f, int newicons_num)
{
	de_byte trns_code, width_code, height_code;
	de_byte b0, b1, tmpb;
	struct deark_bitmap *img = NULL;
	int has_trns;
	de_int64 ncolors;
	dbuf *decoded = NULL;
	de_int64 srcpos;
	de_int64 bitmap_start_pos = 0;
	de_int64 i;
	de_int64 rle_len;
	de_uint32 pal[256];

	de_dbg(c, "decoding NewIcons[%d], size=%d\n", newicons_num,
		(int)f->len);
	de_dbg_indent(c, 1);

	trns_code = dbuf_getbyte(f, 0);
	has_trns = (trns_code=='B');
	width_code = dbuf_getbyte(f, 1);
	height_code = dbuf_getbyte(f, 2);

	b0 = dbuf_getbyte(f, 3);
	b1 = dbuf_getbyte(f, 4);
	ncolors = ((((de_int64)b0)-0x21)<<6) + (((de_int64)b1)-0x21);
	if(ncolors<1) ncolors=1;
	if(ncolors>256) ncolors=256;

	img = de_bitmap_create_noinit(c);
	img->width = (de_int64)width_code - 0x21;
	img->height = (de_int64)height_code - 0x21;
	img->bytes_per_pixel = 4;

	de_dbg(c, "dimensions=%dx%d, transparency=%d, colors=%d\n",
		(int)img->width, (int)img->height, has_trns, (int)ncolors);

	decoded = dbuf_create_membuf(c, 2048, 0);

	d->pending_data = 0;
	d->pending_data_bits_used = 0;

	d->newicons_bits_per_pixel = (int)de_log2_rounded_up(ncolors);

	// We decode both the palette and the bitmap into the same buffer, and
	// keep track of where in the buffer the bitmap starts.

	// Count the number of lines (EOL represented by 0x00 byte).
	// This is only needed because the bitmap starts on the second line.
	d->newicons_line_count=0;

	for(srcpos=5; srcpos<f->len; srcpos++) {
		b0 = f->membuf_buf[srcpos];
		if((b0>=0x20 && b0<=0x6f) || (b0>=0xa1 && b0<=0xd0)) {
			if(b0<=0x6f) b1 = b0-0x20;
			else b1 = 0x50 + (b0-0xa1);

			for(i=0; i<7; i++) {
				tmpb = (b1>>(6-i))&0x01;
				do_newicons_append_bit(c, d, decoded, tmpb);
			}
		}
		else if(b0>=0xd1) {
			// RLE compression for "0" bits
			tmpb = 0;
			rle_len = 7*(de_int64)(b0-0xd0);
			for(i=0; i<rle_len; i++) {
				do_newicons_append_bit(c, d, decoded, tmpb);
			}
		}
		else if(b0==0x00) {
			// End of a line.
			// Throw away any bits we've decoded that haven't been used yet.
			d->pending_data_bits_used = 0;

			if(d->newicons_line_count==0) {
				// The bitmap will start at this position. Remember that.
				bitmap_start_pos = decoded->len;
			}
			d->newicons_line_count++;
		}
	}

	de_dbg2(c, "decoded %d bytes\n", (int)decoded->len);

	// The first ncolors*3 bytes are the palette
	de_dbg2(c, "NewIcons palette\n");
	de_dbg_indent(c, 1);
	for(i=0; i<ncolors; i++) {
		if(i>255) break;
		pal[i] = dbuf_getRGB(decoded, i*3, 0);

		// Educated guess: If the transparency flag is set, it means
		// palette entry 0 is transparent.
		if(i==0 && has_trns)
			pal[i] = DE_SET_ALPHA(pal[i], 0x00);

		de_dbg_pal_entry(c, i, pal[i]);
	}
	de_dbg_indent(c, -1);

	de_convert_image_paletted(decoded, bitmap_start_pos,
		8, img->width, pal, img, 0);
	de_bitmap_write_to_file(img, "n");

	if(decoded) dbuf_close(decoded);
	if(img) de_bitmap_destroy(img);
	de_dbg_indent(c, -1);
}

static int do_read_main_icon(deark *c, lctx *d,
	de_int64 pos, de_int64 icon_index, de_int64 *pbytesused)
{
	de_int64 width, height;
	de_int64 depth;
	de_int64 src_rowspan, src_planespan;
	de_int64 i, j, plane;
	int retval = 0;
	struct deark_bitmap *img = NULL;
	de_byte b, b1;
	de_uint32 pal[256];

	*pbytesused = 0;

	de_dbg(c, "main icon[%d] at %d\n", (int)icon_index, (int)pos);
	de_dbg_indent(c, 1);

	// 20-byte header, followed by one or more bitmap "planes".
	width = de_getui16be(pos+4);
	height = de_getui16be(pos+6);
	depth = de_getui16be(pos+8);
	de_dbg(c, "dimensions=%dx%d, depth=%d\n", (int)width, (int)height,
		(int)depth);

	if(depth<1 || depth>8) {
		de_err(c, "Unsupported bit depth (%d)\n", (int)depth);
		goto done;
	}

	src_rowspan = ((width+15)/16)*2;
	src_planespan = src_rowspan * height;

	// Set this so we remember where to look for the next icon.
	*pbytesused = 20 + src_planespan * depth;

	img = de_bitmap_create(c, width, height, 3);

	// Figure out what palette to use
	for(i=0; i<256; i++) {
		// Start with a meaningless grayscale palette.
		pal[i] = 0x010101 * (de_uint32)i;
	}
	if(d->icon_revision==0 && depth==2) {
		for(i=0; i<4; i++) pal[i] = rev1pal[i];
	}
	else if(depth==2) {
		for(i=0; i<4; i++) pal[i] = rev2pal[i];
	}
	else if(depth==3) {
		for(i=0; i<8; i++) pal[i] = magicwbpal[i];
	}
	else if(depth==8) {
		// Don't ask me. Just doing what other apps seem to do.
		for(i=0; i<256; i++) pal[i] = magicwbpal[i>>5];
	}

	pos += 20;

	for(j=0; j<height; j++) {
		for(i=0; i<width; i++) {
			b = 0x00;
			for(plane=0; plane<depth; plane++) {
				b1 = de_get_bits_symbol(c->infile, 1, pos+plane*src_planespan + j*src_rowspan, i);
				b = (b<<1) | b1;
			}
			de_bitmap_setpixel_rgb(img, i, j, pal[b]);
		}
	}

	de_bitmap_write_to_file(img, NULL);

	retval = 1;

done:
	de_bitmap_destroy(img);
	de_dbg_indent(c, -1);
	return retval;
}

static int do_read_tooltypes_table(deark *c, lctx *d,
	de_int64 orig_pos, de_int64 *pbytesused)
{
	de_int64 num_entries_raw;
	de_int64 num_entries;
	dbuf *newicons_data[2];
	int retval = 0;
	de_int64 i;
	de_int64 len;
	de_byte buf[4];
	int newicons_num;
	de_int64 pos, tpos;

	pos = orig_pos;
	newicons_data[0] = NULL;
	newicons_data[1] = NULL;

	de_dbg(c, "tool types table at %d\n", (int)pos);
	de_dbg_indent(c, 1);

	num_entries_raw = de_getui32be(pos);
	num_entries = num_entries_raw/4 - 1;
	de_dbg(c, "number of tool types: %d\n", (int)num_entries);
	pos+=4;
	if(num_entries<0 || num_entries>1000) {
		goto done;
	}

	for(i=0; i<num_entries; i++) {
		len = de_getui32be(pos);
		pos+=4;
		if(len>10000) {
			de_err(c, "Bad ToolTypes data\n");
			goto done;
		}
		tpos=pos; // Remember where the text starts
		pos+=len;
		if(len<5) {
			// Too small to contain NewIcons data.
			continue;
		}

		de_read(buf, tpos, 4);
		newicons_num = -1;
		if(buf[0]=='I' && buf[1]=='M' && buf[3]=='=') {
			if(buf[2]=='1') newicons_num = 0;
			else if(buf[2]=='2') newicons_num = 1;
		}
		if(newicons_num == -1) {
			continue;
		}

		de_dbg2(c, "NewIcons data [%d] size=%d pos=%d\n", newicons_num, (int)len, (int)tpos);

		if(!newicons_data[newicons_num]) {
			newicons_data[newicons_num] = dbuf_create_membuf(c, 2048, 0);
			// The data we copy includes the terminating NUL.
		}
		dbuf_copy(c->infile, tpos+4, len-4, newicons_data[newicons_num]);
	}

	for(newicons_num=0; newicons_num<2; newicons_num++) {
		if(newicons_data[newicons_num]) {
			do_decode_newicons(c, d, newicons_data[newicons_num], newicons_num);
		}
	}

	retval = 1;
done:
	*pbytesused = pos - orig_pos;
	if(newicons_data[0]) dbuf_close(newicons_data[0]);
	if(newicons_data[1]) dbuf_close(newicons_data[1]);
	de_dbg_indent(c, -1);
	return retval;
}

// Uncompress a slice of f, and append to outf.
// The algorithm is the same as PackBits, except that the data elements may
// be less than 8 bits.
static void glowdata_uncompress(dbuf *f, de_int64 pos, de_int64 len,
	dbuf *outf, int bits_per_pixel)
{
	de_int64 x;
	de_int64 i;
	de_byte b, b2;
	de_int64 bitpos;

	bitpos = 0;

	// Continue as long as at least 8 bits remain.
	while(bitpos <= (len-1)*8) {
		b = de_get_bits_symbol2(f, 8, pos, bitpos);
		bitpos+=8;

		if(b<=127) {
			// 1+b literal pixels
			x = 1+(de_int64)b;
			for(i=0; i<x; i++) {
				b2 = de_get_bits_symbol2(f, bits_per_pixel, pos, bitpos);
				bitpos += bits_per_pixel;
				dbuf_writebyte(outf, b2);
			}
		}
		else if(b>=129) {
			// 257-b repeated pixels
			x = 257 - (de_int64)b;
			b2 = de_get_bits_symbol2(f, bits_per_pixel, pos, bitpos);
			bitpos += bits_per_pixel;
			for(i=0; i<x; i++) {
				dbuf_writebyte(outf, b2);
			}
		}
	}
}

static void do_glowicons_IMAG(deark *c, lctx *d,
	de_int64 pos, de_int64 len)
{
	struct deark_bitmap *img = NULL;
	de_byte trns_color;
	de_int64 num_colors;
	de_byte flags;
	int has_trns;
	int has_palette;
	de_byte cmpr_type;
	de_byte pal_cmpr_type = 0;
	de_int64 bits_per_pixel;
	de_int64 image_size_in_bytes;
	de_int64 pal_size_in_bytes;
	de_int64 image_pos;
	de_int64 pal_pos;
	de_int64 k;
	dbuf *tmpbuf = NULL;

	if(d->glowicons_width<1) {
		// We must not have found a FACE chunk yet.
		de_err(c, "Invalid GlowIcons data\n");
		goto done;
	}

	trns_color = de_getbyte(pos);
	de_dbg(c, "transparent color: 0x%02x\n", (int)trns_color);
	num_colors = 1+(int)de_getbyte(pos+1);
	de_dbg(c, "number of colors: %d\n", (int)num_colors);
	flags = de_getbyte(pos+2);
	has_trns = (flags & 0x01)?1:0;
	has_palette = (flags & 0x02)?1:0;
	de_dbg(c, "has transparency: %d\n", has_trns);
	de_dbg(c, "has palette: %d\n", has_palette);

	cmpr_type = de_getbyte(pos+3);
	de_dbg(c, "compression type: %d\n", cmpr_type);
	if(cmpr_type!=0 && cmpr_type!=1) {
		de_err(c, "Unsupported compression type\n");
		goto done;
	}

	if(cmpr_type!=1) {
		// TODO uncompressed images (Need sample files. I don't know how
		// they are structured.)
		de_err(c, "Uncompressed images are not supported\n");
		goto done;
	}

	if(has_palette) {
		pal_cmpr_type = de_getbyte(pos+4);
		de_dbg(c, "palette compression type: %d\n", pal_cmpr_type);
		if(pal_cmpr_type!=0 && pal_cmpr_type!=1) {
			de_err(c, "Unsupported palette compression type\n");
			goto done;
		}
	}

	bits_per_pixel = (de_int64)de_getbyte(pos+5);
	de_dbg(c, "bits per pixel: %d\n", (int)bits_per_pixel);

	if(bits_per_pixel<1 || bits_per_pixel>8) {
		de_err(c, "Invalid or unsupported bits per pixel (%d)\n", (int)bits_per_pixel);
		goto done;
	}

	image_size_in_bytes = 1+de_getui16be(pos+6);
	pal_size_in_bytes = 1+de_getui16be(pos+8);
	pos+=10;

	tmpbuf = dbuf_create_membuf(c, 10240, 0);

	image_pos = pos;
	pal_pos = image_pos+image_size_in_bytes;
	de_dbg(c, "image data at %d, len=%d\n", (int)image_pos, (int)image_size_in_bytes);

	if(has_palette) {
		de_dbg(c, "palette data at %d, len=%d\n", (int)pal_pos, (int)pal_size_in_bytes);
		de_dbg_indent(c, 1);

		if(pal_cmpr_type==1) {
			glowdata_uncompress(c->infile, pal_pos, pal_size_in_bytes, tmpbuf, 8);
		}
		else {
			dbuf_copy(c->infile, pal_pos, pal_size_in_bytes, tmpbuf);
		}

		for(k=0; k<256; k++) {
			if(k<num_colors) {
				d->glowicons_palette[k] = dbuf_getRGB(tmpbuf, k*3, 0);
				if(has_trns && k==(de_int64)trns_color) {
					d->glowicons_palette[k] = DE_SET_ALPHA(d->glowicons_palette[k], 0x00);
				}
				de_dbg_pal_entry(c, k, d->glowicons_palette[k]);
			}
			else {
				d->glowicons_palette[k] = DE_STOCKCOLOR_BLACK;
			}
		}

		de_dbg_indent(c, -1);
	}

	// Uncompress the pixels
	dbuf_empty(tmpbuf);
	glowdata_uncompress(c->infile, image_pos, image_size_in_bytes, tmpbuf, (int)bits_per_pixel);

	img = de_bitmap_create(c, d->glowicons_width, d->glowicons_height, has_trns?4:3);

	de_convert_image_paletted(tmpbuf, 0,
		8, d->glowicons_width, d->glowicons_palette, img, 0);

	de_bitmap_write_to_file(img, "g");

done:
	if(tmpbuf) dbuf_close(tmpbuf);
	if(img) de_bitmap_destroy(img);
}

static void do_glowicons(deark *c, lctx *d, de_int64 pos)
{
	de_int64 gsize;
	de_int64 startpos;
	de_int64 endpos;
	de_int64 len;
	de_byte chunk_id_buf[4];
	char chunk_id_printable[8];
	de_uint32 chunk_id;
	de_uint32 form_type;
	int indent_count = 0;

	gsize = c->infile->len - pos;
	if(gsize < 24) goto done; // too small

// Chunk types:
#define CODE_FORM 0x464f524dU
#define CODE_FACE 0x46414345U
#define CODE_IMAG 0x494d4147U
// FORM types:
#define CODE_ICON 0x49434f4eU

	chunk_id = (de_uint32)de_getui32be(pos);
	form_type = (de_uint32)de_getui32be(pos+8);
	if(chunk_id!=CODE_FORM || form_type!=CODE_ICON) {
		de_warn(c, "Extra data found at end of file, but not identified as GlowIcons format.");
		goto done;
	}

	startpos = pos;

	len = de_getui32be(pos+4);
	endpos = startpos + 8 + len;
	if(len%2) endpos++;

	de_dbg(c, "GlowIcons data at offset %d (%d bytes)\n", (int)startpos, (int)len);
	de_dbg_indent(c, 1);
	indent_count++;

	de_dbg(c, "expected end of file: %d\n", (int)endpos);
	pos+=12; // Skip past the "FORM" id, length, and FORM type code

	while(pos < endpos) {
		de_read(chunk_id_buf, pos, 4);
		chunk_id = (de_uint32)de_getui32be_direct(chunk_id_buf);
		de_make_printable_ascii(chunk_id_buf, 4, chunk_id_printable, sizeof(chunk_id_printable), 0);
		len = de_getui32be(pos+4);

		de_dbg(c, "chunk '%s' at %d, dlen=%d\n", chunk_id_printable, (int)pos, (int)len);
		pos+=8;

		de_dbg_indent(c, 1);
		indent_count++;

		switch(chunk_id) {
		case CODE_FACE: // FACE (parameters)
			d->glowicons_width = 1+(de_int64)de_getbyte(pos);
			d->glowicons_height = 1+(de_int64)de_getbyte(pos+1);
			de_dbg(c, "dimensions: %dx%d\n", (int)d->glowicons_width, (int)d->glowicons_height);
			break;
		case CODE_IMAG: // IMAG (one of the images that make up this icon)
			do_glowicons_IMAG(c, d, pos, len);
			break;
		}

		de_dbg_indent(c, -1);
		indent_count--;

		pos += len;
		if(len%2) pos++; // skip padding byte
	}

done:
	de_dbg_indent(c, -indent_count);
}

static void de_run_amigaicon(deark *c, de_module_params *mparams)
{
	lctx *d;
	de_int64 num_main_icons;
	de_int64 pos;
	de_int64 i;
	de_int64 x;
	de_int64 bytesused;
	de_int64 version;
	int indent_count = 0;

	d = de_malloc(c, sizeof(lctx));

	de_dbg(c, "DiskObject at %d\n", 0);
	de_dbg_indent(c, 1);
	indent_count++;

	version = de_getui16be(2);
	de_dbg(c, "version: %d\n", (int)version);

	d->main_width = de_getui16be(12);
	d->main_height = de_getui16be(14);
	de_dbg(c, "main canvas size: %dx%d\n", (int)d->main_width, (int)d->main_height);

	num_main_icons = (de_getui32be(26)==0) ? 1 : 2; // "SelectRender" field
	de_dbg(c, "number of (original) icons: %d\n", (int)num_main_icons);

	d->icon_revision = de_getui32be(44) & 0xff;
	de_dbg(c, "icon revision: %d\n", (int)d->icon_revision);

	d->icon_type = de_getbyte(48);
	de_dbg(c, "icon type: %d\n", (int)d->icon_type);

	d->has_defaulttool = (de_getui32be(50)!=0);
	d->has_tooltypes = (de_getui32be(54)!=0);
	d->has_drawerdata = (de_getui32be(66)!=0);
	d->has_toolwindow = (de_getui32be(70)!=0);

	de_dbg_indent(c, -1);
	indent_count--;

	pos = 78;

	// Skip the DrawerData segment
	if(d->has_drawerdata) {
		de_dbg(c, "DrawerData: 56 bytes at %d\n", (int)pos);
		pos+=56;
	}

	// Read the main (original-style) icons
	for(i=0; i<num_main_icons; i++) {
		if(!do_read_main_icon(c, d, pos, i, &bytesused))
			goto done;
		pos += bytesused;
	}

	// Skip the DefaultTool segment
	if(d->has_defaulttool) {
		x = de_getui32be(pos);
		de_dbg(c, "DefaultTool: %d bytes at %d\n", (int)(4+x), (int)pos);
		pos += 4+x;
	}

	if(d->has_tooltypes) {
		// This will also read NewIcons-style icons.
		if(!do_read_tooltypes_table(c, d, pos, &bytesused))
			goto done;
		pos += bytesused;
	}

	// Skip the ToolWindow segment (untested)
	if(d->has_toolwindow) {
		x = de_getui32be(pos);
		de_dbg(c, "ToolWindow: %d bytes at %d\n", (int)(4+x), (int)pos);
		pos += 4+x;
	}

	// Skip DrawerData2
	if(d->has_drawerdata && d->icon_revision==1) {
		de_dbg(c, "DrawerData2: 6 bytes at %d\n", (int)pos);
		pos += 6;
	}

	do_glowicons(c, d, pos);

done:
	de_dbg_indent(c, -indent_count);
	de_free(c, d);
}

static int de_identify_amigaicon(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\xe3\x10", 2))
		return 90;
	return 0;
}

void de_module_amigaicon(deark *c, struct deark_module_info *mi)
{
	mi->id = "amigaicon";
	mi->desc = "Amiga Workbench icon (.info), NewIcons, GlowIcons";
	mi->run_fn = de_run_amigaicon;
	mi->identify_fn = de_identify_amigaicon;
}
