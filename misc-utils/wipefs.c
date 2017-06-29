/*
 * wipefs - utility to wipe filesystems from device
 *
 * Copyright (C) 2009 Red Hat, Inc. All rights reserved.
 * Written by Karel Zak <kzak@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <limits.h>
#include <libgen.h>

#include <blkid.h>
#include <libsmartcols.h>

#include "nls.h"
#include "xalloc.h"
#include "strutils.h"
#include "all-io.h"
#include "match.h"
#include "c.h"
#include "closestream.h"
#include "optutils.h"
#include "blkdev.h"

struct wipe_desc {
	loff_t		offset;		/* magic string offset */
	size_t		len;		/* length of magic string */
	unsigned char	*magic;		/* magic string */

	char		*usage;		/* raid, filesystem, ... */
	char		*type;		/* FS type */
	char		*label;		/* FS label */
	char		*uuid;		/* FS uuid */

	struct wipe_desc	*next;

	unsigned int	zap : 1,
			on_disk : 1,
			is_parttable : 1;

};

struct wipe_control {
	const char	*devname;
	const char	*type_pattern;

	struct libscols_table *outtab;

	unsigned int	noact : 1,
			all : 1,
			quiet : 1,
			backup : 1,
			force : 1,
			json : 1,
			no_headings : 1,
			parsable : 1;
};


/* column IDs */
enum {
	COL_UUID = 0,
	COL_LABEL,
	COL_LEN,
	COL_TYPE,
	COL_OFFSET,
	COL_USAGE,
	COL_DEVICE
};

/* column names */
struct colinfo {
	const char *name;	/* header */
	double whint;		/* width hint (N < 1 is in percent of termwidth) */
	int flags;		/* SCOLS_FL_* */
	const char *help;
};

/* columns descriptions */
static const struct colinfo infos[] = {
	[COL_UUID]    = {"UUID",     4, 0, N_("partition/filesystem UUID")},
	[COL_LABEL]   = {"LABEL",    5, 0, N_("filesystem LABEL")},
	[COL_LEN]     = {"LENGTH",   6, 0, N_("magic string length")},
	[COL_TYPE]    = {"TYPE",     4, 0, N_("superblok type")},
	[COL_OFFSET]  = {"OFFSET",   5, 0, N_("magic string offset")},
	[COL_USAGE]   = {"USAGE",    5, 0, N_("type description")},
	[COL_DEVICE]  = {"DEVICE",   5, 0, N_("block device name")}
};

static int columns[ARRAY_SIZE(infos) * 2];
static size_t ncolumns;

static int column_name_to_id(const char *name, size_t namesz)
{
	size_t i;

	assert(name);

	for (i = 0; i < ARRAY_SIZE(infos); i++) {
		const char *cn = infos[i].name;
		if (!strncasecmp(name, cn, namesz) && !*(cn + namesz))
			return i;
	}
	warnx(_("unknown column: %s"), name);
	return -1;
}

static int get_column_id(size_t num)
{
	assert(num < ncolumns);
	assert(columns[num] < (int)ARRAY_SIZE(infos));
	return columns[num];
}

static const struct colinfo *get_column_info(int num)
{
	return &infos[get_column_id(num)];
}


static void init_output(struct wipe_control *ctl)
{
	struct libscols_table *tb;
	size_t i;

	scols_init_debug(0);
	tb = scols_new_table();
	if (!tb)
		err(EXIT_FAILURE, _("failed to allocate output table"));

	if (ctl->json) {
		scols_table_enable_json(tb, 1);
		scols_table_set_name(tb, "signatures");
	}
	scols_table_enable_noheadings(tb, ctl->no_headings);

	if (ctl->parsable) {
		scols_table_enable_raw(tb, 1);
		scols_table_set_column_separator(tb, ",");
	}

	for (i = 0; i < ncolumns; i++) {
		const struct colinfo *col = get_column_info(i);

		if (!scols_table_new_column(tb, col->name, col->whint,
					    col->flags))
			err(EXIT_FAILURE,
			    _("failed to initialize output column"));
	}
	ctl->outtab = tb;
}

static void finalize_output(struct wipe_control *ctl)
{
	if (ctl->parsable && !ctl->no_headings
	    && !scols_table_is_empty(ctl->outtab)) {
		struct libscols_iter *itr = scols_new_iter(SCOLS_ITER_FORWARD);
		struct libscols_column *cl;
		int i = 0;

		if (!itr)
			err_oom();

		fputs("# ", stdout);
		while (scols_table_next_column(ctl->outtab, itr, &cl) == 0) {
			struct libscols_cell *hdr = scols_column_get_header(cl);
			const char *name = scols_cell_get_data(hdr);

			if (i)
				fputc(',', stdout);
			fputs(name, stdout);
			i++;
		}
		fputc('\n', stdout);
		scols_free_iter(itr);
	}
	scols_print_table(ctl->outtab);
	scols_unref_table(ctl->outtab);
}

static void fill_table_row(struct wipe_control *ctl, struct wipe_desc *wp)
{
	static struct libscols_line *ln;
	size_t i;

	ln = scols_table_new_line(ctl->outtab, NULL);
	if (!ln)
		errx(EXIT_FAILURE, _("failed to allocate output line"));

	for (i = 0; i < ncolumns; i++) {
		char *str = NULL;

		switch (get_column_id(i)) {
		case COL_UUID:
			if (wp->uuid)
				str = xstrdup(wp->uuid);
			break;
		case COL_LABEL:
			if (wp->label)
				str = xstrdup(wp->label);
			break;
		case COL_OFFSET:
			xasprintf(&str, "0x%jx", (intmax_t)wp->offset);
			break;
		case COL_LEN:
			xasprintf(&str, "%zu", wp->len);
			break;
		case COL_USAGE:
			if (wp->usage)
				str = xstrdup(wp->usage);
			break;
		case COL_TYPE:
			if (wp->type)
				str = xstrdup(wp->type);
			break;
		case COL_DEVICE:
			if (ctl->devname) {
				char *dev = xstrdup(ctl->devname);
				str = xstrdup(basename(dev));
				free(dev);
			}
			break;
		default:
			abort();
		}

		if (str && scols_line_refer_data(ln, i, str))
			errx(EXIT_FAILURE, _("failed to add output data"));
	}
}

static void add_to_output(struct wipe_control *ctl, struct wipe_desc *wp)
{
	for (/*nothing*/; wp; wp = wp->next)
		fill_table_row(ctl, wp);
}

static struct wipe_desc *
add_offset(struct wipe_desc *wp0, loff_t offset, int zap)
{
	struct wipe_desc *wp = wp0;

	while (wp) {
		if (wp->offset == offset)
			return wp;
		wp = wp->next;
	}

	wp = xcalloc(1, sizeof(struct wipe_desc));
	wp->offset = offset;
	wp->next = wp0;
	wp->zap = zap ? 1 : 0;
	return wp;
}

static struct wipe_desc *
clone_offset(struct wipe_desc *wp0)
{
	struct wipe_desc *wp = NULL;

	while(wp0) {
		wp = add_offset(wp, wp0->offset, wp0->zap);
		wp0 = wp0->next;
	}

	return wp;
}

static struct wipe_desc *
get_desc_for_probe(struct wipe_control *ctl, struct wipe_desc *wp, blkid_probe pr, int *found)
{
	const char *off, *type, *mag, *p, *usage = NULL;
	size_t len;
	loff_t offset;
	int rc, ispt = 0;

	if (found)
		*found = 0;

	/* superblocks */
	if (blkid_probe_lookup_value(pr, "TYPE", &type, NULL) == 0) {
		rc = blkid_probe_lookup_value(pr, "SBMAGIC_OFFSET", &off, NULL);
		if (!rc)
			rc = blkid_probe_lookup_value(pr, "SBMAGIC", &mag, &len);
		if (rc)
			return wp;

	/* partitions */
	} else if (blkid_probe_lookup_value(pr, "PTTYPE", &type, NULL) == 0) {
		rc = blkid_probe_lookup_value(pr, "PTMAGIC_OFFSET", &off, NULL);
		if (!rc)
			rc = blkid_probe_lookup_value(pr, "PTMAGIC", &mag, &len);
		if (rc)
			return wp;
		usage = N_("partition-table");
		ispt = 1;
	} else
		return wp;

	if (ctl->type_pattern && !match_fstype(type, ctl->type_pattern))
		return wp;

	offset = strtoll(off, NULL, 10);

	wp = add_offset(wp, offset, 0);
	if (!wp)
		return NULL;

	if (usage || blkid_probe_lookup_value(pr, "USAGE", &usage, NULL) == 0)
		wp->usage = xstrdup(usage);

	wp->type = xstrdup(type);
	wp->on_disk = 1;
	wp->is_parttable = ispt ? 1 : 0;

	wp->magic = xmalloc(len);
	memcpy(wp->magic, mag, len);
	wp->len = len;

	if (blkid_probe_lookup_value(pr, "LABEL", &p, NULL) == 0)
		wp->label = xstrdup(p);

	if (blkid_probe_lookup_value(pr, "UUID", &p, NULL) == 0)
		wp->uuid = xstrdup(p);

	if (found)
		*found = 1;
	return wp;
}

static blkid_probe
new_probe(const char *devname, int mode)
{
	blkid_probe pr = NULL;

	if (!devname)
		return NULL;

	if (mode) {
		int fd = open(devname, mode);
		if (fd < 0)
			goto error;

		pr = blkid_new_probe();
		if (!pr || blkid_probe_set_device(pr, fd, 0, 0) != 0) {
			close(fd);
			goto error;
		}
	} else
		pr = blkid_new_probe_from_filename(devname);

	if (!pr)
		goto error;

	blkid_probe_enable_superblocks(pr, 1);
	blkid_probe_set_superblocks_flags(pr,
			BLKID_SUBLKS_MAGIC |	/* return magic string and offset */
			BLKID_SUBLKS_TYPE |	/* return superblock type */
			BLKID_SUBLKS_USAGE |	/* return USAGE= */
			BLKID_SUBLKS_LABEL |	/* return LABEL= */
			BLKID_SUBLKS_UUID |	/* return UUID= */
			BLKID_SUBLKS_BADCSUM);	/* accept bad checksums */

	blkid_probe_enable_partitions(pr, 1);
	blkid_probe_set_partitions_flags(pr, BLKID_PARTS_MAGIC |
					     BLKID_PARTS_FORCE_GPT);
	return pr;
error:
	blkid_free_probe(pr);
	err(EXIT_FAILURE, _("error: %s: probing initialization failed"), devname);
}

static struct wipe_desc *
read_offsets(struct wipe_control *ctl, struct wipe_desc *wp)
{
	blkid_probe pr = new_probe(ctl->devname, 0);

	if (!pr)
		return NULL;

	while (blkid_do_probe(pr) == 0) {
		int found = 0;

		wp = get_desc_for_probe(ctl, wp, pr, &found);
		if (!wp)
			break;

		/* hide last detected signature and scan again */
		if (found) {
			blkid_probe_hide_range(pr, wp->offset, wp->len);
			blkid_probe_step_back(pr);
		}
	}

	blkid_free_probe(pr);
	return wp;
}

static void
free_wipe(struct wipe_desc *wp)
{
	while (wp) {
		struct wipe_desc *next = wp->next;

		free(wp->usage);
		free(wp->type);
		free(wp->magic);
		free(wp->label);
		free(wp->uuid);
		free(wp);

		wp = next;
	}
}

static void do_wipe_real(struct wipe_control *ctl, blkid_probe pr,
			struct wipe_desc *w)
{
	size_t i;

	if (blkid_do_wipe(pr, ctl->noact) != 0)
		err(EXIT_FAILURE, _("%s: failed to erase %s magic string at offset 0x%08jx"),
		     ctl->devname, w->type, (intmax_t)w->offset);

	if (ctl->quiet)
		return;

	printf(P_("%s: %zd byte was erased at offset 0x%08jx (%s): ",
		  "%s: %zd bytes were erased at offset 0x%08jx (%s): ",
		  w->len),
	       ctl->devname, w->len, (intmax_t)w->offset, w->type);

	for (i = 0; i < w->len; i++) {
		printf("%02x", w->magic[i]);
		if (i + 1 < w->len)
			fputc(' ', stdout);
	}
	putchar('\n');
}

static void do_backup(struct wipe_desc *wp, const char *base)
{
	char *fname = NULL;
	int fd;

	xasprintf(&fname, "%s0x%08jx.bak", base, (intmax_t)wp->offset);

	fd = open(fname, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		goto err;
	if (write_all(fd, wp->magic, wp->len) != 0)
		goto err;
	close(fd);
	free(fname);
	return;
err:
	err(EXIT_FAILURE, _("%s: failed to create a signature backup"), fname);
}

#ifdef BLKRRPART
static void rereadpt(int fd, const char *devname)
{
	struct stat st;

	if (fstat(fd, &st) || !S_ISBLK(st.st_mode))
		return;

	errno = 0;
	ioctl(fd, BLKRRPART);
	printf(_("%s: calling ioctl to re-read partition table: %m\n"), devname);
}
#endif

static struct wipe_desc *
do_wipe(struct wipe_control *ctl, struct wipe_desc *wp)
{
	int mode = O_RDWR, reread = 0, need_force = 0;
	blkid_probe pr;
	struct wipe_desc *w, *wp0;
	int zap = ctl->all ? 1 : wp->zap;
	char *backup = NULL;

	if (!ctl->force)
		mode |= O_EXCL;

	pr = new_probe(ctl->devname, mode);
	if (!pr)
		return NULL;

	if (zap && ctl->backup) {
		const char *home = getenv ("HOME");
		char *tmp = xstrdup(ctl->devname);

		if (!home)
			errx(EXIT_FAILURE, _("failed to create a signature backup, $HOME undefined"));
		xasprintf (&backup, "%s/wipefs-%s-", home, basename(tmp));
		free(tmp);
	}

	/* wp0 is the list of wanted offsets */
	wp0 = clone_offset(wp);

	while (blkid_do_probe(pr) == 0) {
		int wiped = 0;

		wp = get_desc_for_probe(ctl, wp, pr, NULL);
		if (!wp)
			break;

		/* Check if offset is in provided list */
		w = wp0;
		while(w && w->offset != wp->offset)
			w = w->next;

		if (wp0 && !w)
			goto done;

		/* Mark done if found in provided list */
		if (w)
			w->on_disk = wp->on_disk;

		if (!wp->on_disk)
			goto done;

		if (!ctl->force
		    && wp->is_parttable
		    && !blkid_probe_is_wholedisk(pr)) {
			warnx(_("%s: ignoring nested \"%s\" partition table "
				"on non-whole disk device"), ctl->devname, wp->type);
			need_force = 1;
			goto done;
		}

		if (zap) {
			if (backup)
				do_backup(wp, backup);
			do_wipe_real(ctl, pr, wp);
			if (wp->is_parttable)
				reread = 1;
			wiped = 1;
		}
	done:
		if (!wiped) {
			/* if the offset has not been wiped (probably because
			 * filtered out by -t or -o) we need to hide it for
			 * libblkid to try another magic string for the same
			 * superblock, otherwise libblkid will continue with
			 * another superblock. Don't forget that the same
			 * superblock could be detected by more magic strings
			 * */
			blkid_probe_hide_range(pr, wp->offset, wp->len);
			blkid_probe_step_back(pr);
		}

	}

	for (w = wp0; w != NULL; w = w->next) {
		if (!w->on_disk && !ctl->quiet)
			warnx(_("%s: offset 0x%jx not found"),
					ctl->devname, (uintmax_t)w->offset);
	}

	if (need_force)
		warnx(_("Use the --force option to force erase."));

	fsync(blkid_probe_get_fd(pr));

#ifdef BLKRRPART
	if (reread && (mode & O_EXCL))
		rereadpt(blkid_probe_get_fd(pr), ctl->devname);
#endif

	close(blkid_probe_get_fd(pr));
	blkid_free_probe(pr);
	free_wipe(wp0);
	free(backup);

	return wp;
}


static void __attribute__((__noreturn__))
usage(void)
{
	FILE *out = stdout;
	size_t i;

	fputs(USAGE_HEADER, out);
	fprintf(out,
	      _(" %s [options] <device>\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Wipe signatures from a device.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -a, --all           wipe all magic strings (BE CAREFUL!)\n"
		" -b, --backup        create a signature backup in $HOME\n"
		" -f, --force         force erasure\n"
		" -n, --no-act        do everything except the actual write() call\n"
		" -o, --offset <num>  offset to erase, in bytes\n"
		" -p, --parsable      print out in parsable instead of printable format\n"
		" -q, --quiet         suppress output messages\n"
		" -t, --types <list>  limit the set of filesystem, RAIDs or partition tables\n"
		), out);

	fputs(_(" -J, --json          use JSON output format\n"), out);
	fputs(_(" -n, --noheadings    don't print headings\n"), out);
	fputs(_(" -o, --output <list> output columns\n"), out);

	print_usage_help_options(21);

	fputs(USAGE_COLUMNS, stdout);
	for (i = 0; i < ARRAY_SIZE(infos); i++)
		fprintf(stdout, " %8s  %s\n", infos[i].name, _(infos[i].help));

	fprintf(out, USAGE_MAN_TAIL("wipefs(8)"));
	exit(EXIT_SUCCESS);
}


int
main(int argc, char **argv)
{
	struct wipe_control ctl = { .devname = NULL };
	struct wipe_desc *wp0 = NULL;
	int c, noffsets = 0;
	char *outarg = NULL;

	static const struct option longopts[] = {
	    { "all",       no_argument,       NULL, 'a' },
	    { "backup",    no_argument,       NULL, 'b' },
	    { "force",     no_argument,       NULL, 'f' },
	    { "help",      no_argument,       NULL, 'h' },
	    { "no-act",    no_argument,       NULL, 'n' },
	    { "offset",    required_argument, NULL, 'o' },
	    { "parsable",  no_argument,       NULL, 'p' },
	    { "quiet",     no_argument,       NULL, 'q' },
	    { "types",     required_argument, NULL, 't' },
	    { "version",   no_argument,       NULL, 'V' },
	    { "json",      no_argument,       NULL, 'J'},
	    { "noheadings",no_argument,       NULL, 'i'},
	    { "output",    required_argument, NULL, 'O'},
	    { NULL,        0, NULL, 0 }
	};

	static const ul_excl_t excl[] = {       /* rows and cols in ASCII order */
		{ 'O','a','o' },
		{ 0 }
	};
	int excl_st[ARRAY_SIZE(excl)] = UL_EXCL_STATUS_INIT;

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((c = getopt_long(argc, argv, "abfhiJnO:o:pqt:V", longopts, NULL)) != -1) {

		err_exclusive_options(c, longopts, excl, excl_st);

		switch(c) {
		case 'a':
			ctl.all = 1;
			break;
		case 'b':
			ctl.backup = 1;
			break;
		case 'f':
			ctl.force = 1;
			break;
		case 'h':
			usage();
			break;
		case 'J':
			ctl.json = 1;
			break;
		case 'i':
			ctl.no_headings = 1;
			break;
		case 'O':
			outarg = optarg;
			break;
		case 'n':
			ctl.noact = 1;
			break;
		case 'o':
			wp0 = add_offset(wp0, strtosize_or_err(optarg,
					 _("invalid offset argument")), 1);
			noffsets++;
			break;
		case 'p':
			ctl.parsable = 1;
			ctl.no_headings = 1;
			break;
		case 'q':
			ctl.quiet = 1;
			break;
		case 't':
			ctl.type_pattern = optarg;
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	if (optind == argc) {
		warnx(_("no device specified"));
		errtryhelp(EXIT_FAILURE);

	}

	if (ctl.backup && !(ctl.all || noffsets))
		warnx(_("The --backup option is meaningless in this context"));

	if (!ctl.all && !noffsets) {
		/*
		 * Print only
		 */
		if (ctl.parsable) {
			/* keep it backward compatible */
			columns[ncolumns++] = COL_OFFSET;
			columns[ncolumns++] = COL_UUID;
			columns[ncolumns++] = COL_LABEL;
			columns[ncolumns++] = COL_TYPE;
		} else {
			/* default, may be modified by -O <list> */
			columns[ncolumns++] = COL_DEVICE;
			columns[ncolumns++] = COL_OFFSET;
			columns[ncolumns++] = COL_TYPE;
			columns[ncolumns++] = COL_UUID;
			columns[ncolumns++] = COL_LABEL;
		}

		if (outarg
		    && string_add_to_idarray(outarg, columns, ARRAY_SIZE(columns),
					     &ncolumns, column_name_to_id) < 0)
			return EXIT_FAILURE;

		init_output(&ctl);

		while (optind < argc) {
			ctl.devname = argv[optind++];
			wp0 = read_offsets(&ctl, NULL);
			if (wp0)
				add_to_output(&ctl, wp0);
			free_wipe(wp0);
		}
		finalize_output(&ctl);
	} else {
		/*
		 * Erase
		 */
		while (optind < argc) {
			struct wipe_desc *wp = clone_offset(wp0);

			ctl.devname = argv[optind++];
			wp = do_wipe(&ctl, wp);
			free_wipe(wp);
		}
	}

	return EXIT_SUCCESS;
}
