/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sound/asound.h>

static const char *errname(int e)
{
    switch (e) {
    case EINVAL: return "EINVAL";
    case ENODEV: return "ENODEV";
    case EBUSY: return "EBUSY";
    case EACCES: return "EACCES";
    case ENOENT: return "ENOENT";
    case EPIPE: return "EPIPE, xrun";
    case ENOTTY: return "ENOTTY, wrong node";
    case EIO: return "EIO";
    default: return strerror(e);
    }
}

static void params_any(struct snd_pcm_hw_params *p)
{
    int i;
    memset(p, 0, sizeof(*p));
    for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; i++)
        memset(&p->masks[i], 0xff, sizeof(p->masks[i]));
    for (i = 0;
         i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
         i++) {
        p->intervals[i].min = 0;
        p->intervals[i].max = UINT32_MAX;
    }
    p->rmask = ~0U;
    p->cmask = 0;
}

static void set_mask(struct snd_pcm_hw_params *p, int var, unsigned int val)
{
    memset(&p->masks[var], 0, sizeof(p->masks[var]));
    p->masks[var].bits[val >> 5] |= 1U << (val & 31);
}

static void set_int(struct snd_pcm_hw_params *p, int var, unsigned int min,
                    unsigned int max, int is_int)
{
    struct snd_interval *iv =
        &p->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    iv->min = min;
    iv->max = max;
    iv->integer = is_int ? 1 : 0;
    iv->empty = 0;
}

static void show_access(const struct snd_mask *m)
{
    static const char *names[] = {
        "MMAP_INTERLEAVED", "MMAP_NONINTERLEAVED", "MMAP_COMPLEX",
        "RW_INTERLEAVED", "RW_NONINTERLEAVED"
    };
    unsigned int b;
    printf("access:");
    for (b = 0; b < 32; b++)
        if ((m->bits[b >> 5] >> (b & 31)) & 1)
            printf(" %s", b < 5 ? names[b] : "?");
    printf("\n");
}

static void show_choice(struct snd_pcm_hw_params *p, int var)
{
    const struct snd_interval *iv =
        &p->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    unsigned int lo = iv->min, hi = iv->max;
    if (lo == hi)
        printf("%u", lo);
    else
        printf("%u-%u", lo, hi);
}

static int find_control(int fd, const char *name, struct snd_ctl_elem_info *info)
{
    struct snd_ctl_elem_list list;
    unsigned int i;

    memset(&list, 0, sizeof(list));
    list.space = 0;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
        return -1;
    for (i = 0; i < list.used; i++) {
        memset(info, 0, sizeof(*info));
        info->id.numid = i + 1;
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, info) < 0)
            continue;
        if (!strcmp((const char *)info->id.name, name))
            return 0;
    }
    return -1;
}

static int do_set(int fd, const char *name, char **vals, int nvals, int quiet)
{
    struct snd_ctl_elem_info info;
    struct snd_ctl_elem_value val;
    int i;

    if (find_control(fd, name, &info)) {
        if (!quiet) printf("no such control: %s\n", name);
        return -1;
    }
    memset(&val, 0, sizeof(val));
    val.id = info.id;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) {
        if (!quiet) printf("read %s: %s\n", name, errname(errno));
        return -1;
    }
    for (i = 0; i < nvals && i < (int)info.count; i++) {
        long v = strtol(vals[i], NULL, 0);
        if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
            val.value.enumerated.item[i] = (unsigned int)v;
        else if (info.type == SNDRV_CTL_ELEM_TYPE_BYTES)
            val.value.bytes.data[i] = (unsigned char)v;
        else
            val.value.integer.value[i] = v;
    }
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0) {
        if (!quiet) printf("write %s: %s\n", name, errname(errno));
        return -1;
    }
    if (!quiet) printf("%s <- %s\n", name, vals[0]);
    return 0;
}

struct wav {
    unsigned int rate, channels, bits;
    long data_off, data_len;
};

static int wav_read(const char *path, struct wav *w)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[12], chunk[8];
    int have_fmt = 0;

    if (!f) {
        printf("cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) ||
        memcmp(hdr + 8, "WAVE", 4)) {
        printf("%s is not a RIFF/WAVE file\n", path);
        fclose(f);
        return -1;
    }
    for (;;) {
        long len;
        if (fread(chunk, 1, 8, f) != 8) {
            printf("no data chunk in %s\n", path);
            fclose(f);
            return -1;
        }
        len = chunk[4] | (chunk[5] << 8) | (chunk[6] << 16) |
              ((long)chunk[7] << 24);
        if (!memcmp(chunk, "fmt ", 4)) {
            unsigned char fmt[40];
            size_t want = (size_t)len < sizeof(fmt) ? (size_t)len : sizeof(fmt);
            if (fread(fmt, 1, want, f) != want) {
                fclose(f);
                return -1;
            }
            w->channels = fmt[2] | (fmt[3] << 8);
            w->rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) |
                      ((unsigned)fmt[7] << 24);
            w->bits = fmt[14] | (fmt[15] << 8);
            have_fmt = 1;
            if ((long)want < len)
                fseek(f, len - (long)want, SEEK_CUR);
        } else if (!memcmp(chunk, "data", 4)) {
            w->data_off = ftell(f);
            w->data_len = len;
            fseek(f, 0, SEEK_END);
            if (w->data_len > ftell(f) - w->data_off)
                w->data_len = ftell(f) - w->data_off;
            break;
        } else {
            fseek(f, len + (len & 1), SEEK_CUR);
        }
    }
    fclose(f);
    if (!have_fmt) {
        printf("no fmt chunk in %s\n", path);
        return -1;
    }
    return 0;
}

static int cmd_play(const char *card, const char *dev, const char *path)
{
    struct wav w;
    char node[128];
    unsigned char *buf;
    struct snd_pcm_hw_params p;
    long done = 0;
    int fd, rc, tries;
    unsigned int fmt, frame_bytes, chunk;

    memset(&w, 0, sizeof(w));
    if (wav_read(path, &w))
        return 1;
    printf("wav: %u hz, %u ch, %u bit, %ld bytes\n", w.rate, w.channels,
           w.bits, w.data_len);

    fmt = SNDRV_PCM_FORMAT_S16_LE;
    if (w.bits == 8) fmt = SNDRV_PCM_FORMAT_U8;
    else if (w.bits == 24) fmt = SNDRV_PCM_FORMAT_S24_LE;
    else if (w.bits == 32) fmt = SNDRV_PCM_FORMAT_S32_LE;

    snprintf(node, sizeof(node), "/dev/snd/pcmC%sD%sp", card, dev);
    fd = open(node, O_WRONLY);
    if (fd < 0) {
        printf("open %s: %s\n", node, errname(errno));
        return 1;
    }
    printf("opened %s\n", node);

    for (tries = 0; tries < 2; tries++) {
        params_any(&p);
        set_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS,
                 SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        set_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, fmt);
        set_int(&p, SNDRV_PCM_HW_PARAM_CHANNELS, w.channels, w.channels, 1);
        set_int(&p, SNDRV_PCM_HW_PARAM_RATE, w.rate, w.rate, 1);
        if (tries == 0) {
            set_int(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 1024, 1024, 1);
            set_int(&p, SNDRV_PCM_HW_PARAM_PERIODS, 4, 4, 1);
        }
        rc = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p);
        if (rc == 0)
            break;
        printf("hw_params try %d: %s\n", tries + 1, errname(errno));
        if (errno != EINVAL)
            break;
    }
    if (rc != 0) {
        close(fd);
        return 1;
    }
    printf("hw params: rate ");
    show_choice(&p, SNDRV_PCM_HW_PARAM_RATE);
    printf(", ch ");
    show_choice(&p, SNDRV_PCM_HW_PARAM_CHANNELS);
    printf(", period ");
    show_choice(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    printf(", buffer ");
    show_choice(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
    printf("\n");
    show_access(&p.masks[SNDRV_PCM_HW_PARAM_ACCESS]);
    printf("info=0x%08x mmap=%d fifo=%lu\n", p.info,
           !!(p.info & SNDRV_PCM_INFO_MMAP), p.fifo_size);

    rc = ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
    if (rc != 0)
        printf("prepare: %s\n", errname(errno));

    buf = malloc((size_t)w.data_len);
    if (!buf) {
        close(fd);
        return 1;
    }
    {
        FILE *f = fopen(path, "rb");
        if (!f || fseek(f, w.data_off, SEEK_SET) != 0 ||
            fread(buf, 1, (size_t)w.data_len, f) != (size_t)w.data_len) {
            printf("cannot read pcm data\n");
            if (f) fclose(f);
            free(buf);
            close(fd);
            return 1;
        }
        fclose(f);
    }

    frame_bytes = (unsigned int)(p.intervals[SNDRV_PCM_HW_PARAM_CHANNELS -
                    SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min * (w.bits / 8));
    chunk = p.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE -
                        SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min * frame_bytes;
    if (chunk == 0)
        chunk = 4096;
    printf("chunk=%u bytes, %u frames\n",
           chunk, chunk / (frame_bytes ? frame_bytes : 1));

    while (done < w.data_len) {
        size_t want = (size_t)(w.data_len - done);
        ssize_t n;
        if (want > chunk)
            want = chunk;
        n = write(fd, buf + done, want);
        if (n < 0) {
            if (errno == EPIPE) {
                ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
                continue;
            }
            printf("write: %s after %ld bytes\n", errname(errno), done);
            if (done == 0) {
                printf("kernel log:\n");
                fflush(stdout);
                system("dmesg 2>/dev/null | tail -n 5");
            }
            break;
        }
        done += n;
    }
    printf("wrote %ld of %ld bytes\n", done, w.data_len);
    if (done > 0) {
        rc = ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
        printf("drain: %s\n", rc == 0 ? "ok" : errname(errno));
    }
    close(fd);
    free(buf);
    return done == w.data_len ? 0 : 1;
}

static int cmd_controls(const char *card)
{
    char node[128];
    struct snd_ctl_elem_list list;
    struct snd_ctl_elem_info info;
    struct snd_ctl_elem_value val;
    unsigned int i, k;
    int fd;

    snprintf(node, sizeof(node), "/dev/snd/controlC%s", card);
    fd = open(node, O_RDWR);
    if (fd < 0) {
        printf("open %s: %s\n", node, errname(errno));
        return 1;
    }
    memset(&list, 0, sizeof(list));
    list.space = 0;
    list.count = 0;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        printf("elem list: %s\n", errname(errno));
        close(fd);
        return 1;
    }
    printf("%u controls\n", list.used);
    for (i = 0; i < list.used; i++) {
        memset(&info, 0, sizeof(info));
        info.id.numid = i + 1;
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
            continue;
        memset(&val, 0, sizeof(val));
        val.id = info.id;
        ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val);
        printf("%3u %-44s ", info.id.numid, (const char *)info.id.name);
        switch (info.type) {
        case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
        case SNDRV_CTL_ELEM_TYPE_INTEGER:
            printf("=");
            for (k = 0; k < info.count && k < 8; k++)
                printf("%s%ld", k ? "," : "", val.value.integer.value[k]);
            break;
        case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
            printf("=");
            for (k = 0; k < info.count && k < 4; k++)
                printf("%s%u", k ? "," : "", val.value.enumerated.item[k]);
            break;
        default:
            printf("(type %u)", info.type);
            break;
        }
        printf("\n");
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s cards|pcms|controls <card>|get <card> <name>|"
               "set <card> <name> <value...>|play <card> <dev> <file.wav>\n",
               argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "cards")) {
        system("cat /proc/asound/cards 2>/dev/null");
        return 0;
    }
    if (!strcmp(argv[1], "pcms")) {
        system("ls -l /dev/snd/pcmC*p 2>/dev/null");
        return 0;
    }
    if (!strcmp(argv[1], "controls") && argc == 3)
        return cmd_controls(argv[2]);
    if (!strcmp(argv[1], "play") && argc == 5)
        return cmd_play(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "get") && argc == 4) {
        char node[128];
        struct snd_ctl_elem_info info;
        struct snd_ctl_elem_value val;
        int fd, i;
        snprintf(node, sizeof(node), "/dev/snd/controlC%s", argv[2]);
        fd = open(node, O_RDONLY);
        if (fd < 0) {
            printf("open %s: %s\n", node, errname(errno));
            return 1;
        }
        if (find_control(fd, argv[3], &info)) {
            printf("no such control: %s\n", argv[3]);
            close(fd);
            return 1;
        }
        memset(&val, 0, sizeof(val));
        val.id = info.id;
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) {
            printf("read: %s\n", errname(errno));
            close(fd);
            return 1;
        }
        printf("%s =", argv[3]);
        for (i = 0; i < (int)info.count && i < 8; i++)
            printf(" %ld", info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED
                               ? (long)val.value.enumerated.item[i]
                               : val.value.integer.value[i]);
        printf("\n");
        close(fd);
        return 0;
    }
    if (!strcmp(argv[1], "set") && argc >= 5) {
        char node[128];
        int fd, rc;
        snprintf(node, sizeof(node), "/dev/snd/controlC%s", argv[2]);
        fd = open(node, O_RDWR);
        if (fd < 0) {
            printf("open %s: %s\n", node, errname(errno));
            return 1;
        }
        rc = do_set(fd, argv[3], &argv[4], argc - 4, 0);
        close(fd);
        return rc ? 1 : 0;
    }
    printf("bad arguments\n");
    return 2;
}
