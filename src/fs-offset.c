/* fs-offset
 *
 * Move the offset of the ids used for owners and groups in part of a unix filesystem
 *
 * Author:
 * Gerrit Venema  <gmoniker@gmail.com>
 *
 * License: 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */
#define _GNU_SOURCE	//mainly because of canonicalize_file_name otherwise could use _XOPEN_SOURCE 600
#define _FILE_OFFSET_BITS 64
#define EXIT_UNSUPPORTED 2
#include <features.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdbool.h>
#include <sys/acl.h>
#include "./acl_functions.h"

void print_usage(void) {
	fprintf(stderr, "\nUsage:  -b base [-n] [-f] -- treeroot(single absolute path of dir)\
			\n-b : new base uid for tree based at basepath\
			\n-n : only do preliminary tests and print statistics (dryrun)\
			\n-f : force, ignore safety questions\
			\n-p : posix, alter posix acl qualifier ids\
			\ntreeroot : the root of the tree to rebase.\
			\n\nCAUTION: The filesystem under the treeroot must not be accessed when running this program.\n\n");
}

void print_error(const char *msg) {
	fprintf(stderr, "Error: %.255s\n", msg);
}

int64_t g_difference;

struct range {
	uint32_t min,max;
} g_range_origin, g_range_dest;

struct inputs {
	uint32_t offset;
	char *basepath;
	bool nflag;
	bool fflag;
	bool pflag;
} g_inputs;

struct statistics {
	uid_t uid_min, uid_max;
	gid_t gid_min, gid_max;
	unsigned long numfiles, numdirs;
	int maxdepth;
	const char *path;
	bool hashardlinks;
	bool hasposixacls;
} g_stats;

struct stat g_basestat;

bool give_fiat() {
	char *line;
	size_t len;
	bool doit = false;

	line = NULL;
	len = 0;
	printf("Do you want to continue? [y/n]\n");
	while (getline(&line, &len, stdin) >= 0)
	{
		/* Check the response.  */
		int res = rpmatch(line);
		if (res >= 0)
		{
			/* We got a definitive answer.  */
			if (res > 0)
				doit = true;
			break;
		}
	}
	/* Free what getline allocated.  */
	free(line);
	return doit;
}

struct inputs get_inputs(int argc, char **argv) {
	char *cvalue = NULL;
	char *basepath = NULL;
	int c, result;
	struct inputs hold_input;
	uint32_t offset;
	const uint32_t MAX_OFFSET = UINT32_C(0xffffffff) - UINT32_C(0xfc00);

	opterr = 0; //inhibit error messages from getopt
	if (argc == 0 || argc > 10) {
		print_usage();
		exit(1);
	}
	hold_input.nflag = 0;
	hold_input.fflag = 0;
	hold_input.pflag = 0;
	hold_input.basepath = NULL;
	hold_input.offset = 0;
	while(-1 != (c = getopt(argc, argv, "+nfpb:"))) //+ sets posixly_correct mode, no re-ordering
		switch (c)
		{
			case 'b':
				cvalue = optarg;
				break;
			case 'n':
				hold_input.nflag = true;
				break;
			case 'f':
				hold_input.fflag = true;
				break;
			case 'p':
				hold_input.pflag = true;
				break;
			case '?':
				if (optopt == 'b')
					fprintf(stderr, "Option -%c requires an argument.\n", optopt);
				else if (isprint(optopt))
					fprintf(stderr, "Unknown option `-%c'.\n", optopt);
				else
					fprintf(stderr,	"Unknown option character `\\x%x'.\n", optopt);
				print_usage();
				exit(EXIT_FAILURE);
			default:
				exit(EXIT_FAILURE);
		}

	if (cvalue != NULL) {
		//If you enter too large numbers the scanned value will overflow...
		result = sscanf(cvalue,"%" SCNu32, &offset);
		if (result == EOF) {
			result = 0;
		} else if (result != 1) {
			result = 0;
		} else if (offset == 0) {
			//Really take a good look if this was the intended value
			if (strlen(cvalue) != 1) {
				result = 0;
			} else if (cvalue[0] != '0') {
				result = 0;
			} else {
				result = 1;
			}
		} else if (offset > MAX_OFFSET){
			result = 0;
		}
		if (result == 0) {
			fprintf(stderr, "Error: illegal input for offset.\n");
			print_usage();
			exit(EXIT_FAILURE);
		}
	} else {
		print_usage();
		exit(EXIT_FAILURE);
	}
	printf("Offset value: %" PRIu32 ".\n", offset);
	if (optind < argc - 1) {
		print_usage();
		exit(EXIT_FAILURE);
	}
	basepath = argv[argc-1];
	if (basepath[0] != '/') {
		print_error("The basepath must be absolute.");
		exit(EXIT_FAILURE);
	}
	//canonicalize_file_name malloc's the buffer for realpath
	if (NULL == (hold_input.basepath = canonicalize_file_name(basepath))) {
		print_error("The path cannot be matched to the filesystem.");
		exit(errno);
	}
	hold_input.offset = offset;
	return hold_input;
}

int basetest(char *path) {
	//The workdir will be set to path if possible
	struct stat statbuf;
	char tempnam[15] = "SHRDLU$#@!.tmp";
	int fd;
	if (-1 == stat(path, &statbuf)) {
		perror("stat impossible");
		exit(EXIT_FAILURE);
	}
	if (!S_ISDIR(statbuf.st_mode)) {
		print_error("The given basepath is not a directory.");
		exit(EXIT_FAILURE);
	}
	g_basestat = statbuf;
	if (0 < geteuid()) {
		print_error("This program only runs with root privileges.");
		exit(EXIT_FAILURE);
	}
	//Test creating a file on the basepath and modifying ownership
	if (-1 == chdir(path)) {
		perror("chdir");
		exit(EXIT_FAILURE);
	}
	if (-1 == (fd = open(tempnam, O_CREAT | O_EXCL | O_RDONLY, S_IRUSR))) {
		print_error("The program is unable to create its access testing file under the basepath.");
		perror("open");
		exit(EXIT_FAILURE);
	}
	if (-1 == fchown(fd, 0, 0)) {
		print_error("The program is not able to change ownership of its testing file.");
		perror("chown");
		exit(EXIT_FAILURE);
	}
	if (-1 == unlink(tempnam)) {
		print_error("The program is not able to remove its testing file.");
		perror("unlink");
		exit(EXIT_FAILURE);
	}
	close(fd);
	return 0;
}

int dpinfo(const char *fpath, const struct stat *sb, int tflag, struct FTW *ftwbuf) {
	g_stats.path = fpath;
	/*
	  printf("%-3s %2d %7ju   %-40s %d %s\n",
			(tflag == FTW_D) ?   "d"   : (tflag == FTW_DNR) ? "dnr" :
			(tflag == FTW_DP) ?  "dp"  : (tflag == FTW_F) ?   "f" :
			(tflag == FTW_NS) ?  "ns"  : (tflag == FTW_SL) ?  "sl" :
			(tflag == FTW_SLN) ? "sln" : "???",
			ftwbuf->level, (uintmax_t) sb->st_size,
			fpath, ftwbuf->base, fpath + ftwbuf->base);
	*/
	if (sb->st_uid < g_stats.uid_min) g_stats.uid_min = sb->st_uid;
	if (sb->st_uid > g_stats.uid_max) g_stats.uid_max = sb->st_uid;
	if (sb->st_gid < g_stats.gid_min) g_stats.gid_min = sb->st_gid;
	if (sb->st_gid > g_stats.gid_max) g_stats.gid_max = sb->st_gid;	
	if (tflag == FTW_F) {
		g_stats.numfiles++;
		if (sb->st_nlink > 1) g_stats.hashardlinks = true;
	}
	if (tflag == FTW_D) g_stats.numdirs++;
	if (ftwbuf->level > g_stats.maxdepth) g_stats.maxdepth++;
	if (g_inputs.pflag && !g_stats.hasposixacls) { 
		//The assumption is a filesystem where acls on symlinks themselves aren't used
		//This is reported to be untrue for MAC OSX at least for basic POSIX permissions
		if (tflag == FTW_F || tflag == FTW_D) {
			bool isdir = tflag == FTW_D ? true : false;
			if (has_extended_acls_lpath(fpath, isdir)) g_stats.hasposixacls = true;
		} 
	}
	return 0;           /* To tell nftw() to continue */
}

int dprebase(const char *fpath, const struct stat *sb, int tflag, struct FTW *ftwbuf) {
	g_stats.path = fpath;
	if (tflag == FTW_F || tflag == FTW_D) {
		//Try to rebase this dir or file.
		//But watch out for hardlinks multiplying the offset!
		//If there are hardlinks then it must be enforced that origin and destination ranges do not overlap.
		bool doit = true;
		if (tflag == FTW_F && sb->st_nlink > 1) {
			if (sb->st_uid >= g_range_dest.min && sb->st_uid <= g_range_dest.max) {
				//Apparently already rebased
				doit = false;
			}
			if (doit && sb->st_gid >= g_range_dest.min && sb->st_gid <= g_range_dest.max) {
				doit = false;
			}
		}
		if (doit) {
			if (-1 == chown(fpath, g_difference + sb->st_uid, g_difference + sb->st_gid)) {
				return -1; /* tell nftw() to stop */
			}
			//Unfortunately even when root does a chown the S_ISUID and/or S_ISGID bits may have become cleared
			//Probably only on executable files, but lets make sure the mode that goes out is the same that came in
			if (sb->st_mode & S_ISUID || sb->st_mode & S_ISGID) {
				if (-1 == chmod(fpath, sb->st_mode)) {
					return -1; /* tell nftw() to stop */
				}
			}
		}
		if (tflag == FTW_F) g_stats.numfiles++;
		if (tflag == FTW_D) g_stats.numdirs++;	
		if (g_inputs.pflag && g_stats.hasposixacls) {
			if (!rebase_acl(g_difference, fpath, tflag == FTW_D)) {
				return -1; /* tell nftw() to stop */
			}
		}
	}
	if (tflag == FTW_SL) {
		//Take care to NOT dereference the link, this may be forbidden by LSM
		if (-1 == lchown(fpath, g_difference + sb->st_uid, g_difference + sb->st_gid)) {
			return -1; /* tell nftw() to stop */
		}
	}
	return 0;
}

int main (int argc, char **argv)
{
	uint32_t offset_dest;
	int result;
	struct inputs inputs;
	char *realpath = NULL;
	
	inputs = get_inputs(argc, argv);
	g_inputs = inputs;
	offset_dest = inputs.offset;
	realpath = inputs.basepath;
	result = basetest(realpath);
	//workdir is now at realpath
	free(realpath);
	if (-1 == result) {
		print_error("Failure in preliminary selftests for file modification access." );
		exit(EXIT_FAILURE);
	}

	g_stats.uid_min = g_basestat.st_uid;
	g_stats.gid_min = g_basestat.st_gid;
	g_stats.numfiles = 0;
	g_stats.maxdepth = 0;
	g_stats.numdirs = 0;
	g_stats.path = NULL;
	g_stats.hashardlinks = false;
	g_stats.hasposixacls = false;

	if (-1 == nftw(".", dpinfo, 2000, FTW_PHYS)) {
		if (g_stats.path != NULL) {
			fprintf(stderr, "Error: During scan at path <%s>\n", g_stats.path);
		}
		perror("nftw");
		exit(EXIT_FAILURE);
	}
	g_stats.numdirs--;
	printf("Min uid: %ju, Max uid: %ju, Difference: %ju\n", (uintmax_t)g_stats.uid_min, (uintmax_t)g_stats.uid_max, (uintmax_t)(g_stats.uid_max - g_stats.uid_min));
	printf("Min gid: %ju, Max gid: %ju, Difference: %ju\n", (uintmax_t)g_stats.gid_min, (uintmax_t)g_stats.gid_max, (uintmax_t)(g_stats.gid_max - g_stats.gid_min));
	//The number of files includes devices, sockets, pipes, and fifos but not symlinks and does not unduplicate hardlinks
	printf("Number of files (not traversing submounts and not counting symlinks): %ju\n", (uintmax_t)g_stats.numfiles);
	printf("Number of directories: %ju\n", (uintmax_t)g_stats.numdirs);
	printf("Max depth reached: %d\n", g_stats.maxdepth);
	printf("Hardlinks present: %s\n", g_stats.hashardlinks == true ? "yes" : "no");
	printf("POSIX acl present: %s\n", g_stats.hasposixacls == true ? "yes" : "no");
	if (g_stats.gid_min < g_stats.uid_min) {
		print_error("The minimal gid is lower than the minimal uid, this case is not supported.");
		exit(EXIT_UNSUPPORTED);
	}
	g_difference = (int64_t)offset_dest - g_stats.uid_min;
	if ((int64_t)g_stats.uid_max + g_difference > UINT32_MAX) {
		print_error("Can't do the required shift. The uid value would overflow.");
		exit(EXIT_UNSUPPORTED);
	}
	if ((int64_t)g_stats.gid_max + g_difference > UINT32_MAX) {
		print_error("Can't do the required shift. The gid value would overflow.");
		exit(EXIT_UNSUPPORTED);
	}
	if (g_stats.uid_min == inputs.offset) {
		printf("\nNo action necessary, base uid already equal to requested base.\n");
		exit(EXIT_SUCCESS);
	}
	g_range_origin.min = g_stats.uid_min;
	g_range_origin.max = g_stats.uid_max >= g_stats.gid_max ? g_stats.uid_max : g_stats.gid_max;
	g_range_dest.min = inputs.offset;
	g_range_dest.max = g_range_origin.max + g_difference;
	if (g_range_dest.min > g_range_origin.max || g_range_dest.max < g_range_origin.min) {
		;
	} else {
		bool doit = false;
		if (g_stats.hashardlinks) {
			print_error("Destination and origin ranges overlap and there are hardlinks. This case is not supported.");
			exit(EXIT_UNSUPPORTED);
		}
		printf("WARNING: The destination range and origin range of uids are overlapping.\n");
		if (!inputs.fflag && !inputs.nflag) {
			doit = give_fiat();
			if (!doit) {
				printf("Exiting.\n");
				exit(EXIT_SUCCESS);
			}
		}
	}
	printf("Max uid after shift: %ju\n", (uintmax_t)(g_stats.uid_max + g_difference));
	printf("Max gid after shift: %ju\n", (uintmax_t)(g_stats.gid_max + g_difference));
	if (inputs.nflag) {
		printf("\nDry-run requested, the program ends.\n");
		exit(EXIT_SUCCESS);
	}
	g_stats.numfiles = 0;
	g_stats.numdirs = 0;
	g_stats.path = NULL;
	if (-1 == nftw(".", dprebase, 2000, FTW_PHYS)) {
		if (g_stats.path != NULL) {
			fprintf(stderr, "Error: During rebase at path <%s>\n", g_stats.path);
		}
		printf("\n");
		printf("Number of files done: %ju\n", (uintmax_t)g_stats.numfiles);
		perror("nftw");
		exit(EXIT_FAILURE);
	}
	g_stats.numdirs--;
	printf("\n");
	printf("SUCCESS\n");
	printf("Number of files done: %ju\n", (uintmax_t)g_stats.numfiles);
	printf("Number of dirs done: %ju\n", (uintmax_t)g_stats.numdirs);
	exit(0);
}
