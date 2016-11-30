// This file is part of Deark.
// Copyright (C) 2016 Jason Summers
// See the file COPYING for terms of use.

// Functions specific to Microsoft Windows, especially those that require
// windows.h.

#include "deark-config.h"

#ifndef DE_WINDOWS
#error "This file is only for Windows builds"
#endif

#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/utime.h>

#include "deark-private.h"

int de_strcasecmp(const char *a, const char *b)
{
	return _stricmp(a, b);
}

void de_vsnprintf(char *buf, size_t buflen, const char *fmt, va_list ap)
{
	_vsnprintf_s(buf,buflen,_TRUNCATE,fmt,ap);
	buf[buflen-1]='\0';
}

char *de_strdup(deark *c, const char *s)
{
	char *s2;

	s2 = _strdup(s);
	if(!s2) {
		de_err(c, "Memory allocation failed\n");
		de_fatalerror(c);
		return NULL;
	}
	return s2;
}

de_int64 de_strtoll(const char *string, char **endptr, int base)
{
	return _strtoi64(string, endptr, base);
}

static char *de_utf16_to_utf8_strdup(deark *c, const WCHAR *src)
{
	char *dst;
	int dstlen;
	int ret;

	// Calculate the size required by the target string.
	ret = WideCharToMultiByte(CP_UTF8,0,src,-1,NULL,0,NULL,NULL);
	if(ret<1) return NULL;

	dstlen = ret;
	dst = (char*)de_malloc(c, dstlen*sizeof(char));

	ret = WideCharToMultiByte(CP_UTF8,0,src,-1,dst,dstlen,NULL,NULL);
	if(ret<1) {
		de_free(c, dst);
		return NULL;
	}
	return dst;
}

wchar_t *de_utf8_to_utf16_strdup(deark *c, const char *src)
{
	WCHAR *dst;
	int dstlen;
	int ret;

	// Calculate the size required by the target string.
	ret = MultiByteToWideChar(CP_UTF8,0,src,-1,NULL,0);
	if(ret<1) return NULL;

	dstlen = ret;
	dst = (WCHAR*)de_malloc(c, dstlen*sizeof(WCHAR));

	ret = MultiByteToWideChar(CP_UTF8,0,src,-1,dst,dstlen);
	if(ret<1) {
		de_free(c, dst);
		return NULL;
	}
	return dst;
}

FILE* de_fopen(deark *c, const char *fn, const char *mode,
	char *errmsg, size_t errmsg_len)
{
	FILE *f = NULL;
	errno_t errcode;
	WCHAR *fnW;
	WCHAR *modeW;

	fnW = de_utf8_to_utf16_strdup(c, fn);
	modeW = de_utf8_to_utf16_strdup(c, mode);

	errcode = _wfopen_s(&f,fnW,modeW);

	de_free(c, fnW);
	de_free(c, modeW);

	errmsg[0] = '\0';

	if(errcode!=0) {
		strerror_s(errmsg, (size_t)errmsg_len, (int)errcode);
		f=NULL;
	}
	return f;
}

int de_fclose(FILE *fp)
{
	return fclose(fp);
}

int de_examine_file_by_name(deark *c, const char *fn, de_int64 *len,
	char *errmsg, size_t errmsg_len)
{
	struct _stat stbuf;
	WCHAR *fnW;
	int retval = 0;

	fnW = de_utf8_to_utf16_strdup(c, fn);

	de_memset(&stbuf, 0, sizeof(struct _stat));

	if(0 != _wstat(fnW, &stbuf)) {
		strerror_s(errmsg, (size_t)errmsg_len, errno);
		goto done;
	}

	if(!(stbuf.st_mode & _S_IFREG)) {
		de_strlcpy(errmsg, "Not a regular file", errmsg_len);
		return 0;
	}

	*len = (de_int64)stbuf.st_size;

	retval = 1;

done:
	de_free(c, fnW);
	return retval;
}

void de_update_file_perms(dbuf *f)
{
	// Not implemented on Windows.
}

void de_update_file_time(dbuf *f)
{
	WCHAR *fnW;
	struct __utimbuf64 times;
	deark *c;

	if(f->btype!=DBUF_TYPE_OFILE) return;
	if(!f->mod_time.is_valid) return;
	if(!f->name) return;
	c = f->c;

	fnW = de_utf8_to_utf16_strdup(c, f->name);

	times.modtime = de_timestamp_to_unix_time(&f->mod_time);
	times.actime = times.modtime;
	_wutime64(fnW, &times);

	de_free(c, fnW);
}

char **de_convert_args_to_utf8(int argc, wchar_t **argvW)
{
	int i;
	char **argvUTF8;

	argvUTF8 = (char**)de_malloc(NULL, argc*sizeof(char*));

	// Convert parameters to UTF-8
	for(i=0;i<argc;i++) {
		argvUTF8[i] = de_utf16_to_utf8_strdup(NULL, argvW[i]);
	}

	return argvUTF8;
}

void de_free_utf8_args(int argc, char **argv)
{
	int i;

	for(i=0;i<argc;i++) {
		de_free(NULL, argv[i]);
	}
	de_free(NULL, argv);
}

// A helper function that returns nonzero if stdout seems to be a Windows console.
// 0 means that stdout is redirected.
int de_stdout_is_windows_console(void)
{
	DWORD consolemode=0;
	BOOL n;

	n=GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &consolemode);
	return n ? 1 : 0;
}

// A helper function that returns nonzero if stderr seems to be a Windows console.
// 0 means that stderr is redirected.
int de_stderr_is_windows_console(void)
{
	DWORD consolemode=0;
	BOOL n;

	n=GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &consolemode);
	return n ? 1 : 0;
}

// Note: Need to keep this function in sync with the implementation in deark-unix.c.
void de_timestamp_to_string(const struct de_timestamp *ts,
	char *buf, size_t buf_len, unsigned int flags)
{
	__time64_t tmpt;
	struct tm tm1;
	const char *tzlabel;
	errno_t ret;

	if(!ts->is_valid) {
		de_strlcpy(buf, "[invalid timestamp]", buf_len);
		return;
	}

	de_memset(&tm1, 0, sizeof(struct tm));
	tmpt = (__time64_t)de_timestamp_to_unix_time(ts);
	ret = _gmtime64_s(&tm1, &tmpt);
	if(ret!=0) {
		de_strlcpy(buf, "[error]", buf_len);
		return;
	}

	tzlabel = (flags&0x1)?" UTC":"";
	de_snprintf(buf, buf_len, "%04d-%02d-%02d %02d:%02d:%02d%s",
		1900+tm1.tm_year, 1+tm1.tm_mon, tm1.tm_mday,
		tm1.tm_hour, tm1.tm_min, tm1.tm_sec, tzlabel);
}

// Note: Need to keep this function in sync with the implementation in deark-unix.c.
void de_current_time_to_timestamp(struct de_timestamp *ts)
{
	__time64_t t;

	de_memset(ts, 0, sizeof(struct de_timestamp));
	_time64(&t);
	ts->unix_time = (de_int64)t;
	ts->is_valid = 1;
}
