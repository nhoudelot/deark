// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// Old Print Shop and PrintMaster formats

#include <deark-config.h>
#include <deark-modules.h>

// **************************************************************************
// The Print Shop .DAT/.NAM format
// **************************************************************************

#define PRINTSHOP_FMT_DAT 1
#define PRINTSHOP_FMT_POG 2
#define PRINTSHOP_FMT_SHP 3

typedef struct localctx_struct {
	int fmt;
	dbuf *namefile;
} lctx;

static void do_printshop_etc_image(deark *c, lctx *d, de_int64 imgnum,
	de_int64 pos, de_int64 *bytes_consumed)
{
	de_int64 width, height;
	de_int64 rowspan;
	de_int64 imgspan;
	de_int64 imgoffset = 0;
	de_finfo *fi = NULL;
	de_byte x;

	*bytes_consumed = 0;

	if(d->fmt==PRINTSHOP_FMT_SHP) {
		x = de_getbyte(pos);
		if(x!=0x0b) goto done; // No more images?
		height = (de_int64)de_getbyte(pos+1);
		width = (de_int64)de_getbyte(pos+2);
		if(width==0 || height==0) goto done;
		rowspan = (width+7)/8; // This is just a guess.
		imgoffset = 4;
		imgspan = 4 + rowspan * height + 1;
		if(pos+imgspan > c->infile->len) goto done;
	}
	else { // DAT or POG format
		width = 88;
		height = 52;
		rowspan = (width+7)/8;
		imgspan = rowspan * height;
		if(pos+imgspan > c->infile->len) goto done; // Reached end of file
	}

	de_dbg(c, "image[%d] at %d, %dx%d\n", (int)imgnum, (int)pos, (int)width, (int)height);

	fi = de_finfo_create(c);

	if(d->namefile) {
		de_finfo_set_name_from_slice(c, fi, d->namefile, imgnum*16, 16, DE_CONVFLAG_STOP_AT_NUL, DE_ENCODING_ASCII);
	}

	de_convert_and_write_image_bilevel(c->infile, pos+ imgoffset,
		width, height, rowspan, DE_CVTF_WHITEISZERO, fi);

	*bytes_consumed = imgspan;
done:
	de_finfo_destroy(c, fi);
}

static void do_printshop_etc(deark *c, lctx *d)
{
	de_int64 headersize = 0;
	const char *namefile_fn = NULL;
	de_int64 bytes_consumed;
	de_int64 pos;
	de_int64 img_count;

	namefile_fn = de_get_ext_option(c, "namefile");
	if(!namefile_fn) namefile_fn = de_get_ext_option(c, "file2");

	if(namefile_fn) {
		d->namefile = dbuf_open_input_file(c, namefile_fn);
	}
	if(d->namefile) {
		de_dbg(c, "Using name file: %s\n", namefile_fn);
	}

	if(d->fmt == PRINTSHOP_FMT_POG) {
		headersize = 10;
	}
	else {
		headersize = 0;
	}

	pos = headersize;
	img_count = 0;
	while(1) {
		if(pos >= c->infile->len) break;
		if(img_count >= DE_MAX_IMAGES_PER_FILE) break;
		do_printshop_etc_image(c, d, img_count, pos, &bytes_consumed);
		if(bytes_consumed<1) break;
		pos += bytes_consumed;
		img_count++;
	}

	dbuf_close(d->namefile);
	d->namefile = NULL;
}

static void de_run_printshop(deark *c, de_module_params *mparams)
{
	lctx *d;
	d = de_malloc(c, sizeof(lctx));
	d->fmt = PRINTSHOP_FMT_DAT;
	de_declare_fmt(c, "The Print Shop (DAT/NAM)");
	do_printshop_etc(c, d);
	de_free(c, d);
}

static int de_identify_printshop(deark *c)
{
	// TODO: Check to see if the base filename begins with "gr".
	// TODO: Check to see if the last [len mod 572] bytes of the file are either
	// 0x00 or 0x1a.

	if(de_input_file_has_ext(c, "dat")) {
		if((c->infile->len % 572)==0) {
			return 10;
		}
	}
	return 0;
}

void de_module_printshop(deark *c, struct deark_module_info *mi)
{
	mi->id = "printshop";
	mi->desc = "The Print Shop .DAT/.NAM";
	mi->run_fn = de_run_printshop;
	mi->identify_fn = de_identify_printshop;
}

// **************************************************************************
// The New Print Shop .POG/.PNM format
// **************************************************************************

static void de_run_newprintshop(deark *c, de_module_params *mparams)
{
	lctx *d;
	d = de_malloc(c, sizeof(lctx));
	d->fmt = PRINTSHOP_FMT_POG;
	de_declare_fmt(c, "The New Print Shop (POG/PNM)");
	do_printshop_etc(c, d);
	de_free(c, d);
}

static int de_identify_newprintshop(deark *c)
{
	if(de_input_file_has_ext(c, "pog")) {
		if((c->infile->len % 572)==10) {
			return 90;
		}
		else {
			return 10;
		}
	}
	return 0;
}

void de_module_newprintshop(deark *c, struct deark_module_info *mi)
{
	// There's no surefire way to distinguish between Print Shop and
	// New Print Shop files, so it's more convenient to put them in separate
	// modules (so the user can simply use -m to select the format).
	mi->id = "newprintshop";
	mi->desc = "The New Print Shop .POG/.PNM";
	mi->run_fn = de_run_newprintshop;
	mi->identify_fn = de_identify_newprintshop;
}

// **************************************************************************
// PrintMaster .SHP/.SDR format
// **************************************************************************

static void de_run_printmaster(deark *c, de_module_params *mparams)
{
	lctx *d;
	d = de_malloc(c, sizeof(lctx));
	d->fmt = PRINTSHOP_FMT_SHP;
	de_declare_fmt(c, "PrintMaster (SHP/SDR)");
	do_printshop_etc(c, d);
	de_free(c, d);
}

static int de_identify_printmaster(deark *c)
{
	de_byte b[4];
	int sdr_ext;

	sdr_ext = de_input_file_has_ext(c, "sdr");
	de_read(b, 0, 4);
	if(!de_memcmp(b, "\x0b\x34\x58", 3)) {
		return sdr_ext ? 90 : 10;
	}
	if(!sdr_ext) return 0;
	if(b[0]==0x0b) return 30;
	return 0;
}

void de_module_printmaster(deark *c, struct deark_module_info *mi)
{
	mi->id = "printmaster";
	mi->desc = "PrintMaster .SHP/.SDR";
	mi->run_fn = de_run_printmaster;
	mi->identify_fn = de_identify_printmaster;
}
