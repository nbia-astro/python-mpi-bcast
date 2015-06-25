/* python-mpi with package broad-casting */
/* Author: Yu Feng */
/* Adapted from the origina python-mpi.c by Lisandro Dalcin   */
/* Contact: rainwoodman@gmail.com */

/* -------------------------------------------------------------------------- */

#include <Python.h>

#define MPICH_IGNORE_CXX_SEEK 1
#define OMPI_IGNORE_CXX_SEEK 1
#include <mpi.h>

#include <unistd.h> /**/

#ifdef __FreeBSD__
#include <floatingpoint.h>
#endif

static int PyMPI_Main(int, char **);

#if PY_MAJOR_VERSION >= 3
static int Py3_Main(int, char **);
#endif
static int VERBOSE = 0;
/* -------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
#ifdef __FreeBSD__
  fp_except_t m;
  m = fpgetmask();
  fpsetmask(m & ~FP_X_OFL);
#endif
  return PyMPI_Main(argc, argv);
}

char ** list_packages(int * npackages) {
    char ** PACKAGES = NULL;
    int NPACKAGES = 0;
    int i;

    char * PYTHON_MPI_PACKAGES = getenv("PYTHON_MPI_PACKAGES");

    if(PYTHON_MPI_PACKAGES && strlen(PYTHON_MPI_PACKAGES) > 0) {
        NPACKAGES = 1;
    }
    for(i = 0; PYTHON_MPI_PACKAGES[i]; i ++) {
        if(PYTHON_MPI_PACKAGES[i] == ':') NPACKAGES ++;
    }

    PACKAGES = (char**) malloc(sizeof(char*) * (NPACKAGES + 1));
    char * p;
    i = 0;
    for(p = strtok(PYTHON_MPI_PACKAGES, ":");
        p;
        p = strtok(NULL, ":")) {
        PACKAGES[i] = strdup(p);
        i ++;
    }
    
    * npackages = i;
    PACKAGES[i] = NULL;
    return PACKAGES;
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
    //        printf("%d :%s:%d\n", i, buffer + i * ml, nid[i]);
        }
    }
    for(i = 0; i < NTask; i ++) {
        if(!strcmp(hostname, buffer + i * ml)) {
            break;
        }
    }
    free(buffer);
    free(nid);
    MPI_Barrier(MPI_COMM_WORLD);
    return nid[i];
}
static int bcast_packages(char ** PACKAGES, int NPACKAGES, char * chroot, char * pkgdir) {
    int i;
    int nid = getnid();

    int ThisTask = 0;
    int NodeRank = -1;

    MPI_Comm_rank(MPI_COMM_WORLD, &ThisTask);

    MPI_Comm NODE_GROUPS;
    MPI_Comm NODE_LEADERS;

    /* First split into ranks on the same node */
    MPI_Comm_split(MPI_COMM_WORLD, nid, ThisTask, &NODE_GROUPS);

    MPI_Comm_rank(NODE_GROUPS, &NodeRank);

    /* Next split by Node Rank */
    MPI_Comm_split(MPI_COMM_WORLD, NodeRank, ThisTask, &NODE_LEADERS);

    /* now bcast packages to PYTHON_MPI_CHROOT */

    if(NodeRank == 0) {
        if(ThisTask == 0) {
            if(VERBOSE) {
                printf("%d Packages\n", NPACKAGES);
                printf("tmpdir:%s\n", chroot);
                printf("PYTHON_MPI_PKGROOT:%s\n", pkgdir);
            }
        }

        if(VERBOSE)
            printf("node nid:%d\n", nid);

        for(i = 0; PACKAGES[i] != NULL; i ++) {
            long fsize;
            char *fcontent;
            char * dest = alloca(strlen(chroot) + 100);
            char * src = alloca(strlen(pkgdir) + 100);
            sprintf(dest, "%s/_thispackage.tar.gz",  chroot, ThisTask);
            sprintf(src, "%s/%s",  pkgdir, PACKAGES[i]);

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
                    printf("operating %s: %ld bytes\n", PACKAGES[i], fsize);
            } else {
                MPI_Bcast(&fsize, 1, MPI_LONG, 0, NODE_LEADERS);
                fcontent = malloc(fsize + 1);
                MPI_Bcast(fcontent, fsize, MPI_BYTE, 0, NODE_LEADERS);
            }
            
            MPI_Barrier(NODE_LEADERS);
            FILE * fp = fopen(dest, "w");
            fwrite(fcontent, 1, fsize, fp);
            fclose(fp);
            free(fcontent);
            
            char * untar = alloca(strlen(dest) + strlen(chroot) + 100);
            sprintf(untar, "tar --overwrite -xzf \"%s\" -C \"%s\"", dest, chroot);
            system(untar);
            unlink(dest);

            MPI_Barrier(NODE_LEADERS);
        }
        char * chmod = alloca(strlen(chroot) + 100);
        sprintf(chmod, "chmod -fR 777 \"%s\"", chroot);
        system(chmod);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if(ThisTask == 0) {
        if(VERBOSE)
            printf("Python packages delivered. Launching python interpreter.\n");
    }
}
static int
PyMPI_Main(int argc, char **argv)
{
  int sts=0, flag=1, finalize=0;

  /* MPI initalization */
  (void)MPI_Initialized(&flag);
  if (!flag) {
#if defined(MPI_VERSION) && (MPI_VERSION > 1)
    int required = MPI_THREAD_MULTIPLE;
    int provided = MPI_THREAD_SINGLE;
    (void)MPI_Init_thread(&argc, &argv, required, &provided);
#else
    (void)MPI_Init(&argc, &argv);
#endif
    finalize = 1;
  }

  char * PYTHON_MPI_VERBOSE = getenv("PYTHON_MPI_VERBOSE");
  if(PYTHON_MPI_VERBOSE && atoi(PYTHON_MPI_VERBOSE)) {
    VERBOSE = 1;
  }

  char * PYTHON_MPI_PKGROOT= getenv("PYTHON_MPI_PKGROOT");
  if(PYTHON_MPI_PKGROOT == NULL) {
      fprintf(stderr, "PYTHON_MPI_PKGROOT must be set to a location to look for .tar.gz files.\n");
      MPI_Abort(MPI_COMM_WORLD, 1);
  }

  char * PYTHON_MPI_CHROOT = getenv("PYTHON_MPI_CHROOT");
  if (PYTHON_MPI_CHROOT == NULL) {
      fprintf(stderr, "PYTHON_MPI_CHROOT must be set to a writable location, for example /dev/shm/\n");
      MPI_Abort(MPI_COMM_WORLD, 1);
  }

  char * PYTHON_MPI_CHROOT_TMP = alloca(strlen(PYTHON_MPI_CHROOT) + 100);
  sprintf(PYTHON_MPI_CHROOT_TMP, "%s/XXXXXX", PYTHON_MPI_CHROOT);
  int ThisTask;
  MPI_Comm_rank(MPI_COMM_WORLD, &ThisTask);
  if(ThisTask == 0) {
      PYTHON_MPI_CHROOT_TMP = mkdtemp(PYTHON_MPI_CHROOT_TMP);
  }
  MPI_Bcast(PYTHON_MPI_CHROOT_TMP, strlen(PYTHON_MPI_CHROOT) + 99, MPI_BYTE, 0, MPI_COMM_WORLD);

  int npackages;
  char ** packages = list_packages(&npackages);
  bcast_packages(packages, npackages, PYTHON_MPI_CHROOT_TMP, PYTHON_MPI_PKGROOT);

  /* completely ignore PYTHONPATH for now */
  setenv("PYTHONUSERBASE", PYTHON_MPI_CHROOT_TMP, 1);
  setenv("PYTHONHOME", PYTHON_MPI_CHROOT_TMP, 1);
  char * buf = malloc(strlen(PYTHON_MPI_CHROOT_TMP) + 100);
  sprintf(buf, "%s/lib/python", PYTHON_MPI_CHROOT_TMP);
  setenv("PYTHONPATH", buf, 1);

  /* Python main */
#if PY_MAJOR_VERSION >= 3
  sts = Py3_Main(argc, argv);
#else
  sts = Py_Main(argc, argv);
#endif

  char * cleanup = alloca(strlen(PYTHON_MPI_CHROOT_TMP) + 100);
  sprintf(cleanup, "rm -rf \"%s\"", PYTHON_MPI_CHROOT_TMP);
  system(cleanup);

  if (sts != 0) (void)MPI_Abort(MPI_COMM_WORLD, sts);

  /* MPI finalization */
  (void)MPI_Finalized(&flag);

  if (!flag) {
    if (sts != 0) (void)MPI_Abort(MPI_COMM_WORLD, sts);
    if (finalize) (void)MPI_Finalize();
  }

  return sts;
}

/* -------------------------------------------------------------------------- */

#if PY_MAJOR_VERSION >= 3

#include <locale.h>

static wchar_t **mk_wargs(int, char **);
static wchar_t **cp_wargs(int, wchar_t **);
static void rm_wargs(wchar_t **, int);

static int
Py3_Main(int argc, char **argv)
{
  int sts = 0;
  wchar_t **wargv  = mk_wargs(argc, argv);
  wchar_t **wargv2 = cp_wargs(argc, wargv);
  if (wargv && wargv2)
    sts = Py_Main(argc, wargv);
  else
    sts = 1;
  rm_wargs(wargv2, 1);
  rm_wargs(wargv,  0);
  return sts;
}

#if PY_VERSION_HEX < 0x03050000
#define Py_DecodeLocale _Py_char2wchar
#endif

#if PY_VERSION_HEX < 0x03040000
#define PyMem_RawMalloc PyMem_Malloc
#define PyMem_RawFree   PyMem_Free
#endif

static wchar_t **
mk_wargs(int argc, char **argv)
{
  int i; char *saved_locale = NULL;
  wchar_t **args = NULL;

  args = (wchar_t **)PyMem_RawMalloc((size_t)(argc+1)*sizeof(wchar_t *));
  if (!args) goto oom;

  saved_locale = strdup(setlocale(LC_ALL, NULL));
  if (!saved_locale) goto oom;
  setlocale(LC_ALL, "");

  for (i=0; i<argc; i++) {
    args[i] = Py_DecodeLocale(argv[i], NULL);
    if (!args[i]) goto oom;
  }
  args[argc] = NULL;

  setlocale(LC_ALL, saved_locale);
  free(saved_locale);

  return args;

 oom:
  fprintf(stderr, "out of memory\n");
  if (saved_locale) {
    setlocale(LC_ALL, saved_locale);
    free(saved_locale);
  }
  if (args)
    rm_wargs(args, 1);
  return NULL;
}

static wchar_t **
cp_wargs(int argc, wchar_t **args)
{
  int i; wchar_t **args_copy = NULL;
  if (!args) return NULL;
  args_copy = (wchar_t **)PyMem_RawMalloc((size_t)(argc+1)*sizeof(wchar_t *));
  if (!args_copy) goto oom;
  for (i=0; i<(argc+1); i++) { args_copy[i] = args[i]; }
  return args_copy;
 oom:
  fprintf(stderr, "out of memory\n");
  return NULL;
}

static void
rm_wargs(wchar_t **args, int deep)
{
  int i = 0;
  if (args && deep)
    while (args[i])
      PyMem_RawFree(args[i++]);
  if (args)
    PyMem_RawFree(args);
}

#endif /* !(PY_MAJOR_VERSION >= 3) */

/* -------------------------------------------------------------------------- */

/*
   Local variables:
   c-basic-offset: 2
   indent-tabs-mode: nil
   End:
*/
