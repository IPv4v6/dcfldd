/* $Id$
 * Enhanced Forensic DD
 * By Nicholas Harbour
 * Originally Modified at the DoD Computer Forensics Lab
 * Based upon GNU dd, which comes as part of the GNU coreutils
 * package from the Free Software Foundation.
 */

/* dd -- convert a file while copying it.
   Copyright (C) 85, 90, 91, 1995-2001 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

/* GNU dd originally written by Paul Rubin, David MacKenzie, and Stuart Kemp. */

#define FILE_OFFSET_BITS 64
#define LARGEFILE_SOURCE

#include "config.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#define SWAB_ALIGN_OFFSET 2

#if HAVE_INTTYPES_H
# include <inttypes.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <fcntl.h>
#include "md5.h"
#include "sha1.h"
#include "sha2.h"
#include "system.h"
#include "getpagesize.h"
#include "human.h"
#include "long-options.h"
#include "safe-read.h"
#include "xstrtol.h"

/* The official name of this program (e.g., no `g' prefix).  */
#define PROGRAM_NAME "dcfldd"
#define PROGRAM_VERSION "1.2"

#define AUTHORS "dcfldd by Nicholas Harbour, GNU dd by Paul Rubin, David MacKenzie and Stuart Kemp"

#ifndef SIGINFO
# define SIGINFO SIGUSR1
#endif

#ifndef S_TYPEISSHM
# define S_TYPEISSHM(Stat_ptr) 0
#endif

#define ROUND_UP_OFFSET(X, M) ((M) - 1 - (((X) + (M) - 1) % (M)))
#define PTR_ALIGN(Ptr, M) ((Ptr) \
                        + ROUND_UP_OFFSET ((char *)(Ptr) - (char *)0, (M)))

#define max(a, b) ((a) > (b) ? (a) : (b))
#define output_char(c)				\
do						\
    {						\
    obuf[oc++] = (c);				\
    if (oc >= output_blocksize)		\
        write_output ();			\
    }						\
while (0)

/* Default input and output blocksize. */
#define DEFAULT_BLOCKSIZE 512

/* Conversions bit masks. */
#define C_ASCII 01
#define C_EBCDIC 02
#define C_IBM 04
#define C_BLOCK 010
#define C_UNBLOCK 020
#define C_LCASE 040
#define C_UCASE 0100
#define C_SWAB 0200
#define C_NOERROR 0400
#define C_NOTRUNC 01000
#define C_SYNC 02000
/* Use separate input and output buffers, and combine partial input blocks. */
#define C_TWOBUFS 04000

int full_write ();

/* The name this program was run with. */
char *program_name;

/* The name of the input file, or NULL for the standard input. */
static char *input_file = NULL;

/* The name of the output file, or NULL for the standard output. */
static char *output_file = NULL;

/* The number of bytes in which atomic reads are done. */
static size_t input_blocksize = 0;

/* The number of bytes in which atomic writes are done. */
static size_t output_blocksize = 0;

/* Conversion buffer size, in bytes.  0 prevents conversions. */
static size_t conversion_blocksize = 0;

/* Skip this many records of `input_blocksize' bytes before input. */
static uintmax_t skip_records = 0;

/* Skip this many records of `output_blocksize' bytes before output. */
static uintmax_t seek_records = 0;

/* Copy only this many records.  The default is effectively infinity.  */
static uintmax_t max_records = (uintmax_t) -1;

/* Bit vector of conversions to apply. */
static int conversions_mask = 0;

/* If nonzero, filter characters through the translation table.  */
static int translation_needed = 0;

/* Number of partial blocks written. */
static uintmax_t w_partial = 0;

/* Number of full blocks written. */
static uintmax_t w_full = 0;

/* Number of partial blocks read. */
static uintmax_t r_partial = 0;

/* Number of full blocks read. */
static uintmax_t r_full = 0;

/* Records truncated by conv=block. */
static uintmax_t r_truncate = 0;

/* Output representation of newline and space characters.
   They change if we're converting to EBCDIC.  */
static unsigned char newline_character = '\n';
static unsigned char space_character = ' ';

/* Output buffer. */
static unsigned char *obuf;

/* Current index into `obuf'. */
static size_t oc = 0;

/* Index into current line, for `conv=block' and `conv=unblock'.  */
static size_t col = 0;

static int do_hash, do_status = 1;

/* Which file (if any) to probe the size of */
enum {PROBE_NONE = 0, PROBE_INPUT, PROBE_OUTPUT};
static int probe = PROBE_NONE; 
static off_t probed_size;
static time_t start_time;

/* Pattern to be written out */
static char *pattern;
static size_t pattern_len;
static int input_from_pattern;

/* Md5 global data */
static MD5_CTX MD5_total_context;
static MD5_CTX MD5_window_context;
static char MD5_hashstr[MD5_DIGEST_STRING_LENGTH + 1];

/* SHA1 global data */
static SHA1Context SHA1_total_context;
static SHA1Context SHA1_window_context;
static char SHA1_hashstr[SHA1_DIGEST_STRING_LENGTH + 1];

/* SHA256 global data */
static SHA256_CTX SHA256_total_context;
static SHA256_CTX SHA256_window_context;
static char SHA256_hashstr[SHA256_DIGEST_STRING_LENGTH + 1];

/* SHA384 global data */
static SHA384_CTX SHA384_total_context;
static SHA384_CTX SHA384_window_context;
static char SHA384_hashstr[SHA384_DIGEST_STRING_LENGTH + 1];

/* SHA512 global data */
static SHA512_CTX SHA512_total_context;
static SHA512_CTX SHA512_window_context;
static char SHA512_hashstr[SHA512_DIGEST_STRING_LENGTH + 1];


static off_t hash_windowlen = 0;
static off_t window_beginning = 0;
static off_t bytes_in_window = 0;
static off_t bytes_in_total = 0;
static FILE *hash_log;
static void *window_context;
static void *total_context;
static (*hashinit)(void *);
static (*hashupdate)(void *, const void *, size_t);
static (*hashfinal)(void *, void *);
static void *hashstr_buf;
static char *hashalgorithm_name;

/* Hash algorithms */
enum {MD5 = 0, SHA1, SHA256, SHA384, SHA512};
static int hash_algorithm = MD5;

struct hashtype
{
    char *name;
    void *window_context;
    void *total_context;
    void (*init)(void *);
    void (*update)(void *, const void *, size_t);
    void (*final)(void *, void *);
    void *hashstr_buf;
};

struct hashtype hashops[] =
{
    {"MD5",
     &MD5_window_context,
     &MD5_total_context,
     (void (*)(void *)) MD5Init,
     (void (*)(const void *, size_t)) MD5Update,
     (void (*)(void *, void *)) MD5Final,
     MD5_hashstr},

    {"SHA1",
     &SHA1_window_context,
     &SHA1_total_context,
     (void (*)(void *)) SHA1Init,
     (void (*)(const void *, size_t)) SHA1Update,
     (void (*)(void *, void *)) SHA1End,
     SHA1_hashstr},

    {"SHA256",
     &SHA256_window_context,
     &SHA256_total_context,
     (void (*)(void *)) SHA256_Init,
     (void (*)(const void *, size_t)) SHA256_Update,
     (void (*)(void *, void *)) SHA256_End,
     SHA256_hashstr},

    {"SHA384",
     &SHA384_window_context,
     &SHA384_total_context,
     (void (*)(void *)) SHA384_Init,
     (void (*)(const void *, size_t)) SHA384_Update,
     (void (*)(void *, void *)) SHA384_End,
     SHA384_hashstr},

    {"SHA512",
     &SHA512_window_context,
     &SHA512_total_context,
     (void (*)(void *)) SHA512_Init,
     (void (*)(const void *, size_t)) SHA512_Update,
     (void (*)(void *, void *)) SHA512_End,
     SHA512_hashstr}     
};

struct conversion
{
    char *convname;
    int conversion;
};

static struct conversion conversions[] =
{
    {"ascii", C_ASCII | C_TWOBUFS},	/* EBCDIC to ASCII. */
    {"ebcdic", C_EBCDIC | C_TWOBUFS},	/* ASCII to EBCDIC. */
    {"ibm", C_IBM | C_TWOBUFS},	/* Slightly different ASCII to EBCDIC. */
    {"block", C_BLOCK | C_TWOBUFS},	/* Variable to fixed length records. */
    {"unblock", C_UNBLOCK | C_TWOBUFS},	/* Fixed to variable length records. */
    {"lcase", C_LCASE | C_TWOBUFS},	/* Translate upper to lower case. */
    {"ucase", C_UCASE | C_TWOBUFS},	/* Translate lower to upper case. */
    {"swab", C_SWAB | C_TWOBUFS},	/* Swap bytes of input. */
    {"noerror", C_NOERROR},	/* Ignore i/o errors. */
    {"notrunc", C_NOTRUNC},	/* Do not truncate output file. */
    {"sync", C_SYNC},		/* Pad input records to ibs with NULs. */
    {NULL, 0}
};

/* Translation table formed by applying successive transformations. */
static unsigned char trans_table[256];

static unsigned char const ascii_to_ebcdic[] =
{
    0, 01, 02, 03, 067, 055, 056, 057,
    026, 05, 045, 013, 014, 015, 016, 017,
    020, 021, 022, 023, 074, 075, 062, 046,
    030, 031, 077, 047, 034, 035, 036, 037,
    0100, 0117, 0177, 0173, 0133, 0154, 0120, 0175,
    0115, 0135, 0134, 0116, 0153, 0140, 0113, 0141,
    0360, 0361, 0362, 0363, 0364, 0365, 0366, 0367,
    0370, 0371, 0172, 0136, 0114, 0176, 0156, 0157,
    0174, 0301, 0302, 0303, 0304, 0305, 0306, 0307,
    0310, 0311, 0321, 0322, 0323, 0324, 0325, 0326,
    0327, 0330, 0331, 0342, 0343, 0344, 0345, 0346,
    0347, 0350, 0351, 0112, 0340, 0132, 0137, 0155,
    0171, 0201, 0202, 0203, 0204, 0205, 0206, 0207,
    0210, 0211, 0221, 0222, 0223, 0224, 0225, 0226,
    0227, 0230, 0231, 0242, 0243, 0244, 0245, 0246,
    0247, 0250, 0251, 0300, 0152, 0320, 0241, 07,
    040, 041, 042, 043, 044, 025, 06, 027,
    050, 051, 052, 053, 054, 011, 012, 033,
    060, 061, 032, 063, 064, 065, 066, 010,
    070, 071, 072, 073, 04, 024, 076, 0341,
    0101, 0102, 0103, 0104, 0105, 0106, 0107, 0110,
    0111, 0121, 0122, 0123, 0124, 0125, 0126, 0127,
    0130, 0131, 0142, 0143, 0144, 0145, 0146, 0147,
    0150, 0151, 0160, 0161, 0162, 0163, 0164, 0165,
    0166, 0167, 0170, 0200, 0212, 0213, 0214, 0215,
    0216, 0217, 0220, 0232, 0233, 0234, 0235, 0236,
    0237, 0240, 0252, 0253, 0254, 0255, 0256, 0257,
    0260, 0261, 0262, 0263, 0264, 0265, 0266, 0267,
    0270, 0271, 0272, 0273, 0274, 0275, 0276, 0277,
    0312, 0313, 0314, 0315, 0316, 0317, 0332, 0333,
    0334, 0335, 0336, 0337, 0352, 0353, 0354, 0355,
    0356, 0357, 0372, 0373, 0374, 0375, 0376, 0377
};

static unsigned char const ascii_to_ibm[] =
{
    0, 01, 02, 03, 067, 055, 056, 057,
    026, 05, 045, 013, 014, 015, 016, 017,
    020, 021, 022, 023, 074, 075, 062, 046,
    030, 031, 077, 047, 034, 035, 036, 037,
    0100, 0132, 0177, 0173, 0133, 0154, 0120, 0175,
    0115, 0135, 0134, 0116, 0153, 0140, 0113, 0141,
    0360, 0361, 0362, 0363, 0364, 0365, 0366, 0367,
    0370, 0371, 0172, 0136, 0114, 0176, 0156, 0157,
    0174, 0301, 0302, 0303, 0304, 0305, 0306, 0307,
    0310, 0311, 0321, 0322, 0323, 0324, 0325, 0326,
    0327, 0330, 0331, 0342, 0343, 0344, 0345, 0346,
    0347, 0350, 0351, 0255, 0340, 0275, 0137, 0155,
    0171, 0201, 0202, 0203, 0204, 0205, 0206, 0207,
    0210, 0211, 0221, 0222, 0223, 0224, 0225, 0226,
    0227, 0230, 0231, 0242, 0243, 0244, 0245, 0246,
    0247, 0250, 0251, 0300, 0117, 0320, 0241, 07,
    040, 041, 042, 043, 044, 025, 06, 027,
    050, 051, 052, 053, 054, 011, 012, 033,
    060, 061, 032, 063, 064, 065, 066, 010,
    070, 071, 072, 073, 04, 024, 076, 0341,
    0101, 0102, 0103, 0104, 0105, 0106, 0107, 0110,
    0111, 0121, 0122, 0123, 0124, 0125, 0126, 0127,
    0130, 0131, 0142, 0143, 0144, 0145, 0146, 0147,
    0150, 0151, 0160, 0161, 0162, 0163, 0164, 0165,
    0166, 0167, 0170, 0200, 0212, 0213, 0214, 0215,
    0216, 0217, 0220, 0232, 0233, 0234, 0235, 0236,
    0237, 0240, 0252, 0253, 0254, 0255, 0256, 0257,
    0260, 0261, 0262, 0263, 0264, 0265, 0266, 0267,
    0270, 0271, 0272, 0273, 0274, 0275, 0276, 0277,
    0312, 0313, 0314, 0315, 0316, 0317, 0332, 0333,
    0334, 0335, 0336, 0337, 0352, 0353, 0354, 0355,
    0356, 0357, 0372, 0373, 0374, 0375, 0376, 0377
};

static unsigned char const ebcdic_to_ascii[] =
{
    0, 01, 02, 03, 0234, 011, 0206, 0177,
    0227, 0215, 0216, 013, 014, 015, 016, 017,
    020, 021, 022, 023, 0235, 0205, 010, 0207,
    030, 031, 0222, 0217, 034, 035, 036, 037,
    0200, 0201, 0202, 0203, 0204, 012, 027, 033,
    0210, 0211, 0212, 0213, 0214, 05, 06, 07,
    0220, 0221, 026, 0223, 0224, 0225, 0226, 04,
    0230, 0231, 0232, 0233, 024, 025, 0236, 032,
    040, 0240, 0241, 0242, 0243, 0244, 0245, 0246,
    0247, 0250, 0133, 056, 074, 050, 053, 041,
    046, 0251, 0252, 0253, 0254, 0255, 0256, 0257,
    0260, 0261, 0135, 044, 052, 051, 073, 0136,
    055, 057, 0262, 0263, 0264, 0265, 0266, 0267,
    0270, 0271, 0174, 054, 045, 0137, 076, 077,
    0272, 0273, 0274, 0275, 0276, 0277, 0300, 0301,
    0302, 0140, 072, 043, 0100, 047, 075, 042,
    0303, 0141, 0142, 0143, 0144, 0145, 0146, 0147,
    0150, 0151, 0304, 0305, 0306, 0307, 0310, 0311,
    0312, 0152, 0153, 0154, 0155, 0156, 0157, 0160,
    0161, 0162, 0313, 0314, 0315, 0316, 0317, 0320,
    0321, 0176, 0163, 0164, 0165, 0166, 0167, 0170,
    0171, 0172, 0322, 0323, 0324, 0325, 0326, 0327,
    0330, 0331, 0332, 0333, 0334, 0335, 0336, 0337,
    0340, 0341, 0342, 0343, 0344, 0345, 0346, 0347,
    0173, 0101, 0102, 0103, 0104, 0105, 0106, 0107,
    0110, 0111, 0350, 0351, 0352, 0353, 0354, 0355,
    0175, 0112, 0113, 0114, 0115, 0116, 0117, 0120,
    0121, 0122, 0356, 0357, 0360, 0361, 0362, 0363,
    0134, 0237, 0123, 0124, 0125, 0126, 0127, 0130,
    0131, 0132, 0364, 0365, 0366, 0367, 0370, 0371,
    060, 061, 062, 063, 064, 065, 066, 067,
    070, 071, 0372, 0373, 0374, 0375, 0376, 0377
};

void
usage (int status)
{
    if (status != 0)
        fprintf (stderr, "Try `%s --help' for more information.\n",
                 program_name);
    else
    {
        printf ("Usage: %s [OPTION]...\n", program_name);
        printf ("\
Copy a file, converting and formatting according to the options.\n\
\n\
bs=BYTES             force ibs=BYTES and obs=BYTES\n\
cbs=BYTES            convert BYTES bytes at a time\n\
conv=KEYWORDS        convert the file as per the comma separated keyword list\n\
count=BLOCKS         copy only BLOCKS input blocks\n\
ibs=BYTES            read BYTES bytes at a time\n\
if=FILE              read from FILE instead of stdin\n\
obs=BYTES            write BYTES bytes at a time\n\
of=FILE              write to FILE instead of stdout\n\
seek=BLOCKS          skip BLOCKS obs-sized blocks at start of output\n\
skip=BLOCKS          skip BLOCKS ibs-sized blocks at start of input\n\
pattern=BYTES        use the specified binary pattern as input\n\
textpattern=TEXT     use repeating TEXT as input\n\
hashwindow=BYTES     perform a hash on every BYTES amount of data\n\
hashalgorithm=NAME   either MD5, SHA1, SHA256, SHA384 or SHA512\n\
                        default algorithm is MD5\n\
hashlog=FILE         send MD5 hash output to FILE instead of stderr\n\
status=[on|off]      display a continual status message on stderr\n\
                        default state is \"on\"\n\
sizeprobe=[if|of]    determine the size of the input or output file\n\
                        for use with status messages. (this option\n\
                        gives you a percentage indicator)\n\
                        WARNING: Read the manual before using this\n\
                                option.\n\
    --help           display this help and exit\n\
    --version        output version information and exit\n\
\n\
BLOCKS and BYTES may be followed by the following multiplicative suffixes:\n\
xM M, c 1, w 2, b 512, kD 1000, k 1024, MD 1,000,000, M 1,048,576,\n\
GD 1,000,000,000, G 1,073,741,824, and so on for T, P, E, Z, Y.\n\
Each KEYWORD may be:\n\
\n\
ascii     from EBCDIC to ASCII\n\
ebcdic    from ASCII to EBCDIC\n\
ibm       from ASCII to alternated EBCDIC\n\
block     pad newline-terminated records with spaces to cbs-size\n\
unblock   replace trailing spaces in cbs-size records with newline\n\
lcase     change upper case to lower case\n\
notrunc   do not truncate the output file\n\
ucase     change lower case to upper case\n\
swab      swap every pair of input bytes\n\
noerror   continue after read errors\n\
sync      pad every input block with NULs to ibs-size; when used\n\
            with block or unblock, pad with spaces rather than NULs\n\
");
        puts ("\nReport bugs to <nicholasharbour@yahoo.com>.");
    }
    exit (status);
}

static void
translate_charset (const unsigned char *new_trans)
{
    unsigned int i;

    for (i = 0; i < 256; i++)
        trans_table[i] = new_trans[trans_table[i]];
    translation_needed = 1;
}

/* Return the number of 1 bits in `i'. */

static int
bit_count (register unsigned int i)
{
    register int set_bits;

    for (set_bits = 0; i != 0; set_bits++)
        i &= i - 1;
    return set_bits;
}

static void
print_stats (void)
{
    char buf[2][LONGEST_HUMAN_READABLE + 1];
    fprintf (stderr, "%s+%s records in\n",
             human_readable (r_full, buf[0], 1, 1),
             human_readable (r_partial, buf[1], 1, 1));
    fprintf (stderr, "%s+%s records out\n",
             human_readable (w_full, buf[0], 1, 1),
             human_readable (w_partial, buf[1], 1, 1));
    if (r_truncate > 0)
    {
        fprintf (stderr, "%s %s\n",
                 human_readable (r_truncate, buf[0], 1, 1),
                 (r_truncate == 1
                  ? "truncated record"
                  : "truncated records"));
    }
}

static void
cleanup (void)
{
    print_stats ();
    if (close (STDIN_FILENO) < 0)
        ;
    if (close (STDOUT_FILENO) < 0)
        ;
}

static inline void
quit (int code)
{
    cleanup ();
    exit (code);
}

static void
interrupt_handler (int sig)
{
#ifdef SA_NOCLDSTOP
    struct sigaction sigact;

    sigact.sa_handler = SIG_DFL;
    sigemptyset (&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction (sig, &sigact, NULL);
#else
    signal (sig, SIG_DFL);
#endif
    cleanup ();
    kill (getpid (), sig);
}

static void
siginfo_handler (int sig)
{
    print_stats ();
}

/* Encapsulate portability mess of establishing signal handlers.  */

static void
install_handler (int sig_num, void (*sig_handler) (int sig))
{
#ifdef SA_NOCLDSTOP
    struct sigaction sigact;
    sigaction (sig_num, NULL, &sigact);
    if (sigact.sa_handler != SIG_IGN)
    {
        sigact.sa_handler = sig_handler;
        sigemptyset (&sigact.sa_mask);
        sigact.sa_flags = 0;
        sigaction (sig_num, &sigact, NULL);
    }
#else
    if (signal (sig_num, SIG_IGN) != SIG_IGN)
        signal (sig_num, sig_handler);
#endif
}

/* Open a file to a particular file descriptor.  This is like standard
   `open', except it always returns DESIRED_FD if successful.  */
static int
open_fd (int desired_fd, char const *filename, int options, mode_t mode)
{
    int fd;
    close (desired_fd);
    fd = open (filename, options, mode);
    if (fd < 0)
        return -1;
    
    if (fd != desired_fd)
    {
        if (dup2 (fd, desired_fd) != desired_fd)
            desired_fd = -1;
        if (close (fd) != 0)
            return -1;
    }
    
    return desired_fd;
}

/* Write, then empty, the output buffer `obuf'. */

static void
write_output (void)
{
    int nwritten = full_write (STDOUT_FILENO, obuf, output_blocksize);
    if (nwritten != output_blocksize)
    {
        if (nwritten > 0)
            w_partial++;
        quit (1);
    }
    else
        w_full++;
    oc = 0;
}

/* Interpret one "conv=..." option.
   As a by product, this function replaces each `,' in STR with a NUL byte.  */

static void
parse_conversion (char *str)
{
    char *new;
    unsigned int i;
    
    do
    {
        new = strchr (str, ',');
        if (new != NULL)
            *new++ = '\0';
        for (i = 0; conversions[i].convname != NULL; i++)
            if (STREQ (conversions[i].convname, str))
            {
                conversions_mask |= conversions[i].conversion;
                break;
            }
        if (conversions[i].convname == NULL)
        {
            fprintf(stderr, "invalid conversion: %s", str);
            usage (1);
        }
        str = new;
    } while (new != NULL);
}

/* Return the value of STR, interpreted as a non-negative decimal integer,
   optionally multiplied by various values.
   Assign nonzero to *INVALID if STR does not represent a number in
   this format. */

static uintmax_t
parse_integer (const char *str, int *invalid)
{
    uintmax_t n;
    char *suffix;
    enum strtol_error e = xstrtoumax (str, &suffix, 10, &n, "bcEGkMPTwYZ0");
    
    if (e == LONGINT_INVALID_SUFFIX_CHAR && *suffix == 'x')
    {
        uintmax_t multiplier = parse_integer (suffix + 1, invalid);
        
        if (multiplier != 0 && n * multiplier / multiplier != n)
        {
            *invalid = 1;
            return 0;
        }
        
        n *= multiplier;
    }
    else if (e != LONGINT_OK)
    {
        *invalid = 1;
        return 0;
    }
    
    return n;
}

static int
hex2char (char *hstr)
{
    int retval;
    
    if (strlen(hstr) != 2)
        return -1;
    if (EOF == sscanf(hstr, "%x", &retval))
        return -1;
    return retval;
}

static char *
make_pattern (char *pattern)
{
    size_t plen, numbytes, i;
    char *buffer;
    
    plen = strlen (pattern);
    if (plen == 0 || plen % 2 != 0)
        return NULL;
    numbytes = plen / 2;
    buffer = malloc (numbytes);
    for (i = 0; i < numbytes; i++) {
        char tmpstring[3];
        int byteval;
        strncpy(tmpstring, &pattern[i*2], 2);
        tmpstring[2] = '\0';
        byteval = hex2char (tmpstring);
        if (byteval == -1) {
            free (buffer);
            return NULL;
        }
        buffer[i] = (char)byteval;
    }
    pattern_len = numbytes;
    return buffer;
}

static void
replicate_pattern (char *pattern, char *buffer, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++)
        buffer[i] = pattern[i % pattern_len];
}

static void
scanargs (int argc, char **argv)
{
    int i;
    
    --argc;
    ++argv;
    
    for (i = optind; i < argc; i++)
    {
        char *name, *val;

        name = argv[i];
        val = strchr (name, '=');
        if (val == NULL)
        {
            fprintf(stderr, "unrecognized option %s", name);
            usage (1);
        }
        *val++ = '\0';
        
        if (STREQ (name, "if"))
            input_file = val;
        else if (STREQ (name, "of"))
            output_file = val;
        else if (STREQ (name, "conv"))
            parse_conversion (val);
        else if (STREQ (name, "pattern")) {
            pattern = make_pattern (val);
            if (pattern == NULL) {
                fprintf(stderr, "invalid hex pattern: %s", val);
                quit (1);
            }
            input_from_pattern = 1;
        }
        else if (STREQ (name, "textpattern")) {
            pattern = val;
            pattern_len = strlen(pattern);
            input_from_pattern = 1;
        }
        else if (STREQ (name, "hashlog")) {
            hash_log = fopen(val, "w");
            if (hash_log == NULL)
                fprintf(stderr, "%s: opening %s", strerror(errno), val);
        }
        else if (STREQ (name, "status")) {
            if (STREQ (val, "off"))
                do_status = 0;
            else if (STREQ (val, "on")) 
                do_status = 1;
        }
        else if (STREQ (name, "hashalgorithm")) {
            if (STREQ (val, "md5"))
                hash_algorithm = MD5;
            else if (STREQ (val, "sha1") || STREQ(val, "sha-1"))
                hash_algorithm = SHA1;
            else if (STREQ (val, "sha256") || STREQ(val, "sha-256"))
                hash_algorithm = SHA256;
            else if (STREQ (val, "sha384") || STREQ(val, "sha-384"))
                hash_algorithm = SHA384;
            else if (STREQ (val, "sha512") || STREQ(val, "sha-512"))
                hash_algorithm = SHA512;
        }
        else if (STREQ (name, "sizeprobe")) {
            if (STREQ (val, "if"))
                probe = PROBE_INPUT;
            else if (STREQ (val, "of"))
                probe = PROBE_OUTPUT;
            else 
                probe = PROBE_NONE;
        }
        else
        {
            int invalid = 0;
            uintmax_t n = parse_integer (val, &invalid);
            
            if (STREQ (name, "ibs"))
            {
                input_blocksize = n;
                invalid |= input_blocksize != n || input_blocksize == 0;
                conversions_mask |= C_TWOBUFS;
            }
            else if (STREQ (name, "obs"))
            {
                output_blocksize = n;
                invalid |= output_blocksize != n || output_blocksize == 0;
                conversions_mask |= C_TWOBUFS;
            }
            else if (STREQ (name, "bs"))
            {
                output_blocksize = input_blocksize = n;
                invalid |= output_blocksize != n || output_blocksize == 0;
            }
            else if (STREQ (name, "cbs"))
            {
                conversion_blocksize = n;
                invalid |= (conversion_blocksize != n
                            || conversion_blocksize == 0);
            }
            else if (STREQ (name, "skip"))
                skip_records = n;
            else if (STREQ (name, "seek"))
                seek_records = n;
            else if (STREQ (name, "count"))
                max_records = n;
            else if (STREQ (name, "hashwindow"))
            {
                hash_windowlen = n;
                do_hash++;
            }
            else
            {
                fprintf(stderr, "unrecognized option %s=%s",
                       name, val);
                usage (1);
            }
            
            if (invalid)
                fprintf(stderr, "invalid number %s", val);
        }
    }
    
/* If bs= was given, both `input_blocksize' and `output_blocksize' will
   have been set to positive values.  If either has not been set,
   bs= was not given, so make sure two buffers are used. */
    if (input_blocksize == 0 || output_blocksize == 0)
        conversions_mask |= C_TWOBUFS;
    if (input_blocksize == 0)
        input_blocksize = DEFAULT_BLOCKSIZE;
    if (output_blocksize == 0)
        output_blocksize = DEFAULT_BLOCKSIZE;
    if (conversion_blocksize == 0)
        conversions_mask &= ~(C_BLOCK | C_UNBLOCK);

    if (do_hash)
    {
        /* Set all the convienence pointers to keep from indirection hell */
        
        struct hashtype *hashop = &hashops[hash_algorithm];

        hashalgorithm_name = hashop->name;
        window_context = hashop->window_context;
        total_context = hashop->total_context;
        hashinit = hashop->init;
        hashupdate = hashop->update;
        hashfinal = hashop->final;
        hashstr_buf = hashop->hashstr_buf;
    }
}

/* Fix up translation table. */

static void
apply_translations (void)
{
    unsigned int i;
    
#define MX(a) (bit_count (conversions_mask & (a)))
    if ((MX (C_ASCII | C_EBCDIC | C_IBM) > 1)
        || (MX (C_BLOCK | C_UNBLOCK) > 1)
        || (MX (C_LCASE | C_UCASE) > 1)
        || (MX (C_UNBLOCK | C_SYNC) > 1))
    {
        fprintf(stderr, "\
only one conv in {ascii,ebcdic,ibm}, {lcase,ucase}, {block,unblock}, {unblock,sync}");
    }
#undef MX
    
    if (conversions_mask & C_ASCII)
        translate_charset (ebcdic_to_ascii);
    
    if (conversions_mask & C_UCASE)
    {
        for (i = 0; i < 256; i++)
            if (ISLOWER (trans_table[i]))
                trans_table[i] = TOUPPER (trans_table[i]);
        translation_needed = 1;
    }
    else if (conversions_mask & C_LCASE)
    {
        for (i = 0; i < 256; i++)
            if (ISUPPER (trans_table[i]))
                trans_table[i] = TOLOWER (trans_table[i]);
        translation_needed = 1;
    }
    
    if (conversions_mask & C_EBCDIC)
    {
        translate_charset (ascii_to_ebcdic);
        newline_character = ascii_to_ebcdic['\n'];
        space_character = ascii_to_ebcdic[' '];
    }
    else if (conversions_mask & C_IBM)
    {
        translate_charset (ascii_to_ibm);
        newline_character = ascii_to_ibm['\n'];
        space_character = ascii_to_ibm[' '];
    }
}

/* Apply the character-set translations specified by the user
   to the NREAD bytes in BUF.  */

static void
translate_buffer (unsigned char *buf, size_t nread)
{
    unsigned char *cp;
    size_t i;
    
    for (i = nread, cp = buf; i; i--, cp++)
        *cp = trans_table[*cp];
}

/* If nonnzero, the last char from the previous call to `swab_buffer'
   is saved in `saved_char'.  */
static int char_is_saved = 0;

/* Odd char from previous call.  */
static unsigned char saved_char;

/* Swap NREAD bytes in BUF, plus possibly an initial char from the
   previous call.  If NREAD is odd, save the last char for the
   next call.   Return the new start of the BUF buffer.  */

static unsigned char *
swab_buffer (unsigned char *buf, size_t *nread)
{
    unsigned char *bufstart = buf;
    register unsigned char *cp;
    register int i;
    
/* Is a char left from last time?  */
    if (char_is_saved)
    {
        *--bufstart = saved_char;
        (*nread)++;
        char_is_saved = 0;
    }
    
    if (*nread & 1)
    {
        /* An odd number of chars are in the buffer.  */
        saved_char = bufstart[--*nread];
        char_is_saved = 1;
    }
    
/* Do the byte-swapping by moving every second character two
   positions toward the end, working from the end of the buffer
   toward the beginning.  This way we only move half of the data.  */
    
    cp = bufstart + *nread;	/* Start one char past the last.  */
    for (i = *nread / 2; i; i--, cp -= 2)
        *cp = *(cp - 2);
    
    return ++bufstart;
}

/* Return nonzero iff the file referenced by FDESC is of a type for
   which lseek's return value is known to be invalid on some systems.
   Otherwise, return zero.
   For example, return nonzero if FDESC references a character device
   (on any system) because the lseek on many Linux systems incorrectly
   returns an offset implying it succeeds for tape devices, even though
   the function fails to perform the requested operation.  In that case,
   lseek should return nonzero and set errno.  */

static int
buggy_lseek_support (int fdesc)
{
/* We have to resort to this because on some systems, lseek doesn't work
   on some special files but doesn't return an error, either.
   In particular, the Linux tape drivers are a problem.
   For example, when I did the following using dd-4.0y or earlier on a
   Linux-2.2.17 system with an Exabyte SCSI tape drive:
   
   dev=/dev/nst0
   reset='mt -f $dev rewind; mt -f $dev fsf 1'
   eval $reset; dd if=$dev bs=32k of=out1
   eval $reset; dd if=$dev bs=32k of=out2 skip=1
   
   the resulting files, out1 and out2, would compare equal.  */
    
    struct stat stats;
    
    return (fstat (fdesc, &stats) == 0
            && (S_ISCHR (stats.st_mode)));
}

/* Throw away RECORDS blocks of BLOCKSIZE bytes on file descriptor FDESC,
   which is open with read permission for FILE.  Store up to BLOCKSIZE
   bytes of the data at a time in BUF, if necessary.  RECORDS must be
   nonzero.  */

static void
skip (int fdesc, char *file, uintmax_t records, size_t blocksize,
      unsigned char *buf)
{
    off_t offset = records * blocksize;
    
/* Try lseek and if an error indicates it was an inappropriate
   operation, fall back on using read.  Some broken versions of
   lseek may return zero, so count that as an error too as a valid
   zero return is not possible here.  */
    
    if (offset / blocksize != records
        || buggy_lseek_support (fdesc)
        || lseek (fdesc, offset, SEEK_CUR) <= 0)
    {
        while (records--)
        {
            ssize_t nread = safe_read (fdesc, buf, blocksize);
            if (nread < 0)
            {
                fprintf(stderr, "%s: reading %s", strerror(errno), file);
                quit (1);
            }
            /* POSIX doesn't say what to do when dd detects it has been
               asked to skip past EOF, so I assume it's non-fatal.
               FIXME: maybe give a warning.  */
            if (nread == 0)
                break;
        }
    }
}

/* Copy NREAD bytes of BUF, with no conversions.  */

static void
copy_simple (unsigned char const *buf, int nread)
{
    int nfree;			/* Number of unused bytes in `obuf'.  */
    const unsigned char *start = buf; /* First uncopied char in BUF.  */
    
    do
    {
        nfree = output_blocksize - oc;
        if (nfree > nread)
            nfree = nread;
        
        memcpy ((char *) (obuf + oc), (char *) start, nfree);
        
        nread -= nfree;		/* Update the number of bytes left to copy. */
        start += nfree;
        oc += nfree;
        if (oc >= output_blocksize)
            write_output ();
    }
    while (nread > 0);
}

/* Copy NREAD bytes of BUF, doing conv=block
   (pad newline-terminated records to `conversion_blocksize',
   replacing the newline with trailing spaces).  */

static void
copy_with_block (unsigned char const *buf, size_t nread)
{
    size_t i;
    
    for (i = nread; i; i--, buf++)
    {
        if (*buf == newline_character)
        {
            if (col < conversion_blocksize)
            {
                size_t j;
                for (j = col; j < conversion_blocksize; j++)
                    output_char (space_character);
            }
            col = 0;
        }
        else
        {
            if (col == conversion_blocksize)
                r_truncate++;
            else if (col < conversion_blocksize)
                output_char (*buf);
            col++;
        }
    }
}

/* Copy NREAD bytes of BUF, doing conv=unblock
   (replace trailing spaces in `conversion_blocksize'-sized records
   with a newline).  */

static void
copy_with_unblock (unsigned char const *buf, size_t nread)
{
    size_t i;
    unsigned char c;
    static int pending_spaces = 0;
    
    for (i = 0; i < nread; i++)
    {
        c = buf[i];
        
        if (col++ >= conversion_blocksize)
        {
            col = pending_spaces = 0; /* Wipe out any pending spaces.  */
            i--;			/* Push the char back; get it later. */
            output_char (newline_character);
        }
        else if (c == space_character)
            pending_spaces++;
        else
        {
            /* `c' is the character after a run of spaces that were not
               at the end of the conversion buffer.  Output them.  */
            while (pending_spaces)
            {
                output_char (space_character);
                --pending_spaces;
            }
            output_char (c);
        }
    }
}

static void
display_windowhash (char *hash, off_t windowlen)
{
    fprintf(hash_log, "%llu - %llu: %s\n", (unsigned long long int)window_beginning,
            (unsigned long long int)(window_beginning + windowlen), hash);
}

static void
display_totalhash (void)
{
    hashfinal(total_context, hashstr_buf);
    fprintf(hash_log, "Total: %s\n", (char *) hashstr_buf);
}

static void
hash_update_buf (void *buf, size_t len)
{
    if (hash_windowlen != 0) {
        hashupdate(window_context, buf, len);
        bytes_in_window += len;
    }
    hashupdate(total_context, buf, len);
    bytes_in_total += len;
}

static void hash_update (void *buf, size_t len)
{
    size_t left_in_window = hash_windowlen - bytes_in_window;

    if (!do_hash)
        return;
    if (bytes_in_total == 0)
        hashinit(total_context);

    if (hash_windowlen == 0)
        hash_update_buf(buf, len);
    else {
        if (bytes_in_window == 0)
            hashinit(window_context);
        if (len >= left_in_window) {
            hash_update_buf(buf, left_in_window);
            hashfinal(window_context, hashstr_buf);
            display_windowhash(hashstr_buf, hash_windowlen);
            window_beginning += hash_windowlen;
            bytes_in_window = 0;
            hash_update(buf + left_in_window, len - left_in_window);
        } else 
            hash_update_buf(buf, len);
    }
}

static void hash_remainder (void)
{
    if (!do_hash)
        return;
    if (hash_windowlen > 0 && bytes_in_window > 0)
    {
        hashfinal(window_context, hashstr_buf);
        display_windowhash(hashstr_buf, bytes_in_window);
    }
}

static void
time_left (char *secstr, size_t bufsize, int seconds)
{
    int hr = seconds / (60 * 60);
    int min = seconds / 60 - hr * 60;
    int sec = seconds - (hr * 60 * 60 + min * 60);

    snprintf(secstr, bufsize, "%.02d:%.02d:%.02d remaining.", hr, min, sec);
}

/* The main loop.  */

static int
dd_copy (void)
{
    unsigned char *ibuf, *bufstart; /* Input buffer. */
    unsigned char *real_buf;	  /* real buffer address before alignment */
    unsigned char *real_obuf;
    ssize_t nread;		/* Bytes read in the current block. */
    int exit_status = 0;
    int input_from_stream = !!input_file;
    int input_from_pattern = !input_from_stream;
    size_t page_size = getpagesize ();
    size_t n_bytes_read;
    const ssize_t update_thresh = 256;
    
    /* Leave at least one extra byte at the beginning and end of `ibuf'
       for conv=swab, but keep the buffer address even.  But some peculiar
       device drivers work only with word-aligned buffers, so leave an
       extra two bytes.  */
    
    /* Some devices require alignment on a sector or page boundary
       (e.g. character disk devices).  Align the input buffer to a
       page boundary to cover all bases.  Note that due to the swab
       algorithm, we must have at least one byte in the page before
       the input buffer;  thus we allocate 2 pages of slop in the
       real buffer.  8k above the blocksize shouldn't bother anyone.
    
       The page alignment is necessary on any linux system that supports
       either the SGI raw I/O patch or Steven Tweedies raw I/O patch.
       It is necessary when accessing raw (i.e. character special) disk
       devices on Unixware or other SVR4-derived system.  */
    
    real_buf = (unsigned char *) malloc (input_blocksize
                                          + 2 * SWAB_ALIGN_OFFSET
                                          + 2 * page_size - 1);
    ibuf = real_buf;
    ibuf += SWAB_ALIGN_OFFSET;	/* allow space for swab */
    
    ibuf = PTR_ALIGN (ibuf, page_size);
    
    if (conversions_mask & C_TWOBUFS)
    {
        /* Page-align the output buffer, too.  */
        real_obuf = (unsigned char *) malloc (output_blocksize + page_size - 1);
        obuf = PTR_ALIGN (real_obuf, page_size);
    }
    else
    {
        real_obuf = NULL;
        obuf = ibuf;
    }
    
    if (!input_from_pattern)
        if (skip_records != 0)
            skip (STDIN_FILENO, input_file, skip_records, input_blocksize, ibuf);
    
    if (seek_records != 0)
    {
        /* FIXME: this loses for
           % ./dd if=dd seek=1 |:
           ./dd: standard output: Bad file descriptor
           0+0 records in
           0+0 records out
        */
        skip (STDOUT_FILENO, output_file, seek_records, output_blocksize, obuf);
    }
    
    if (max_records == 0)
        quit (exit_status);
    
    if (input_from_pattern) {
        replicate_pattern(pattern, ibuf, input_blocksize);
        nread = n_bytes_read = input_blocksize;
    }
    
    while (1)
    {
        /* Display an update message */
        if (do_status && w_full % update_thresh == 0 && w_full != 0)
        {
            off_t total_bytes = w_full * input_blocksize;
            off_t total_mb = total_bytes / 1048576;
    
            if (probe == PROBE_NONE || probed_size == 0)
                fprintf(stderr, "\r%llu blocks (%lluMb) written.", 
                        w_full, total_mb);
            else {
                time_t curr_time = time(NULL);
                int seconds = (int)difftime(curr_time, start_time);
                off_t probed_mb = probed_size / 1048576;
                float fprcnt = total_bytes / (float)probed_size;
                float fprcnt_remaining = 1.0 - fprcnt;
                int prcnt = (int)(fprcnt * 100);
                int seconds_remaining = (int)(seconds * (fprcnt_remaining / fprcnt));
                char secstr[100];
    
                time_left(secstr, sizeof secstr, seconds_remaining);
                fprintf(stderr, "\r[%d%% of %lluMb] %llu blocks (%lluMb) written. %s",
                        prcnt, probed_mb, w_full, total_mb, secstr);
            }	
        }
    
        if (r_partial + r_full >= max_records)
            break;
    
        /* Zero the buffer before reading, so that if we get a read error,
           whatever data we are able to read is followed by zeros.
           This minimizes data loss. */
        if (!input_from_pattern) {
            if ((conversions_mask & C_SYNC) && (conversions_mask & C_NOERROR))
                memset ((char *) ibuf,
                        (conversions_mask & (C_BLOCK | C_UNBLOCK)) ? ' ' : '\0',
                        input_blocksize);
    
            nread = safe_read (STDIN_FILENO, ibuf, input_blocksize);
        }
        if (nread == 0)
            break;			/* EOF.  */
    
        if (nread < 0 && !input_from_pattern)
        {
            fprintf(stderr, "%s: reading %s", strerror(errno), input_file);
            if (conversions_mask & C_NOERROR)
            {
                print_stats ();
                /* Seek past the bad block if possible. */
                lseek (STDIN_FILENO, (off_t) input_blocksize, SEEK_CUR);
                if (conversions_mask & C_SYNC)
                    /* Replace the missing input with null bytes and
                       proceed normally.  */
                    nread = 0;
                else
                    continue;
            }
            else
            {
                /* Write any partial block. */
                exit_status = 2;
                break;
            }
        }
        n_bytes_read = nread;
    
        if (do_hash)
            hash_update(ibuf, n_bytes_read);
        
        if (n_bytes_read < input_blocksize)
        {
            r_partial++;
            if (conversions_mask & C_SYNC)
            {
                if (!(conversions_mask & C_NOERROR))
                    /* If C_NOERROR, we zeroed the block before reading. */
                    memset ((char *) (ibuf + n_bytes_read),
                            (conversions_mask & (C_BLOCK | C_UNBLOCK)) ? ' ' : '\0',
                            input_blocksize - n_bytes_read);
                n_bytes_read = input_blocksize;
            }
        }
        else
            r_full++;
        
        if (ibuf == obuf)		/* If not C_TWOBUFS. */
        {
            int nwritten = full_write (STDOUT_FILENO, obuf, n_bytes_read);
            if (nwritten < 0)
            {
                fprintf(stderr, "%s: writing %s", strerror(errno), output_file);
                quit (1);
            }
            else if (n_bytes_read == input_blocksize)
                w_full++;
            else
                w_partial++;
            continue;
        }
    
        /* Do any translations on the whole buffer at once.  */
    
        if (translation_needed)
            translate_buffer (ibuf, n_bytes_read);
    
        if (conversions_mask & C_SWAB)
            bufstart = swab_buffer (ibuf, &n_bytes_read);
        else
            bufstart = ibuf;
    
        if (conversions_mask & C_BLOCK)
            copy_with_block (bufstart, n_bytes_read);
        else if (conversions_mask & C_UNBLOCK)
            copy_with_unblock (bufstart, n_bytes_read);
        else
            copy_simple (bufstart, n_bytes_read);
    
    }
    
    /* If we have a char left as a result of conv=swab, output it.  */
    if (char_is_saved)
    {
        if (conversions_mask & C_BLOCK)
            copy_with_block (&saved_char, 1);
        else if (conversions_mask & C_UNBLOCK)
            copy_with_unblock (&saved_char, 1);
        else
            output_char (saved_char);
    }
    
    if ((conversions_mask & C_BLOCK) && col > 0)
    {
        /* If the final input line didn't end with a '\n', pad
           the output block to `conversion_blocksize' chars.  */
        unsigned int i;
        for (i = col; i < conversion_blocksize; i++)
            output_char (space_character);
    }
    
    if ((conversions_mask & C_UNBLOCK) && col == conversion_blocksize)
        /* Add a final '\n' if there are exactly `conversion_blocksize'
           characters in the final record. */
        output_char (newline_character);
    
    /* Write out the last block. */
    if (oc != 0)
    {
        int nwritten = full_write (STDOUT_FILENO, obuf, oc);
        if (nwritten > 0)
            w_partial++;
        if (nwritten < 0)
        {
            fprintf(stderr, "%s: writing %s", strerror(errno), output_file);
            quit (1);
        }
    }
    
    free (real_buf);
    if (real_obuf)
        free (real_obuf);
    
    if (do_hash) {
        hash_remainder();
        fprintf(stderr, "\n");
        display_totalhash();
        fprintf(stderr, "Hash Algorithm: %s\n", hashalgorithm_name);
        fprintf(stderr, "\n");
    }
        
    return exit_status;
}

/*
 * Compute a block-resolution midpoint (c) of a and b
 */
static off_t
midpoint (off_t a, off_t b, long blksize)
{
    off_t aprime = a / blksize;
    off_t bprime = b / blksize;
    off_t c, cprime;

    cprime = (bprime - aprime) / 2 + aprime;
    c = cprime * blksize;

    return c;
}

/*
 * Guess the size of a device file.
 */
static off_t
guess_dev_size (int fd, long blksize)
{
    off_t curr = 0, amount = 0; 
    void *buf;
    off_t told;

    if (blksize == 0)
        return 0;

    buf = malloc(blksize);

    for (;;) {
        ssize_t nread;

        lseek(fd, curr, SEEK_SET);
        nread = read(fd, buf, blksize);
        if (nread < blksize) {
            if (nread <= 0) {
                if (curr == amount) {
                    free(buf);
                    lseek(fd, 0, SEEK_SET);
                    return amount;
                }
                curr = midpoint(amount, curr, blksize);
            } else { /* 0 < nread < blksize */
                free(buf);
                lseek(fd, 0, SEEK_SET);
                return amount + nread;
            }
        } else {
            amount = curr + blksize;
            curr = amount * 2;
        }
    }
    free(buf);
    lseek(fd, 0, SEEK_SET);
    return amount;
}

static void
sizeprobe (int fd)
{
    struct stat statbuf;

    if (fstat(fd, &statbuf) == -1) {
        fprintf(stderr, "%s: stating file", strerror(errno));
        return;
    }

    if (S_ISREG(statbuf.st_mode) || S_ISDIR(statbuf.st_mode))
        probed_size = statbuf.st_size;
    else if (S_ISCHR(statbuf.st_mode) || S_ISBLK(statbuf.st_mode))
        probed_size = guess_dev_size(fd, statbuf.st_blksize);
}

int
main (int argc, char **argv)
{
    int i;
    int exit_status;
    
    /* disable buffering on stderr */
    setbuf(stderr, NULL);
    
    hash_log = stderr;
    
    program_name = argv[0];
    
    /* Arrange to close stdout if parse_long_options exits.  */
    //atexit (close_stdout_wrapper);
    
    parse_long_options (argc, argv, PROGRAM_NAME, PACKAGE, VERSION,
                        AUTHORS, usage);
    
    /* Don't close stdout on exit from here on.  */
    //closeout_func = NULL;
    
    /* Initialize translation table to identity translation. */
    for (i = 0; i < 256; i++)
        trans_table[i] = i;
    
    /* Decode arguments. */
    scanargs (argc, argv);
    
    apply_translations ();
    
    if (input_file != NULL)
    {
        if (open_fd (STDIN_FILENO, input_file, O_RDONLY, 0) < 0)
            fprintf(stderr, "%s: opening %s", strerror(errno), input_file);
    }
    else if (pattern == NULL)
        input_file = "standard input";
    
    if (output_file != NULL)
    {
        mode_t perms = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        int opts
            = (O_CREAT
               | (seek_records || (conversions_mask & C_NOTRUNC) ? 0 : O_TRUNC));
    
        /* Open the output file with *read* access only if we might
           need to read to satisfy a `seek=' request.  If we can't read
           the file, go ahead with write-only access; it might work.  */
        if ((! seek_records
             || open_fd (STDOUT_FILENO, output_file, O_RDWR | opts, perms) < 0)
            && open_fd (STDOUT_FILENO, output_file, O_WRONLY | opts, perms) < 0)
            fprintf(stderr, "%s: opening %s", strerror(errno), output_file);
    
#if HAVE_FTRUNCATE
        if (seek_records != 0 && !(conversions_mask & C_NOTRUNC))
        {
            struct stat stdout_stat;
            off_t o = seek_records * output_blocksize;
            if (o / output_blocksize != seek_records)
                fprintf(stderr, "%s: file offset out of range", strerror(errno));
    
            if (fstat (STDOUT_FILENO, &stdout_stat) != 0)
                fprintf(stderr, "%s: cannot fstat %s", strerror(errno), output_file);
    
            /* Complain only when ftruncate fails on a regular file, a
               directory, or a shared memory object, as the 2000-08
               POSIX draft specifies ftruncate's behavior only for these
               file types.  For example, do not complain when Linux 2.4
               ftruncate fails on /dev/fd0.  */
            if (ftruncate (STDOUT_FILENO, o) != 0
                && (S_ISREG (stdout_stat.st_mode)
                    || S_ISDIR (stdout_stat.st_mode)
                    || S_TYPEISSHM (&stdout_stat)))
            {
                char buf[LONGEST_HUMAN_READABLE + 1];
                fprintf(stderr, "%s: advancing past %s bytes in output file %s",
                        strerror(errno),
                        human_readable (o, buf, 1, 1),
                        output_file);
            }
        }
#endif
    }
    else
    {
        output_file = "standard output";
    }
    
    install_handler (SIGINT, interrupt_handler);
    install_handler (SIGQUIT, interrupt_handler);
    install_handler (SIGPIPE, interrupt_handler);
    install_handler (SIGINFO, siginfo_handler);
    
    if (probe == PROBE_INPUT)
        sizeprobe(STDIN_FILENO);
    else if (probe == PROBE_OUTPUT)
        sizeprobe(STDOUT_FILENO);
    start_time = time(NULL);
    
    exit_status = dd_copy ();

    close(1);
    quit (exit_status);
}