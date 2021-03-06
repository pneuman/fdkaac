#if HAVE_CONFIG_H
#  include "config.h"
#endif
#if HAVE_STDINT_H
#  include <stdint.h>
#endif
#if HAVE_INTTYPES_H
#  include <inttypes.h>
#elif defined(_MSC_VER)
#  define SCNd64 "I64d"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "m4af.h"
#include "metadata.h"
#include "compat.h"
#include "parson.h"

typedef struct tag_key_mapping_t {
    const char *name;
    uint32_t fcc;
} tag_key_mapping_t;

enum {
    TAG_TOTAL_DISCS  = 1,
    TAG_TOTAL_TRACKS = 2
};

static tag_key_mapping_t tag_mapping_table[] = {
    { "album",                      M4AF_TAG_ALBUM               },
    { "albumartist",                M4AF_TAG_ALBUM_ARTIST        },
    { "albumartistsort",            M4AF_FOURCC('s','o','a','a') },
    { "albumartistsortorder",       M4AF_FOURCC('s','o','a','a') },
    { "albumsort",                  M4AF_FOURCC('s','o','a','l') },
    { "albumsortorder",             M4AF_FOURCC('s','o','a','l') },
    { "artist",                     M4AF_TAG_ARTIST              },
    { "artistsort",                 M4AF_FOURCC('s','o','a','r') },
    { "artistsortorder",            M4AF_FOURCC('s','o','a','r') },
    { "band",                       M4AF_TAG_ALBUM_ARTIST        },
    { "bpm",                        M4AF_TAG_TEMPO               },
    { "comment",                    M4AF_TAG_COMMENT             },
    { "compilation",                M4AF_TAG_COMPILATION         },
    { "composer",                   M4AF_TAG_COMPOSER            },
    { "composersort",               M4AF_FOURCC('s','o','c','o') },
    { "composersortorder",          M4AF_FOURCC('s','o','c','o') },
    { "contentgroup",               M4AF_TAG_GROUPING            },
    { "copyright",                  M4AF_TAG_COPYRIGHT           },
    { "date",                       M4AF_TAG_DATE                },
    { "disc",                       M4AF_TAG_DISK                },
    { "disctotal",                  TAG_TOTAL_DISCS              },
    { "discnumber",                 M4AF_TAG_DISK                },
    { "genre",                      M4AF_TAG_GENRE               },
    { "grouping",                   M4AF_TAG_GROUPING            },
    { "itunescompilation",          M4AF_TAG_COMPILATION         },
    { "lyrics",                     M4AF_TAG_LYRICS              },
    { "title",                      M4AF_TAG_TITLE               },
    { "titlesort",                  M4AF_FOURCC('s','o','n','m') },
    { "titlesortorder",             M4AF_FOURCC('s','o','n','m') },
    { "totaldiscs",                 TAG_TOTAL_DISCS              },
    { "totaltracks",                TAG_TOTAL_TRACKS             },
    { "track",                      M4AF_TAG_TRACK               },
    { "tracknumber",                M4AF_TAG_TRACK               },
    { "tracktotal",                 TAG_TOTAL_TRACKS             },
    { "unsyncedlyrics",             M4AF_TAG_LYRICS              },
    { "year",                       M4AF_TAG_DATE                },
};

static
int tag_key_comparator(const void *k, const void *v)
{
    return strcmp((const char *)k, ((tag_key_mapping_t*)v)->name);
}

static
uint32_t get_tag_fcc_from_name(const char *name)
{
    char *name_p = 0, *p;
    const tag_key_mapping_t *ent;

    name_p = malloc(strlen(name) + 1);
    for (p = name_p; *name; ++name) {
        unsigned char c = *name;
        if (c != ' ' && c != '-' && c != '_')
            *p++ = tolower(c);
    }
    *p = 0;
    ent = bsearch(name_p, tag_mapping_table,
                  sizeof(tag_mapping_table) / sizeof(tag_mapping_table[0]),
                  sizeof(tag_mapping_table[0]),
                  tag_key_comparator);
    free(name_p);
    return ent ? ent->fcc : 0;
}

char *aacenc_load_tag_from_file(const char *path, uint32_t *data_size)
{
    FILE *fp = 0;
    char *data = 0;
    int64_t size;

    if ((fp = aacenc_fopen(path, "rb")) == NULL) {
        aacenc_fprintf(stderr, "WARNING: %s: %s\n", path, strerror(errno));
        goto END;
    }
    fseeko(fp, 0, SEEK_END);
    size = ftello(fp);
    if (size > 5*1024*1024) {
        aacenc_fprintf(stderr, "WARNING: %s: size too large\n", path);
        goto END;
    }
    fseeko(fp, 0, SEEK_SET);
    data = malloc(size + 1);
    if (data) fread(data, 1, size, fp);
    data[size] = 0;
    *data_size = (uint32_t)size;
END:
    if (fp) fclose(fp);
    return data;
}

void aacenc_param_add_itmf_entry(aacenc_tag_param_t *params, uint32_t tag,
                                 const char *key, const char *value,
                                 uint32_t size, int is_file_name)
{
    aacenc_tag_entry_t *entry;

    if (!is_file_name && !size)
        return;
    if (params->tag_count == params->tag_table_capacity) {
        unsigned newsize = params->tag_table_capacity;
        newsize = newsize ? newsize * 2 : 1;
        params->tag_table =
            realloc(params->tag_table, newsize * sizeof(aacenc_tag_entry_t));
        params->tag_table_capacity = newsize;
    }
    entry = params->tag_table + params->tag_count;
    entry->tag = tag;
    if (tag == M4AF_FOURCC('-','-','-','-'))
        entry->name = key;
    entry->data = value;
    entry->data_size = size;
    entry->is_file_name = is_file_name;
    params->tag_count++;
}

static
void tag_put_number_pair(m4af_ctx_t *m4af, uint32_t fcc,
                         const char *snumber, const char *stotal)
{
    unsigned number = 0, total = 0;
    char buf[128];
    aacenc_tag_entry_t entry = { 0 };

    if (snumber) sscanf(snumber, "%u", &number);
    if (stotal)  sscanf(stotal,  "%u", &total);
    if (number) {
        if (total) sprintf(buf, "%u/%u", number, total);
        else       sprintf(buf, "%u",    number);
        entry.tag = fcc;
        entry.data = buf;
        entry.data_size = strlen(buf);
        aacenc_put_tag_entry(m4af, &entry);
    }
}

static
const char *aacenc_json_object_get_string(JSON_Object *obj, const char *key,
                                          char *buf)
{
    JSON_Value_Type type;
    const char *val = 0;

    type = json_value_get_type(json_object_get_value(obj, key));
    if (type == JSONString)
        val = json_object_get_string(obj, key);
    else if (type == JSONNumber) {
        double num = json_object_get_number(obj, key);
        sprintf(buf, "%.15g", num);
        val = buf;
    } else if (type == JSONBoolean) {
        int n = json_object_get_boolean(obj, key);
        sprintf(buf, "%d", n);
        val = buf;
    }
    return val;
}

void aacenc_put_tags_from_json(m4af_ctx_t *m4af, const char *json_filename)
{
    char *data = 0;
    JSON_Value *json = 0;
    JSON_Object *root;
    size_t i, nelts;
    uint32_t data_size;
    char *json_dot_path;
    char *filename = 0;
    char *disc = 0;
    char *track = 0;
    char *total_discs = 0;
    char *total_tracks = 0;
    aacenc_tag_entry_t entry = { 0 };
    
    filename = strdup(json_filename);
    if ((json_dot_path = strchr(filename, '?')) != 0)
        *json_dot_path++ = '\0';

    if (!(data = aacenc_load_tag_from_file(filename, &data_size)))
        goto DONE;
    if (!(json = json_parse_string(data))) {
        aacenc_fprintf(stderr, "WARNING: failed to parse JSON\n");
        goto DONE;
    }
    root = json_value_get_object(json);
    if (json_dot_path) {
        if (!(root = json_object_dotget_object(root, json_dot_path))) {
            aacenc_fprintf(stderr, "WARNING: %s not found in JSON\n",
                           json_dot_path);
            goto DONE;
        }
    }
    nelts = json_object_get_count(root);
    for (i = 0; i < nelts; ++i) {
        char buf[256];
        const char *key = json_object_get_name(root, i);
        const char *val = aacenc_json_object_get_string(root, key, buf);
        uint32_t fcc = get_tag_fcc_from_name(key);
        if (!val || !fcc)
            continue;

        switch (fcc) {
        case TAG_TOTAL_DISCS:
            total_discs = realloc(total_discs, strlen(val) + 1);
            strcpy(total_discs, val);
            break;
        case TAG_TOTAL_TRACKS:
            total_tracks = realloc(total_tracks, strlen(val) + 1);
            strcpy(total_tracks, val);
            break;
        case M4AF_TAG_DISK:
            disc = realloc(disc, strlen(val) + 1);
            strcpy(disc, val);
            break;
        case M4AF_TAG_TRACK:
            track = realloc(track, strlen(val) + 1);
            strcpy(track, val);
            break;
        default:
            {
                entry.tag = fcc;
                entry.data = val;
                entry.data_size = strlen(val);
                aacenc_put_tag_entry(m4af, &entry);
            }
        }
    }
    tag_put_number_pair(m4af, M4AF_TAG_TRACK, track, total_tracks);
    tag_put_number_pair(m4af, M4AF_TAG_DISK,  disc,  total_discs);
DONE:
    if (track) free(track);
    if (disc) free(disc);
    if (total_tracks) free(total_tracks);
    if (total_discs) free(total_discs);
    if (data) free(data);
    if (filename) free(filename);
    if (json) json_value_free(json);
}

void aacenc_put_tag_entry(m4af_ctx_t *m4af, const aacenc_tag_entry_t *tag)
{
    unsigned m, n = 0;
    const char *data = tag->data;
    uint32_t data_size = tag->data_size;
    char *file_contents = 0;

    if (tag->is_file_name) {
        data = file_contents = aacenc_load_tag_from_file(tag->data, &data_size);
        if (!data) return;
    }
    switch (tag->tag) {
    case M4AF_TAG_TRACK:
        if (sscanf(data, "%u/%u", &m, &n) >= 1)
            m4af_add_itmf_track_tag(m4af, m, n);
        break;
    case M4AF_TAG_DISK:
        if (sscanf(data, "%u/%u", &m, &n) >= 1)
            m4af_add_itmf_disk_tag(m4af, m, n);
        break;
    case M4AF_TAG_GENRE_ID3:
        if (sscanf(data, "%u", &n) == 1)
            m4af_add_itmf_genre_tag(m4af, n);
        break;
    case M4AF_TAG_TEMPO:
        if (sscanf(data, "%u", &n) == 1)
            m4af_add_itmf_int16_tag(m4af, tag->tag, n);
        break;
    case M4AF_TAG_COMPILATION:
    case M4AF_FOURCC('a','k','I','D'):
    case M4AF_FOURCC('h','d','v','d'):
    case M4AF_FOURCC('p','c','s','t'):
    case M4AF_FOURCC('p','g','a','p'):
    case M4AF_FOURCC('r','t','n','g'):
    case M4AF_FOURCC('s','t','i','k'):
        if (sscanf(data, "%u", &n) == 1)
            m4af_add_itmf_int8_tag(m4af, tag->tag, n);
        break;
    case M4AF_FOURCC('a','t','I','D'):
    case M4AF_FOURCC('c','m','I','D'):
    case M4AF_FOURCC('c','n','I','D'):
    case M4AF_FOURCC('g','e','I','D'):
    case M4AF_FOURCC('s','f','I','D'):
    case M4AF_FOURCC('t','v','s','n'):
    case M4AF_FOURCC('t','v','s','s'):
        if (sscanf(data, "%u", &n) == 1)
            m4af_add_itmf_int32_tag(m4af, tag->tag, n);
        break;
    case M4AF_FOURCC('p','l','I','D'):
        {
            int64_t qn;
            if (sscanf(data, "%" SCNd64, &qn) == 1)
                m4af_add_itmf_int64_tag(m4af, tag->tag, qn);
            break;
        }
    case M4AF_TAG_ARTWORK:
        {
            int data_type = 0;
            if (!memcmp(data, "GIF", 3))
                data_type = M4AF_GIF;
            else if (!memcmp(data, "\xff\xd8\xff", 3))
                data_type = M4AF_JPEG;
            else if (!memcmp(data, "\x89PNG", 4))
                data_type = M4AF_PNG;
            if (data_type)
                m4af_add_itmf_short_tag(m4af, tag->tag, data_type,
                                        data, data_size);
            break;
        }
    case M4AF_FOURCC('-','-','-','-'):
        {
            char *u8 = aacenc_to_utf8(data);
            m4af_add_itmf_long_tag(m4af, tag->name, u8);
            free(u8);
            break;
        }
    case M4AF_TAG_TITLE:
    case M4AF_TAG_ARTIST:
    case M4AF_TAG_ALBUM:
    case M4AF_TAG_GENRE:
    case M4AF_TAG_DATE:
    case M4AF_TAG_COMPOSER:
    case M4AF_TAG_GROUPING:
    case M4AF_TAG_COMMENT:
    case M4AF_TAG_LYRICS:
    case M4AF_TAG_TOOL:
    case M4AF_TAG_ALBUM_ARTIST:
    case M4AF_TAG_DESCRIPTION:
    case M4AF_TAG_LONG_DESCRIPTION:
    case M4AF_TAG_COPYRIGHT:
    case M4AF_FOURCC('a','p','I','D'):
    case M4AF_FOURCC('c','a','t','g'):
    case M4AF_FOURCC('k','e','y','w'):
    case M4AF_FOURCC('p','u','r','d'):
    case M4AF_FOURCC('p','u','r','l'):
    case M4AF_FOURCC('s','o','a','a'):
    case M4AF_FOURCC('s','o','a','l'):
    case M4AF_FOURCC('s','o','a','r'):
    case M4AF_FOURCC('s','o','c','o'):
    case M4AF_FOURCC('s','o','n','m'):
    case M4AF_FOURCC('s','o','s','n'):
    case M4AF_FOURCC('t','v','e','n'):
    case M4AF_FOURCC('t','v','n','n'):
    case M4AF_FOURCC('t','v','s','h'):
    case M4AF_FOURCC('x','i','d',' '):
    case M4AF_FOURCC('\xa9','e','n','c'):
    case M4AF_FOURCC('\xa9','s','t','3'):
        {
            char *u8 = aacenc_to_utf8(data);
            m4af_add_itmf_string_tag(m4af, tag->tag, u8);
            free(u8);
            break;
        }
    default:
        fprintf(stderr, "WARNING: unknown/unsupported tag: %c%c%c%c\n",
                tag->tag >> 24, (tag->tag >> 16) & 0xff,
                (tag->tag >> 8) & 0xff, tag->tag & 0xff);
    }
    if (file_contents) free(file_contents);
}


