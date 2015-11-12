/* broadcasting pre-built packages to computing nodes */
/* Author: Yu Feng */
/* Contact: rainwoodman@gmail.com */

/* -------------------------------------------------------------------------- */

#define MPICH_IGNORE_CXX_SEEK 1
#define OMPI_IGNORE_CXX_SEEK 1
#include <mpi.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> 

static int VERBOSE = 0;

struct fnlist {
    char * path;
    struct fnlist * next;
} fnlist;

static void 
_mkdir(const char *dir);

static char *
basename(const char * path);

static void 
fix_permissions(char * PREFIX);

static int getnid();

static int ThisTask = 0;
static int NodeRank = -1;
MPI_Comm NODE_GROUPS;
MPI_Comm NODE_LEADERS;

static void initialize(int nid) {
    MPI_Comm_rank(MPI_COMM_WORLD, &ThisTask);

    /* First split into ranks on the same node */
    MPI_Comm_split(MPI_COMM_WORLD, nid, ThisTask, &NODE_GROUPS);

    MPI_Comm_rank(NODE_GROUPS, &NodeRank);

    /* Next split by Node Rank */
    MPI_Comm_split(MPI_COMM_WORLD, NodeRank, ThisTask, &NODE_LEADERS);

}

static void bcast(char * src, char * PREFIX) {
    if(NodeRank != 0) {
        MPI_Barrier(MPI_COMM_WORLD);
        return;
    }

    long fsize;
    char *fcontent;
    char * dest = alloca(strlen(PREFIX) + 100);
    char * filename = basename(src);

    sprintf(dest, "%s/%s",  PREFIX, filename, ThisTask);

    free(filename);

    if(ThisTask == 0) {
        FILE * fp = fopen(src, "r");
        if(fp == NULL) {
            fprintf(stderr, "package file %s not found\n", src);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        fseek(fp, 0, SEEK_END);
        fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        fcontent = malloc(fsize + 1);
        fread(fcontent, 1, fsize, fp);
        fclose(fp);
        MPI_Bcast(&fsize, 1, MPI_LONG, 0, NODE_LEADERS);
        MPI_Bcast(fcontent, fsize, MPI_BYTE, 0, NODE_LEADERS);
        if(VERBOSE)
            printf("Bcasting %s: %ld bytes\n", src, fsize);
    } else {
        MPI_Bcast(&fsize, 1, MPI_LONG, 0, NODE_LEADERS);
        fcontent = malloc(fsize + 1);
        MPI_Bcast(fcontent, fsize, MPI_BYTE, 0, NODE_LEADERS);
    }
    
    MPI_Barrier(NODE_LEADERS);
    FILE * fp = fopen(dest, "w");
    if(fp == NULL) {
        fprintf(stderr, "Cannot write to %s\n", dest);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    fwrite(fcontent, 1, fsize, fp);
    fclose(fp);
    free(fcontent);
    
    char * untar = alloca(strlen(dest) + strlen(PREFIX) + 100);
    if(!strcmp(dest + strlen(dest) - 3, "bz2")) {
        sprintf(untar, "tar --overwrite -xjf \"%s\" -C \"%s\"", dest, PREFIX);
    } else
    if(!strcmp(dest + strlen(dest) - 2, "gz")) {
        sprintf(untar, "tar --overwrite -xzf \"%s\" -C \"%s\"", dest, PREFIX);
    } else {
        sprintf(untar, "cp \"%s\" \"%s\"", dest, PREFIX);
    }

    if(VERBOSE)
        printf("Running command: %s\n", untar);

    system(untar);
    unlink(dest);

    MPI_Barrier(NODE_LEADERS);

    MPI_Barrier(MPI_COMM_WORLD);

    if(ThisTask == 0) {
        if(VERBOSE)
            printf("Packages delivered. \n");
    }
}

static void 
process_file(char * filename, char * PREFIX) 
{
    char path[1024];
    int nc = 0;
    if(ThisTask == 0) {
        FILE * fp = fopen(filename, "r");
        if(fp == NULL) {
            fprintf(stderr, "failed to open package list %s.\n", filename);
            MPI_Abort(0, MPI_COMM_WORLD);
        }
        while(!feof(fp)) {
            fgets(path, 1020, fp);
            nc = strlen(path);
            char * p = path + nc;
            for(p = path + nc - 1; p >= path; p --) {
                if(isspace(*p)) *p = 0;
                else break;
            }
            for(p = path; *p; p ++) {
                if(!isspace(*p)) break;
            }
            nc = strlen(p) + 1;
            if(nc == 1) continue;
            if(p[0] == '#') continue;
            /* fix \n */
            MPI_Bcast(&nc, 1, MPI_INT, 0, MPI_COMM_WORLD);
            MPI_Bcast(p, nc, MPI_BYTE, 0, MPI_COMM_WORLD);
            bcast(p, PREFIX);
        }
        fclose(fp);
        nc = 0;
        MPI_Bcast(&nc, 1, MPI_INT, 0, MPI_COMM_WORLD);
    } else {
        while(1) {
            MPI_Bcast(&nc, 1, MPI_INT, 0, MPI_COMM_WORLD);
            if(nc == 0) break;
            MPI_Bcast(path, nc, MPI_BYTE, 0, MPI_COMM_WORLD);
            bcast(path, PREFIX);
        }
    }
}

int
main(int argc, char **argv)
{
    /* first allow everyone to purge files created by me */
    umask(0);

    MPI_Init(&argc, &argv);

    int nid = getnid();
    initialize(nid);

    int ch;
    extern char * optarg;
    extern int optind;     
    char * PREFIX = "/dev/shm/python";
    fnlist.next = NULL; 

    while((ch = getopt(argc, argv, "vf:p:")) != -1) {
        switch(ch) {
            case 'v':
                VERBOSE = 1;
                break;
            case 'p':
                PREFIX = optarg;
                break;
            case 'f':
                {
                    struct fnlist * p = malloc(sizeof(fnlist));
                    p->next = fnlist.next;
                    fnlist.next = p;
                    p->path = strdup(optarg);
                }
                break;
            case '?':
                if(ThisTask == 0) {
                    fprintf(stderr, "usage: bcast [-v] [-f filelist] [-p /dev/shm/python] [packages ...]\n");
                }
                goto quit;
        }
    }

    if(ThisTask == 0) {
        if(VERBOSE) {
            printf("tmpdir:%s\n", PREFIX);
        }
    }

    if(NodeRank == 0) {
        _mkdir(PREFIX);
    }

    MPI_Barrier(MPI_COMM_WORLD);

    struct fnlist * p;
    for(p = fnlist.next; p ; p = p->next) {
        process_file(p->path, PREFIX);
    }
    int i;
    for(i = optind; i < argc; i ++) {
        bcast(argv[i], PREFIX);
    }

    fix_permissions(PREFIX);
quit:
    MPI_Barrier(MPI_COMM_WORLD);

    MPI_Finalize();
    return 0;
}

static void 
_mkdir(const char *dir) 
{
        char * tmp = strdup(dir);
        char *p = NULL;
        size_t len;

        len = strlen(tmp);
        if(tmp[len - 1] == '/')
                tmp[len - 1] = 0;
        for(p = tmp + 1; *p; p++)
                if(*p == '/') {
                        *p = 0;
                        mkdir(tmp, 0777);
                        *p = '/';
                }
        mkdir(tmp, 0777);
        free(tmp);
}

static char *
basename(const char * path) {
    const char * p =  path + strlen(path);
    while(p >= path) {
        if(*p == '/') {
            break;
        }
        p--;
    }
    return strdup(p+1);
}

static int getnid() {
    char hostname[1024];
    int i;
    gethostname(hostname, 1024);

    MPI_Barrier(MPI_COMM_WORLD);

    int l = strlen(hostname) + 4;
    int ml = 0;
    int NTask;
    int ThisTask;
    char * buffer;
    int * nid;
    MPI_Comm_size(MPI_COMM_WORLD, &NTask);
    MPI_Comm_rank(MPI_COMM_WORLD, &ThisTask);
    MPI_Allreduce(&l, &ml, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    
    buffer = malloc(ml * NTask);
    nid = malloc(sizeof(int) * NTask);
    MPI_Allgather(hostname, ml, MPI_BYTE, buffer, ml, MPI_BYTE, MPI_COMM_WORLD);

    qsort(buffer, NTask, ml, strcmp);
    
    nid[0] = 0;
    for(i = 1; i < NTask; i ++) {
        if(strcmp(buffer + i * ml, buffer + (i - 1) *ml)) {
            nid[i] = nid[i - 1] + 1;
        } else {
            nid[i] = nid[i - 1];
        }
    }
    if(ThisTask == 0) {
        for(i = 0; i < NTask; i ++) {
            //printf("%d :%s:%d\n", i, buffer + i * ml, nid[i]);
        }
    }
    for(i = 0; i < NTask; i ++) {
        if(!strcmp(hostname, buffer + i * ml)) {
            break;
        }
    }
    int rt = nid[i];
    free(buffer);
    free(nid);
    MPI_Barrier(MPI_COMM_WORLD);
    return rt;
}

static void 
fix_permissions(char * PREFIX) 
{
    if(NodeRank == 0) {
        char * chmod = alloca(strlen(PREFIX) + 100);
        sprintf(chmod, "chmod -fR 777 \"%s\"", PREFIX);
        system(chmod);
    }
}