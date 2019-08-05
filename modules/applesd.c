// This file is part of Deark.
// Copyright (C) 2018 Jason Summers
// See the file COPYING for terms of use.

// AppleDouble, etc.

#include <deark-config.h>
#include <deark-private.h>
#include <deark-fmtutil.h>
DE_DECLARE_MODULE(de_module_applesingle);
DE_DECLARE_MODULE(de_module_appledouble);

typedef struct localctx_struct {
	u32 version;
	int is_appledouble;
	int input_encoding;
	int extract_rsrc;
	struct de_advfile *advf;
	i64 rsrc_fork_pos;
	i64 data_fork_pos;
} lctx;

struct entry_id_struct;

struct entry_struct {
	unsigned int idx;
	unsigned int id;
	i64 offset;
	i64 length;
	const struct entry_id_struct *eid;
};

typedef void (*handler_fn_type)(deark *c, lctx *d, struct entry_struct *e);

struct entry_id_struct {
	unsigned int id;
	const char *name;
	handler_fn_type hfn;
};

// Allocates a new de_stringreaderdata. Caller is responsible for destroying.
// len = the total number of bytes available
static struct de_stringreaderdata *read_pascal_string(deark *c, lctx *d, i64 pos, i64 len)
{
	struct de_stringreaderdata *srd;
	i64 slen;

	if(len>=1) {
		slen = (i64)de_getbyte(pos);
		de_dbg(c, "string len: %u", (unsigned int)slen);
	}
	else {
		slen = 0;
	}

	if(slen>(len-1) && slen>0) {
		de_warn(c, "Oversize string (need %d bytes, have %d)", (int)(1+slen), (int)len);
		slen = 0;
	}
	srd = dbuf_read_string(c->infile, pos+1, slen, slen, 0, d->input_encoding);
	return srd;
}

static void handler_string(deark *c, lctx *d, struct entry_struct *e)
{
	struct de_stringreaderdata *srd = NULL;

	// TODO: What is the format of the "Real name" and "Comment" fields?
	// The spec does not seem to say.

	srd = read_pascal_string(c, d, e->offset, e->length);
	de_dbg(c, "%s: \"%s\"", e->eid->name, ucstring_getpsz_d(srd->str));

	if(e->id==3 && srd->str->len>0) { // id 3 = real name
		ucstring_empty(d->advf->filename);
		ucstring_append_ucstring(d->advf->filename, srd->str);
		d->advf->original_filename_flag = 1;
		de_advfile_set_orig_filename(d->advf, (const u8*)srd->sz, (i64)de_strlen(srd->sz));
	}

	de_destroy_stringreaderdata(c, srd);
}

static void do_one_date(deark *c, lctx *d, i64 pos, const char *name,
	int is_modtime)
{
	i64 dt;
	char timestamp_buf[64];

	dt = de_geti32be(pos);
	if(dt == -0x80000000LL) {
		de_strlcpy(timestamp_buf, "unknown", sizeof(timestamp_buf));
	}
	else {
		struct de_timestamp ts;
		// Epoch is Jan 1, 2001. There are 30 years, with 7 leap days, between
		// that and the Unix time epoch.
		de_unix_time_to_timestamp(dt + ((365*30 + 7)*86400), &ts, 0x1);
		if(is_modtime) {
			d->advf->mainfork.fi->mod_time = ts;
		}
		de_timestamp_to_string(&ts, timestamp_buf, sizeof(timestamp_buf), 0);
	}
	de_dbg(c, "%s: %"I64_FMT" (%s)", name, dt, timestamp_buf);
}

static void handler_dates(deark *c, lctx *d, struct entry_struct *e)
{
	if(e->length<16) return;
	do_one_date(c, d, e->offset, "creation date", 0);
	do_one_date(c, d, e->offset+4, "mod date", 1);
	do_one_date(c, d, e->offset+8, "backup date", 0);
	do_one_date(c, d, e->offset+12, "access date", 0);
}

static void do_finder_orig(deark *c, lctx *d, struct entry_struct *e)
{
	i64 pos = e->offset;
	struct de_fourcc filetype;
	struct de_fourcc creator;

	dbuf_read_fourcc(c->infile, pos, &filetype, 4, 0x0);
	de_dbg(c, "filetype: '%s'", filetype.id_dbgstr);
	pos += 4;
	dbuf_read_fourcc(c->infile, pos, &creator, 4, 0x0);
	de_dbg(c, "creator: '%s'", creator.id_dbgstr);
	pos += 4;
}

static void do_xattr_entry(deark *c, lctx *d, struct de_stringreaderdata *name,
	i64 pos1, i64 len)
{
	if(pos1+len > c->infile->len) return;

	if(len>=8 && !dbuf_memcmp(c->infile, pos1, (const void*)"bplist00", 8)) {
		de_dbg(c, "binary plist");
		de_dbg_indent(c, 1);
		de_fmtutil_handle_plist(c, c->infile, pos1, len, NULL, 0);
		de_dbg_indent(c, -1);
	}
	else {
		de_dbg_hexdump(c, c->infile, pos1, len, 256, NULL, 0x1);
	}
}

static void do_finder_xattr(deark *c, lctx *d, struct entry_struct *e)
{
	i64 total_size;
	i64 data_start;
	i64 data_length;
	i64 num_attrs;
	i64 k;
	unsigned int flags;
	i64 pos = e->offset;
	int saved_indent_level;
	struct de_stringreaderdata *name = NULL;

	de_dbg_indent_save(c, &saved_indent_level);
	pos += 32; // original finder data

	// At this point, we are most likely at file offset 82, and there are
	// normally 2 padding bytes for alignment. (This is really a hybrid format
	// that violates the AppleDouble conventions.)
	// I don't know for sure what we should do if we're somehow not at an
	// offset such that (offset mod 4)==2.
	pos = de_pad_to_4(pos);

	de_dbg(c, "xattr table at %"I64_FMT, pos);
	de_dbg_indent(c, 1);
	pos += 4; // magic "ATTR"
	pos += 4; // debug_tag
	total_size = de_getu32be_p(&pos);
	de_dbg(c, "total size: %"I64_FMT, total_size);
	data_start = de_getu32be_p(&pos);
	de_dbg(c, "data start: %"I64_FMT, data_start);
	data_length = de_getu32be_p(&pos);
	de_dbg(c, "data length: %"I64_FMT, data_length);
	pos += 3*4; // reserved
	flags = (unsigned int)de_getu16be_p(&pos);
	de_dbg(c, "flags: 0x%04x", flags);
	num_attrs = de_getu16be_p(&pos);
	de_dbg(c, "num attrs: %d", (int)num_attrs);

	for(k=0; k<num_attrs; k++) {
		i64 entry_dpos, entry_dlen, entry_nlen;
		unsigned int entry_flags;

		// "Entries are aligned on 4 byte boundaries"
		pos = de_pad_to_4(pos);

		if(pos >= c->infile->len) goto done;

		// TODO:  but I don't know
		// what that means for the decoder.

		de_dbg(c, "xattr entry[%d] at %"I64_FMT, (int)k, pos);
		de_dbg_indent(c, 1);
		entry_dpos = de_getu32be_p(&pos);
		de_dbg(c, "dpos: %"I64_FMT, entry_dpos);
		entry_dlen = de_getu32be_p(&pos);
		de_dbg(c, "dlen: %"I64_FMT, entry_dlen);
		entry_flags = (unsigned int)de_getu16be_p(&pos);
		de_dbg(c, "flags: 0x%04x", entry_flags);
		entry_nlen = (i64)de_getbyte_p(&pos);

		if(name) {
			de_destroy_stringreaderdata(c, name);
		}
		name = dbuf_read_string(c->infile, pos, entry_nlen, entry_nlen,
			DE_CONVFLAG_STOP_AT_NUL, DE_ENCODING_UTF8);
		de_dbg(c, "name: \"%s\"", ucstring_getpsz_d(name->str));

		do_xattr_entry(c, d, name, entry_dpos, entry_dlen);
		pos += entry_nlen;
		de_dbg_indent(c, -1);
	}

done:
	de_destroy_stringreaderdata(c, name);
	de_dbg_indent_restore(c, saved_indent_level);
}

static void handler_finder(deark *c, lctx *d, struct entry_struct *e)
{
	int has_orig_finder_info = 0;
	int has_xattr = 0;

	if(e->length>=32 && (de_getbyte(e->offset) || de_getbyte(e->offset+4))) {
		has_orig_finder_info = 1;
	}
	if(e->length>=62 && !dbuf_memcmp(c->infile, e->offset+34, (const void*)"ATTR", 4)) {
		has_xattr = 1;
	}

	if(has_orig_finder_info) {
		do_finder_orig(c, d, e);
	}
	if(has_xattr) {
		do_finder_xattr(c, d, e);
	}
}

static void handler_data(deark *c, lctx *d, struct entry_struct *e)
{
	if(d->is_appledouble) {
		de_warn(c, "AppleDouble header files should not have a data fork.");
	}

	d->advf->mainfork.fork_exists = 1;
	d->data_fork_pos = e->offset;
	d->advf->mainfork.fork_len = e->length;
}

static void do_extract_rsrc(deark *c, lctx *d, struct entry_struct *e)
{
	de_finfo *fi = NULL;
	de_ucstring *fname = NULL;

	if(e->length<1) goto done;

	d->advf->rsrcfork.fork_exists = 1;
	d->rsrc_fork_pos = e->offset;
	d->advf->rsrcfork.fork_len = e->length;

done:
	de_finfo_destroy(c, fi);
	ucstring_destroy(fname);
}

static void do_decode_rsrc(deark *c, lctx *d, struct entry_struct *e)
{
	de_dbg(c, "decoding as resource format");
	de_dbg_indent(c, 1);
	de_run_module_by_id_on_slice2(c, "macrsrc", NULL, c->infile,
		e->offset, e->length);
	de_dbg_indent(c, -1);
}

static void handler_rsrc(deark *c, lctx *d, struct entry_struct *e)
{
	if(d->extract_rsrc) {
		do_extract_rsrc(c, d, e);
	}
	else {
		do_decode_rsrc(c, d, e);
	}
}

static const struct entry_id_struct entry_id_arr[] = {
	{1, "data fork", handler_data},
	{2, "resource fork", handler_rsrc},
	{3, "real name", handler_string},
	{4, "comment", handler_string},
	{5, "b/w icon", NULL},
	{6, "color icon", NULL},
	{8, "file dates", handler_dates},
	{9, "Finder info", handler_finder},
	{10, "Macintosh file info", NULL},
	{11, "ProDOS file info", NULL},
	{12, "MS-DOS file info", NULL},
	{13, "short name", NULL},
	{14, "AFP file info", NULL},
	{15, "directory ID", NULL}
};

static const struct entry_id_struct *find_entry_id_info(unsigned int id)
{
	size_t k;

	for(k=0; k<DE_ITEMS_IN_ARRAY(entry_id_arr); k++) {
		if(entry_id_arr[k].id==id) return &entry_id_arr[k];
	}
	return NULL;
}

static void do_sd_entry(deark *c, lctx *d, unsigned int idx, i64 pos1)
{
	struct entry_struct e;
	const struct entry_id_struct *eid;
	i64 pos = pos1;

	de_zeromem(&e, sizeof(struct entry_struct));
	e.idx = idx;
	e.id = (unsigned int)de_getu32be_p(&pos);
	eid =  find_entry_id_info(e.id);
	de_dbg(c, "id: %u (%s)", e.id, eid?eid->name:"?");
	e.offset = de_getu32be_p(&pos);
	de_dbg(c, "offset: %"I64_FMT, e.offset);
	e.length = de_getu32be_p(&pos);
	de_dbg(c, "length: %"I64_FMT, e.length);

	if(e.offset > c->infile->len) goto done;
	if(e.offset+e.length > c->infile->len) {
		de_warn(c, "Entry %u goes beyond end of file. Reducing size from %"I64_FMT
			" to %"I64_FMT".", e.idx, e.length, c->infile->len-e.offset);
		e.length = c->infile->len - e.offset;
	}

	if(eid && eid->hfn) {
		e.eid = eid;
		eid->hfn(c, d, &e);
	}

done:
	;
}

static int my_advfile_cbfn(deark *c, struct de_advfile *advf,
	struct de_advfile_cbparams *afp)
{
	lctx *d = (lctx*)advf->userdata;

	if(afp->whattodo == DE_ADVFILE_WRITEMAIN) {
		dbuf_copy(c->infile, d->data_fork_pos, advf->mainfork.fork_len, afp->outf);
	}
	else if(afp->whattodo == DE_ADVFILE_WRITERSRC) {
		dbuf_copy(c->infile, d->rsrc_fork_pos, advf->rsrcfork.fork_len, afp->outf);
	}
	return 1;
}

static void de_run_sd_internal(deark *c, lctx *d)
{
	i64 pos = 0;
	i64 nentries;
	i64 k;
	i64 entry_descriptors_pos;

	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_MACROMAN);

	d->advf = de_advfile_create(c);
	d->advf->userdata = (void*)d;
	d->advf->writefork_cbfn = my_advfile_cbfn;
	ucstring_append_sz(d->advf->filename, "bin", DE_ENCODING_LATIN1);

	pos += 4; // signature
	d->version = (u32)de_getu32be_p(&pos);
	de_dbg(c, "version: 0x%08x", (unsigned int)d->version);

	// For v1, this field is "Home file system" (TODO: Decode this.)
	// For v2, it is unused.
	pos += 16;

	nentries = de_getu16be_p(&pos);
	de_dbg(c, "number of entries: %d", (int)nentries);

	entry_descriptors_pos = pos;

	for(k=0; k<nentries; k++) {
		if(pos+12>c->infile->len) break;
		de_dbg(c, "entry[%u]", (unsigned int)k);
		de_dbg_indent(c, 1);
		do_sd_entry(c, d, (unsigned int)k, entry_descriptors_pos+12*k);
		de_dbg_indent(c, -1);
	}

	// There's no good reason to ever "convert" to AppleSingle. (We don't
	// have a way to combine forks that start out in separate files.)
	d->advf->no_applesingle = 1;

	if(!d->advf->mainfork.fork_exists || !d->advf->rsrcfork.fork_exists) {
		// If either fork does not exist, don't do anything fancy.
		// (If both exist, we allow conversion to AppleDouble.)
		d->advf->no_appledouble = 1;
	}

	de_advfile_run(d->advf);

	de_advfile_destroy(d->advf);
}

static void de_run_appledouble(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));
	d->is_appledouble = 1;
	d->extract_rsrc = de_get_ext_option_bool(c, "appledouble:extractrsrc", 1);
	de_run_sd_internal(c, d);
	de_free(c, d);
}

static int de_identify_appledouble(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\x00\x05\x16\x07", 4))
		return 100;
	return 0;
}

void de_module_appledouble(deark *c, struct deark_module_info *mi)
{
	mi->id = "appledouble";
	mi->desc = "AppleDouble Header file";
	mi->run_fn = de_run_appledouble;
	mi->identify_fn = de_identify_appledouble;
}

static void de_run_applesingle(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));
	d->is_appledouble = 0;
	d->extract_rsrc = 1;
	de_run_sd_internal(c, d);
	de_free(c, d);
}

static int de_identify_applesingle(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\x00\x05\x16\x00", 4))
		return 100;
	return 0;
}

void de_module_applesingle(deark *c, struct deark_module_info *mi)
{
	mi->id = "applesingle";
	mi->desc = "AppleSingle";
	mi->run_fn = de_run_applesingle;
	mi->identify_fn = de_identify_applesingle;
}
