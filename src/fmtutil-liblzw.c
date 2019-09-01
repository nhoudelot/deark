// This file is part of Deark.
// Copyright (C) 2018 Jason Summers
// See the file COPYING for terms of use.

// Interface to liblzw

#define DE_NOT_IN_MODULE
#include "deark-config.h"
#include "deark-private.h"
#include "deark-fmtutil.h"
#include "../foreign/liblzw.h"

// dflags:
//  0x1 = has "compress" style header
//  0x2 = arcfs mode
// lzwmode: Like compress format. Used if there's no header.
int de_fmtutil_decompress_liblzw(dbuf *inf1, i64 pos1, i64 len,
	dbuf *outf, unsigned int has_maxlen, i64 max_out_len,
	unsigned int flags, u8 lzwmode)
{
	u8 buf[1024];
	i64 n;
	dbuf *inf = NULL;
	struct de_liblzwctx *lzw = NULL;
	i64 nbytes_still_to_write;
	int retval = 0;
	int ret;
	deark *c = inf1->c;

	// TODO: We shouldn't really need a subfile here.
	inf = dbuf_open_input_subfile(inf1, pos1, len);

	lzw = de_liblzw_create(c);
	ret = de_liblzw_dbufopen(lzw, inf, flags, lzwmode);
	if(!ret) goto done;

	nbytes_still_to_write = has_maxlen ? max_out_len : 0;

	while(1) {
		if(has_maxlen && (nbytes_still_to_write<1)) break;
		n = de_liblzw_read(lzw, buf, sizeof(buf));
		if(n<0) {
			goto done;
		}
		if(n<1) break;

		if(has_maxlen && (n > nbytes_still_to_write)) {
			// Make sure we don't write more bytes than expected.
			n = nbytes_still_to_write;
		}

		dbuf_write(outf, buf, n);
		nbytes_still_to_write -= n;
	}
	retval = 1;

done:
	if(lzw) {
		if(lzw->errcode) {
			de_err(c, "[liblzw] %s", lzw->errmsg);
		}
		de_liblzw_destroy(lzw);
	}
	dbuf_close(inf);
	return retval;
}