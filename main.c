/*
 * VideoInfo (vidinf) — utility for extracting metadata from video files
 * Usage: vidinf -<path> [arg]
 *
 * Built on top of FFmpeg's libavformat/libavcodec/libavutil.
 * Structure and style mirror the "imginf" (ImageInfo) tool.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>
#include <libavutil/display.h>
#include <libavutil/rational.h>

#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_CYAN    "\033[36m"
#define C_YELLOW  "\033[33m"
#define C_GREEN   "\033[32m"
#define C_RED     "\033[31m"
#define C_BLUE    "\033[34m"
#define C_MAGENTA "\033[35m"
#define C_GRAY    "\033[90m"

/* ─── Version ─────────────────────────────────────────────────── */
#define VIDINF_VERSION "2.3.1"

/* ─── Output mode flags ────────────────────────────────────────── */
typedef struct {
    int show_basic;      /* -b  basic fields only (container, duration, size) */
    int show_all;        /* -a  all metadata tags + every stream in detail    */
    int show_gps;        /* -g  GPS / location only                           */
    int show_streams;    /* -c  stream / codec data only ("camera" analogue)  */
    int show_json;       /* -j  output in JSON format                         */
    int show_size;       /* -s  detailed file size information                */
    int no_color;        /* --no-color  without ANSI colors                   */
    int verbose;         /* -v  verbose mode                                  */
} Options;

/* ═══════════════════════════════════════════════════════════════
   Helpers
   ═══════════════════════════════════════════════════════════════ */

/* Format bytes into a human-readable form */
static void format_size(long long bytes, char *buf, size_t bufsz)
{
    if (bytes < 1024)
        snprintf(buf, bufsz, "%lld B", bytes);
    else if (bytes < 1024LL * 1024)
        snprintf(buf, bufsz, "%.2f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024)
        snprintf(buf, bufsz, "%.2f MB", bytes / (1024.0 * 1024));
    else
        snprintf(buf, bufsz, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
}

/* Format seconds (double) as HH:MM:SS.mmm */
static void format_duration(double seconds, char *buf, size_t bufsz)
{
    if (seconds < 0) { snprintf(buf, bufsz, "unknown"); return; }
    int h = (int)(seconds / 3600);
    int m = (int)(fmod(seconds, 3600) / 60);
    double s = fmod(seconds, 60);
    snprintf(buf, bufsz, "%02d:%02d:%06.3f", h, m, s);
}

/* Get file extension (including the dot) */
static const char *get_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) return "(no extension)";
    return dot;
}

/* Look up a metadata tag from an AVDictionary, case-insensitive-ish
 * (FFmpeg tags are usually lowercase already). Returns NULL if absent. */
static const char *dict_get(const AVDictionary *dict, const char *key)
{
    AVDictionaryEntry *e = av_dict_get(dict, key, NULL, 0);
    return e ? e->value : NULL;
}

/* ── Parse ISO 6709 location strings, e.g. "+40.6892-074.0445/"
 *    which QuickTime/MP4 "location" or "com.apple.quicktime.location.ISO6709"
 *    tags commonly use. Returns 1 on success. ── */
static int parse_iso6709(const char *s, double *lat, double *lon)
{
    if (!s || (*s != '+' && *s != '-')) return 0;

    char *end1 = NULL;
    double a = strtod(s, &end1);
    if (end1 == s || (*end1 != '+' && *end1 != '-')) return 0;

    char *end2 = NULL;
    double b = strtod(end1, &end2);
    if (end2 == end1) return 0;

    *lat = a;
    *lon = b;
    return 1;
}

/* Approximate country by coordinates (simplified table, same heuristic
 * as the image tool) */
static const char *approx_country(double lat, double lon)
{
    if (lat >= 44.0 && lat <= 52.5 && lon >= 22.0 && lon <= 40.0)
        return "Ukraine";
    if (lat >= 41.0 && lat <= 71.5 && lon >= 19.0 && lon <= 180.0)
        return "Russia / CIS";
    if (lat >= 51.0 && lat <= 71.0 && lon >= -8.0 && lon <= 2.0)
        return "United Kingdom / Ireland";
    if (lat >= 41.0 && lat <= 51.0 && lon >= -5.0 && lon <= 9.5)
        return "France / Spain";
    if (lat >= 47.0 && lat <= 55.0 && lon >= 6.0  && lon <= 15.0)
        return "Germany / Austria";
    if (lat >= 24.0 && lat <= 50.0 && lon >= -125.0 && lon <= -66.0)
        return "USA";
    if (lat >= 42.0 && lat <= 83.0 && lon >= -141.0 && lon <= -52.0)
        return "Canada";
    if (lat >= -55.0 && lat <= -15.0 && lon >= -74.0 && lon <= -35.0)
        return "Brazil";
    if (lat >= 18.0 && lat <= 53.0 && lon >= 73.0  && lon <= 135.0)
        return "China / Asia";
    if (lat >= 30.0 && lat <= 37.0 && lon >= 129.0 && lon <= 146.0)
        return "Japan";
    if (lat >= -43.0 && lat <= -11.0 && lon >= 113.0 && lon <= 154.0)
        return "Australia";
    return "Unknown";
}

/* Escape a string for safe embedding inside a JSON string literal */
static void print_json_escaped(const char *s)
{
    if (!s) return;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            case '\n': printf("\\n");  break;
            case '\r': printf("\\r");  break;
            case '\t': printf("\\t");  break;
            default:
                if ((unsigned char)*p < 0x20)
                    printf("\\u%04x", *p);
                else
                    putchar(*p);
        }
    }
}

/* ─── Field / section output macros ─────────────────────────────── */

#define FIELD(label, fmt, ...) \
    do { \
        if (o->no_color) \
            printf("%-22s " fmt "\n", label":", ##__VA_ARGS__); \
        else \
            printf(C_CYAN "%-22s" C_RESET " " fmt "\n", label":", ##__VA_ARGS__); \
    } while (0)

#define SECTION(title) \
    do { \
        if (!o->show_json) { \
            if (o->no_color) \
                printf("\n--- %s ---\n", title); \
            else \
                printf("\n" C_YELLOW C_BOLD "▶ %s" C_RESET "\n", title); \
        } \
    } while (0)

/* ─── Banner / help ──────────────────────────────────────────────── */

static void print_banner(const Options *o)
{
    if (o->show_json) return;
    if (o->no_color) {
        printf("========================================\n");
        printf("  VideoInfo v%s\n", VIDINF_VERSION);
        printf("========================================\n");
    } else {
        printf(C_CYAN C_BOLD
               "╔══════════════════════════════════════╗\n"
               "║      VideoInfo v%-6s               ║\n"
               "╚══════════════════════════════════════╝\n"
               C_RESET, VIDINF_VERSION);
    }
}

static void print_help(void)
{
    printf(C_BOLD "VideoInfo v%s" C_RESET
           " — utility for extracting metadata from video files\n\n", VIDINF_VERSION);

    printf(C_YELLOW "Usage:" C_RESET "\n");
    printf("  vidinf -<path_to_file> [options]\n\n");

    printf(C_YELLOW "Options:" C_RESET "\n");
    printf("  " C_GREEN "-b" C_RESET
           "            Basic fields only (container, duration, size)\n");
    printf("  " C_GREEN "-a" C_RESET
           "            All metadata tags and full per-stream detail\n");
    printf("  " C_GREEN "-g" C_RESET
           "            GPS / location only\n");
    printf("  " C_GREEN "-c" C_RESET
           "            Stream / codec data only (video, audio, subtitles)\n");
    printf("  " C_GREEN "-s" C_RESET
           "            Detailed file size information\n");
    printf("  " C_GREEN "-j" C_RESET
           "            Output in JSON format\n");
    printf("  " C_GREEN "-v" C_RESET
           "            Verbose mode\n");
    printf("  " C_GREEN "--no-color" C_RESET
           "    Disable colored output\n");
    printf("  " C_GREEN "--help" C_RESET
           "        Show this help message\n");
    printf("  " C_GREEN "--version" C_RESET
           "     Program version\n\n");

    printf(C_YELLOW "Examples:" C_RESET "\n");
    printf("  vidinf -/home/user/clip.mp4\n");
    printf("  vidinf -./video.mov -g\n");
    printf("  vidinf -/tmp/rec.mkv -j\n");
    printf("  vidinf -movie.mp4 -c -v\n");
    printf("  vidinf -movie.mp4 -a --no-color\n\n");

    printf(C_YELLOW "Supported formats:" C_RESET
           " anything demuxable by FFmpeg/libavformat\n"
           "                     (MP4, MOV, MKV, AVI, WEBM, FLV, ...)\n");
}

/* ═══════════════════════════════════════════════════════════════
   Metadata extraction
   ═══════════════════════════════════════════════════════════════ */

/* Print all entries of an AVDictionary as a tag dump (used by -a) */
static void print_all_tags(const AVDictionary *dict, const Options *o, int indent)
{
    const AVDictionaryEntry *e = NULL;
    while ((e = av_dict_get(dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (o->no_color)
            printf("%*s%-28s %s\n", indent, "", e->key, e->value);
        else
            printf("%*s" C_BLUE "%-28s" C_RESET " %s\n", indent, "", e->key, e->value);
    }
}

/* Try to find a location tag among common keys used by different muxers */
static const char *find_location_tag(const AVDictionary *dict)
{
    static const char *keys[] = {
        "location",
        "com.apple.quicktime.location.ISO6709",
        "location-eng",
        "ISO6709",
        NULL
    };
    for (int i = 0; keys[i]; i++) {
        const char *v = dict_get(dict, keys[i]);
        if (v && v[0]) return v;
    }
    return NULL;
}

static void print_gps_section(const AVDictionary *fmt_meta, const Options *o)
{
    const char *loc = find_location_tag(fmt_meta);
    double lat = 0, lon = 0;

    if (loc && parse_iso6709(loc, &lat, &lon)) {
        const char *country = approx_country(lat, lon);
        if (!o->show_json) {
            SECTION("GPS / Location");
            FIELD("Approximate location", "%s", country);
            FIELD("Coordinates", "%.6f°%c, %.6f°%c",
                  fabs(lat), lat >= 0 ? 'N' : 'S',
                  fabs(lon), lon >= 0 ? 'E' : 'W');
            if (!o->no_color)
                printf(C_GRAY "  → Google Maps: https://maps.google.com/?q=%.6f,%.6f\n"
                       C_RESET, lat, lon);
            else
                printf("  Google Maps: https://maps.google.com/?q=%.6f,%.6f\n", lat, lon);
        } else {
            printf(",\n  \"gps\": {\n");
            printf("    \"latitude\": %.6f,\n", lat);
            printf("    \"longitude\": %.6f,\n", lon);
            printf("    \"approx_country\": \"%s\",\n", country);
            printf("    \"raw\": \"%s\"", loc);
            printf("\n  }");
        }
    } else {
        if (!o->show_json) {
            if (o->show_gps || o->verbose) {
                if (o->no_color)
                    printf("\n[GPS] Location data not found.\n");
                else
                    printf("\n" C_GRAY "[GPS] Location data unavailable.\n" C_RESET);
            }
        } else if (o->show_gps) {
            printf(",\n  \"gps\": {\"found\": false}");
        }
    }
}

/* Human-readable name for AVMediaType */
static const char *media_type_name(enum AVMediaType t)
{
    switch (t) {
        case AVMEDIA_TYPE_VIDEO:      return "Video";
        case AVMEDIA_TYPE_AUDIO:      return "Audio";
        case AVMEDIA_TYPE_SUBTITLE:   return "Subtitle";
        case AVMEDIA_TYPE_DATA:       return "Data";
        case AVMEDIA_TYPE_ATTACHMENT: return "Attachment";
        default:                      return "Unknown";
    }
}

/* Extract rotation (degrees) from a video stream's side data / display matrix */
static double stream_rotation(const AVStream *st)
{
    for (int i = 0; i < st->codecpar->nb_coded_side_data; i++) {
        const AVPacketSideData *sd = &st->codecpar->coded_side_data[i];
        if (sd->type == AV_PKT_DATA_DISPLAYMATRIX && sd->data) {
            double theta = av_display_rotation_get((int32_t *)sd->data);
            if (!isnan(theta)) return theta;
        }
    }
    return 0.0;
}

static void print_streams_section(AVFormatContext *fc, const Options *o)
{
    if (!o->show_json) SECTION("Streams");

    int first_json = 1;
    if (o->show_json) printf(",\n  \"streams\": [");

    for (unsigned int i = 0; i < fc->nb_streams; i++) {
        AVStream *st = fc->streams[i];
        AVCodecParameters *cp = st->codecpar;
        const AVCodec *codec = avcodec_find_decoder(cp->codec_id);
        const char *codec_name = codec ? codec->name : "unknown";
        const char *type_name = media_type_name(cp->codec_type);

        double fps = 0;
        if (st->avg_frame_rate.den)
            fps = av_q2d(st->avg_frame_rate);
        else if (st->r_frame_rate.den)
            fps = av_q2d(st->r_frame_rate);

        char bitrate_str[32] = "";
        if (cp->bit_rate > 0)
            snprintf(bitrate_str, sizeof(bitrate_str), "%.0f kbps", cp->bit_rate / 1000.0);

        const char *lang = dict_get(st->metadata, "language");
        const char *handler = dict_get(st->metadata, "handler_name");

        if (!o->show_json) {
            printf("\n  " C_GREEN "[%u] %s" C_RESET " — %s\n", i, type_name, codec_name);
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
                FIELD("  Resolution", "%d × %d", cp->width, cp->height);
                if (fps > 0) FIELD("  Frame rate", "%.3f fps", fps);
                double rot = stream_rotation(st);
                if (rot != 0.0) FIELD("  Rotation", "%.0f°", rot);
                const char *pix = av_get_pix_fmt_name((enum AVPixelFormat)cp->format);
                if (pix) FIELD("  Pixel format", "%s", pix);
            } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
                FIELD("  Sample rate", "%d Hz", cp->sample_rate);
                FIELD("  Channels", "%d", cp->ch_layout.nb_channels);
            }
            if (bitrate_str[0]) FIELD("  Bit rate", "%s", bitrate_str);
            if (lang)    FIELD("  Language", "%s", lang);
            if (handler) FIELD("  Handler",  "%s", handler);
        } else {
            printf(first_json ? "\n    {" : ",\n    {");
            first_json = 0;
            printf("\n      \"index\": %u", i);
            printf(",\n      \"type\": \"%s\"", type_name);
            printf(",\n      \"codec\": \"%s\"", codec_name);
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
                printf(",\n      \"width\": %d", cp->width);
                printf(",\n      \"height\": %d", cp->height);
                if (fps > 0) printf(",\n      \"fps\": %.3f", fps);
                double rot = stream_rotation(st);
                if (rot != 0.0) printf(",\n      \"rotation\": %.0f", rot);
            } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
                printf(",\n      \"sample_rate\": %d", cp->sample_rate);
                printf(",\n      \"channels\": %d", cp->ch_layout.nb_channels);
            }
            if (cp->bit_rate > 0) printf(",\n      \"bit_rate\": %lld", (long long)cp->bit_rate);
            if (lang)    { printf(",\n      \"language\": \""); print_json_escaped(lang); printf("\""); }
            if (handler) { printf(",\n      \"handler\": \"");  print_json_escaped(handler); printf("\""); }
            printf("\n    }");
        }
    }

    if (o->show_json) printf("\n  ]");
}

static void print_common_tags(const AVDictionary *meta, const Options *o)
{
    static const struct { const char *key; const char *label; } tags[] = {
        {"title",         "Title"},
        {"artist",        "Artist"},
        {"album",         "Album"},
        {"comment",       "Comment"},
        {"description",   "Description"},
        {"genre",         "Genre"},
        {"copyright",     "Copyright"},
        {"encoder",       "Encoder"},
        {"creation_time", "Creation time"},
        {NULL, NULL}
    };

    int any = 0;
    for (int i = 0; tags[i].key; i++)
        if (dict_get(meta, tags[i].key)) { any = 1; break; }
    if (!any) return;

    if (!o->show_json) {
        SECTION("Tags");
        for (int i = 0; tags[i].key; i++) {
            const char *v = dict_get(meta, tags[i].key);
            if (!v) continue;
            char label_colon[32];
            snprintf(label_colon, sizeof(label_colon), "%s:", tags[i].label);
            if (o->no_color)
                printf("%-22s %s\n", label_colon, v);
            else
                printf(C_CYAN "%-22s" C_RESET " %s\n", label_colon, v);
        }
    } else {
        for (int i = 0; tags[i].key; i++) {
            const char *v = dict_get(meta, tags[i].key);
            if (v) {
                printf(",\n  \"%s\": \"", tags[i].key);
                print_json_escaped(v);
                printf("\"");
            }
        }
    }
}

/* ─── Main file processing ───────────────────────────────── */
static void process_file(const char *path, const Options *o)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, C_RED "Error:" C_RESET " file not found: %s\n", path);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, C_RED "Error:" C_RESET " is not a file: %s\n", path);
        return;
    }

    AVFormatContext *fc = NULL;
    int rc = avformat_open_input(&fc, path, NULL, NULL);
    if (rc < 0) {
        char errbuf[128];
        av_strerror(rc, errbuf, sizeof(errbuf));
        fprintf(stderr, C_RED "Error:" C_RESET " cannot open '%s': %s\n", path, errbuf);
        return;
    }
    if (avformat_find_stream_info(fc, NULL) < 0) {
        fprintf(stderr, C_RED "Error:" C_RESET
                " could not read stream information from '%s'\n", path);
        avformat_close_input(&fc);
        return;
    }

    print_banner(o);

    const char *ext = get_extension(path);

    char timebuf[64];
    struct tm *tm_info = localtime(&st.st_mtime);
    strftime(timebuf, sizeof(timebuf), "%d.%m.%Y %H:%M:%S", tm_info);

    char sizebuf[32];
    format_size((long long)st.st_size, sizebuf, sizeof(sizebuf));

    double duration_s = fc->duration > 0 ? (double)fc->duration / AV_TIME_BASE : -1;
    char durbuf[32];
    format_duration(duration_s, durbuf, sizeof(durbuf));

    long long overall_bitrate = fc->bit_rate;

    /* ── JSON header ── */
    if (o->show_json) {
        printf("{\n");
        printf("  \"file\": \"%s\",\n", path);
        printf("  \"extension\": \"%s\",\n", ext);
        printf("  \"container\": \"%s\",\n", fc->iformat->name ? fc->iformat->name : "unknown");
        printf("  \"size_bytes\": %ld,\n", (long)st.st_size);
        printf("  \"size_human\": \"%s\",\n", sizebuf);
        printf("  \"mtime\": \"%s\",\n", timebuf);
        printf("  \"duration\": \"%s\"", durbuf);
        if (overall_bitrate > 0)
            printf(",\n  \"overall_bit_rate_kbps\": %.0f", overall_bitrate / 1000.0);
    }

    /* ── General info (default view) ── */
    if (!o->show_json && !o->show_gps && !o->show_streams) {
        SECTION("General Information");
        FIELD("Extension",     "%s", ext);
        FIELD("File path",     "%s", path);
        FIELD("Container",     "%s (%s)", fc->iformat->name ? fc->iformat->name : "unknown",
              fc->iformat->long_name ? fc->iformat->long_name : "?");
        FIELD("File size",     "%s (%ld bytes)", sizebuf, (long)st.st_size);
        FIELD("Last modified", "%s", timebuf);
        FIELD("Duration",      "%s", durbuf);
        if (overall_bitrate > 0)
            FIELD("Overall bit rate", "%.0f kbps", overall_bitrate / 1000.0);
    }

    /* If -b (basic) only, stop here */
    if (o->show_basic && !o->show_all && !o->show_gps && !o->show_streams) {
        if (o->show_json) printf("\n}\n");
        else {
            if (!o->no_color)
                printf("\n" C_GRAY "────────────────────────────────────────\n" C_RESET);
            else
                printf("\n----------------------------------------\n");
        }
        avformat_close_input(&fc);
        return;
    }

    /* ── Detailed file size (-s) ── */
    if (o->show_size && !o->show_json) {
        SECTION("File Size Detail");
        FIELD("Bytes",     "%ld", (long)st.st_size);
        FIELD("Kilobytes", "%.2f KB", st.st_size / 1024.0);
        FIELD("Megabytes", "%.2f MB", st.st_size / (1024.0 * 1024));
    }

    /* ── Streams (-c) ── */
    if (!o->show_gps && (o->show_streams || o->show_all || !o->show_gps))
        print_streams_section(fc, o);

    /* ── Common metadata tags ── */
    if (!o->show_gps && !o->show_streams)
        print_common_tags(fc->metadata, o);

    /* ── GPS / Location ── */
    print_gps_section(fc->metadata, o);

    /* ── All raw tags (-a) ── */
    if (o->show_all && !o->show_json) {
        SECTION("All Format Metadata Tags");
        print_all_tags(fc->metadata, o, 2);

        for (unsigned int i = 0; i < fc->nb_streams; i++) {
            AVStream *st = fc->streams[i];
            if (!st->metadata) continue;
            const AVDictionaryEntry *probe = av_dict_get(st->metadata, "", NULL, AV_DICT_IGNORE_SUFFIX);
            if (!probe) continue;
            printf("\n  " C_GREEN "Stream #%u tags:" C_RESET "\n", i);
            print_all_tags(st->metadata, o, 4);
        }
    }

    /* ── Close ── */
    if (o->show_json) {
        printf("\n}\n");
    } else {
        if (!o->no_color)
            printf("\n" C_GRAY "────────────────────────────────────────\n" C_RESET);
        else
            printf("\n----------------------------------------\n");
    }

    avformat_close_input(&fc);
}

/* ─── MAIN ───────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            C_RED "Error:" C_RESET " no file or flag specified.\n"
            "Use " C_GREEN "--help" C_RESET " for help.\n");
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("VideoInfo v%s\n", VIDINF_VERSION);
            return EXIT_SUCCESS;
        }
    }

#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
#endif
    av_log_set_level(AV_LOG_QUIET);

    const char *filepath = NULL;
    Options opts = {0};

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        /* Long options */
        if (strcmp(arg, "--no-color") == 0) {
            opts.no_color = 1; continue;
        }

        /* Path: starts with '-' but second char is not a known option letter */
        if (arg[0] == '-' && arg[1] != '-' && arg[1] != '\0') {
            const char single_opts[] = "bagcsjv";
            int found = 0;
            for (int k = 0; single_opts[k]; k++) {
                if (arg[1] == single_opts[k] && arg[2] == '\0') {
                    found = 1; break;
                }
            }
            if (!found) {
                filepath = arg + 1;
                continue;
            }
        }

        /* Single-letter options */
        if (arg[0] == '-' && arg[1] != '-') {
            for (int k = 1; arg[k]; k++) {
                switch (arg[k]) {
                    case 'b': opts.show_basic   = 1; break;
                    case 'a': opts.show_all     = 1; break;
                    case 'g': opts.show_gps     = 1; break;
                    case 'c': opts.show_streams = 1; break;
                    case 's': opts.show_size    = 1; break;
                    case 'j': opts.show_json    = 1; break;
                    case 'v': opts.verbose      = 1; break;
                    default:
                        fprintf(stderr, C_RED "Unknown option:" C_RESET " -%c\n", arg[k]);
                        return EXIT_FAILURE;
                }
            }
        }
    }

    if (!filepath) {
        fprintf(stderr,
            C_RED "Error:" C_RESET " file path not specified.\n"
            "Example: " C_GREEN "vidinf -/path/to/video.mp4\n" C_RESET);
        return EXIT_FAILURE;
    }

    process_file(filepath, &opts);
    return EXIT_SUCCESS;
}
