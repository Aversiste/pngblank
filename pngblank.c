/*
 * Copyright (c) 2018,2020 Tristan Le Guern <tleguern@bouledef.eu>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <arpa/inet.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "lgpng.h"

#define PNGBLANK_MAX_SIZE 8192

static void usage(void);

static int
prepare_IDAT(struct IDAT *idat, struct IHDR *ihdr, int level, int strategy)
{
	size_t		 dataz, deflatedz;
	uint8_t		*data = NULL, *deflated = NULL;
	z_stream	 strm;
	uint32_t	 width;

	width = ntohl(ihdr->data.width);
	/* Calculate the buffer size using bitdepth and colour type */
	if (COLOUR_TYPE_TRUECOLOUR == ihdr->data.colourtype) {
		dataz = width * width * 3 * ihdr->data.bitdepth / 8 + width;
	} else if (COLOUR_TYPE_GREYSCALE == ihdr->data.colourtype) {
		dataz = (width / (8 / ihdr->data.bitdepth) + \
		    (width % (8 / ihdr->data.bitdepth) != 0 ? 1 : 0) + 1) * width;
	} else if (COLOUR_TYPE_INDEXED == ihdr->data.colourtype) {
		dataz = width * width * ihdr->data.bitdepth / 8 + width;
	} else {
		fprintf(stderr, "Invalid colourtype\n");
		return(-1);
	}
	/* The data is a stream of zero so calloc is perfect */
	if (NULL == (data = calloc(dataz, sizeof(*data)))) {
		goto exit;
	}
	/* Prepare for a single-step compression */
	strm.zalloc = NULL;
	strm.zfree = NULL;
	strm.opaque = NULL;
	switch (deflateInit(&strm, level)) {
	case Z_OK:
		break;
	default:
		fprintf(stderr, "deflateInit: %s\n", strm.msg);
		goto exit;
	}
	deflatedz = deflateBound(&strm, dataz);
	if (NULL == (deflated = calloc(deflatedz, sizeof(*deflated)))) {
		fprintf(stderr, "calloc()\n");
		goto exit;
	}
	strm.next_in = data;
	strm.avail_in = dataz;
	strm.next_out = deflated;
	strm.avail_out = deflatedz;
	if (Z_OK != deflateParams(&strm, level, strategy)) {
		fprintf(stderr, "deflateParams stream error: %s\n", strm.msg);
		goto exit;
	}
	/* Finaly compress data */
	switch (deflate(&strm, Z_FINISH)) {
	case Z_OK:
		fprintf(stderr, "deflate: more space was needed\n");
		goto exit;
	case Z_STREAM_END:
		break;
	default:
		fprintf(stderr, "deflate: %s\n", strm.msg);
		goto exit;
	}
	if (Z_OK != deflateEnd(&strm)) {
		fprintf(stderr, "%s\n", strm.msg);
		goto exit;
	}
	free(data);
	data = NULL;
	/* Set deflatedz to the real compressed size */
	deflatedz = strm.total_out;
	idat->length = deflatedz;
	idat->data = deflated;
	return(0);
exit:
	free(data);
	free(deflated);
	return(-1);
}

int
main(int argc, char *argv[])
{
	FILE		*f = stdout;
	uint8_t		*buf;
	const char	*errstr = NULL;
	size_t		 width, off;
	int		 ch, colourtype;
	int		 bflag;
	int		 gflag;
	int		 lflag;
	int		 nflag;
	int		 pflag;
	int		 sflag;
	struct IHDR	 ihdr;
	struct PLTE	 plte;
	struct IDAT	 idat;
	struct tRNS	 trns;

#if HAVE_PLEDGE
        pledge("stdio", NULL);
#endif

	bflag = 8;
	gflag = 0;
	lflag = Z_DEFAULT_COMPRESSION;
	nflag = 0;
	pflag = 0;
	sflag = Z_DEFAULT_STRATEGY;
	colourtype = COLOUR_TYPE_TRUECOLOUR;
	while (-1 != (ch = getopt(argc, argv, "b:gl:nps:")))
		switch (ch) {
		case 'b':
			if (0 == (bflag = strtonum(optarg, 1, 16, &errstr))) {
				fprintf(stderr, "value is %s -- b\n", errstr);
				return(EX_DATAERR);
			}
			if (bflag != 1 && bflag != 2 && bflag != 4
			    && bflag != 8 && bflag != 16) {
				fprintf(stderr, "value is invalid -- b\n");
				return(EX_DATAERR);
			}
			break;
		case 'g':
			gflag = 1;
			break;
		case 'l':
			lflag = strtonum(optarg, 1, 9, &errstr);
			if (NULL != errstr) {
				fprintf(stderr, "value is %s, should be between 1 and 9 -- l\n", errstr);
				return(EX_DATAERR);
			}
			break;
		case 'n':
			nflag = 1;
			break;
		case 'p':
			pflag = 1;
			break;
		case 's':
			if (strcmp(optarg, "default") == 0) {
				sflag = Z_DEFAULT_STRATEGY;
			} else if (strcmp(optarg, "filtered") == 0) {
				sflag = Z_FILTERED;
			} else if (strcmp(optarg, "huffmanonly") == 0) {
				sflag = Z_HUFFMAN_ONLY;
			} else if (strcmp(optarg, "fixed") == 0) {
				sflag = Z_FIXED;
			} else if (strcmp(optarg, "rle") == 0) {
				sflag = Z_RLE;
			} else {
				fprintf(stderr, "unknown compression"
				    " strategy -- s\n");
				return(EX_DATAERR);
			}
			break;
		default:
			usage();
			exit(EX_USAGE);
		}
	argc -= optind;
	argv += optind;

	if (argc == 0 || argc > 1) {
		fprintf(stderr, "Width expected\n");
		usage();
		return(EX_USAGE);
	}
	if (0 == (width = strtonum(argv[0], 1, 512, NULL))) {
		fprintf(stderr, "Width should be between 1 and 512\n");
		return(EX_DATAERR);
	}
	if (1 == gflag && 1 == pflag) {
		fprintf(stderr, "Options -g and -p are mutualy exclusive\n");
		usage();
		return(EX_USAGE);
	} else if (1 == gflag) {
		colourtype = COLOUR_TYPE_GREYSCALE;
	} else if (1 == pflag) {
		colourtype = COLOUR_TYPE_INDEXED;
	}
	if (NULL == (buf = calloc(PNGBLANK_MAX_SIZE, 1))) {
		fprintf(stderr, "malloc(%i)\n", PNGBLANK_MAX_SIZE);
		return(EX_OSERR);
	}

	/* IHDR preparation */
	init_IHDR(&ihdr);
	ihdr.data.width = htonl(width);
	ihdr.data.height = htonl(width);
	ihdr.data.bitdepth = bflag;
	ihdr.data.colourtype = colourtype;
	update_crc((struct chunk *)&ihdr);

	/* PLTE preparation */
	if (1 == pflag) {
		init_PLTE(&plte);
		plte.length = 3; /* Three bytes in a PLTE entry, it's RGB */
		update_crc((struct chunk *)&plte);
	}

	/* tRNS preparation */
	init_tRNS(&trns, colourtype);
	if (1 == pflag) {
		trns.length = 1;
	}
	update_crc((struct chunk *)&trns);

	/* IDAT preparation */
	init_IDAT(&idat);
	if (-1 == prepare_IDAT(&idat, &ihdr, lflag, sflag)) {
		return(1);
	}
	update_crc((struct chunk *)&idat);

	off = 0;
	off += write_png_sig(buf);
	off += write_chunk(buf + off, (struct chunk *)&ihdr);
	if (1 == pflag) {
		off += write_chunk(buf + off, (struct chunk *)&plte);
	}
	off += write_chunk(buf + off, (struct chunk *)&trns);
	off += write_chunk(buf + off, (struct chunk *)&idat);
	off += write_IEND(buf + off);
	free(idat.data);
	if (0 == nflag) {
		fwrite(buf, sizeof(uint8_t), off, f);
	} else {
		printf("%zu\n", off);
	}
	return(0);
}

void
usage(void)
{
	fprintf(stderr, "usage: %s [-gnp] [-b bitdepth] [-l level]"
			" [-s strategy] width\n", getprogname());
}

