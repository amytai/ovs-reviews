/*
 * Copyright (c) 2018, 2019 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* TODO:
 *
 * - mode for selecting records in error.
 * - avoid degzipping whole file at a time.
 * - support tgz or at least tar
 * - bt is slow, use heap+hmap?
 * - tab completion for e.g. component, based on statistics
 * - full query view (show as command-line options?)
 * - checksumming to figure out whether anything has changed behind our back
 * - hitting Enter when there's a single column should limit to matches?
 *   (or bring up a menu of options?)
 * - saving results
 * - backup to previous query (tabs?) - automatically saving the session
 * - adjust page size
 * - pull-down menu interface
 * - obtain context around messages
 * - negated matches
 * - coarser-grained time (day, hour, etc.)
 * - '/' to search within display
 * - right-click on field value to limit display to that value
 * - when a query has no results, explain the query and the results
 * - histograms by time
 */

#include <config.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <ncurses.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include "bt.h"
#include "command-line.h"
#include "fatal-signal.h"
#include "hash.h"
#include "heap.h"
#include "jsonrpc.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/json.h"
#include "openvswitch/poll-loop.h"
#include "ovs-rcu.h"
#include "ovs-thread.h"
#include "ovsdb-error.h"
#include "ovsdb-parser.h"
#include "process.h"
#include "random.h"
#include "rculist.h"
#include "seq.h"
#include "socket-util.h"
#include "sort.h"
#include "sset.h"
#include "stream-fd.h"
#include "svec.h"
#include "util.h"

#include "openvswitch/vlog.h"
VLOG_DEFINE_THIS_MODULE(hv);

/* --remote: If true, this process is to be controlled via JSON-RPC on
 * stdin. */
static bool remote;

/* These are the values defined in RFC 5424. */
enum priority {
    PRI_EMERG = 0,
    PRI_ALERT = 1,
    PRI_CRIT = 2,
    PRI_ERR = 3,
    PRI_WARNING = 4,
    PRI_NOTICE = 5,
    PRI_INFO = 6,
    PRI_DEBUG = 7,
};
#define ALL_PRIORITIES 0xff

static const char *priority_to_string(enum priority);
static bool priority_from_string(const char *, enum priority *);

/* Facilities. */
enum facility {
    LOG_KERN = 0,
    LOG_USER = 1,
    LOG_MAIL = 2,
    LOG_DAEMON = 3,
    LOG_AUTH = 4,
    LOG_SYSLOG = 5,
    LOG_LPR = 6,
    LOG_NEWS = 7,
    LOG_UUCP = 8,
    LOG_CRON = 9,
    LOG_AUTHPRIV = 10,
    LOG_FTP = 11,
    LOG_NTP = 12,
    LOG_AUDIT = 13,
    LOG_ALERT = 14,
    LOG_CLOCK = 15,
    LOG_LOCAL0 = 16,
    LOG_LOCAL1 = 17,
    LOG_LOCAL2 = 18,
    LOG_LOCAL3 = 19,
    LOG_LOCAL4 = 20,
    LOG_LOCAL5 = 21,
    LOG_LOCAL6 = 22,
    LOG_LOCAL7 = 23,
};

#define ALL_FACILITIES ((1u << 24) - 1)

static const char *facility_to_string(int);
static bool facility_from_string(const char *, enum facility *facility);
static char *facilities_from_string(const char *, unsigned int *facilities)
    OVS_WARN_UNUSED_RESULT;

struct topkapi {
    struct log_record *rec;
    long long int count;
};

struct state {
    unsigned long long int population; /* Number of records passed through. */

    struct log_record *reservoir;
    int allocated;              /* Allocated elements of reservoir. */
    int n;                      /* Number of used elements of reservor. */

    /* SHOW_FIRST, SHOW_LAST. */
    struct bt bt;
    unsigned long long int skipped; /* Number of records < spec->start. */

    /* SHOW_TOP. */
#define TK_L 4                  /* Number of hashes */
#define TK_B 1024               /* Number of buckets */
    struct topkapi *tk[TK_L];
};

struct task {
    struct rculist list_node;
    struct job *job;
    char *filename;
    struct jsonrpc *rpc;
    struct json *request_id;
    off_t size;
    struct state state;
};

enum show {
    SHOW_FIRST,
    SHOW_LAST,
    SHOW_SAMPLE,
    SHOW_TOP
};

#define COLUMNS                                 \
    COLUMN(SRC_HOST, "src_host")                \
    COLUMN(SRC_FILE, "src_file")                \
    COLUMN(WHEN, "when")                        \
    COLUMN(FACILITY, "facility")                \
    COLUMN(PRIORITY, "priority")                \
    COLUMN(HOSTNAME, "hostname")                \
    COLUMN(APP_NAME, "app_name")                \
    COLUMN(PROCID, "procid")                    \
    COLUMN(MSGID, "msgid")                      \
    COLUMN(SDID, "sdid")                        \
    COLUMN(COMP, "comp")                        \
    COLUMN(SUBCOMP, "subcomp")                  \
    COLUMN(ERROR_CODE, "error_code")            \
    COLUMN(MSG, "msg")                          \
    COLUMN(LINE, "line")                        \
    COLUMN(VALID, "valid")

enum {
#define COLUMN(ENUM, NAME) COL_IDX_##ENUM,
    COLUMNS
#undef COLUMN
};

enum column {
#define COLUMN(ENUM, NAME) COL_##ENUM = 1u << COL_IDX_##ENUM,
    COLUMNS
#undef COLUMN
};

#define COLUMN(ENUM, NAME) + 1
enum { N_COLUMNS = 0 COLUMNS };
#undef COLUMN

//static char *columns_to_string(enum column);
static struct json *columns_to_json(enum column);
static char *columns_from_string(const char *, enum column *columnsp)
    OVS_WARN_UNUSED_RESULT;
static struct ovsdb_error *columns_from_json(const struct json *array,
                                             enum column *columns)
    OVS_WARN_UNUSED_RESULT;

struct spec {
    enum show show;
    char *host;

    struct log_record *start;
    char *match;
    unsigned int priorities;
    unsigned int facilities;
    struct sset sdids;
    struct sset components;
    struct sset subcomponents;
    struct sset error_codes;
    double date_since;
    double date_until;
    double at;

    enum column columns;
    struct svec targets;
};

static void spec_uninit(struct spec *);
static void spec_copy(struct spec *, const struct spec *);
static struct json *spec_to_json(struct spec *);

struct results {
    struct log_record **recs;
    size_t n;
    unsigned long long int before;
    unsigned long long int after;
};

struct job {
    /* Job specification. */
    struct spec spec;

    /* Job progress. */
    pthread_t thread;
    struct seq *seq;
    atomic_bool cancel;
    atomic_bool done;

    OVSRCU_TYPE(struct state *) state;

    /* Statistics. */
    struct ovs_mutex stats_lock; /* Protects all the members below. */
    unsigned int progress OVS_GUARDED;
    unsigned int goal OVS_GUARDED;

    unsigned long long int total_bytes OVS_GUARDED;
    unsigned long long int total_decompressed OVS_GUARDED;
    unsigned long long int total_recs OVS_GUARDED;

    /* Internals. */
    struct ovs_mutex task_lock;
    struct rculist queued_tasks;
    struct rculist remote_tasks;
    struct rculist completed_tasks;
};

struct substring {
    char *s;
    size_t length;
};

static struct substring
ss_buffer(const char *s, size_t length)
{
    return (struct substring) { CONST_CAST(char *, s), length };
}

static bool
ss_equals(struct substring a, struct substring b)
{
    return a.length == b.length && !memcmp(a.s, b.s, a.length);
}

static int
ss_compare(struct substring a, struct substring b)
{
    int result = memcmp(a.s, b.s, MIN(a.length, b.length));
    return (result ? result
            : a.length < b.length ? -1
            : a.length > b.length);
}

static struct substring
ss_cstr(const char *s)
{
    return (struct substring) { .s = CONST_CAST(char *, s),
                                .length = strlen(s) };
}

static bool OVS_UNUSED
ss_starts_with(struct substring s, struct substring prefix)
{
    return s.length >= prefix.length && !memcmp(s.s, prefix.s, prefix.length);
}

static bool OVS_UNUSED
ss_ends_with(struct substring s, struct substring suffix)
{
    return s.length >= suffix.length && !memcmp(&s.s[s.length - suffix.length],
                                                suffix.s, suffix.length);
}

static bool OVS_UNUSED
ss_contains(struct substring haystack, struct substring needle)
{
    return memmem(haystack.s, haystack.length,
                  needle.s, needle.length) != NULL;
}

static unsigned char
c_tolower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int OVS_UNUSED
ss_find_case(struct substring haystack, struct substring needle)
{
    if (haystack.length < needle.length) {
        return -1;
    }
    for (size_t i = 0; i < haystack.length - needle.length; i++) {
        for (size_t j = 0; j < needle.length; j++) {
            if (c_tolower(haystack.s[i + j]) != c_tolower(needle.s[j])) {
                goto mismatch;
            }
        }
        return i;

    mismatch: ;
    }
    return -1;
}

static int OVS_UNUSED
ss_rfind(struct substring haystack, struct substring needle)
{
    if (haystack.length < needle.length) {
        return -1;
    }
    for (int i = haystack.length - needle.length + 1; i > 0; i--) {
        if (!memcmp(&haystack.s[i], needle.s, needle.length)) {
            return i;
        }
    }
    return -1;
}

static uint32_t
ss_hash(struct substring substring, uint32_t basis)
{
    return hash_bytes(substring.s, substring.length, basis);
}

static struct substring
ss_clone(struct substring substring)
{
    return (struct substring) { .s = xmemdup(substring.s, substring.length),
                                .length = substring.length };
}

static char *
ss_xstrdup(struct substring substring)
{
    return xmemdup0(substring.s, substring.length);
}

static struct substring
ss_rstrip(struct substring s)
{
    while (s.length > 0 && s.s[s.length - 1] == ' ') {
        s.length--;
    }
    return s;
}

enum log_record_type {
    REC_TYPE_RFC5424,
    REC_TYPE_OTHER
};

struct log_record {
    struct bt_node bt_node;
    long long int count;
    struct substring src_host;
    struct substring src_file;
    bool valid;                 /* Fully parsed record? */
    struct substring line;      /* Full log line. */
    enum facility facility;     /* 0...23. */
    enum priority priority;     /* 0...7. */
    struct substring timestamp; /* Date and time. */
    double when;                /* Seconds since the epoch. */
    struct substring hostname;  /* Hostname. */
    struct substring app_name;  /* Application. */
    struct substring procid;    /* Process ID. */
    struct substring msgid;     /* Message ID. */
    struct substring sdid;      /* Structured data ID. */
    struct substring comp;      /* From structured data. */
    struct substring subcomp;   /* From structured data. */
    struct substring error_code; /* From structured data. */
    struct substring msg;       /* Message content. */
};

static void usage(void);
static void parse_command_line(int argc, char *argv[], struct spec *);

struct parse_ctx {
    const char *host;
    const char *fn;
    int ln;

    const char *line_start;
    const char *line_end;
    const char *p;
};

static void OVS_PRINTF_FORMAT(2, 3)
warn(const struct parse_ctx *ctx, const char *format, ...)
{
    if (VLOG_IS_WARN_ENABLED()) {
        va_list args;
        va_start(args, format);
        char *msg = xvasprintf(format, args);
        va_end(args);

        VLOG_WARN("%s:%d.%"PRIdPTR": %s",
                  ctx->fn, ctx->ln, ctx->p - ctx->line_start + 1, msg);
        free(msg);
    }
}

static void OVS_PRINTF_FORMAT(2, 3)
debug(const struct parse_ctx *ctx, const char *format, ...)
{
    if (VLOG_IS_DBG_ENABLED()) {
        va_list args;
        va_start(args, format);
        char *msg = xvasprintf(format, args);
        va_end(args);

        VLOG_DBG("%s:%d.%"PRIdPTR": %s",
                 ctx->fn, ctx->ln, ctx->p - ctx->line_start + 1, msg);
        free(msg);
    }
}

static bool
match_spaces(struct parse_ctx *ctx)
{
    if (*ctx->p != ' ') {
        return false;
    }
    do {
        ctx->p++;
    } while (*ctx->p == ' ');
    return true;
}

static bool
must_match_spaces(struct parse_ctx *ctx)
{
    bool matched = match_spaces(ctx);
    if (!matched) {
        warn(ctx, "expected ' '");
    }
    return matched;
}

static bool
match(struct parse_ctx *ctx, char c)
{
    if (*ctx->p == c) {
        ctx->p++;
        return true;
    } else {
        return false;
    }
}

static bool
must_match(struct parse_ctx *ctx, char c)
{
    bool matched = match(ctx, c);
    if (!matched) {
        warn(ctx, "expected '%c'", c);
    }
    return matched;
}

static bool
c_isdigit(int c)
{
    return c >= '0' && c <= '9';
}

static bool OVS_UNUSED
c_isalpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool
get_header_token__(struct parse_ctx *ctx, char delimiter,
                   struct substring *token)
{
    token->s = CONST_CAST(char *, ctx->p);
    token->length = 0;
    while (token->s[token->length] != delimiter) {
        if (token->s[token->length] == '\n') {
            warn(ctx, "unexpected end of message parsing header");
            return false;
        }
        token->length++;
    }
    ctx->p += token->length;

    return true;
}

static bool
get_header_token(struct parse_ctx *ctx, struct substring *token)
{
    return must_match_spaces(ctx) && get_header_token__(ctx, ' ', token);
}

static bool
get_sd_name(struct parse_ctx *ctx, struct substring *dst)
{
    dst->s = CONST_CAST(char *, ctx->p);
    dst->length = strcspn (dst->s, " =]\"\n");
    if (dst->length == 0) {
        warn(ctx, "parse error expecting SDNAME");
        return false;
    }
    ctx->p += dst->length;
    return true;
}

static bool
get_sd_param(struct parse_ctx *ctx, struct log_record *rec)
{
    struct substring name;
    if (!get_sd_name(ctx, &name)) {
        return false;
    }

    if (!must_match(ctx, '=') || !must_match(ctx, '"')) {
        return false;
    }

    struct substring value;
    value.s = CONST_CAST(char *, ctx->p);
    for (;;) {
        if (*ctx->p == '\\' && ctx->p[1] != '\n') {
            ctx->p++;
        } else if (*ctx->p == '"') {
            break;
        } else if (*ctx->p == '\n') {
            warn(ctx, "unexpected end of line parsing parameter value");
            return false;
        }
        ctx->p++;
    }
    value.length = ctx->p - value.s;
    ctx->p++;                   /* Skip end quote. */

    if (ss_equals(name, ss_cstr("comp"))) {
        rec->comp = value;
    } else if (ss_equals(name, ss_cstr("subcomp"))) {
        rec->subcomp = value;
    } else if (ss_equals(name, ss_cstr("errorCode"))) {
        rec->error_code = value;
    }
    return true;
}

static bool
matches_template(const char *s, const char *template)
{
    size_t i;
    for (i = 0; template[i]; i++) {
        if (!(template[i] == '#' ? c_isdigit(s[i]) : s[i] == template[i])) {
            return false;
        }
    }
    return true;
}

static int
atoi2(const char *s)
{
    int d1 = s[0] - '0';
    int d2 = s[1] - '0';
    return d1 * 10 + d2;
}

static int
atoi4(const char *s)
{
    int d1 = s[0] - '0';
    int d2 = s[1] - '0';
    int d3 = s[2] - '0';
    int d4 = s[3] - '0';
    return d1 * 1000 + d2 * 100 + d3 * 10 + d4;
}

static bool
is_leap_year (int y)
{
  return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}

/* Expects y >= 1900, 1 <= m <= 12, 1 <= d <= 31. */
static int
ymd_to_julian(int y, int m, int d)
{
    return (365 * (y - 1)
            + (y - 1) / 4
            - (y - 1) / 100
            + (y - 1) / 400
            + (367 * m - 362) / 12
            + (m <= 2 ? 0 : (m >= 2 && is_leap_year (y) ? -1 : -2))
            + d);
}

static int
epoch(void)
{
    return ymd_to_julian(1970, 1, 1);
}

static double
parse_timestamp(const char *s, size_t len)
{
    if (len == 1 && s[0] == '-') {
        return 0;
    }

    static const char template[] = "####-##-##T##:##:##";
    if (len < strlen(template + 1) || !matches_template(s, template)) {
        return -1;
    }

    int tz_ofs = strlen(template);
    int numerator = 0;
    int denominator = 1;
    if (s[tz_ofs] == '.') {
        for (tz_ofs++; tz_ofs < len; tz_ofs++) {
            int c = s[tz_ofs];
            if (!c_isdigit(c) || denominator > INT_MAX / 10) {
                break;
            }
            numerator = numerator * 10 + (c - '0');
            denominator *= 10;
        }
    }

    int gmtoff;
    if (tz_ofs >= len) {
        return -1;
    }
    if (len - tz_ofs == 1 && s[tz_ofs] == 'Z') {
        gmtoff = 0;
    } else if (len - tz_ofs == 6
               && (s[tz_ofs] == '+' || s[tz_ofs] == '-')
               && matches_template(&s[tz_ofs + 1], "##:##")) {
        int h_off = atoi2(&s[tz_ofs + 1]);
        int m_off = atoi2(&s[tz_ofs + 4]);
        gmtoff = h_off * 60 + m_off;
        if (s[tz_ofs] == '-') {
            gmtoff = -gmtoff;
        }
    } else {
        return -1;
    }

    int y = atoi4(s);
    int m = atoi2(s + 5);
    int d = atoi2(s + 5);
    int H = atoi2(s + 11);
    int M = atoi2(s + 14);
    int S = atoi2(s + 17);
    int date = ymd_to_julian(y, m, d) - epoch();
    int time = H * 3600 + M * 60 + S - gmtoff * 60;
    double t = date * 86400 + time;
    if (numerator) {
        t += (double) numerator / denominator;
    }
    return t;
}

static void
format_timestamp(double t, struct ds *s)
{
    time_t time = t;
    struct tm tm;
    if (gmtime_r(&time, &tm) != &tm) {
        ds_put_format(s, "<error>");
        return;
    }

    int msec = (t - floor(t)) * 1000 + .5;
    ds_put_format(s, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, MIN(999, msec));
}

static bool
parse_structured_data(struct parse_ctx *ctx, struct log_record *rec)
{
    if (match(ctx, '[')) {
        if (!get_sd_name(ctx, &rec->sdid)) {
            return false;
        }
        while (match_spaces(ctx)) {
            if (!get_sd_param(ctx, rec)) {
                return false;
            }
        }
        if (!must_match(ctx, ']')) {
            return false;
        }
    } else if (!match(ctx, '-')) {
        /* Some NSX log files have this problem.  Keep going. */
        debug(ctx, "expected '-' or '['");
    }
    return true;
}

static bool
starts_with_date(struct substring s)
{
    /* This code has a Y2100 problem ;-) */
    return (s.length >= 20
            && s.s[0] == '2'
            && s.s[1] == '0'
            && c_isdigit(s.s[2])
            && c_isdigit(s.s[3])
            && s.s[4] == '-'
            && s.s[10] == 'T');
}

static enum log_record_type
classify_log_record(const char *buffer, size_t size)
{
    return (size >= 2 && buffer[0] == '<' && c_isdigit(buffer[1])
            ? REC_TYPE_RFC5424
            : starts_with_date(ss_buffer(buffer, size))
            ? REC_TYPE_OTHER
            : 0);
}

static bool
parse_rfc5424_record(struct parse_ctx *ctx, struct log_record *rec)
{
    rec->count = 1;

    rec->src_host = ss_cstr(ctx->host);
    rec->src_file = ss_cstr(ctx->fn);

    /* PRI. */
    if (!must_match(ctx, '<')) {
        return false;
    }
    unsigned int pri = 0;
    while (c_isdigit(*ctx->p)) {
        pri = pri * 10 + (*ctx->p++ - '0');
    }
    rec->facility = pri / 8;
    rec->priority = pri % 8;
    if (!must_match(ctx, '>')) {
        return false;
    }

    /* VERSION. */
    if (!must_match(ctx, '1')) {
        return false;
    }

    /* Timestamp. */
    if (!get_header_token(ctx, &rec->timestamp)) {
        return false;
    }

    rec->when = parse_timestamp(rec->timestamp.s, rec->timestamp.length);
    if (rec->when == -1) {
        return false;
    }

    if (!get_header_token(ctx, &rec->hostname)
        || !get_header_token(ctx, &rec->app_name)
        || !get_header_token(ctx, &rec->procid)) {
        return false;
    }

    /* Workaround for log lines that have an empty string for the msgid rather
     * than -, e.g.:
     * <182>1 2018-12-13T00:39:00.669Z nsx-manager NSX 24302  - type=COUNTER, name=com.vmware.nsx.rpc.NsxRpcProvider.created, count=1
    */
    if (ctx->p[0] == ' ' && ctx->p[1] == ' '
        && (ctx->p[2] == '-' || ctx->p[2] == '[')) {
        ctx->p++;
    } else if (!get_header_token(ctx, &rec->msgid)) {
        return false;
    }

    /* Structured data. */
    if (!must_match_spaces(ctx) || !parse_structured_data(ctx, rec)) {
        return false;
    }

    match_spaces(ctx);

    rec->msg.s = CONST_CAST(char *, ctx->p);
    rec->msg.length = ctx->line_end - ctx->p;

    rec->valid = true;

    return true;
}

#if 0
static int
compare_strings(const void *a_, const void *b_)
{
    const char *const *a = a_;
    const char *const *b = b_;
    return strcmp(*a, *b);
}
#endif

static bool
has_digit(const char *s)
{
    for (; *s; s++) {
        if (c_isdigit(*s)) {
            return true;
        }
    }
    return false;
}

static void OVS_UNUSED
split(const struct substring *msg)
{
    char *tokens[64];
    size_t n_tokens = 0;

    char *s = xmemdup0(msg->s, msg->length);
    char *save_ptr = NULL;
    for (char *token = strtok_r(s, " \t\r\n", &save_ptr); token;
         token = strtok_r(NULL, " \t\r\n", &save_ptr)) {
        tokens[n_tokens++] = has_digit(token) ? "_" : token;
        if (n_tokens >= ARRAY_SIZE(tokens)) {
            break;
        }
    }

    if (n_tokens) {
        for (size_t i = 0; i < n_tokens; i++) {
#if 0
            if (i) {
                putchar(' ');
            }
            fputs(tokens[i], stdout);
#endif
        }
        //putchar('\n');
    }

    free(s);
}

static void
log_record_copy(struct log_record *dst, const struct log_record *src)
{
    dst->count = src->count;
    dst->valid = src->valid;
    dst->src_host = ss_clone(src->src_host);
    dst->src_file = ss_clone(src->src_file);
    dst->line = ss_clone(src->line);
    dst->facility = src->facility;
    dst->priority = src->priority;
    dst->timestamp = ss_clone(src->timestamp);
    dst->when = src->when;
    dst->hostname = ss_clone(src->hostname);
    dst->app_name = ss_clone(src->app_name);
    dst->procid = ss_clone(src->procid);
    dst->msgid = ss_clone(src->msgid);
    dst->sdid = ss_clone(src->sdid);
    dst->comp = ss_clone(src->comp);
    dst->subcomp = ss_clone(src->subcomp);
    dst->error_code = ss_clone(src->error_code);
    dst->msg = ss_clone(src->msg);
}

static void
json_put_substring(struct json *obj, const char *name, struct substring value)
{
    json_object_put(obj, name, json_string_create_nocopy(ss_xstrdup(value)));
}

static struct json *
log_record_to_json(const struct log_record *r, enum column columns)
{
    struct json *obj = json_object_create();
    if (r->count != 1) {
        json_object_put(obj, "count", json_integer_create(r->count));
    }
    if (columns & COL_VALID && !r->valid) {
        json_object_put(obj, "valid", json_boolean_create(r->valid));
    }
    if (columns & COL_SRC_HOST) {
        json_put_substring(obj, "src_host", r->src_host);
    }
    if (columns & COL_SRC_FILE) {
        json_put_substring(obj, "src_file", r->src_file);
    }
    if (columns & COL_LINE) {
        json_put_substring(obj, "line", r->line);
    }
    if (columns & COL_FACILITY) {
        json_object_put_string(obj, "facility",
                               facility_to_string(r->facility));
    }
    if (columns & COL_PRIORITY) {
        json_object_put_string(obj, "priority",
                               priority_to_string(r->priority));
    }
    if (columns & COL_WHEN) {
        json_object_put(obj, "when", json_real_create(r->when));
    }
    if (columns & COL_HOSTNAME) {
        json_put_substring(obj, "hostname", r->hostname);
    }
    if (columns & COL_APP_NAME) {
        json_put_substring(obj, "app_name", r->app_name);
    }
    if (columns & COL_PROCID) {
        json_put_substring(obj, "procid", r->procid);
    }
    if (columns & COL_MSGID) {
        json_put_substring(obj, "msgid", r->msgid);
    }
    if (columns & COL_SDID) {
        json_put_substring(obj, "sdid", r->sdid);
    }
    if (columns & COL_COMP) {
        json_put_substring(obj, "component", r->comp);
    }
    if (columns & COL_SUBCOMP) {
        json_put_substring(obj, "subcomponent", r->subcomp);
    }
    if (columns & COL_ERROR_CODE) {
        json_put_substring(obj, "error_code", r->error_code);
    }
    if (columns & COL_MSG) {
        json_put_substring(obj, "msg", r->msg);
    }
    return obj;
}

static void
parse_substring(struct ovsdb_parser *p, const char *name,
                struct substring *value)
{
    const struct json *string = ovsdb_parser_member(
        p, name, OP_STRING | OP_OPTIONAL);
    if (string) {
        *value = ss_clone(ss_cstr(json_string(string)));
    }
}

static void
log_record_uninit(struct log_record *r)
{
    if (r) {
        free(r->src_host.s);
        free(r->src_file.s);
        free(r->line.s);
        free(r->timestamp.s);
        free(r->hostname.s);
        free(r->app_name.s);
        free(r->procid.s);
        free(r->msgid.s);
        free(r->sdid.s);
        free(r->comp.s);
        free(r->subcomp.s);
        free(r->error_code.s);
        free(r->msg.s);
    }
}

static void
log_record_destroy(struct log_record *r)
{
    if (r) {
        log_record_uninit(r);
        free(r);
    }
}

static struct log_record *
log_record_clone(const struct log_record *src)
{
    struct log_record *dst = xmalloc(sizeof *dst);
    log_record_copy(dst, src);
    return dst;
}

static uint32_t
log_record_hash(const struct log_record *r, uint32_t basis,
                enum column columns)
{
    uint32_t hash = basis;
    for (; columns; columns = zero_rightmost_1bit(columns)) {
        switch (rightmost_1bit(columns)) {
        case COL_SRC_HOST:
            hash = ss_hash(r->src_host, basis);
            break;
        case COL_SRC_FILE:
            hash = ss_hash(r->src_file, basis);
            break;
        case COL_WHEN:
            hash = hash_double(r->when, basis);
            break;
        case COL_FACILITY:
            hash = hash_int(r->facility, basis);
            break;
        case COL_PRIORITY:
            hash = hash_int(r->priority, basis);
            break;
        case COL_HOSTNAME:
            hash = ss_hash(r->hostname, basis);
            break;
        case COL_APP_NAME:
            hash = ss_hash(r->app_name, basis);
            break;
        case COL_PROCID:
            hash = ss_hash(r->procid, basis);
            break;
        case COL_MSGID:
            hash = ss_hash(r->msgid, basis);
            break;
        case COL_SDID:
            hash = ss_hash(r->sdid, basis);
            break;
        case COL_COMP:
            hash = ss_hash(r->comp, basis);
            break;
        case COL_SUBCOMP:
            hash = ss_hash(r->subcomp, basis);
            break;
        case COL_ERROR_CODE:
            hash = ss_hash(r->error_code, basis);
            break;
        case COL_MSG:
            hash = ss_hash(r->msg, basis);
            break;
        case COL_LINE:
            hash = ss_hash(r->line, basis);
            break;
        case COL_VALID:
            hash = hash_boolean(r->valid, basis);
            break;
        default:
            OVS_NOT_REACHED();
        }
    }
    return hash;
}

static int
compare_double(double a, double b)
{
    return a < b ? -1 : a > b;
}

static int
compare_int(int a, int b)
{
    return a < b ? -1 : a > b;
}

static int
log_record_compare(const struct log_record *a, const struct log_record *b,
                   const struct spec *spec)
{
    for (enum column columns = spec->columns; columns;
         columns = zero_rightmost_1bit(columns)) {
        int cmp;

        switch (rightmost_1bit(columns)) {
        case COL_SRC_HOST:
            cmp = ss_compare(a->src_host, b->src_host);
            break;
        case COL_SRC_FILE:
            cmp = ss_compare(a->src_file, b->src_file);
            break;
        case COL_WHEN:
            cmp = compare_double(a->when, b->when);
            break;
        case COL_FACILITY:
            cmp = compare_int(a->facility, b->facility);
            break;
        case COL_PRIORITY:
            cmp = compare_int(a->priority, b->priority);
            break;
        case COL_HOSTNAME:
            cmp = ss_compare(a->hostname, b->hostname);
            break;
        case COL_APP_NAME:
            cmp = ss_compare(a->app_name, b->app_name);
            break;
        case COL_PROCID:
            /* XXX It would be better to compare numerically if possible,
             * e.g. like the GNU function strverscmp(). */
            cmp = ss_compare(a->procid, b->procid);
            break;
        case COL_MSGID:
            cmp = ss_compare(a->msgid, b->msgid);
            break;
        case COL_SDID:
            cmp = ss_compare(a->sdid, b->sdid);
            break;
        case COL_COMP:
            cmp = ss_compare(a->comp, b->comp);
            break;
        case COL_SUBCOMP:
            cmp = ss_compare(a->subcomp, b->subcomp);
            break;
        case COL_ERROR_CODE:
            cmp = ss_compare(a->error_code, b->error_code);
            break;
        case COL_MSG:
            cmp = ss_compare(a->msg, b->msg);
            break;
        case COL_LINE:
            cmp = ss_compare(a->line, b->line);
            break;
        case COL_VALID:
            cmp = compare_int(a->valid, b->valid);
            break;
        default:
            OVS_NOT_REACHED();
        }
        if (cmp) {
            return spec->show == SHOW_LAST ? -cmp : cmp;
        }
    }
    return 0;
}

static int
compare_log_records_for_bt(const struct bt_node *a_,
                           const struct bt_node *b_,
                           const void *spec_)
{
    const struct spec *spec = spec_;
    const struct log_record *a = CONTAINER_OF(a_, struct log_record, bt_node);
    const struct log_record *b = CONTAINER_OF(b_, struct log_record, bt_node);
    return log_record_compare(a, b, spec);
}

static struct ovsdb_error *
log_record_from_json(const struct json *json, struct log_record **rp)
{
    struct log_record *r = xzalloc(sizeof *r);

    struct ovsdb_parser p;
    ovsdb_parser_init(&p, json, "log_record");

    const struct json *count = ovsdb_parser_member(
        &p, "count", OP_INTEGER | OP_OPTIONAL);
    r->count = count ? json_integer(count) : 1;

    parse_substring(&p, "src_host", &r->src_host);
    parse_substring(&p, "src_file", &r->src_file);

    const struct json *valid = ovsdb_parser_member(
        &p, "valid", OP_BOOLEAN | OP_OPTIONAL);
    r->valid = valid ? json_boolean(valid) : true;

    const struct json *facility = ovsdb_parser_member(
        &p, "facility", OP_STRING | OP_OPTIONAL);
    if (facility
        && !facility_from_string(json_string(facility), &r->facility)) {
        ovsdb_parser_raise_error(&p, "%s: unknown facility",
                                 json_string(facility));
    }

    const struct json *priority = ovsdb_parser_member(
        &p, "priority", OP_STRING | OP_OPTIONAL);
    if (priority
        && !priority_from_string(json_string(priority), &r->priority)) {
        ovsdb_parser_raise_error(&p, "%s: unknown priority",
                                 json_string(priority));
    }

    const struct json *when = ovsdb_parser_member(
        &p, "when", OP_NUMBER | OP_OPTIONAL);
    if (when) {
        r->when = json_real(when);
    }

    parse_substring(&p, "hostname", &r->hostname);
    parse_substring(&p, "app_name", &r->app_name);
    parse_substring(&p, "procid", &r->procid);
    parse_substring(&p, "msgid", &r->msgid);
    parse_substring(&p, "sdid", &r->sdid);
    parse_substring(&p, "component", &r->comp);
    parse_substring(&p, "subcomponent", &r->subcomp);
    parse_substring(&p, "error_code", &r->error_code);
    parse_substring(&p, "msg", &r->msg);
    parse_substring(&p, "line", &r->line);

    struct ovsdb_error *error = ovsdb_parser_finish(&p);
    if (error) {
        log_record_destroy(r);
        free(r);
        *rp = NULL;
        return error;
    }
    *rp = r;
    return NULL;
}

static void
state_init(struct state *state, const struct spec *spec)
{
    memset(state, 0, sizeof *state);
    state->allocated = 1000;
    state->reservoir = xmalloc(state->allocated * sizeof *state->reservoir);
    state->n = 0;
    state->population = 0;
    state->skipped = 0;
    if (spec->show == SHOW_FIRST || spec->show == SHOW_LAST) {
        bt_init(&state->bt, compare_log_records_for_bt, spec);
    } else {
        for (int i = 0; i < TK_L; i++) {
            state->tk[i] = xcalloc(TK_B, sizeof *state->tk[i]);
        }
    }
}

static void
state_add(struct state *state, const struct log_record *rec,
          const struct spec *spec)
{
    state->population++;
    if (spec->show == SHOW_SAMPLE) {
        size_t idx = (state->n < state->allocated
                      ? state->n++
                      : random_uint64() % state->population);
        if (idx < state->allocated) {
            state->reservoir[idx] = *rec;
        }
    } else if (spec->show == SHOW_FIRST || spec->show == SHOW_LAST) {
        struct bt_node *last = NULL;
        if (state->n >= state->allocated) {
            last = bt_last(&state->bt);
            if (compare_log_records_for_bt(&rec->bt_node, last, spec) > 0) {
                return;
            }
        }

        struct bt_node *node = bt_find(&state->bt, &rec->bt_node);
        if (node) {
            struct log_record *pos = CONTAINER_OF(node, struct log_record,
                                                  bt_node);
            pos->count += rec->count;
        } else {
            struct log_record *pos;
            if (state->n < state->allocated) {
                pos = &state->reservoir[state->n++];
            } else {
                bt_delete(&state->bt, last);
                pos = CONTAINER_OF(last, struct log_record, bt_node);
            }
            *pos = *rec;
            bt_insert(&state->bt, &pos->bt_node);
        }
    } else if (spec->show == SHOW_TOP) {
        for (int i = 0; i < TK_L; i++) {
            uint32_t hash = log_record_hash(rec, i, spec->columns);
            struct topkapi *tk = &state->tk[i][hash % TK_B];
            if (!tk->rec) {
                tk->rec = xmemdup(rec, sizeof *rec);
                tk->count = 1;
            } else if (!log_record_compare(rec, tk->rec, spec)) {
                tk->count++;
            } else if (--tk->count < 0) {
                *tk->rec = *rec;
                tk->count = 1;
            }
        }
    }
}

static void
state_uninit(struct state *state OVS_UNUSED)
{
}

static bool
job_is_canceled(struct job *job)
{
    bool cancel;
    atomic_read(&job->cancel, &cancel);
    return cancel;
}

static void
job_cancel(struct job *job)
{
    atomic_store(&job->cancel, true);
}

static bool
include_record(const struct log_record *rec, const struct spec *spec,
               struct state *state)
{
    if (rec->when < spec->date_since || rec->when > spec->date_until) {
        return false;
    }
    if (!(spec->priorities & (1u << rec->priority))) {
        return false;
    }
    if (!(spec->facilities & (1u << rec->facility))) {
        return false;
    }
    if (!sset_is_empty(&spec->sdids)
        && !sset_contains_len(&spec->sdids, rec->sdid.s, rec->sdid.length)) {
        return false;
    }
    if (!sset_is_empty(&spec->components)
        && !sset_contains_len(&spec->components, rec->comp.s,
                              rec->comp.length)) {
        return false;
    }
    if (!sset_is_empty(&spec->subcomponents)
        && !sset_contains_len(&spec->subcomponents,
                              rec->subcomp.s, rec->subcomp.length)) {
        return false;
    }
    if (!sset_is_empty(&spec->error_codes)
        && !sset_contains_len(&spec->error_codes,
                              rec->error_code.s, rec->error_code.length)) {
        return false;
    }
    if (spec->match && ss_find_case(rec->msg, ss_cstr(spec->match)) == -1) {
        return false;
    }
    if (spec->start && log_record_compare(rec, spec->start, spec) < 0) {
        state->skipped++;
        return false;
    }
    return true;
}

#define WITH_MUTEX(MUTEX, ...)                  \
    ovs_mutex_lock(MUTEX);                      \
    __VA_ARGS__;                                \
    ovs_mutex_unlock(MUTEX);

static bool
parse_priority(struct parse_ctx *ctx, struct substring priority,
               struct log_record *rec)
{
    if (ss_equals(priority, ss_cstr("FATAL"))) {
        rec->priority = PRI_EMERG;
    } else if (ss_equals(priority, ss_cstr("SEVERE"))) {
        rec->priority = PRI_ALERT;
    } else if (ss_equals(priority, ss_cstr("CRIT"))) {
        rec->priority = PRI_CRIT;
    } else if (ss_equals(priority, ss_cstr("ERROR"))) {
        rec->priority = PRI_ERR;
    } else if (ss_equals(priority, ss_cstr("WARN"))) {
        rec->priority = PRI_WARNING;
    } else if (ss_equals(priority, ss_cstr("NOTICE"))) {
        rec->priority = PRI_NOTICE;
    } else if (ss_equals(priority, ss_cstr("INFO"))) {
        rec->priority = PRI_INFO;
    } else if (ss_equals(priority, ss_cstr("DEBUG"))) {
        rec->priority = PRI_DEBUG;
    } else {
        warn(ctx, "%.*s: unknown severity",
             (int) priority.length, priority.s);
        return false;
    }

    return true;
}

static int
escape_length(struct substring s)
{
    if (s.length < 2 || s.s[0] != 27 || s.s[1] != '[') {
        return 0;
    }
    size_t n = 2;
    for (; n < s.length; n++) {
        if (!c_isdigit(s.s[n]) && s.s[n] != ';') {
            return n + 1;
        }
    }
    return n;
}

/* Returns 's' stripped of ANSI escape sequences at its beginning and end. */
static struct substring
strip_escapes(struct substring s)
{
    /* Remove escapes from the beginning. */
    for (;;) {
        size_t n = escape_length(s);
        if (!n) {
            break;
        }
        s.s += n;
        s.length -= n;
    }

    /* Remove escapes from the end. */
    for (;;) {
        int ofs = ss_rfind(s, ss_cstr("\033["));
        if (ofs < 0) {
            break;
        }
        struct substring rest = { s.s + ofs, s.length - ofs };
        if (escape_length(rest) != rest.length) {
            break;
        }
        s.length -= rest.length;
    }

    return s;
}

static bool
get_pipe_token(struct parse_ctx *ctx, struct substring *token)
{
    match_spaces(ctx);
    if (!must_match(ctx, '|')
        || !must_match_spaces(ctx)
        || !get_header_token__(ctx, '|', token)) {
        return false;
    }

    *token = ss_rstrip(*token);
    return true;
}

static bool
is_digits(struct substring token)
{
    for (size_t i = 0; i < token.length; i++) {
        if (!c_isdigit(token.s[i])) {
            return false;
        }
    }
    return token.length > 0;
}

/* Parses many log formats that start with an RFC 3339 timestamp, most notably
 * ones like this:
 *
 * 2018-12-11T18:18:05.359Z  INFO http-nio-127.0.0.1-6440-exec-2 AuditingServiceImpl - - [nsx@6876 audit="true" comp="nsx-manager" reqId="63865aba-97de-4598-9099-be490c73be1f" subcomp="policy"] UserName="admin", ModuleName="AAA", Operation="GetUserFeaturePermissions", Operation status="success"
 */
static bool
parse_date_first_record(struct parse_ctx *ctx, struct log_record *rec)
{
    rec->count = 1;

    rec->src_host = ss_cstr(ctx->host);
    rec->src_file = ss_cstr(ctx->fn);

    if (!get_header_token__(ctx, ' ', &rec->timestamp)) {
        return false;
    }
    rec->when = parse_timestamp(rec->timestamp.s, rec->timestamp.length);
    if (rec->when == -1) {
        return false;
    }

    if (ctx->p[0] == ' ' && ctx->p[1] == '|') {
        /* Parses log lines in the following format:
         *
         * 2018-12-19T08:15:23.697Z | ESC[39mDEBUGESC[0;39m |          SharedServerThread-20 |            o.c.i.LogUnitServer | log write: global: 211639, streams: {6d260a60-08ea-3faf-a0b3-43f4fa45ae2a=211541, a2d97d18-0afc-3d77-bf7a-a06b568ed24a=211510}, backpointers: {}
         */
        struct substring priority;
        if (!get_pipe_token(ctx, &priority)
            || !parse_priority(ctx, ss_rstrip(strip_escapes(priority)), rec)) {
            return false;
        }

        if (!get_pipe_token(ctx, &rec->procid)         /* Thread name. */
            || !get_pipe_token(ctx, &rec->app_name)) { /* Class name. */
            return false;
        }
        match_spaces(ctx);
        if (!must_match(ctx, '|')) {
            return false;
        }
    } else {
        rec->facility = LOG_LOCAL6;

        struct substring token;
        if (!get_header_token(ctx, &token)) {
            return false;
        }

        if (is_digits(token)) {
            /* Parses log lines in the following format:
             *
             * 2018-12-19T04:47:59.730Z 7344 cli INFO NSX CLI started (Manager, Policy, Controller) for user: admin
             */
            rec->procid = token;

            struct substring priority;
            if (!get_header_token(ctx, &rec->comp)
                || !get_header_token(ctx, &priority)
                || !parse_priority(ctx, priority, rec)) {
                return false;
            }
        } else {
            if (!parse_priority(ctx, token, rec)) {
                return false;
            }

            /* Distinguish a couple of formats based on the third token, which
             * might be:
             *
             *   - A thread name.  Hard to pick these out.
             *
             *     2018-12-11T18:18:05.359Z  INFO http-nio-127.0.0.1-6440-exec-2 AuditingServiceImpl - - [nsx@6876 audit="true" comp="nsx-manager" reqId="63865aba-97de-4598-9099-be490c73be1f" subcomp="policy"] UserName="admin", ModuleName="AAA", Operation="GetUserFeaturePermissions", Operation status="success"
             *
             *   - The full name of a Java class.  In the cases I've noticed, these
             *     start with "com.", so let's just use that as a heuristic.
             *
             *     Example:
             *     2018-12-08T14:12:54.534Z INFO org.apache.coyote.http11.Http11Processor service Error parsing HTTP request header
             */
            if (!get_header_token(ctx, &token)) {
                return false;
            }
            if (ss_starts_with(token, ss_cstr("com."))) {
                rec->app_name = token;  /* Class name. */
                if (!get_header_token(ctx, &rec->comp)) {
                    return false;
                }
            } else {
                rec->procid = token;                      /* Thread name. */
                if (!get_header_token(ctx, &rec->app_name) /* Class name. */
                    || !get_header_token(ctx, &rec->procid)
                    || !get_header_token(ctx, &rec->msgid)
                    || !must_match_spaces(ctx)) {
                    return false;
                }

                if (*ctx->p == '[') {
                    if (!parse_structured_data(ctx, rec)) {
                        return false;
                    }
                }
            }
        }
    }

    match_spaces(ctx);

    rec->msg.s = CONST_CAST(char *, ctx->p);
    rec->msg.length = ctx->line_end - ctx->p;

    rec->valid = true;

    return true;
}

static void
parse_file(const char *fn, const char *buffer, off_t size, struct task *task)
{
    if (!classify_log_record(buffer, size)) {
        VLOG_DBG("%s: unknown log file format", fn);
        return;
    }

    struct job *job = task->job;
    const struct spec *spec = &job->spec;
    const char *end = buffer + size;

    WITH_MUTEX(&job->stats_lock, job->total_bytes += size);

    struct state *state = &task->state;
    state_init(state, spec);

    struct parse_ctx ctx = { .host = job->spec.host,
                             .fn = fn,
                             .ln = 1,
                             .line_start = buffer };
    for (; ctx.line_start < end; ctx.line_start = ctx.line_end + 1, ctx.ln++) {
        ctx.line_end = memchr(ctx.line_start, '\n', end - ctx.line_start);
        if (!ctx.line_end) {
            /* Don't bother with lines that lack a new-line. */
            break;
        }
        while (ctx.line_end + 1 < end) {
            const char *next = ctx.line_end + 1;
            if (starts_with_date(ss_buffer(next, end - next))) {
                break;
            }

            ctx.line_end = memchr(next, '\n', end - next);
            if (!ctx.line_end) {
                break;
            }
        }

        ctx.p = ctx.line_start;

        ovs_mutex_lock(&job->stats_lock);
        unsigned long long int total_recs = job->total_recs++;
        ovs_mutex_unlock(&job->stats_lock);
        if (!(total_recs % 1024)) {
            fatal_signal_run();
            if (job_is_canceled(job)) {
                break;
            }
        }

        struct log_record rec;
        memset(&rec, 0, sizeof rec);
        rec.line.s = CONST_CAST(char *, ctx.line_start);
        rec.line.length = ctx.line_end - ctx.line_start;

        if (classify_log_record(rec.line.s, rec.line.length)
            == REC_TYPE_RFC5424) {
            parse_rfc5424_record(&ctx, &rec);
        } else {
            parse_date_first_record(&ctx, &rec);
        }

        if (include_record(&rec, spec, state)) {
            state_add(state, &rec, spec);
        }
    }

    if (spec->show != SHOW_TOP) {
        for (size_t i = 0; i < state->n; i++) {
            log_record_copy(&state->reservoir[i], &state->reservoir[i]);
        }
    } else {
        for (int i = 0; i < TK_L; i++) {
            for (int j = 0; j < TK_B; j++) {
                struct log_record *rec = state->tk[i][j].rec;
                if (rec) {
                    log_record_copy(rec, rec);
                }
            }
        }
    }
    state_uninit(state);
}

static void
read_gzipped(const char *name,
             const char *in, size_t in_size, struct task *task)
{
    z_stream z = {
        .next_in = (unsigned char *) in,
        .avail_in = in_size,
    };
    int retval = inflateInit2(&z, 15 + 16);
    if (retval != Z_OK) {
        VLOG_WARN("%s: failed to initiate decompression (%s)", name, z.msg);
        return;
    }

    char sample[128];
    z.next_out = (unsigned char *) sample;
    z.avail_out = sizeof sample;
    retval = inflate(&z, Z_SYNC_FLUSH);
    if (retval != Z_OK && retval != Z_STREAM_END) {
        VLOG_WARN("%s: decompression failed (%s)", name, z.msg);
        inflateEnd(&z);
        return;
    }

    if ((sample[0] != '<' || !c_isdigit(sample[1]))
        && !starts_with_date(ss_buffer(sample, sizeof sample))) {
        VLOG_DBG("%s: not a (gzipped) log file", name);
        inflateEnd(&z);
        return;
    }

    size_t allocated = in_size * 16;
    char *out = xmalloc(allocated);
    memcpy(out, sample, z.total_out);
    for (;;) {
        if (z.total_out >= allocated) {
            allocated = allocated * 5 / 4;
            out = xrealloc(out, allocated);
            ovs_assert(z.total_out < allocated);
        }
        z.next_out = (unsigned char *) &out[z.total_out];
        z.avail_out = allocated - z.total_out;

        fatal_signal_run();
        if (job_is_canceled(task->job)) {
            inflateEnd(&z);
            free(out);
            return;
        }
        retval = inflate(&z, Z_SYNC_FLUSH);
        if (retval == Z_STREAM_END) {
            break;
        } else if (retval != Z_OK) {
            VLOG_WARN("%s: decompression failed (%s)", name, z.msg);
            inflateEnd(&z);
            free(out);
            return;
        }
    }
    parse_file(name, out, z.total_out, task);
    free(out);

    WITH_MUTEX(&task->job->stats_lock,
               task->job->total_decompressed += z.total_out);
    inflateEnd(&z);
}

static void
task_execute(struct task *task)
{
    const char *fn = task->filename;

    int fd = open(fn, O_RDONLY);
    if (fd < 0) {
        ovs_fatal(errno, "%s: open failed", fn);
    }

    struct stat s;
    if (fstat(fd, &s) < 0) {
        ovs_fatal(errno, "%s; stat failed", fn);
    }

    random_set_seed(s.st_size);

    off_t size = s.st_size;
    char *buffer = mmap (NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (buffer == MAP_FAILED) {
        ovs_fatal(errno, "%s: mmap failed", fn);
    }
    close(fd);

    if (size > 2 && !memcmp(buffer, "\x1f\x8b", 2)) {
        read_gzipped(fn, buffer, size, task);
    } else {
        if (madvise(buffer, size, MADV_WILLNEED) < 0) {
            ovs_fatal(errno, "%s: madvise failed", fn);
        }
        parse_file(fn, buffer, size, task);
    }

    if (munmap(buffer, size) < 0) {
        ovs_error(errno, "%s: munmap failed", fn);
    }
}

static void
open_remote_target(const char *name, struct job *job)
{
    char *s = xstrdup(name);
    char *save_ptr = NULL;
    char *host = strtok_r(s, ":", &save_ptr);
    char *dir = strtok_r(NULL, "", &save_ptr);
    if (!host || !dir) {
        ovs_error(0, "%s: bad remote target format", name);
        free(s);
        return;
    }

    /* XXX should this be a new "ssh:" stream type? */
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        ovs_error(errno, "socketpair failed");
        free(s);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        ovs_error(errno, "fork failed");
        close(fds[0]);
        close(fds[1]);
        free(s);
        return;
    } else if (pid) {
        /* Parent process. */
        close(fds[1]);

        struct stream *stream;
        new_fd_stream(xasprintf("ssh %s", name), fds[0], 0, AF_UNIX, &stream);
        struct jsonrpc *rpc = jsonrpc_open(stream);

        struct spec spec;
        spec_copy(&spec, &job->spec);
        free(spec.host);
        spec.host = xstrdup(host);

        struct json *id;
        jsonrpc_send(rpc, jsonrpc_create_request(
                         "analyze",
                         json_array_create_1(spec_to_json(&spec)), &id));
        free(s);

        spec_uninit(&spec);

        struct task *task = xzalloc(sizeof *task);
        task->job = job;
        task->filename = xstrdup(name);
        task->rpc = rpc;
        task->request_id = id;
        rculist_push_back(&job->remote_tasks, &task->list_node);
    } else {
        /* Child process. */
        close(fds[0]);
        dup2(fds[1], STDIN_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);

        int max_fds = get_max_fds();
        for (int i = 3; i < max_fds; i++) {
            close(i);
        }

        execlp("ssh", "ssh", "--", host, "bin/hv", "--log-file=log", "-vjsonrpc", "--remote", dir, (void *) NULL);
        exit(1);
    }
}

static void
open_target(const char *name, struct job *job)
{
    if (strchr(name, ':')) {
        open_remote_target(name, job);
        return;
    }

    struct stat s;
    if (stat(name, &s) < 0) {
        ovs_error(errno, "%s; stat failed", name);
        return;
    }

    if (S_ISREG(s.st_mode)) {
        if (s.st_size > 0 && !strstr(name, "metrics")) {
            struct task *task = xzalloc(sizeof *task);
            task->job = job;
            task->filename = xstrdup(name);
            task->size = s.st_size;
            rculist_push_back(&job->queued_tasks, &task->list_node);
        }
        return;
    } else if (!S_ISDIR(s.st_mode)) {
        VLOG_DBG("%s: ignoring special file", name);
        return;
    }

    DIR *dir = opendir(name);
    if (!dir) {
        ovs_error(errno, "%s: open failed", name);
        return;
    }

    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) {
            if (errno) {
                ovs_error(errno, "%s: readdir failed", name);
            }
            break;
        }

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }

        char *name2 = xasprintf("%s/%s", name, de->d_name);
        open_target(name2, job);
        free(name2);
    }
    closedir(dir);
}

static int
count_cores(void)
{
    cpu_set_t cpus;
    if (sched_getaffinity(0, sizeof cpus, &cpus) < 0) {
        ovs_error(errno, "sched_getaffinity failed");
        return 1;
    } else {
        return CPU_COUNT(&cpus);
    }
}

static const char *
priority_to_string(enum priority priority)
{
    const char *priorities[] = {
        [PRI_EMERG] = "emer",
        [PRI_ALERT] = "alert",
        [PRI_CRIT] = "crit",
        [PRI_ERR] = "err",
        [PRI_WARNING] = "warn",
        [PRI_NOTICE] = "notice",
        [PRI_INFO] = "info",
        [PRI_DEBUG] = "debug",
    };

    return (priority >= 0 && priority < ARRAY_SIZE(priorities)
            ? priorities[priority]
            : "-");
}

static bool
priority_from_string(const char *s, enum priority *priority)
{
    const char *levels[] = {
        [0] = "emergency",
        [1] = "alert",
        [2] = "critical",
        [3] = "error",
        [4] = "warning",
        [5] = "notice",
        [6] = "informational",
        [7] = "debug"
    };

    int s_len = strcspn(s, "-+");
    for (size_t i = 0; i < ARRAY_SIZE(levels); i++) {
        if (!strncmp(s, levels[i], s_len)) {
            *priority = i;
            return true;
        }
    }
    return false;
}

static char * OVS_WARN_UNUSED_RESULT
priorities_from_string(const char *s_, unsigned int *priorities)
{
    char *s = xstrdup(s_);
    *priorities = 0;

    char *save_ptr = NULL;
    for (char *token = strtok_r(s, ", ", &save_ptr); token;
         token = strtok_r(NULL, ", ", &save_ptr)) {
        enum priority p;
        if (!priority_from_string(s, &p)) {
            char *error = xasprintf("%s: unknown priority", token);
            free(s);
            return error;
        }

        if (strchr(s, '+')) {
            *priorities |= (1u << (p + 1)) - 1;
        } else if (strchr(s, '-')) {
            *priorities |= ((1u << p) - 1) ^ 0xff;
        } else {
            *priorities |= 1u << p;
        }
    }
    free(s);
    return NULL;
}

static const char *
facility_to_string(int facility)
{
    const char *facility_strings[] = {
        [0] = "kernel",
        [1] = "user",
        [2] = "mail",
        [3] = "system",
        [4] = "auth",
        [5] = "log",
        [6] = "lpd",
        [7] = "news",
        [8] = "uucp",
        [9] = "clock",
        [10] = "auth",
        [11] = "ftp",
        [12] = "ntp",
        [13] = "log_audit",
        [14] = "log_alert",
        [15] = "clock",
        [16] = "local0",
        [17] = "local1",
        [18] = "local2",
        [19] = "local3",
        [20] = "local4",
        [21] = "local5",
        [22] = "local6",
        [23] = "local7",
    };

    return (facility >= 0 && facility < ARRAY_SIZE(facility_strings)
            ? facility_strings[facility]
            : "-");
}

static void
put_substring(struct ds *dst, const struct substring src)
{
    if (src.length) {
        ds_put_buffer(dst, src.s, src.length);
    } else {
        ds_put_char(dst, '-');
    }
}

static void
log_record_format__(const struct log_record *r, enum column columns,
                    struct ds *s)
{
    for (; columns; columns = zero_rightmost_1bit(columns)) {
        ds_put_char(s, ' ');
        switch (rightmost_1bit(columns)) {
        case COL_SRC_HOST:
            put_substring(s, r->src_host);
            break;
        case COL_SRC_FILE:
            put_substring(s, r->src_file);
            break;
        case COL_WHEN:
            format_timestamp(r->when, s);
            break;
        case COL_FACILITY:
            ds_put_cstr(s, facility_to_string(r->facility));
            break;
        case COL_PRIORITY:
            ds_put_cstr(s, priority_to_string(r->priority));
            break;
        case COL_HOSTNAME:
            put_substring(s, r->hostname);
            break;
        case COL_APP_NAME:
            put_substring(s, r->app_name);
            break;
        case COL_PROCID:
            put_substring(s, r->procid);
            break;
        case COL_MSGID:
            put_substring(s, r->msgid);
            break;
        case COL_SDID:
            put_substring(s, r->sdid);
            break;
        case COL_COMP:
            put_substring(s, r->comp);
            break;
        case COL_SUBCOMP:
            put_substring(s, r->subcomp);
            break;
        case COL_ERROR_CODE:
            put_substring(s, r->error_code);
            break;
        case COL_MSG:
            put_substring(s, r->msg);
            break;
        case COL_LINE:
            put_substring(s, r->line);
            break;
        case COL_VALID:
            ds_put_cstr(s, r->valid ? "ok" : "invalid");
            break;
        default:
            OVS_NOT_REACHED();
        }
    }
}

static void
log_record_format(const struct log_record *r, int i, int n,
                  const struct spec *spec, struct ds *s)
{
    ds_put_format(s, "%7lld", r->count);

    if (spec->show == SHOW_SAMPLE && n) {
        ds_put_format(s, " %5.2f%% ", 100.0 * i / n);
    }

    log_record_format__(r, spec->columns, s);
}

static int
compare_tk_by_count_desc(const void *a_, const void *b_)
{
    const struct topkapi *a = a_;
    const struct topkapi *b = b_;
    return a->count > b->count ? -1 : a->count < b->count;
}

static void
add_record(struct log_record *record,
           struct log_record ***resultsp, size_t *n_resultsp,
           size_t *allocated_resultsp)
{
    if (*n_resultsp >= *allocated_resultsp) {
        *resultsp = x2nrealloc(*resultsp, allocated_resultsp,
                               sizeof **resultsp);
    }
    (*resultsp)[(*n_resultsp)++] = record;
}

static void
state_merge(const struct state *src, const struct spec *spec,
            struct state *dst)
{
    switch (spec->show) {
    case SHOW_FIRST:
    case SHOW_LAST:
    case SHOW_SAMPLE:
        for (size_t i = 0; i < src->n; i++) {
            state_add(dst, &src->reservoir[i], spec);
        }
        dst->population += src->population;
        dst->skipped += src->skipped;
        break;

    case SHOW_TOP:
        //fprintf(stderr, "merge %p\n", src->tk[0]);
        if (!src->tk[0]) {
            break;
        }
        for (int i = 0; i < TK_L; i++) {
            for (int j = 0; j < TK_B; j++) {
                struct topkapi *d = &dst->tk[i][j];
                struct topkapi *s = &src->tk[i][j];

                if (!s->rec) {
                    /* Nothing to do. */
                } else if (!d->rec) {
                    *d = *s;
                } else if (!log_record_compare(d->rec, s->rec, spec)) {
                    d->count += s->count;
                } else if (d->count >= s->count) {
                    d->count -= s->count;
                } else {
                    d->rec = s->rec;
                    d->count = s->count - d->count;
                }
            }
        }
        break;
    }
}

static int
compare_log_records_for_sort(const void *a_, const void *b_,
                             const void *spec_)
{
    const struct spec *spec = spec_;
    const struct log_record *const *ap = a_;
    const struct log_record *const *bp = b_;
    const struct log_record *a = *ap;
    const struct log_record *b = *bp;
    return log_record_compare(a, b, spec);
}

static struct results *
state_to_results(const struct state *state, const struct spec *spec)
{
    struct log_record **results = NULL;
    size_t n_results = 0;
    size_t allocated_results = 0;
    unsigned long long int total = 0;
    unsigned long long int skipped = 0;

    if (spec->show != SHOW_TOP) {
        results = xmalloc(state->n * sizeof *results);
        n_results = state->n;
        for (size_t i = 0; i < n_results; i++) {
            results[i] = &state->reservoir[i];
        }

        qsort_aux(results, n_results, sizeof *results,
                  compare_log_records_for_sort, spec);
        if (n_results) {
            if (spec->at >= 0 && spec->at <= 100) {
                size_t pos = MIN(spec->at / 100.0 * state->n, state->n - 1);
                results[0] = results[pos];
                n_results = 1;
            } else if (spec->show == SHOW_FIRST) {
                /* Nothing to do. */
                skipped = state->skipped;
                total = state->population;
            } else {
                for (size_t i = 0; i < n_results / 2; i++) {
                    struct log_record **a = &results[i];
                    struct log_record **b = &results[n_results - i - 1];
                    struct log_record *tmp = *a;
                    *a = *b;
                    *b = tmp;
                }
                skipped = total - (skipped + state->n);
                total = state->population;
            }
        }
    } else {
        struct topkapi *tk = xcalloc(TK_B, sizeof *tk);

        int K = 100;
        int frac_epsilon = 10 * K;
        int threshold = ((double) TK_B / K) - ((double) TK_B / frac_epsilon);
        for (int j = 0; j < TK_B; j++) {
            struct log_record *rec = state->tk[0][j].rec;
            if (!rec) {
                tk[j].count = 0;
                continue;
            }

            long long int count = state->tk[0][j].count;
            for (int i = 1; i < TK_L; i++) {
                int idx = (log_record_hash(state->tk[0][j].rec, i,
                                           spec->columns) % TK_B);
                if (!state->tk[i][idx].rec
                    || log_record_compare(state->tk[0][j].rec,
                                          state->tk[i][idx].rec, spec)) {
                    continue;
                }
                count = MAX(count, state->tk[i][idx].count);
            }
            tk[j].rec = rec;
            tk[j].count = count;
        }

        qsort(tk, TK_B, sizeof *tk, compare_tk_by_count_desc);
        for (int j = 0; j < TK_B; j++) {
            if (tk[j].count >= threshold) {
                tk[j].rec->count = tk[j].count;
                add_record(tk[j].rec,
                           &results, &n_results, &allocated_results);
            } else {
                break;
            }
        }
        total = n_results;
    }

    struct results *r = xmalloc(sizeof *r);
    r->recs = results;
    r->n = n_results;
    r->after = total - skipped - n_results;
    r->before = skipped;
    return r;
}

static struct state *
merge_state(struct job *job)
{
    struct state *state = xmalloc(sizeof *state);
    state_init(state, &job->spec);

    struct task *task;
    RCULIST_FOR_EACH (task, list_node, &job->completed_tasks) {
        state_merge(&task->state, &job->spec, state);
    }
    return state;
}

static int
compare_tasks(const void *a_, const void *b_)
{
    const struct task *const *ap = a_;
    const struct task *const *bp = b_;
    const struct task *a = *ap;
    const struct task *b = *bp;
    return a->size < b->size ? -1 : a->size > b->size;
}

static void *
task_thread(void *job_)
{
    struct job *job = job_;
    for (;;) {
        ovs_mutex_lock(&job->task_lock);
        struct task *task = NULL;
        if (!rculist_is_empty(&job->queued_tasks)) {
            task = CONTAINER_OF(rculist_pop_back(&job->queued_tasks),
                                struct task, list_node);
        }
        ovs_mutex_unlock(&job->task_lock);

        if (!task) {
            return NULL;
        }

        task_execute(task);
        fatal_signal_run();

        WITH_MUTEX(&job->task_lock,
                   rculist_push_back(&job->completed_tasks, &task->list_node));

        WITH_MUTEX(&job->stats_lock, job->progress++);
        seq_change(job->seq);
    }
}

static struct ovsdb_error *
state_from_json(const struct json *json, struct state *state)
{
    struct ovsdb_parser p;
    ovsdb_parser_init(&p, json, "state");

    const struct json *population = ovsdb_parser_member(
        &p, "population", OP_INTEGER);
    if (population) {
        state->population = json_integer(population);
    }

    const struct json *records = ovsdb_parser_member(
        &p, "reservoir", OP_ARRAY | OP_OPTIONAL);
    if (records) {
        size_t n = json_array(records)->n;
        state->reservoir = xmalloc(n * sizeof *state->reservoir);
        state->allocated = n;
        for (size_t i = 0; i < n; i++) {
            struct log_record *rec;
            struct ovsdb_error *error = log_record_from_json(
                json_array(records)->elems[i], &rec);
            if (error) {
                ovsdb_parser_put_error(&p, error);
                break;
            }
            state->reservoir[i] = *rec;
            free(rec);
            state->n++;
        }
    }

    const struct json *skipped = ovsdb_parser_member(
        &p, "skipped", OP_INTEGER | OP_OPTIONAL);
    if (skipped) {
        state->skipped = json_integer(skipped);
    }

    const struct json *tk_json = ovsdb_parser_member(
        &p, "tk", OP_ARRAY | OP_OPTIONAL);
    if (tk_json) {
        const struct json_array *tk = json_array(tk_json);
        if (tk->n != TK_L) {
            ovsdb_parser_raise_error(&p, "tk has wrong number of elements");
        } else {
            for (int i = 0; i < TK_L; i++) {
                state->tk[i] = xcalloc(TK_B, sizeof *state->tk[i]);
            }

            int n_parsed = 0;
            for (int i = 0; i < TK_L; i++) {
                const struct json *tk_i_json = tk->elems[i];
                if (tk_i_json->type != JSON_ARRAY ||
                    json_array(tk_i_json)->n != TK_B) {
                    ovsdb_parser_raise_error(&p, "tk[%d] has %"PRIuSIZE" "
                                             "elements (expected %d)", i,
                                             json_array(tk_i_json)->n, TK_B);
                } else {
                    for (int j = 0; j < TK_B; j++) {
                        const struct json *tk_ij_json
                            = json_array(tk_i_json)->elems[j];
                        if (tk_ij_json->type == JSON_OBJECT) {
                            n_parsed++;

                            struct ovsdb_parser p2;
                            ovsdb_parser_init(&p2, tk_ij_json, "tk");
                            const struct json *count = ovsdb_parser_member(
                                &p2, "count", OP_INTEGER | OP_OPTIONAL);
                            if (count) {
                                state->tk[i][j].count = json_integer(count);
                            }

                            const struct json *record = ovsdb_parser_member(
                                &p2, "record", OP_OBJECT | OP_OPTIONAL);
                            if (record) {
                                ovsdb_parser_put_error(
                                    &p2, log_record_from_json(
                                        record, &state->tk[i][j].rec));
                            }
                            ovsdb_parser_put_error(&p,
                                                   ovsdb_parser_finish(&p2));
                        }
                    }
                }
            }
            fprintf(stderr, "got %d\n", n_parsed);
        }
    }

    struct ovsdb_error *error = ovsdb_parser_finish(&p);
    if (error) {
#if 0                           /* XXX */
        state_destroy(state);
#endif
        return error;
    }
    return NULL;
}

static void
remote_task_handle_reply(struct task *task, struct jsonrpc_msg *reply)
{
    struct ovsdb_error *error = state_from_json(reply->result, &task->state);
    if (error) {
        VLOG_ERR("%s", ovsdb_error_to_string_free(error)); /* XXX */
        return;
    }

    rculist_remove(&task->list_node);

    struct job *job = task->job;
    WITH_MUTEX(&job->task_lock,
               rculist_push_back(&job->completed_tasks, &task->list_node));

    WITH_MUTEX(&job->stats_lock, job->progress++);
}

static void
remote_task_run(struct task *task)
{
    jsonrpc_run(task->rpc);

    struct jsonrpc_msg *msg;
    int error = jsonrpc_recv(task->rpc, &msg);
    if (!error) {
        if (msg->type == JSONRPC_REPLY
            && json_equal(msg->id, task->request_id)) {
            remote_task_handle_reply(task, msg);
        } else {
            VLOG_ERR("%s: received unexpected %s message",
                     jsonrpc_get_name(task->rpc),
                     jsonrpc_msg_type_to_string(msg->type));
            jsonrpc_msg_destroy(msg);
            return;
        }
        jsonrpc_msg_destroy(msg);
    } else if (error != EAGAIN) {
        ovs_fatal(error, "error receiving JSON-RPC message"); /* XXX */
    }
}

static void
remote_task_wait(struct task *task)
{
    jsonrpc_recv_wait(task->rpc);
    jsonrpc_wait(task->rpc);
}

static void *
job_thread(void *job_)
{
    struct job *job = job_;
    for (size_t i = 0; i < job->spec.targets.n; i++) {
        open_target(job->spec.targets.names[i], job);
    }

    /* Sort tasks by size.
     *
     * We can only do this to an rculist because we know that it's not been
     * accessed concurrently yet. */
    struct task **tasks = xmalloc(rculist_size(&job->queued_tasks)
                                  * sizeof *tasks);
    size_t n_tasks = 0;
    struct task *task;
    RCULIST_FOR_EACH_PROTECTED (task, list_node, &job->queued_tasks) {
        tasks[n_tasks++] = task;
    }
    qsort(tasks, n_tasks, sizeof *tasks, compare_tasks);
    rculist_init(&job->queued_tasks);
    for (size_t i = 0; i < n_tasks; i++) {
        rculist_push_back(&job->queued_tasks, &tasks[i]->list_node);
    }
    free(tasks);

    size_t n_remote_tasks = rculist_size(&job->remote_tasks);
    unsigned int goal = n_tasks + n_remote_tasks;

    WITH_MUTEX(&job->stats_lock, job->goal = goal);

    int cores = count_cores();
    int n_threads = MIN(4 * cores, n_tasks);
    pthread_t *threads = xmalloc(n_threads * sizeof *threads);
    for (int i = 0; i < n_threads; i++) {
        threads[i] = ovs_thread_create("read", task_thread, job);
    }
    unsigned int progress = 0;
    for (;;) {
        struct task *next;
        RCULIST_FOR_EACH_SAFE_PROTECTED (task, next,
                                         list_node, &job->remote_tasks) {
    VLOG_WARN("%s:%d", __FILE__, __LINE__);
            remote_task_run(task);
        }

        uint64_t seq = seq_read(job->seq);
        WITH_MUTEX(&job->stats_lock, unsigned int p = job->progress);
        if (p > progress) {
            progress = p;
            ovsrcu_set(&job->state, merge_state(job));
        }
        if (progress >= goal) {
            break;
        }
        RCULIST_FOR_EACH_PROTECTED (task, list_node, &job->remote_tasks) {
    VLOG_WARN("%s:%d", __FILE__, __LINE__);
            remote_task_wait(task);
        }
        seq_wait(job->seq, seq);
        poll_block();
    }
    VLOG_WARN("%s:%d", __FILE__, __LINE__);
    for (int i = 0; i < n_threads; i++) {
        xpthread_join(threads[i], NULL);
    }
    free(threads);

    WITH_MUTEX(&job->stats_lock, job->goal = 0);

    atomic_store(&job->done, true);
    seq_change(job->seq);

    return NULL;
}

static void
spec_init(struct spec *spec)
{
    *spec = (struct spec) {
        .host = xstrdup("-"),
        .show = SHOW_SAMPLE,
        .priorities = ALL_PRIORITIES,
        .facilities = ALL_FACILITIES,
        .sdids = SSET_INITIALIZER(&spec->sdids),
        .components = SSET_INITIALIZER(&spec->components),
        .subcomponents = SSET_INITIALIZER(&spec->subcomponents),
        .error_codes = SSET_INITIALIZER(&spec->error_codes),
        .date_since = -DBL_MAX,
        .date_until = DBL_MAX,
        .at = -DBL_MAX,
        .columns = (COL_WHEN | COL_FACILITY | COL_PRIORITY
                    | COL_COMP | COL_SUBCOMP | COL_MSG),
    };
}

static void
spec_uninit(struct spec *spec)
{
    if (spec) {
        free(spec->host);
        free(spec->match);
        log_record_destroy(spec->start);
        sset_destroy(&spec->sdids);
        sset_destroy(&spec->components);
        sset_destroy(&spec->subcomponents);
        sset_destroy(&spec->error_codes);
        svec_destroy(&spec->targets);
    }
}

static void
spec_copy(struct spec *dst, const struct spec *src)
{
    *dst = *src;
    dst->host = xstrdup(src->host);
    dst->start = src->start ? log_record_clone(src->start) : NULL;
    dst->match = nullable_xstrdup(src->match);
    sset_clone(&dst->sdids, &src->sdids);
    sset_clone(&dst->components, &src->components);
    sset_clone(&dst->subcomponents, &src->subcomponents);
    sset_clone(&dst->error_codes, &src->error_codes);
    svec_clone(&dst->targets, &src->targets);
}

static bool
spec_equals(const struct spec *a, const struct spec *b)
{
    return (a->show == b->show
            && nullable_string_is_equal(a->match, b->match)
            && a->priorities == b->priorities
            && a->facilities == b->facilities
            && sset_equals(&a->sdids, &b->sdids)
            && sset_equals(&a->components, &b->components)
            && sset_equals(&a->subcomponents, &b->subcomponents)
            && sset_equals(&a->error_codes, &b->error_codes)
            && a->date_since == b->date_since
            && a->date_until == b->date_until
            && a->at == b->at
            && a->columns == b->columns
            && svec_equal(&a->targets, &b->targets)
            && (!a->start
                ? !b->start
                : b->start && !log_record_compare(a->start, b->start, a)));
}

static const char *
show_to_string(enum show show)
{
    switch (show) {
    case SHOW_FIRST:
        return "first";
    case SHOW_LAST:
        return "last";
    case SHOW_SAMPLE:
        return "sample";
    case SHOW_TOP:
        return "top";
    }
    OVS_NOT_REACHED();
}

static bool
show_from_string(const char *s, enum show *show)
{
    if (!strcmp(s, "first")) {
        *show = SHOW_FIRST;
    } else if (!strcmp(s, "last")) {
        *show = SHOW_LAST;
    } else if (!strcmp(s, "sample")) {
        *show = SHOW_SAMPLE;
    } else if (!strcmp(s, "top")) {
        *show = SHOW_TOP;
    } else {
        return false;
    }
    return true;
}

static struct json *
sset_to_json(const struct sset *sset)
{
    struct json *array;
    const char *s;

    array = json_array_create_empty();
    SSET_FOR_EACH (s, sset) {
        json_array_add(array, json_string_create(s));
    }
    return array;
}

static struct json *
spec_to_json(struct spec *spec)
{
    struct json *obj = json_object_create();
    json_object_put_string(obj, "show", show_to_string(spec->show));
    json_object_put_string(obj, "host", spec->host);
    if (spec->start) {
        json_object_put(obj, "start", log_record_to_json(spec->start,
                                                         spec->columns));
    }
    if (spec->match) {
        json_object_put_string(obj, "match", spec->match);
    }
    if (spec->priorities != ALL_PRIORITIES) {
        json_object_put(obj, "priorities",
                        json_integer_create(spec->priorities));
    }
    if (spec->facilities != ALL_FACILITIES) {
        json_object_put(obj, "facilities",
                        json_integer_create(spec->facilities));
    }
    if (!sset_is_empty(&spec->sdids)) {
        json_object_put(obj, "sdids", sset_to_json(&spec->sdids));
    }
    if (!sset_is_empty(&spec->components)) {
        json_object_put(obj, "components", sset_to_json(&spec->components));
    }
    if (!sset_is_empty(&spec->subcomponents)) {
        json_object_put(obj, "subcomponents",
                        sset_to_json(&spec->subcomponents));
    }
    if (!sset_is_empty(&spec->error_codes)) {
        json_object_put(obj, "error_codes",
                        sset_to_json(&spec->error_codes));
    }
    if (spec->date_since != -DBL_MAX) {
        json_object_put(obj, "date_since", json_real_create(spec->date_since));
    }
    if (spec->date_until != DBL_MAX) {
        json_object_put(obj, "date_until", json_real_create(spec->date_until));
    }
    if (spec->at != -DBL_MAX) {
        json_object_put(obj, "at", json_real_create(spec->at));
    }
    json_object_put(obj, "columns", columns_to_json(spec->columns));
    /* XXX targets not included */
    return obj;
}

static void
sset_from_json(const struct json *array, struct sset *sset)
{
    sset_clear(sset);

    ovs_assert(array->type == JSON_ARRAY);
    for (size_t i = 0; i < array->array.n; i++) {
        const struct json *elem = array->array.elems[i];
        if (elem->type == JSON_STRING) {
            sset_add(sset, json_string(elem));
        }
    }
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
spec_from_json(const struct json *json, struct spec **specp)
{
    struct spec *spec = xmalloc(sizeof *spec);
    spec_init(spec);

    struct ovsdb_parser p;
    ovsdb_parser_init(&p, json, "spec");

    const struct json *show = ovsdb_parser_member(&p, "show", OP_STRING);
    if (show && !show_from_string(json_string(show), &spec->show)) {
        ovsdb_parser_raise_error(&p, "%s: unknown 'show'", json_string(show));
    }

    const struct json *host = ovsdb_parser_member(&p, "host", OP_STRING);
    if (host) {
        spec->host = xstrdup(json_string(host));
    }

    const struct json *start = ovsdb_parser_member(
        &p, "start", OP_OBJECT | OP_OPTIONAL);
    if (start) {
        ovsdb_parser_put_error(&p, log_record_from_json(start, &spec->start));
    }

    const struct json *match = ovsdb_parser_member(
        &p, "match", OP_STRING | OP_OPTIONAL);
    if (match) {
        spec->match = xstrdup(json_string(match));
    }

    const struct json *priorities = ovsdb_parser_member(
        &p, "priorities", OP_INTEGER | OP_OPTIONAL);
    if (priorities) {
        spec->priorities = json_integer(priorities);
    }

    const struct json *facilities = ovsdb_parser_member(
        &p, "facilities", OP_INTEGER | OP_OPTIONAL);
    if (facilities) {
        spec->facilities = json_integer(facilities);
    }

    const struct json *sdids = ovsdb_parser_member(
        &p, "sdids", OP_ARRAY | OP_OPTIONAL);
    if (sdids) {
        sset_from_json(sdids, &spec->sdids);
    }

    const struct json *components = ovsdb_parser_member(
        &p, "components", OP_ARRAY | OP_OPTIONAL);
    if (components) {
        sset_from_json(components, &spec->components);
    }

    const struct json *subcomponents = ovsdb_parser_member(
        &p, "subcomponents", OP_ARRAY | OP_OPTIONAL);
    if (subcomponents) {
        sset_from_json(subcomponents, &spec->subcomponents);
    }

    const struct json *error_codes = ovsdb_parser_member(
        &p, "error_codes", OP_ARRAY | OP_OPTIONAL);
    if (error_codes) {
        sset_from_json(error_codes, &spec->error_codes);
    }

    const struct json *date_since = ovsdb_parser_member(
        &p, "date_since", OP_NUMBER | OP_OPTIONAL);
    if (date_since) {
        spec->date_since = json_real(date_since);
    }

    const struct json *date_until = ovsdb_parser_member(
        &p, "date_until", OP_NUMBER | OP_OPTIONAL);
    if (date_until) {
        spec->date_until = json_real(date_until);
    }

    const struct json *at = ovsdb_parser_member(
        &p, "at", OP_NUMBER | OP_OPTIONAL);
    if (at) {
        spec->at = json_real(at);
    }

    const struct json *columns = ovsdb_parser_member(&p, "columns", OP_ARRAY);
    if (columns) {
        ovsdb_parser_put_error(&p, columns_from_json(columns, &spec->columns));
    }

    struct ovsdb_error *error = ovsdb_parser_finish(&p);
    if (error) {
        spec_uninit(spec);
        free(spec);
        *specp = NULL;
        return error;
    }
    *specp = spec;
    return NULL;
}

static struct job *
job_create(const struct spec *spec)
{
    struct job *job = xzalloc(sizeof *job);

    spec_copy(&job->spec, spec);

    job->seq = seq_create();
    atomic_init(&job->cancel, false);
    atomic_init(&job->done, false);

    struct state *state = xmalloc(sizeof *state);
    state_init(state, spec);
    ovsrcu_init(&job->state, state);

    ovs_mutex_init(&job->stats_lock);

    ovs_mutex_init(&job->task_lock);
    rculist_init(&job->queued_tasks);
    rculist_init(&job->remote_tasks);
    rculist_init(&job->completed_tasks);

    ovs_thread_create("job", job_thread, job);

    return job;
}

/* CTRL('A') or CTRL('a') yields the keycode for Control+A. */
#define CTRL(KEY) ((KEY) & 0x1f)

/* META('a') yields the keycode for Meta+A. */
#define META(KEY) ((KEY) | 0x80)

static int
range(int x, int min, int max)
{
    return x < min ? min : x > max ? max : x;
}

static bool
is_word(unsigned char c)
{
    return isalnum(c);
}

static size_t
count_word_forward(const char *s)
{
    size_t n = 0;
    while (s[n] && !is_word(s[n])) {
        n++;
    }
    while (s[n] && is_word(s[n])) {
        n++;
    }
    return n;
}

static size_t
count_word_backward(const char *s, size_t pos)
{
    size_t n = 0;
    while (pos - n > 0 && !is_word(s[pos - n - 1])) {
        n++;
    }
    while (pos - n > 0 && is_word(s[pos - n - 1])) {
        n++;
    }
    return n;
}

static char *
readstr(const char *prompt, const char *initial, struct svec *history,
        char *(*validate)(const char *))
{
    int cur_hist = -1;
    struct ds s = DS_EMPTY_INITIALIZER;
    if (initial) {
        ds_put_cstr(&s, initial);
    }
    if (history && history->n) {
        svec_add(history, "");
        cur_hist = history->n - 1;
    }

    char *error = NULL;

    curs_set(1);
    int ofs = 0, pos = s.length;
    for (;;) {
        int ch = getch();
        bool ignore = false;
        switch (ch) {
        case KEY_UP:
            if (cur_hist > 0) {
                free(history->names[cur_hist]);
                history->names[cur_hist] = ds_steal_cstr(&s);

                cur_hist--;
                ds_put_cstr(&s, history->names[cur_hist]);
                pos = s.length;
            }
            break;
        case KEY_DOWN:
            if (history && cur_hist + 1 < history->n) {
                free(history->names[cur_hist]);
                history->names[cur_hist] = ds_steal_cstr(&s);

                cur_hist++;
                ds_put_cstr(&s, history->names[cur_hist]);
                pos = s.length;
            }
            break;
        case KEY_LEFT:
            pos--;
            break;
        case KEY_RIGHT:
            pos++;
            break;
        case CTRL('G'):
            if (history && history->n) {
                svec_pop_back(history);
            }
            curs_set(0);
            return NULL;
        case CTRL('L'):
            redrawwin(stdscr);
            break;
        case '\b': case KEY_BACKSPACE:
            if (pos > 0) {
                ds_remove(&s, pos - 1, 1);
                pos--;
            }
            break;
        case KEY_DC: case CTRL('D'):
            ds_remove(&s, pos, 1);
            break;
        case CTRL('U'):
            ds_remove(&s, 0, pos);
            pos = 0;
            break;
        case KEY_HOME: case CTRL('A'):
            pos = 0;
            break;
        case KEY_END: case CTRL('E'):
            pos = s.length;
            break;
        case META('f'):
            pos += count_word_forward(ds_cstr(&s) + pos);
            break;
        case META('b'):
            pos -= count_word_backward(ds_cstr(&s), pos);
            break;
        case META('d'):
            ds_remove(&s, pos, count_word_forward(ds_cstr(&s) + pos));
            break;
        case META(127):
            {
                size_t n = count_word_backward(ds_cstr(&s), pos);
                ds_remove(&s, pos - n, n);
                pos -= n;
            }
            break;
        case '\n': case '\r':
            if (validate) {
                free(error);
                error = validate(ds_cstr(&s));
                if (error) {
                    ignore = true;
                    break;
                }
            }
            if (history) {
                if (history->n > 1
                    && !strcmp(ds_cstr(&s),
                               history->names[history->n - 1])) {
                    svec_pop_back(history);
                } else if (cur_hist >= 0) {
                    free(history->names[cur_hist]);
                    history->names[cur_hist] = xstrdup(ds_cstr(&s));
                } else {
                    svec_add(history, ds_cstr(&s));
                }
            }
            curs_set(0);
            return ds_steal_cstr(&s);
        default:
            if (ch >= ' ' && ch <= '~') {
                *ds_insert_uninit(&s, pos++, 1) = ch;
            } else {
                ignore = true;
            }
            break;
        }
        if (!ignore) {
            free(error);
            error = NULL;
        }

        int y_max = getmaxy(stdscr);
        int x_max = MAX(getmaxx(stdscr), 10);

        int prompt_len = strlen(prompt);
        if (prompt_len > x_max - 6) {
            prompt_len = x_max - 6;
        }
        mvprintw(y_max - 1, 0, "%.*s: ", prompt_len, prompt);

        int avail = x_max - (prompt_len + 2);
        pos = range(pos, 0, s.length);
        ofs = range(ofs, 0, s.length);
        if (ofs > pos) {
            ofs = pos;
        }
        addnstr(ds_cstr(&s) + ofs, avail);
        if (error) {
            printw(" [%s]", error);
        }
        clrtoeol();
        move(y_max - 1, prompt_len + 2 + (pos - ofs));
        refresh();

        poll_fd_wait(STDIN_FILENO, POLLIN);
        poll_block();
    }
}

static struct svec columns_history = SVEC_EMPTY_INITIALIZER;
static struct svec sdids_history = SVEC_EMPTY_INITIALIZER;
static struct svec components_history = SVEC_EMPTY_INITIALIZER;
static struct svec subcomponents_history = SVEC_EMPTY_INITIALIZER;
static struct svec error_codes_history = SVEC_EMPTY_INITIALIZER;
static struct svec priorities_history = SVEC_EMPTY_INITIALIZER;
static struct svec facilities_history = SVEC_EMPTY_INITIALIZER;
static struct svec match_history = SVEC_EMPTY_INITIALIZER;

static char *
validate_columns(const char *s)
{
    enum column columns;
    return columns_from_string(s, &columns);
}

static char *
validate_priorities(const char *s)
{
    unsigned int priorities;
    return priorities_from_string(s, &priorities);
}

static char *
validate_facilities(const char *s)
{
    unsigned int facilities;
    return facilities_from_string(s, &facilities);
}

static struct json *
state_to_json(struct state *state, enum column columns)
{
    struct json *obj = json_object_create();

    json_object_put(obj, "population", json_integer_create(state->population));

    if (state->n) {
        struct json *array = json_array_create_empty();
        for (size_t i = 0; i < state->n; i++) {
            json_array_add(array,
                           log_record_to_json(&state->reservoir[i], columns));
        }
        json_object_put(obj, "reservoir", array);
    }

    if (state->skipped) {
        json_object_put(obj, "skipped", json_integer_create(state->skipped));
    }

    if (state->tk[0]) {
        struct json *tk = json_array_create_empty();
        int count = 0;
        for (size_t i = 0; i < TK_L; i++) {
            struct json *tk_i = json_array_create_empty();
            for (size_t j = 0; j < TK_B; j++) {
                const struct topkapi *tk_ij = &state->tk[i][j];
                struct json *elem;
                if (tk_ij->rec || tk_ij->count) {
                    elem = json_object_create();
                    if (tk_ij->count) {
                        json_object_put(elem, "count",
                                        json_integer_create(tk_ij->count));
                    }
                    if (tk_ij->rec) {
                        json_object_put(elem, "record",
                                        log_record_to_json(tk_ij->rec,
                                                           columns));
                    }
                    count++;
                } else {
                    elem = json_null_create();
                }
                json_array_add(tk_i, elem);
            }
            json_array_add(tk, tk_i);
        }
        json_object_put(obj, "tk", tk);
        fprintf(stderr, "added %d\n", count);
    }

    return obj;
}

static struct jsonrpc_msg *
hv_handle_analyze_request(const struct jsonrpc_msg *request,
                          const struct svec *targets)
{
    struct spec *spec;
    struct ovsdb_error *error = spec_from_json(
        json_array(request->params)->elems[0], &spec);
    if (error) {
        return jsonrpc_create_error(ovsdb_error_to_json_free(error),
                                    request->id);

    }

    svec_clone(&spec->targets, targets);

    struct job *job = job_create(spec);
    for (;;) {
        uint64_t seq = seq_read(job->seq);

        bool done;
        atomic_read(&job->done, &done);
        if (done) {
            break;
        }

        seq_wait(job->seq, seq);
        poll_block();
    }

    struct state *state = ovsrcu_get(struct state *, &job->state);
    struct jsonrpc_msg *reply = jsonrpc_create_reply(
        state_to_json(state, spec->columns), request->id);
    spec_uninit(spec);
    free(spec);
    return reply;
}

static void
hv_handle_request(struct jsonrpc *rpc, const struct jsonrpc_msg *request,
                  const struct svec *targets)
{
    struct jsonrpc_msg *reply;
    if (!strcmp(request->method, "analyze")
        && json_array(request->params)->n == 1) {
        reply = hv_handle_analyze_request(request, targets);
    } else if (request->type == JSONRPC_REQUEST
           && !strcmp(request->method, "echo")) {
        reply = jsonrpc_create_reply(json_clone(request->params), request->id);
    } else {
        reply = jsonrpc_create_error(json_string_create("unknown method"),
                                     request->id);
    }

    jsonrpc_send(rpc, reply);
}

static void
remote_loop(const struct svec *targets)
{
    struct stream *stream;
    new_fd_stream(xstrdup("remote"), STDOUT_FILENO, 0, AF_UNIX, &stream);
    struct jsonrpc *rpc = jsonrpc_open(stream);

    for (;;) {
        jsonrpc_run(rpc);

        if (!jsonrpc_get_backlog(rpc)) {
            struct jsonrpc_msg *msg;
            int error = jsonrpc_recv(rpc, &msg);
            if (!error) {
                if (msg->type == JSONRPC_REQUEST) {
                    hv_handle_request(rpc, msg, targets);
                } else {
                    VLOG_ERR("%s: received unexpected %s message",
                             jsonrpc_get_name(rpc),
                             jsonrpc_msg_type_to_string(msg->type));
                    jsonrpc_msg_destroy(msg);
                    break;
                }
                jsonrpc_msg_destroy(msg);
            } else if (error != EAGAIN) {
                if (error == EOF) {
                    return;
                }
                ovs_fatal(error, "error receiving JSON-RPC message");
            }
        }

        jsonrpc_wait(rpc);
        if (!jsonrpc_get_backlog(rpc)) {
            jsonrpc_recv_wait(rpc);
        }
        poll_block();
    }
}

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    set_program_name(argv[0]);
    vlog_init();

    struct spec spec;
    parse_command_line(argc, argv, &spec);

    if (remote) {
        remote_loop(&spec.targets);
        return 0;
    }

    initscr();
    cbreak();
    noecho();
    nonl();
    intrflush(stdscr, false);
    keypad(stdscr, true);
    mousemask(ALL_MOUSE_EVENTS, NULL);
    nodelay(stdscr, true);
    meta(stdscr, true);
    curs_set(0);

    for (int c = 'a'; c <= 'z'; c++) {
        char s[3] = { 27, c, '\0' };
        define_key(s, META(c));
    }
    define_key("\033\177", META(127));

    fatal_signal_init();

    struct job *job = job_create(&spec);

    struct spec new_spec;
    spec_copy(&new_spec, &spec);

    int y_ofs = 0, x_ofs = 0;
    int y = 0;
    bool highlight_match = true;
    bool scroll_bar = true;
    for (;;) {
        uint64_t display_seqno = seq_read(job->seq);
        struct state *state = ovsrcu_get(struct state *, &job->state);
        struct results *r = state_to_results(state, &job->spec);

        int y_max = getmaxy(stdscr);
        int x_max = getmaxx(stdscr);

        int page = y_max - 1;

        switch (getch()) {
        case KEY_UP: case 'k': case CTRL('P'):
            if (y == 0 && r->n > 0) {
                new_spec.show = SHOW_LAST;
                log_record_destroy(new_spec.start);
                new_spec.start = log_record_clone(r->recs[0]);
            } else {
                y--;
            }
            break;
        case KEY_DOWN: case 'j': case CTRL('N'):
            if (r->n && y == r->n - 1) {
                new_spec.show = SHOW_FIRST;
                log_record_destroy(new_spec.start);
                new_spec.start = log_record_clone(r->recs[y]);
            } else {
                y++;
            }
            break;
        case KEY_LEFT: case 'h':
            if (x_ofs > 0) {
                x_ofs = MAX(x_ofs - 10, 0);
            }
            break;
        case KEY_RIGHT: case 'l':
            x_ofs += 10;
            break;
        case KEY_NPAGE: case ' ': case CTRL('F'):
            y_ofs += page;
            y = y_ofs;
            break;
        case KEY_PPAGE: case KEY_BACKSPACE: case CTRL('B'):
            y_ofs -= page;
            y = y_ofs + page - 1;
            break;
        case KEY_HOME: case '<':
            y_ofs = y = 0;
            break;
        case KEY_END: case '>':
            y_ofs = r->n - page;
            y = r->n - 1;
            break;
        case KEY_MOUSE:
            for (;;) {
                MEVENT event;
                if (getmouse(&event) != OK) {
                    break;
                }
                if (event.bstate == BUTTON4_PRESSED) {
                    y -= page / 10;
                } else if (event.bstate == BUTTON5_PRESSED) {
                    y += page / 10;
                } else if (event.bstate == BUTTON1_CLICKED) {
                    int new_y = event.y + y_ofs;
                    if (new_y < r->n) {
                        y = new_y;
                    }
                } else if (event.bstate == BUTTON1_DOUBLE_CLICKED) {
                    int new_y = event.y + y_ofs;
                    if (new_y < r->n) {
                        y = new_y;
                    }
                    if (spec.show == SHOW_SAMPLE && new_y < r->n) {
                        new_spec.show = SHOW_FIRST;
                        log_record_destroy(new_spec.start);
                        new_spec.start = log_record_clone(r->recs[y]);
                    }
                }
            }
            break;
        case 'q': case 'Q':
            goto exit;
        case CTRL('L'):
            redrawwin(stdscr);
            break;
        case '\n': case '\r':
            if (spec.show == SHOW_SAMPLE && y < r->n) {
                new_spec.show = SHOW_FIRST;
                log_record_destroy(new_spec.start);
                new_spec.start = log_record_clone(r->recs[y]);
            }
            break;
        case 'm':
            {
                char *match = readstr("message substring match", NULL,
                                      &match_history, NULL);
                if (match) {
                    free(new_spec.match);
                    if (match[0]) {
                        new_spec.match = match;
                    } else {
                        free(match);
                        new_spec.match = NULL;
                    }
                }
            }
            break;
        case 'c':
            {
                char *columns_s = readstr("columns", NULL, &columns_history,
                                          validate_columns);
                if (columns_s) {
                    char *error = columns_from_string(columns_s, &new_spec.columns);
                    ovs_assert(!error);
                }
                free(columns_s);
            }
            break;
        case 's':
            {
                char *sdids_s = readstr("sdids", NULL, &sdids_history, NULL);
                if (sdids_s) {
                    sset_clear(&new_spec.sdids);
                    sset_add_delimited(&new_spec.sdids, sdids_s, " ,");
                    free(sdids_s);
                }
            }
            break;
        case 'C':
            {
                char *components_s = readstr("components", NULL, &components_history, NULL);
                if (components_s) {
                    sset_clear(&new_spec.components);
                    sset_add_delimited(&new_spec.components,
                                       components_s, " ,");
                    free(components_s);
                }
            }
            break;
        case 'S':
            {
                char *subcomponents_s = readstr("subcomponents", NULL, &subcomponents_history, NULL);
                if (subcomponents_s) {
                    sset_clear(&new_spec.subcomponents);
                    sset_add_delimited(&new_spec.subcomponents,
                                       subcomponents_s, " ,");
                    free(subcomponents_s);
                }
            }
            break;
        case 'E':
            {
                char *error_codes_s = readstr("error codes", NULL, &error_codes_history, NULL);
                if (error_codes_s) {
                    sset_clear(&new_spec.error_codes);
                    sset_add_delimited(&new_spec.error_codes,
                                       error_codes_s, " ,");
                    free(error_codes_s);
                }
            }
            break;
        case 'p':
            {
                char *priorities_s = readstr("priorities", NULL, &priorities_history, validate_priorities);
                if (priorities_s) {
                    char *error = priorities_from_string(priorities_s, &new_spec.priorities);
                    ovs_assert(!error);
                }
            }
            break;
        case 'f':
            {
                char *facilities_s = readstr("facilities", NULL, &facilities_history, validate_facilities);
                if (facilities_s) {
                    char *error = facilities_from_string(facilities_s, &new_spec.facilities);
                    ovs_assert(!error);
                }
            }
            break;

        case 'T':
            new_spec.show = new_spec.show == SHOW_TOP ? SHOW_FIRST : SHOW_TOP;
            break;

        case META('u'):
            highlight_match ^= 1;
            break;

        case '\\':
            scroll_bar ^= 1;
            break;
        }

        if (!spec_equals(&spec, &new_spec)) {
            job_cancel(job);
            spec_uninit(&spec);
            spec_copy(&spec, &new_spec);
            job = job_create(&spec);
        }

        y = range(y, 0, r->n ? r->n - 1 : 0);
        y_ofs = range(y_ofs, MAX(y - page + 1, 0), y);

        for (size_t i = 0; i < y_max - 1; i++) {
            struct ds s = DS_EMPTY_INITIALIZER;
            if (i + y_ofs < r->n) {
                log_record_format(r->recs[i + y_ofs],
                                  i + y_ofs, r->n, &job->spec, &s);
            } else {
                ds_put_char(&s, '~');
            }

            struct substring line = ss_cstr(ds_cstr(&s) + MIN(x_ofs, s.length));
            line.length = MIN(line.length, x_ofs + x_max - 3);
            mvaddnstr(i, 0, line.s, line.length);
            clrtoeol();

            if (highlight_match && job->spec.match) {
                int ofs = ss_find_case(line, ss_cstr(job->spec.match));
                if (ofs) {
                    mvchgat(i, ofs, strlen(job->spec.match), A_BOLD, 0, NULL);
                }
            }

            if (r->n && i + y_ofs == y) {
                mvchgat(i, 0, x_max - 2, A_REVERSE, 0, NULL);
            }
            ds_destroy(&s);
        }

        unsigned int total = r->before + r->n + r->after;
        if (scroll_bar && total) {
            int y0 = y_ofs + r->before;
            int y1 = MIN(y_ofs + page, r->n) + r->before - 1;
            int y0s = y0 * (page - 2) / total + 1;
            int y1s = y1 * (page - 2) / total + 1;
            mvaddch(0, x_max - 1, ACS_TTEE);
            for (int i = 1; i < y_max - 2; i++) {
                mvaddch(i, x_max - 1, ACS_VLINE);
            }
            mvaddch(y_max - 2, x_max - 1, ACS_BTEE);
            for (int i = y0s; i <= y1s; i++) {
                mvaddch(i, x_max - 1, ACS_CKBOARD);
            }
        }

        ovs_mutex_lock(&job->stats_lock);
        unsigned int p = job->progress;
        unsigned int g = job->goal;
        ovs_mutex_unlock(&job->stats_lock);

        move(y_max - 1, 0);
        if (g) {
            int n_ = x_max * p / g;
            int n = n_ < 0 ? 0 : n_ > x_max ? x_max : n_;
            for (int x = 0; x < n; x++) {
                addch(ACS_CKBOARD);
            }
        } else {
            mvprintw(y_max - 1, 0, "row %llu of %llu",
                     r->before + y + 1, r->before + r->n + r->after);
        }
        clrtoeol();
        refresh();

        seq_wait(job->seq, display_seqno);
        poll_fd_wait(STDIN_FILENO, POLLIN);
        poll_block();
    }
exit:

#if 0
    printf("parsed %.1f MB of logs containing %llu records\n",
           total_bytes / 1024.0 / 1024.0, total_recs);
    printf("decompressed %.1f MB of gzipped data\n",
           total_decompressed / 1024.0 / 1024.0);
#endif

    endwin();
}

static void
usage(void)
{
    printf("\
%s, for querying log files\n\
usage: %s [TARGET] COMMAND [ARG...]\n\
\n\
Common commands:\n\
  hh   List sampled heavy hitters\n\
Other options:\n\
  -h, --help         Print this helpful information\n\
  -V, --version      Display ovs-appctl version information\n",
           program_name, program_name);
    exit(EXIT_SUCCESS);
}

static bool
facility_from_string(const char *s, enum facility *facility)
{
    for (int i = 0; i < 24; i++) {
        if (!strcmp(s, facility_to_string(i))) {
            *facility = i;
            return true;
        }
    }
    return false;
}

static char * OVS_WARN_UNUSED_RESULT
facilities_from_string(const char *s_, unsigned int *facilities)
{
    char *s = xstrdup(s_);
    unsigned int xor = 0;
    if (*s == '^' || *s == '!') {
        s++;
        xor = (1u << 24) - 1;
    }

    *facilities = 0;

    char *save_ptr = NULL;
    for (char *token = strtok_r(s, ", ", &save_ptr); token;
         token = strtok_r(NULL, ", ", &save_ptr)) {
        enum facility f;
        if (!facility_from_string(s, &f)) {
            char *error = xasprintf("%s: unknown facility", token);
            free(s);
            return error;
        }
        *facilities |= 1u << f;
    }
    *facilities ^= xor;
    free(s);
    return NULL;
}

static double
parse_date(const char *s)
{
    /* XXX Date parsing is hard.  This is kind of a cop-out. */

    if (!strcmp(s, "-")) {
        return 0;
    }

    double when = parse_timestamp(s, strlen(s));
    if (when > 0) {
        return when;
    }

    char *args[] = { "date", "-d", CONST_CAST(char *, s), "+%s", NULL };
    char *command = process_escape_args(args);
    FILE *stream = popen(command, "r");
    if (!stream) {
        ovs_fatal(errno, "%s: popen failed", command);
    }
    bool ok = fscanf(stream, "%lf", &when) == 1;
    int status = pclose(stream);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        if (ok) {
            free(command);
            return when;
        } else {
            ovs_fatal(errno, "%s: unexpected output parsing date %s",
                      command, s);
        }
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
        ovs_fatal(errno, "%s: bad date", s);
    }
    ovs_fatal(errno, "%s: error parsing date (%s)",
              command, process_status_msg(status));
}

static const char *
column_to_string(enum column c)
{
    switch (c) {
#define COLUMN(ENUM, NAME) case COL_##ENUM: return NAME;
        COLUMNS
#undef COLUMN
    default: return NULL;
    }
}

#if 0
static char *
columns_to_string(enum column columns)
{
    struct ds s = DS_EMPTY_INITIALIZER;
    for (enum column c = columns; c; c = zero_rightmost_1bit(c)) {
        ds_put_format(&s, "%s ", column_to_string(rightmost_1bit(c)));
    }
    ds_chomp(&s, ' ');
    return ds_steal_cstr(&s);
}
#endif

static struct json *
columns_to_json(enum column columns)
{
    struct json *array = json_array_create_empty();
    for (enum column c = columns; c; c = zero_rightmost_1bit(c)) {
        json_array_add(
            array, json_string_create(column_to_string(rightmost_1bit(c))));
    }
    return array;
}

static enum column
column_from_string(const char *s)
{
#define COLUMN(ENUM, NAME) if (!strcmp(s, NAME)) return COL_##ENUM;
    COLUMNS
#undef COLUMNS
    return 0;
}

static char * OVS_WARN_UNUSED_RESULT
columns_from_string(const char *s_, enum column *columnsp)
{
    char *s = xstrdup(s_);
    char *save_ptr = NULL;
    enum column columns = 0;
    for (char *token = strtok_r(s, ", ", &save_ptr); token;
         token = strtok_r(NULL, ", ", &save_ptr)) {
        enum column c = column_from_string(token);
        if (!c) {
            char *error = xasprintf("%s: unknown column", token);
            free(s);
            return error;
        }
        columns |= c;
    }
    free(s);
    *columnsp = columns;
    return NULL;
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
columns_from_json(const struct json *array, enum column *columns)
{
    *columns = 0;

    ovs_assert(array->type == JSON_ARRAY);
    for (size_t i = 0; i < array->array.n; i++) {
        const struct json *elem = array->array.elems[i];
        enum column c = (elem->type == JSON_STRING
                         ? column_from_string(json_string(elem))
                         : 0);
        if (!c) {
            return ovsdb_syntax_error(elem, NULL, "column name expected");
        }
        *columns |= c;
    }
    return NULL;
}

static void
do_debug_parse(void)
{
    struct ds line = DS_EMPTY_INITIALIZER;
    for (int ln = 1; !ds_get_line(&line, stdin); ln++) {
        struct log_record rec;
        memset(&rec, 0, sizeof rec);

        struct parse_ctx ctx = {
            .host = "localhost",
            .fn = "stdin",
            .ln = ln,
            .line_start = ds_cstr(&line),
            .line_end = strchr(ds_cstr(&line), '\n'),
            .p = ds_cstr(&line),
        };
        parse_date_first_record(&ctx, &rec);

        struct ds out = DS_EMPTY_INITIALIZER;
        log_record_format__(&rec, (COL_WHEN | COL_FACILITY | COL_PRIORITY |
                                   COL_HOSTNAME | COL_APP_NAME | COL_PROCID |
                                   COL_MSGID | COL_SDID | COL_COMP |
                                   COL_SUBCOMP | COL_ERROR_CODE | COL_MSG |
                                   COL_VALID), &out);
        puts(ds_cstr(&out));
        ds_destroy(&out);
    }

}

static void
parse_command_line(int argc, char *argv[], struct spec *spec)
{
    enum {
        OPT_SINCE = UCHAR_MAX + 1,
        OPT_UNTIL,
        OPT_REMOTE,
        OPT_SDIDS,
        OPT_DEBUG_PARSE,
        VLOG_OPTION_ENUMS,
    };
    static const struct option long_options[] = {
        {"remote", no_argument, NULL, OPT_REMOTE},
        {"columns", required_argument, NULL, 'c'},
        {"at", required_argument, NULL, 'a'},
        {"show", required_argument, NULL, 's'},
        {"match", required_argument, NULL, 'm'},
        {"priorities", required_argument, NULL, 'p'},
        {"facilities", required_argument, NULL, 'f'},
        {"sdids", required_argument, NULL, OPT_SDIDS},
        {"component", required_argument, NULL, 'C'},
        {"subcomponent", required_argument, NULL, 'S'},
        {"since", required_argument, NULL, OPT_SINCE},
        {"after", required_argument, NULL, OPT_SINCE},
        {"until", required_argument, NULL, OPT_UNTIL},
        {"before", required_argument, NULL, OPT_UNTIL},
        {"debug-parse", no_argument, NULL, OPT_DEBUG_PARSE},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        VLOG_LONG_OPTIONS,
        {NULL, 0, NULL, 0},
    };
    char *short_options = ovs_cmdl_long_options_to_short_options(long_options);
    bool debug_parse = false;

    spec_init(spec);
    svec_add(&columns_history, "when facility priority comp subcomp msg");
    spec->columns = (COL_WHEN | COL_FACILITY | COL_PRIORITY | COL_COMP
                     | COL_SUBCOMP | COL_MSG);
    for (;;) {
        int option;

        option = getopt_long(argc, argv, short_options, long_options, NULL);
        if (option == -1) {
            break;
        }

        char *error_s;
        switch (option) {
        case OPT_REMOTE:
            remote = true;
            break;

        case 'c':
            svec_add(&columns_history, optarg);
            error_s = columns_from_string(optarg, &spec->columns);
            if (error_s) {
                ovs_fatal(0, "%s", error_s);
            }
            break;

        case 'a':
            spec->at = strtod(optarg, NULL);
            break;

        case 's':
            if (!show_from_string(optarg, &spec->show)) {
                ovs_fatal(0, "%s: unknown \"show\"", optarg);
            }
            break;

        case 'm':
            svec_add(&match_history, optarg);
            spec->match = xstrdup(optarg);
            break;

        case 'p':
            svec_add(&priorities_history, optarg);
            error_s = priorities_from_string(optarg, &spec->priorities);
            if (error_s) {
                ovs_fatal(0, "%s", error_s);
            }
            break;

        case 'f':
            svec_add(&facilities_history, optarg);
            error_s = facilities_from_string(optarg, &spec->facilities);
            if (error_s) {
                ovs_fatal(0, "%s", error_s);
            }
            break;

        case OPT_SINCE:
            spec->date_since = parse_date(optarg);
            break;

        case OPT_UNTIL:
            spec->date_until = parse_date(optarg);
            break;

        case OPT_SDIDS:
            svec_add(&sdids_history, optarg);
            sset_add_delimited(&spec->sdids, optarg, " ,");
            break;

        case 'C':
            svec_add(&components_history, optarg);
            sset_add_delimited(&spec->components, optarg, " ,");
            break;

        case 'S':
            svec_add(&subcomponents_history, optarg);
            sset_add_delimited(&spec->subcomponents, optarg, " ,");
            break;

        case 'E':
            svec_add(&error_codes_history, optarg);
            sset_add_delimited(&spec->error_codes, optarg, " ,");
            break;

        case OPT_DEBUG_PARSE:
            debug_parse = true;
            break;

        case 'h':
            usage();
            break;

        case 'V':
            ovs_print_version(0, 0);
            exit(EXIT_SUCCESS);

        VLOG_OPTION_HANDLERS

        case '?':
            exit(EXIT_FAILURE);

        default:
            OVS_NOT_REACHED();
        }
    }
    free(short_options);

    if (debug_parse) {
        do_debug_parse();
        exit(0);
    }

    if (optind < argc) {
        for (size_t i = optind; i < argc; i++) {
            svec_add(&spec->targets, argv[i]);
        }
    } else {
        ovs_fatal(0, "at least one non-option argument is required "
                  "(use --help for help)");
    }
}
