#include "common.h"

#define MAX_INTERRUPT_NAME 50

struct interrupt {
    int used;
    char *id;
    char name[MAX_INTERRUPT_NAME + 1];
    unsigned long long total;
    unsigned long long value[];
};

// since each interrupt is variable in size
// we use this to calculate its record size
#define recordsize(cpus) (sizeof(struct interrupt) + (cpus * sizeof(unsigned long long)))

// given a base, get a pointer to each record
#define irrindex(base, line, cpus) ((struct interrupt *)&((char *)(base))[line * recordsize(cpus)])

static inline struct interrupt *get_interrupts_array(int lines, int cpus) {
    static struct interrupt *irrs = NULL;
    static int allocated = 0;

    if(unlikely(lines > allocated)) {
        irrs = (struct interrupt *)reallocz(irrs, lines * recordsize(cpus));
        allocated = lines;
    }

    return irrs;
}

int do_proc_interrupts(int update_every, unsigned long long dt) {
    (void)dt;

    static procfile *ff = NULL;
    static int cpus = -1, do_per_core = -1;
    struct interrupt *irrs = NULL;

    if(unlikely(do_per_core == -1))
        do_per_core = config_get_boolean("plugin:proc:/proc/interrupts", "interrupts per core", 1);

    if(unlikely(!ff)) {
        char filename[FILENAME_MAX + 1];
        snprintfz(filename, FILENAME_MAX, "%s%s", global_host_prefix, "/proc/interrupts");
        ff = procfile_open(config_get("plugin:proc:/proc/interrupts", "filename to monitor", filename), " \t", PROCFILE_FLAG_DEFAULT);
    }
    if(unlikely(!ff))
        return 1;

    ff = procfile_readall(ff);
    if(unlikely(!ff))
        return 0; // we return 0, so that we will retry to open it next time

    uint32_t lines = procfile_lines(ff), l;
    uint32_t words = procfile_linewords(ff, 0), w;

    if(unlikely(!lines)) {
        error("Cannot read /proc/interrupts, zero lines reported.");
        return 1;
    }

    // find how many CPUs are there
    if(unlikely(cpus == -1)) {
        cpus = 0;
        for(w = 0; w < words ; w++) {
            if(unlikely(strncmp(procfile_lineword(ff, 0, w), "CPU", 3) == 0))
                cpus++;
        }
    }

    if(unlikely(!cpus)) {
        error("PLUGIN: PROC_INTERRUPTS: Cannot find the number of CPUs in /proc/interrupts");
        return 1;
    }

    // allocate the size we need;
    irrs = get_interrupts_array(lines, cpus);
    irrs[0].used = 0;

    // loop through all lines
    for(l = 1; l < lines ;l++) {
        struct interrupt *irr = irrindex(irrs, l, cpus);
        irr->used = 0;
        irr->total = 0;

        words = procfile_linewords(ff, l);
        if(unlikely(!words)) continue;

        irr->id = procfile_lineword(ff, l, 0);
        if(unlikely(!irr->id || !irr->id[0])) continue;

        int idlen = strlen(irr->id);
        if(unlikely(irr->id[idlen - 1] == ':'))
            irr->id[idlen - 1] = '\0';

        int c;
        for(c = 0; c < cpus ;c++) {
            if(unlikely((c + 1) < (int)words))
                irr->value[c] = strtoull(procfile_lineword(ff, l, (uint32_t)(c + 1)), NULL, 10);
            else
                irr->value[c] = 0;

            irr->total += irr->value[c];
        }

        if(unlikely(isdigit(irr->id[0]) && (uint32_t)(cpus + 2) < words)) {
            strncpyz(irr->name, procfile_lineword(ff, l, words - 1), MAX_INTERRUPT_NAME);
            int nlen = strlen(irr->name);
            if(unlikely(nlen < (MAX_INTERRUPT_NAME-1))) {
                irr->name[nlen] = '_';
                strncpyz(&irr->name[nlen + 1], irr->id, MAX_INTERRUPT_NAME - nlen);
            }
        }
        else {
            strncpyz(irr->name, irr->id, MAX_INTERRUPT_NAME);
        }

        irr->used = 1;
    }

    RRDSET *st;

    // --------------------------------------------------------------------

    st = rrdset_find_bytype("system", "interrupts");
    if(unlikely(!st)) {
        st = rrdset_create("system", "interrupts", NULL, "interrupts", NULL, "System interrupts", "interrupts/s", 1000, update_every, RRDSET_TYPE_STACKED);

        for(l = 0; l < lines ;l++) {
            struct interrupt *irr = irrindex(irrs, l, cpus);
            if(unlikely(!irr->used)) continue;
            rrddim_add(st, irr->id, irr->name, 1, 1, RRDDIM_INCREMENTAL);
        }
    }
    else rrdset_next(st);

    for(l = 0; l < lines ;l++) {
        struct interrupt *irr = irrindex(irrs, l, cpus);
        if(unlikely(!irr->used)) continue;
        rrddim_set(st, irr->id, irr->total);
    }
    rrdset_done(st);

    if(likely(do_per_core)) {
        int c;

        for(c = 0; c < cpus ; c++) {
            char id[50+1];
            snprintfz(id, 50, "cpu%d_interrupts", c);

            st = rrdset_find_bytype("cpu", id);
            if(unlikely(!st)) {
                char title[100+1];
                snprintfz(title, 100, "CPU%d Interrupts", c);
                st = rrdset_create("cpu", id, NULL, "interrupts", "cpu.interrupts", title, "interrupts/s", 1100 + c, update_every, RRDSET_TYPE_STACKED);

                for(l = 0; l < lines ;l++) {
                    struct interrupt *irr = irrindex(irrs, l, cpus);
                    if(unlikely(!irr->used)) continue;
                    rrddim_add(st, irr->id, irr->name, 1, 1, RRDDIM_INCREMENTAL);
                }
            }
            else rrdset_next(st);

            for(l = 0; l < lines ;l++) {
                struct interrupt *irr = irrindex(irrs, l, cpus);
                if(unlikely(!irr->used)) continue;
                rrddim_set(st, irr->id, irr->value[c]);
            }
            rrdset_done(st);
        }
    }

    return 0;
}
