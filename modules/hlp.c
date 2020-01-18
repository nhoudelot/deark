// This file is part of Deark.
// Copyright (C) 2017 Jason Summers
// See the file COPYING for terms of use.

// Windows HLP

#include <deark-config.h>
#include <deark-private.h>
#include <deark-fmtutil.h>
DE_DECLARE_MODULE(de_module_hlp);

enum hlp_filetype {
	FILETYPE_UNKNOWN = 0,
	FILETYPE_INTERNALDIR,
	FILETYPE_SYSTEM,
	FILETYPE_TOPIC,
	FILETYPE_SHG,
	FILETYPE_BMP,
	FILETYPE_PHRASES,
	FILETYPE_PHRINDEX,
	FILETYPE_PHRIMAGE,
	FILETYPE_TOMAP
};

struct bptree {
	unsigned int flags;
	i64 pagesize;
	i64 root_page;
	i64 num_levels;
	i64 num_pages;
	i64 num_entries;
	i64 pagesdata_pos;
	i64 first_leaf_page;
};

typedef struct localctx_struct {
	int input_encoding;
	int extract_text;
	i64 internal_dir_FILEHEADER_offs;
	struct bptree bpt;
	u8 found_system_file;
	u8 found_Phrases_file;
	u8 found_PhrIndex_file;
	u8 found_PhrImage_file;
	u8 phrase_compression_warned;
	int ver_major;
	int ver_minor;
	i64 topic_block_size;
	int is_compressed;
	int pass;
	int has_shg, has_ico, has_bmp;
	i64 internal_dir_num_levels;
	dbuf *outf_text;
	i64 offset_of_Phrases;
} lctx;

static void do_file(deark *c, lctx *d, i64 pos1, enum hlp_filetype file_fmt);

struct systemrec_info {
	unsigned int rectype;

	// low 8 bits = version info
	// 0x0010 = STRINGZ type
	unsigned int flags;

	const char *name;
	void *reserved;
};
static const struct systemrec_info systemrec_info_arr[] = {
	{ 1,  0x0010, "Title", NULL },
	{ 2,  0x0010, "Copyright", NULL },
	{ 3,  0x0000, "Contents", NULL },
	{ 4,  0x0010, "Macro", NULL },
	{ 5,  0x0000, "Icon", NULL },
	{ 6,  0x0000, "Window", NULL },
	{ 8,  0x0010, "Citation", NULL },
	{ 9,  0x0000, "Language ID", NULL },
	{ 10, 0x0010, "CNT file name", NULL },
	{ 11, 0x0000, "Charset", NULL },
	{ 12, 0x0000, "Default dialog font", NULL },
	{ 13, 0x0010, "Defined GROUPs", NULL },
	{ 14, 0x0011, "IndexSeparators separators", NULL },
	{ 14, 0x0002, "Multimedia Help Files", NULL },
	{ 18, 0x0010, "Defined language", NULL },
	{ 19, 0x0000, "Defined DLLMAPS", NULL }
};
static const struct systemrec_info systemrec_info_default =
	{ 0, 0x0000, "?", NULL };

// "compressed unsigned short" - a variable-length integer format
// TODO: This is duplicated in shg.c
static i64 get_cus(dbuf *f, i64 *pos)
{
	i64 x1, x2;

	x1 = (i64)dbuf_getbyte(f, *pos);
	*pos += 1;
	if(x1%2 == 0) {
		// If it's even, divide by two.
		return x1>>1;
	}
	// If it's odd, divide by two, and add 128 times the value of
	// the next byte.
	x2 = (i64)dbuf_getbyte(f, *pos);
	*pos += 1;
	return (x1>>1) | (x2<<7);
}

// "compressed signed short"
static i64 get_css(dbuf *f, i64 *ppos)
{
	i64 x1, x2;

	x1 = (i64)dbuf_getbyte_p(f, ppos);
	if(x1%2 == 0) {
		// If it's even, divide by two, and subtract 64
		return (x1>>1) - 64;
	}
	// If it's odd, divide by two, add 128 times the value of
	// the next byte, and subtract 16384.
	x1 >>= 1;
	x2 = (i64)dbuf_getbyte_p(f, ppos);
	x1 += x2 * 128;
	x1 -= 16384;
	return x1;
}

// "compressed signed long"
static i64 get_csl(dbuf *f, i64 *ppos)
{
	i64 x1, x2;

	x1 = dbuf_getu16le_p(f, ppos);

	if(x1%2 == 0) {
		// If it's even, divide by two, and subtract 16384
		return (x1>>1) - 16384;
	}
	// If it's odd, divide by two, add 32768 times the value of
	// the next two bytes, and subtract 67108864.
	x1 >>= 1;
	x2 = dbuf_getu16le_p(f, ppos);
	x1 += x2*32768;
	x1 -= 67108864;
	return x1;
}

static void hlptime_to_timestamp(i64 ht, struct de_timestamp *ts)
{
	if(ht!=0) {
		// This appears to be a Unix-style timestamp, though some documentation
		// says otherwise.
		de_unix_time_to_timestamp(ht, ts, 0);
	}
	else {
		de_zeromem(ts, sizeof(struct de_timestamp));
	}
}

static void do_display_STRINGZ(deark *c, lctx *d, i64 pos1, i64 len,
	const char *name)
{
	de_ucstring *s = NULL;

	if(len<1) return;
	s = ucstring_create(c);
	dbuf_read_to_ucstring_n(c->infile,
		pos1, len, DE_DBG_MAX_STRLEN,
		s, DE_CONVFLAG_STOP_AT_NUL, d->input_encoding);
	de_dbg(c, "%s: \"%s\"", name, ucstring_getpsz(s));
	ucstring_destroy(s);
}

static void do_SYSTEMREC_STRINGZ(deark *c, lctx *d, unsigned int recordtype,
	i64 pos1, i64 len, const struct systemrec_info *sti)
{
	do_display_STRINGZ(c, d, pos1, len, sti->name);
}

static void do_SYSTEMREC(deark *c, lctx *d, unsigned int recordtype,
	i64 pos1, i64 len, const struct systemrec_info *sti)
{
	if(recordtype==5) { // Icon
		d->has_ico = 1;
		dbuf_create_file_from_slice(c->infile, pos1, len, "ico", NULL, DE_CREATEFLAG_IS_AUX);
	}
	else if(sti->flags&0x10) {
		do_SYSTEMREC_STRINGZ(c, d, recordtype, pos1, len, sti);
	}
	else {
		if(c->debug_level>=2) {
			de_dbg_hexdump(c, c->infile, pos1, len, 256, NULL, 0x1);
		}
	}
}

static const struct systemrec_info *find_sysrec_info(deark *c, lctx *d, unsigned int t)
{
	size_t i;

	for(i=0; i<DE_ARRAYCOUNT(systemrec_info_arr); i++) {
		const struct systemrec_info *sti;
		sti = &systemrec_info_arr[i];
		if(sti->rectype==t &&
			(sti->flags&0x0f)==0)
		{
			return sti;
		}
	}
	return &systemrec_info_default;
}

static int do_file_SYSTEM_header(deark *c, lctx *d, i64 pos1)
{
	i64 pos = pos1;
	i64 magic;
	i64 gen_date;
	unsigned int flags;
	struct de_timestamp ts;
	char timestamp_buf[64];
	int retval = 0;

	magic = de_getu16le_p(&pos);
	if(magic!=0x036c) {
		de_err(c, "Expected SYSTEM data at %d not found", (int)pos1);
		goto done;
	}

	de_dbg(c, "SYSTEM file data at %d", (int)pos1);
	de_dbg_indent(c, 1);

	d->ver_minor = (int)de_getu16le_p(&pos);
	d->ver_major = (int)de_getu16le_p(&pos);
	de_dbg(c, "help format version: %d.%d", d->ver_major, d->ver_minor);

	if(d->ver_major!=1) {
		de_err(c, "Unsupported file version: %d.%d", d->ver_major, d->ver_minor);
		goto done;
	}

	gen_date = de_geti32le_p(&pos);
	hlptime_to_timestamp(gen_date, &ts);
	de_timestamp_to_string(&ts, timestamp_buf, sizeof(timestamp_buf), 0);
	de_dbg(c, "GenDate: %d (%s)", (int)gen_date, timestamp_buf);

	flags = (unsigned int)de_getu16le_p(&pos);
	de_dbg(c, "flags: 0x%04x", flags);

	if(d->ver_minor>16) {
		if(flags==8) {
			d->is_compressed = 1;
			d->topic_block_size = 2048;
		}
		else if(flags==4) {
			d->is_compressed = 1;
			d->topic_block_size = 4096;
		}
		else {
			d->is_compressed = 0;
			d->topic_block_size = 4096;
		}
	}
	else {
		d->is_compressed = 0;
		d->topic_block_size = 2048;
	}
	de_dbg(c, "compressed: %d", d->is_compressed);
	de_dbg(c, "topic block size: %d", (int)d->topic_block_size);

	retval = 1;
done:
	return retval;
}

static void do_file_SYSTEM_SYSTEMRECS(deark *c, lctx *d, i64 pos1, i64 len,
	int systemrecs_pass)
{
	i64 pos = pos1;

	while((pos1+len)-pos >=4) {
		unsigned int recordtype;
		i64 datasize;
		i64 systemrec_startpos;
		const struct systemrec_info *sti;

		systemrec_startpos = pos;

		recordtype = (unsigned int)de_getu16le_p(&pos);
		datasize = de_getu16le_p(&pos);

		sti = find_sysrec_info(c, d, recordtype);
		de_dbg(c, "SYSTEMREC type %u (%s) at %d, dpos=%d, dlen=%d",
			recordtype, sti->name,
			(int)systemrec_startpos, (int)pos, (int)datasize);

		if(pos+datasize > pos1+len) break; // bad data
		de_dbg_indent(c, 1);
		do_SYSTEMREC(c, d, recordtype, pos, datasize, sti);
		de_dbg_indent(c, -1);
		pos += datasize;
	}
}

static void do_file_SYSTEM(deark *c, lctx *d, i64 pos1, i64 len)
{
	i64 pos = pos1;
	int saved_indent_level;

	de_dbg_indent_save(c, &saved_indent_level);

	// We'll read the SYSTEM "file" only in pass 1, most importantly to record
	// the format version information.
	//
	// The SYSTEM file may contain a series of SYSTEMREC records that we want
	// to parse. We might [someday] have to make two (sub)passes over the
	// SYSTEMREC records, the first pass to collect "charset" setting, so it
	// can be used when parsing the other SYSTEMREC records.
	// (We can do it this way because there doesn't seem to be anything in the
	// SYSTEM header that would require knowing the charset.)

	if(d->pass!=1) goto done;
	d->found_system_file = 1;

	if(!do_file_SYSTEM_header(c, d, pos)) goto done;
	pos += 12;

	if(d->ver_minor<=16) {
		do_display_STRINGZ(c, d, pos, (pos1+len)-pos, "HelpFileTitle");
	}
	else {
		// A sequence of variable-sized SYSTEMRECs
		do_file_SYSTEM_SYSTEMRECS(c, d, pos, (pos1+len)-pos, 1);
	}

done:
	de_dbg_indent_restore(c, saved_indent_level);
}

static void do_file_SHG(deark *c, lctx *d, i64 pos1, i64 used_space)
{
	i64 num_images;
	i64 sig;
	const char *ext;
	dbuf *outf = NULL;

	// Ignore the file SHG vs. MRB file type signature, and replace it with
	// the correct one based on the number of images in the file.
	num_images = de_getu16le(pos1+2);
	if(num_images>1) {
		ext="mrb";
		sig = 0x706c;
	}
	else {
		ext="shg";
		sig = 0x506c;
	}

	outf = dbuf_create_output_file(c, ext, NULL, 0);
	dbuf_writeu16le(outf, sig);
	dbuf_copy(c->infile, pos1+2, used_space-2, outf);
	dbuf_close(outf);
}

struct topiclink_data {
	i64 blocksize;
	i64 datalen2;
	i64 prevblock;
	i64 nextblock;
	i64 datalen1;
	u8 recordtype;

	i64 linkdata1_pos;
	i64 linkdata1_len;
	i64 linkdata2_pos;
	i64 linkdata2_len;

	i64 paragraphinfo_pos;
	u8 seems_compressed;
};

static int do_topiclink_rectype_32_linkdata1(deark *c, lctx *d,
	struct topiclink_data *tld, dbuf *inf)
{
	i64 pos = tld->linkdata1_pos;
	i64 topicsize;
	i64 topiclength;
	unsigned int id;
	unsigned int bits;
	int retval = 0;

	// TODO: type 33 (table)
	if(tld->recordtype!=1 && tld->recordtype!=32) goto done;

	topicsize = get_csl(inf, &pos);
	de_dbg(c, "topic size: %"I64_FMT, topicsize);

	if(tld->recordtype==32) {
		topiclength = get_cus(inf, &pos);
		de_dbg(c, "topic length: %"I64_FMT, topiclength);
	}

	pos++; // unknownUnsignedChar
	pos++; // unknownBiasedChar
	id = (unsigned int)dbuf_getu16le_p(inf, &pos);
	de_dbg(c, "id: %u", id);
	bits = (unsigned int)dbuf_getu16le_p(inf, &pos);
	de_dbg(c, "bits: 0x%04x", bits);

	if(bits & 0x0001) { // Unknown
		(void)get_csl(inf, &pos);
	}
	if(bits & 0x0002) { // SpacingAbove
		(void)get_css(inf, &pos);
	}
	if(bits & 0x0004) { // SpacingBelow
		(void)get_css(inf, &pos);
	}
	if(bits & 0x0008) { // SpacingLines
		(void)get_css(inf, &pos);
	}
	if(bits & 0x0010) { // LeftIndent
		(void)get_css(inf, &pos);
	}
	if(bits & 0x0020) { // RightIndent
		(void)get_css(inf, &pos);
	}
	if(bits & 0x0040) { // FirstlineIndent
		(void)get_css(inf, &pos);
	}
	// 0x0080 = unused
	if(bits & 0x0100) { // Borderinfo
		goto done; // TODO
	}
	if(bits & 0x0200) { // Tabinfo
		goto done; // TODO
	}
	// 0x0400 = RightAlignedParagraph
	// 0x0800 = CenterAlignedParagraph

	tld->paragraphinfo_pos = pos;
	retval = 1;
done:
	return retval;
}

static void ensure_text_output_file_open(deark *c, lctx *d)
{
	if(d->outf_text) return;
	d->outf_text = dbuf_create_output_file(c, "dump.txt", NULL, 0);
}

static void do_topiclink_rectype_1_32(deark *c, lctx *d,
	struct topiclink_data *tld, dbuf *inf)
{
	i64 pos;
	int in_string = 0;
	int string_count = 0;
	int byte_count = 0;

	if(!d->extract_text) goto done;
	ensure_text_output_file_open(c, d);

	do_topiclink_rectype_32_linkdata1(c, d, tld, inf);

	// TODO: This is very quick & dirty.
	// The linkdata2 is a collection of NUL-terminated strings. We'd have to
	// interpret the command bytes from linkdata1 to know how to format them.

	pos = tld->linkdata2_pos;
	while(1) {
		u8 b;

		if(pos >= tld->linkdata2_pos+tld->linkdata2_len) break;
		if(pos >= inf->len) break;

		b = dbuf_getbyte_p(inf, &pos);
		if(b==0x00) {
			if(in_string) {
				dbuf_writebyte(d->outf_text, '\n');
				string_count++;
				in_string = 0;
			}
		}
		else {
			dbuf_writebyte(d->outf_text, b);
			byte_count++;
			in_string = 1;
		}
	}
	if(in_string) {
		dbuf_writebyte(d->outf_text, '\n');
		string_count++;
	}
	de_dbg(c, "[emitted %d strings, totaling %d bytes]", string_count, byte_count);

done:
	;
}

static void do_topiclink_rectype_2_linkdata2(deark *c, lctx *d,
	struct topiclink_data *tld, dbuf *inf)
{
	i64 k;
	int bytecount = 0;

	dbuf_puts(d->outf_text, "# ");
	for(k=0; k<tld->linkdata2_len; k++) {
		u8 b;

		b = dbuf_getbyte(inf, tld->linkdata2_pos+k);
		if(b==0) break;
		dbuf_writebyte(d->outf_text, b);
		bytecount++;
	}

	if(bytecount==0) {
		dbuf_puts(d->outf_text, "(untitled topic)");
	}

	dbuf_puts(d->outf_text, " #\n");
}

// topic header and title
static void do_topiclink_rectype_2(deark *c, lctx *d,
	struct topiclink_data *tld, dbuf *inf)
{
	if(!d->extract_text) goto done;
	ensure_text_output_file_open(c, d);

	do_topiclink_rectype_2_linkdata2(c, d, tld, inf);
done:
	;
}

// Returns 1 if we set next_pos_code
static int do_topiclink(deark *c, lctx *d, dbuf *inf, i64 pos1, u32 *next_pos_code)
{
	struct topiclink_data *tld = NULL;
	i64 pos = pos1;
	int retval = 0;

	tld = de_malloc(c, sizeof(struct topiclink_data));

	tld->blocksize = dbuf_geti32le_p(inf, &pos);
	de_dbg(c, "blocksize: %d", (int)tld->blocksize);
	if((tld->blocksize<21) || (pos1 + tld->blocksize > inf->len)) {
		de_dbg(c, "bad topiclink blocksize");
		goto done;
	}
	tld->datalen2 = dbuf_geti32le_p(inf, &pos);
	de_dbg(c, "datalen2 (after any decompression): %d", (int)tld->datalen2);

	tld->prevblock = dbuf_getu32le_p(inf, &pos);
	if(d->ver_minor<=16) {
		de_dbg(c, "prevblock: %"I64_FMT, tld->prevblock);
	}
	else {
		de_dbg(c, "prevblock: 0x%08x", (unsigned int)tld->prevblock);
	}

	tld->nextblock = dbuf_getu32le_p(inf, &pos);
	if(d->ver_minor<=16) {
		de_dbg(c, "nextblock: %"I64_FMT, tld->nextblock);
	}
	else {
		de_dbg(c, "nextblock: 0x%08x", (unsigned int)tld->nextblock);
	}
	*next_pos_code = (u32)tld->nextblock;
	retval = 1;

	tld->datalen1 = dbuf_geti32le_p(inf, &pos);
	de_dbg(c, "datalen1: %d", (int)tld->datalen1);
	tld->recordtype = dbuf_getbyte_p(inf, &pos);
	de_dbg(c, "record type: %d", (int)tld->recordtype);

	tld->linkdata1_pos = pos1 + 21;
	tld->linkdata1_len = tld->datalen1 - 21;
	de_dbg(c, "linkdata1: pos=[%"I64_FMT"], len=%"I64_FMT, tld->linkdata1_pos, tld->linkdata1_len);

	tld->linkdata2_pos = tld->linkdata1_pos + tld->linkdata1_len;
	tld->linkdata2_len = tld->blocksize - tld->datalen1;
	if(tld->datalen2 > (tld->blocksize - tld->datalen1)) {
		tld->seems_compressed = 1;
	}

	if(tld->seems_compressed && d->extract_text && !d->phrase_compression_warned &&
		(d->found_Phrases_file || d->found_PhrIndex_file || d->found_PhrImage_file))
	{
		de_warn(c, "This file uses a type of compression that is not supported");
		d->phrase_compression_warned = 1;
	}

	if((tld->linkdata1_pos<pos1) || (tld->linkdata2_pos<pos1) ||
		(tld->linkdata1_len<0) || (tld->linkdata2_len<0) ||
		(tld->linkdata1_pos + tld->linkdata1_len > pos1+tld->blocksize) ||
		(tld->linkdata2_pos + tld->linkdata2_len > pos1+tld->blocksize))
	{
		de_dbg(c, "bad linkdata");
		goto done;
	}

	de_dbg(c, "linkdata2: pos=[%"I64_FMT"], len=%"I64_FMT,
		tld->linkdata2_pos, tld->linkdata2_len);
	switch(tld->recordtype) {
	case 1:
	case 32:
		do_topiclink_rectype_1_32(c, d, tld, inf);
		break;
	case 2:
		do_topiclink_rectype_2(c, d, tld, inf);
		break;
	default:
		de_dbg(c, "[not processing record type %d]", (int)tld->recordtype);
	}

done:
	de_free(c, tld);
	return retval;
}

static int topicpos_to_abspos(deark *c, lctx *d, i64 topicpos, i64 *pabspos)
{
	i64 blknum, blkoffs;

	if(!d->topic_block_size) return 0;
	if(d->is_compressed) return 0;
	blkoffs = topicpos % 16384;
	if(blkoffs<12) return 0;
	blknum = topicpos / 16384;
	*pabspos = (d->topic_block_size-12) * blknum + (blkoffs-12);
	return 1;
}

static i64 hc30_abspos_plus_offset_to_abspos(deark *c, lctx *d, i64 pos, i64 offset)
{
	i64 blksize = d->topic_block_size-12;
	i64 start_of_curr_block;
	i64 end_of_curr_block;
	i64 n;

	// We're at a position in blocks of size (d->topic_block_size-12). We need to add
	// 'offset', but subtract 12 every time we cross a block boundary.
	start_of_curr_block = (pos/blksize)*blksize;
	end_of_curr_block = start_of_curr_block + blksize;
	if(pos+offset <= end_of_curr_block) {
		return pos + offset;
	}

	n = pos+offset - end_of_curr_block;
	return pos + offset - 12*(1+(n / blksize));
}

static void do_topicdata(deark *c, lctx *d, dbuf *inf)
{
	i64 pos;
	int saved_indent_level, saved_indent_level2;

	de_dbg_indent_save(c, &saved_indent_level);

	de_dbg(c, "topic data");

	de_dbg_indent(c, 1);
	if(!inf) {
		goto done;
	}

	de_dbg_indent_save(c, &saved_indent_level2);

	pos = 0; // TODO: Is the first topiclink always at 0?

	while(1) {
		u32 next_pos_code;

		if(pos > inf->len) {
			de_dbg(c, "[stopping TOPIC parsing, exceeded end of data]");
			break;
		}
		if(pos == inf->len) {
			de_dbg(c, "[stopping TOPIC parsing, reached end of data]");
			break;
		}
		if(pos + 21 > inf->len) {
			de_warn(c, "Error parsing TOPIC, not enough room for another TOPICLINK (%"I64_FMT
				", %"I64_FMT")", pos, inf->len);
			break;
		}

		de_dbg(c, "topiclink at [%"I64_FMT"]", pos);
		de_dbg_indent(c, 1);
		next_pos_code = 0;
		if(!do_topiclink(c, d, inf, pos, &next_pos_code)) goto done;
		de_dbg_indent(c, -1);

		if(d->ver_minor<=16) {
			if(next_pos_code < 21) {
				de_dbg(c, "[stopping TOPIC parsing, no nextblock available]");
				break;
			}
			pos = hc30_abspos_plus_offset_to_abspos(c, d, pos, next_pos_code);
		}
		else {
			i64 next_pos = 0;

			if(next_pos_code==0xffffffffLL) {
				de_dbg(c, "[stopping TOPIC parsing, end-of-links marker found]");
				break;
			}

			if(!topicpos_to_abspos(c, d, next_pos_code, &next_pos)) {
				de_dbg(c, "[stopping TOPIC parsing, no nextblock available]");
				break;
			}

			if(next_pos <= pos) {
				de_dbg(c, "[stopping TOPIC parsing, blocks not in order]");
				break;
			}

			pos = next_pos;
		}
	}

done:
	de_dbg_indent_restore(c, saved_indent_level);
}

static void decompress_topic_block(deark *c, lctx *d, i64 blk_dpos, i64 blk_dlen,
	dbuf *outf)
{
	struct de_dfilter_in_params dcmpri;
	struct de_dfilter_out_params dcmpro;
	struct de_dfilter_results dres;
	i64 len_before;

	de_dfilter_init_objects(c, &dcmpri, &dcmpro, &dres);

	dcmpri.f = c->infile;
	dcmpri.pos = blk_dpos;
	dcmpri.len = blk_dlen;
	dcmpro.f = outf;
	dcmpro.len_known = 1;
	dcmpro.expected_len = 16384-12;
	len_before = dcmpro.f->len;
	fmtutil_decompress_hlp_lz77(c, &dcmpri, &dcmpro, &dres);
	de_dbg(c, "decompressed %"I64_FMT" to %"I64_FMT" bytes", blk_dlen,
		dcmpro.f->len - len_before);
}

static void do_file_TOPIC(deark *c, lctx *d, i64 pos1, i64 len)
{
	i64 pos = pos1;
	int saved_indent_level;
	dbuf *unc_topicdata = NULL;

	de_dbg_indent_save(c, &saved_indent_level);
	de_dbg(c, "TOPIC at %"I64_FMT", len=%"I64_FMT, pos1, len);
	de_dbg_indent(c, 1);

	if(!d->found_system_file || d->topic_block_size<2048) {
		de_err(c, "SYSTEM file not found");
		goto done;
	}

	if(d->extract_text) {
		unc_topicdata = dbuf_create_membuf(c, 0, 0);
	}

	// A series of blocks, each with a 12-byte header
	while(1) {
		i64 lastlink, firstlink, lastheader;
		i64 blklen;
		i64 blk_dpos;
		i64 blk_dlen;

		blklen = (pos1+len)-pos;
		if(blklen<12) break;
		if(blklen > d->topic_block_size) blklen = d->topic_block_size;
		blk_dpos = pos+12;
		blk_dlen = blklen-12;

		de_dbg(c, "TOPIC block at %d, dpos=%d, dlen=%d", (int)pos,
			(int)blk_dpos, (int)blk_dlen);
		de_dbg_indent(c, 1);
		lastlink = de_geti32le(pos);
		firstlink = de_geti32le(pos+4);
		lastheader = de_geti32le(pos+8);
		de_dbg(c, "LastLink=%d, FirstLink=%d, LastHeader=%d",
			(int)lastlink, (int)firstlink, (int)lastheader);

		if(d->extract_text && unc_topicdata) {
			if(d->is_compressed) {
				decompress_topic_block(c, d, blk_dpos, blk_dlen, unc_topicdata);
			}
			else {
				dbuf_copy(c->infile, blk_dpos, blk_dlen, unc_topicdata);
			}
			de_dbg2(c, "[current decompressed size: %"I64_FMT"]", unc_topicdata->len);
		}

		de_dbg_indent(c, -1);
		pos += blklen;
	}

	if(unc_topicdata && unc_topicdata->len>0) {
		do_topicdata(c, d, unc_topicdata);
	}

done:
	dbuf_close(unc_topicdata);
	de_dbg_indent_restore(c, saved_indent_level);

}
static void do_index_page(deark *c, lctx *d, i64 pos1, i64 *prev_page)
{
	*prev_page = de_geti16le(pos1+4);
	de_dbg(c, "PreviousPage: %d", (int)*prev_page);
}

static int de_is_digit(char x)
{
	return (x>='0' && x<='9');
}

static enum hlp_filetype filename_to_filetype(deark *c, lctx *d, const char *fn)
{
	if(!de_strcmp(fn, "|TOPIC")) return FILETYPE_TOPIC;
	if(!de_strcmp(fn, "|TOMAP")) return FILETYPE_TOMAP;
	if(!de_strcmp(fn, "|SYSTEM")) return FILETYPE_SYSTEM;
	if(!de_strncmp(fn, "|bm", 3) && de_is_digit(fn[3])) return FILETYPE_SHG;
	if(!de_strncmp(fn, "bm", 2) && de_is_digit(fn[2])) return FILETYPE_SHG;
	if(!de_strcmp(fn, "|Phrases")) return FILETYPE_PHRASES;
	if(!de_strcmp(fn, "|PhrIndex")) return FILETYPE_PHRINDEX;
	if(!de_strcmp(fn, "|PhrImage")) return FILETYPE_PHRIMAGE;
	if(de_sz_has_ext(fn, "bmp")) return FILETYPE_BMP;
	return FILETYPE_UNKNOWN;
}

static void do_leaf_page(deark *c, lctx *d, i64 pos1, i64 *pnext_page)
{
	i64 n;
	i64 pos = pos1;
	i64 foundpos;
	i64 num_entries;
	i64 file_offset;
	i64 k;
	struct de_stringreaderdata *fn_srd = NULL;
	enum hlp_filetype file_type;
	int saved_indent_level;

	de_dbg_indent_save(c, &saved_indent_level);
	n = de_getu16le_p(&pos); // "Unused"
	de_dbg(c, "free bytes at end of this page: %d", (int)n);

	num_entries = de_geti16le_p(&pos);
	de_dbg(c, "NEntries: %d", (int)num_entries);

	n = de_geti16le_p(&pos);
	de_dbg(c, "PreviousPage: %d", (int)n);

	n = de_geti16le_p(&pos);
	de_dbg(c, "NextPage: %d", (int)n);
	if(pnext_page) *pnext_page = n;

	for(k=0; k<num_entries; k++) {
		int pass_for_this_file;

		de_dbg(c, "entry[%d]", (int)k);
		de_dbg_indent(c, 1);

		if(!dbuf_search_byte(c->infile, 0x00, pos, 260, &foundpos)) {
			de_err(c, "Malformed leaf page at %d", (int)pos1);
			goto done;
		}

		if(fn_srd) {
			de_destroy_stringreaderdata(c, fn_srd);
		}
		fn_srd = dbuf_read_string(c->infile, pos, foundpos-pos, foundpos-pos, 0, d->input_encoding);
		de_dbg(c, "FileName: \"%s\"", ucstring_getpsz_d(fn_srd->str));
		pos = foundpos + 1;

		file_offset = de_geti32le_p(&pos);
		de_dbg(c, "FileOffset: %d", (int)file_offset);

		file_type = filename_to_filetype(c, d, fn_srd->sz);

		switch(file_type) {
		case FILETYPE_SYSTEM:
			pass_for_this_file = 1;
			break;
		case FILETYPE_TOMAP:
			pass_for_this_file = 1;
			break;
		case FILETYPE_PHRASES:
			d->found_Phrases_file = 1;
			d->offset_of_Phrases = file_offset;
			pass_for_this_file = 1;
			break;
		case FILETYPE_PHRINDEX:
			d->found_PhrIndex_file = 1;
			pass_for_this_file = 1;
			break;
		case FILETYPE_PHRIMAGE:
			d->found_PhrImage_file = 1;
			pass_for_this_file = 1;
			break;
		default:
			pass_for_this_file = 2;
		}
		if(d->pass==pass_for_this_file) {
			do_file(c, d, file_offset, file_type);
		}

		de_dbg_indent(c, -1);
	}

done:
	de_destroy_stringreaderdata(c, fn_srd);
	de_dbg_indent_restore(c, saved_indent_level);
}

// Sets d->bpt.first_leaf_page
static int find_first_leaf_page(deark *c, lctx *d)
{
	i64 curr_page;
	i64 curr_level;
	int saved_indent_level;
	int retval = 0;

	de_dbg_indent_save(c, &saved_indent_level);
	curr_page = d->bpt.root_page;
	curr_level = d->bpt.num_levels;

	de_dbg(c, "looking for first leaf page");
	de_dbg_indent(c, 1);

	while(curr_level>1) {
		i64 prev_page;
		i64 page_pos;

		if(curr_page<0) goto done;
		page_pos = d->bpt.pagesdata_pos + curr_page*d->bpt.pagesize;

		de_dbg(c, "page %d is an index page, level=%d", (int)curr_page, (int)curr_level);

		prev_page = -1;
		de_dbg_indent(c, 1);
		do_index_page(c, d, page_pos, &prev_page);
		de_dbg_indent(c, -1);

		curr_page = prev_page;
		curr_level--;
	}

	de_dbg(c, "page %d is the first leaf page", (int)curr_page);
	d->bpt.first_leaf_page = curr_page;

	retval = 1;

done:
	de_dbg_indent_restore(c, saved_indent_level);
	return retval;
}

// This function is only for the "internal directory" tree.
// There are other data objects in HLP files that use the same kind of data
// structure. If we ever want to parse them, this function will have to be
// genericized.
static void do_bplustree(deark *c, lctx *d, i64 pos1, i64 len,
	int is_internaldir)
{
	i64 pos = pos1;
	i64 n;
	int saved_indent_level;
	u8 *page_seen = NULL;

	if(!is_internaldir) return;

	de_dbg_indent_save(c, &saved_indent_level);

	n = de_getu16le_p(&pos);
	if(n != 0x293b) {
		de_err(c, "Expected B+ tree structure at %d not found", (int)pos1);
		goto done;
	}

	//de_dbg(c, "B+ tree at %d", (int)pos1);
	de_dbg_indent(c, 1);

	d->bpt.flags = (unsigned int)de_getu16le_p(&pos);
	de_dbg(c, "flags: 0x%04x", d->bpt.flags);

	d->bpt.pagesize = de_getu16le_p(&pos);
	de_dbg(c, "PageSize: %d", (int)d->bpt.pagesize);

	// TODO: Understand the Structure field
	pos += 16;

	pos += 2; // MustBeZero
	pos += 2; // PageSplits

	d->bpt.root_page = de_geti16le_p(&pos);
	de_dbg(c, "RootPage: %d", (int)d->bpt.root_page);

	pos += 2; // MustBeNegOne

	d->bpt.num_pages = de_geti16le_p(&pos);
	de_dbg(c, "TotalPages: %d", (int)d->bpt.num_pages);

	d->bpt.num_levels = de_geti16le_p(&pos);
	de_dbg(c, "NLevels: %d", (int)d->bpt.num_levels);
	if(is_internaldir) d->internal_dir_num_levels = d->bpt.num_levels;

	d->bpt.num_entries = de_geti32le_p(&pos);
	de_dbg(c, "TotalBtreeEntries: %d", (int)d->bpt.num_entries);

	d->bpt.pagesdata_pos = pos;
	de_dbg(c, "num pages: %d, %d bytes each, at %d (total size=%d)",
		(int)d->bpt.num_pages, (int)d->bpt.pagesize, (int)d->bpt.pagesdata_pos,
		(int)(d->bpt.num_pages * d->bpt.pagesize));

	if(!find_first_leaf_page(c, d)) goto done;

	page_seen = de_malloc(c, d->bpt.num_pages); // For loop detection

	for(d->pass=1; d->pass<=2; d->pass++) {
		i64 curr_page;

		de_zeromem(page_seen, (size_t)d->bpt.num_pages);

		de_dbg(c, "pass %d", d->pass);
		de_dbg_indent(c, 1);

		curr_page = d->bpt.first_leaf_page;

		while(1) {
			i64 page_pos;
			i64 next_page;

			if(curr_page<0) break;
			if(curr_page>d->bpt.num_pages) goto done;

			if(d->pass==1 && page_seen[curr_page]) {
				de_err(c, "Page loop detected");
				goto done;
			}
			page_seen[curr_page] = 1;

			page_pos = d->bpt.pagesdata_pos + curr_page*d->bpt.pagesize;

			de_dbg(c, "page[%d] at %d (leaf page)", (int)curr_page, (int)page_pos);

			next_page = -1;
			de_dbg_indent(c, 1);
			do_leaf_page(c, d, page_pos, &next_page);
			de_dbg_indent(c, -1);

			curr_page = next_page;
		}

		de_dbg_indent(c, -1);
	}

done:
	de_free(c, page_seen);
	de_dbg_indent_restore(c, saved_indent_level);
}

static void do_file_INTERNALDIR(deark *c, lctx *d, i64 pos1, i64 len)
{
	de_dbg(c, "internal dir data at %d", (int)pos1);
	do_bplustree(c, d, pos1, len, 1);
}

static void do_file_TOMAP(deark *c, lctx *d, i64 pos1, i64 len)
{
	// I'm not sure if we ever need to parse this, so we can find the first
	// 'topiclink'.
}

static const char* file_type_to_type_name(enum hlp_filetype file_fmt)
{
	const char *name = "unspecified";
	switch(file_fmt) {
	case FILETYPE_SYSTEM: name="system"; break;
	case FILETYPE_TOPIC: name="topic"; break;
	case FILETYPE_SHG: name="SHG/MRB"; break;
	case FILETYPE_INTERNALDIR: name="directory"; break;
	default: ;
	}
	return name;
}

static void do_file(deark *c, lctx *d, i64 pos1, enum hlp_filetype file_fmt)
{
	i64 reserved_space;
	i64 used_space;
	i64 pos = pos1;
	unsigned int fileflags;

	de_dbg(c, "file at %d, type=%s", (int)pos1, file_type_to_type_name(file_fmt));
	de_dbg_indent(c, 1);

	// FILEHEADER
	reserved_space = de_getu32le_p(&pos);
	de_dbg(c, "ReservedSpace: %d", (int)reserved_space);

	used_space = de_getu32le_p(&pos);
	de_dbg(c, "UsedSpace: %d", (int)used_space);

	fileflags = (unsigned int)de_getbyte_p(&pos);
	de_dbg(c, "FileFlags: 0x%02x", fileflags);

	if(pos+used_space > c->infile->len) {
		de_err(c, "Bad file size");
		goto done;
	}

	switch(file_fmt) {
	case FILETYPE_INTERNALDIR:
		do_file_INTERNALDIR(c, d, pos, used_space);
		break;
	case FILETYPE_TOPIC:
		do_file_TOPIC(c, d, pos, used_space);
		break;
	case FILETYPE_TOMAP:
		do_file_TOMAP(c, d, pos, used_space);
		break;
	case FILETYPE_SYSTEM:
		do_file_SYSTEM(c, d, pos, used_space);
		break;
	case FILETYPE_SHG:
		d->has_shg = 1;
		do_file_SHG(c, d, pos, used_space);
		break;
	case FILETYPE_BMP:
		d->has_bmp = 1;
		break;
	default: ;
	}

done:
	de_dbg_indent(c, -1);
}

static void do_header(deark *c, lctx *d, i64 pos)
{
	i64 n;

	de_dbg(c, "header at %d", (int)pos);
	de_dbg_indent(c, 1);

	d->internal_dir_FILEHEADER_offs = de_geti32le(4);
	de_dbg(c, "internal dir FILEHEADER pos: %d", (int)d->internal_dir_FILEHEADER_offs);

	n = de_geti32le(8);
	de_dbg(c, "FREEHEADER pos: %d", (int)n);

	n = de_geti32le(12);
	de_dbg(c, "reported file size: %d", (int)n);

	de_dbg_indent(c, -1);
}

static void de_run_hlp(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;
	i64 pos;

	d = de_malloc(c, sizeof(lctx));

	d->input_encoding = de_get_input_encoding(c, NULL, DE_ENCODING_WINDOWS1252);
	d->extract_text = de_get_ext_option_bool(c, "hlp:extracttext", 0);

	pos = 0;
	do_header(c, d, pos);

	do_file(c, d, d->internal_dir_FILEHEADER_offs, FILETYPE_INTERNALDIR);

	de_dbg(c, "summary: v%d.%d cmpr=%s%s%s blksize=%d levels=%d%s%s%s",
		d->ver_major, d->ver_minor,
		d->is_compressed?"lz77":"none",
		d->found_Phrases_file?" phrase_compression":"",
		(d->found_PhrIndex_file||d->found_PhrImage_file)?" Hall_compression":"",
		(int)d->topic_block_size,
		(int)d->internal_dir_num_levels,
		d->has_shg?" has-shg":"",
		d->has_ico?" has-ico":"",
		d->has_bmp?" has-bmp":"");

	if(d) {
		if(d->outf_text) {
			dbuf_close(d->outf_text);
		}
		de_free(c, d);
	}
}

static int de_identify_hlp(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\x3f\x5f\x03\x00", 4))
		return 100;
	return 0;
}

void de_module_hlp(deark *c, struct deark_module_info *mi)
{
	mi->id = "hlp";
	mi->desc = "HLP";
	mi->run_fn = de_run_hlp;
	mi->identify_fn = de_identify_hlp;
}
