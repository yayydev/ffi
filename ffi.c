#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <tchar.h>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#else
#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fnmatch.h>
#include <regex.h>
#include <errno.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0x01
#endif

typedef struct Task {
    char *path;
    struct Task *next;
} Task;

typedef struct {
#ifdef _WIN32
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
#else
    pthread_mutex_t m;
    pthread_cond_t cv;
#endif
    Task *head;
    Task *tail;
    int closed;
} TaskQueue;

static TaskQueue queue;
static volatile long long visited_cnt = 0;
static volatile long long found_cnt = 0;
static int max_depth = -1;
static int ignore_case = 0;
static int follow_symlinks = 0;
static int show_progress = 0;
static int use_glob = 0;
static int use_regex = 0;
static char *pattern = NULL;
static char *start_path = NULL;
static int thread_count = 0;
static char **excludes = NULL;
static int exclude_count = 0;
#ifndef _WIN32
static regex_t compiled_rx;
static int rx_compiled = 0;
#endif

static char *xstrdup(const char *s){
#ifdef _WIN32
    return _strdup(s);
#else
    return strdup(s);
#endif
}

#ifdef _WIN32
#define atomic_inc64(ptr) InterlockedIncrement64((volatile LONG64*)(ptr))
#define atomic_get64(ptr) InterlockedCompareExchange64((volatile LONG64*)(ptr), 0, 0)
#else
#define atomic_inc64(ptr) __atomic_add_fetch(ptr, 1, __ATOMIC_RELAXED)
#define atomic_get64(ptr) ({ long long v; __atomic_load(ptr, &v, __ATOMIC_RELAXED); v; })
#endif

static void tq_init(TaskQueue *q){
    q->head = q->tail = NULL;
    q->closed = 0;
#ifdef _WIN32
    InitializeCriticalSection(&q->cs);
    InitializeConditionVariable(&q->cv);
#else
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->cv, NULL);
#endif
}

static void tq_push(TaskQueue *q, const char *path){
    Task *t = malloc(sizeof(Task));
    if(!t) return;
    t->path = xstrdup(path);
    t->next = NULL;
#ifdef _WIN32
    EnterCriticalSection(&q->cs);
    if(q->tail) q->tail->next = t; else q->head = t;
    q->tail = t;
    WakeConditionVariable(&q->cv);
    LeaveCriticalSection(&q->cs);
#else
    pthread_mutex_lock(&q->m);
    if(q->tail) q->tail->next = t; else q->head = t;
    q->tail = t;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->m);
#endif
}

static char *tq_pop(TaskQueue *q){
#ifdef _WIN32
    EnterCriticalSection(&q->cs);
    while(q->head == NULL && !q->closed)
        SleepConditionVariableCS(&q->cv, &q->cs, INFINITE);
    if(!q->head){ LeaveCriticalSection(&q->cs); return NULL; }
    Task *t = q->head;
    q->head = t->next;
    if(!q->head) q->tail = NULL;
    LeaveCriticalSection(&q->cs);
#else
    pthread_mutex_lock(&q->m);
    while(q->head == NULL && !q->closed)
        pthread_cond_wait(&q->cv, &q->m);
    if(!q->head){ pthread_mutex_unlock(&q->m); return NULL; }
    Task *t = q->head;
    q->head = t->next;
    if(!q->head) q->tail = NULL;
    pthread_mutex_unlock(&q->m);
#endif
    char *p = t->path;
    free(t);
    return p;
}

static void tq_close(TaskQueue *q){
#ifdef _WIN32
    EnterCriticalSection(&q->cs);
    q->closed = 1;
    WakeAllConditionVariable(&q->cv);
    LeaveCriticalSection(&q->cs);
#else
    pthread_mutex_lock(&q->m);
    q->closed = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->m);
#endif
}

static int is_admin(){
#ifdef _WIN32
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuth = SECURITY_NT_AUTHORITY;
    if(AllocateAndInitializeSid(&NtAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0,
                                &adminGroup)){
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
#else
    return geteuid() == 0;
#endif
}

static int path_is_excluded(const char *p){
    for(int i=0;i<exclude_count;i++){
        size_t L = strlen(excludes[i]);
        if(L && strncmp(p, excludes[i], L)==0) return 1;
    }
    return 0;
}

#ifndef _WIN32
static int name_matches(const char *name){
    if(use_regex && rx_compiled)
        return regexec(&compiled_rx, name, 0, NULL, 0) == 0;
    if(use_glob)
        return fnmatch(pattern, name, ignore_case ? FNM_CASEFOLD : 0) == 0;
    if(ignore_case){
        for(size_t i=0; name[i] && pattern[i]; i++)
            if(tolower((unsigned char)name[i]) != tolower((unsigned char)pattern[i]))
                return 0;
        return name[strlen(pattern)] == 0;
    }
    return strcmp(name, pattern) == 0;
}
#else
static int name_matches(const char *name){
    if(use_glob){
        int flags = ignore_case ? FNM_CASEFOLD : 0;
        return PathMatchSpecExA(name, pattern, flags) == S_OK;
    }
    if(ignore_case)
        return _stricmp(name, pattern) == 0;
    return strcmp(name, pattern) == 0;
}
#endif

#ifndef _WIN32
static void process_dir(const char *dirpath){
    DIR *d = opendir(dirpath);
    if(!d) return;
    struct dirent *e;
    char full[PATH_MAX];
    struct stat st;
    while((e = readdir(d))){
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        if(snprintf(full, sizeof(full), "%s/%s", dirpath, e->d_name) >= (int)sizeof(full)) continue;
        if(path_is_excluded(full)) continue;
        atomic_inc64(&visited_cnt);
        if((follow_symlinks ? stat(full,&st) : lstat(full,&st)) != 0) continue;
        if(name_matches(e->d_name)){
            atomic_inc64(&found_cnt);
            printf("%s\n", full);
        }
        if(S_ISDIR(st.st_mode)) tq_push(&queue, full);
    }
    closedir(d);
}
#else
static void process_dir(const char *dirpath){
    char pat[MAX_PATH];
    size_t len = strlen(dirpath);
    if(len + 3 >= sizeof(pat)) return;
    snprintf(pat, sizeof(pat), "%s\\*", dirpath);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if(h == INVALID_HANDLE_VALUE) return;
    do {
        if(strcmp(fd.cFileName,".")==0 || strcmp(fd.cFileName,"..")==0) continue;
        char full[MAX_PATH];
        if(len + strlen(fd.cFileName) + 2 >= sizeof(full)) continue;
        snprintf(full, sizeof(full), "%s\\%s", dirpath, fd.cFileName);
        if(path_is_excluded(full)) continue;
        atomic_inc64(&visited_cnt);
        if(name_matches(fd.cFileName)){
            atomic_inc64(&found_cnt);
            printf("%s\n", full);
        }
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            tq_push(&queue, full);
    } while(FindNextFileA(h, &fd));
    FindClose(h);
}
#endif

#ifdef _WIN32
DWORD WINAPI worker(LPVOID a){
    (void)a;
    while(1){
        char *p = tq_pop(&queue);
        if(!p) break;
        process_dir(p);
        free(p);
    }
    return 0;
}
#else
void *worker(void *a){
    (void)a;
    while(1){
        char *p = tq_pop(&queue);
        if(!p) break;
        process_dir(p);
        free(p);
    }
    return NULL;
}
#endif

static void usage(const char *n){
    fprintf(stderr,"Usage: %s -fn <name> [-p <path>] [-t threads] [--glob] [--regex] [--ignore-case] [--progress]\n",n);
}

int main(int argc, char **argv){
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si);
    thread_count = si.dwNumberOfProcessors;
#else
    long nc = sysconf(_SC_NPROCESSORS_ONLN);
    thread_count = (nc > 0) ? (int)nc : 4;
#endif
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-p") && i+1<argc) start_path = argv[++i];
        else if(!strcmp(argv[i],"-fn") && i+1<argc) pattern = argv[++i];
        else if(!strcmp(argv[i],"--ignore-case")) ignore_case=1;
        else if(!strcmp(argv[i],"--glob")) use_glob=1;
        else if(!strcmp(argv[i],"--regex")) use_regex=1;
        else if(!strcmp(argv[i],"--progress")) show_progress=1;
        else if(!strcmp(argv[i],"-t") && i+1<argc) thread_count = atoi(argv[++i]);
    }
    if(!pattern){ usage(argv[0]); return 1; }
    if(!start_path){
#ifdef _WIN32
        start_path = "C:\\";
#else
        start_path = "/";
#endif
    }
    if(!is_admin()){ fprintf(stderr,"Run as Administrator/root\n"); return 1; }
#ifndef _WIN32
    if(use_regex){
        int flags = REG_EXTENDED | (ignore_case ? REG_ICASE : 0);
        if(regcomp(&compiled_rx, pattern, flags)){ fprintf(stderr,"Invalid regex\n"); return 1; }
        rx_compiled = 1;
    }
#endif
    tq_init(&queue);
    tq_push(&queue,start_path);
#ifdef _WIN32
    HANDLE *th = malloc(sizeof(HANDLE)*thread_count);
    for(int i=0;i<thread_count;i++) th[i]=CreateThread(NULL,0,worker,NULL,0,NULL);
    Sleep(100);
    tq_close(&queue);
    WaitForMultipleObjects(thread_count,th,TRUE,INFINITE);
    for(int i=0;i<thread_count;i++) CloseHandle(th[i]);
#else
    pthread_t *th = malloc(sizeof(pthread_t)*thread_count);
    for(int i=0;i<thread_count;i++) pthread_create(&th[i],NULL,worker,NULL);
    sleep(100000);
    tq_close(&queue);
    for(int i=0;i<thread_count;i++) pthread_join(th[i],NULL);
#endif
    printf("\nDone. visited=%lld found=%lld\n", atomic_get64(&visited_cnt), atomic_get64(&found_cnt));
    return 0;
}
