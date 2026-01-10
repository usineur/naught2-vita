/* main.c -- Naught 2 .so loader
 *
 * Copyright (C) 2025 Matthieu Milan
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>
#include <zlib.h>
#include <zip.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <setjmp.h>

#include <math.h>
#include <math_neon.h>

#include <netdb.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#include <openssl/ssl.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "so_util.h"
#include "splash.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>
#include <AL/efx.h>

#include "vorbis/vorbisfile.h"

//#define ENABLE_DEBUG

#ifdef ENABLE_DEBUG
#define dlog sceClibPrintf
#else
#define dlog(...)
#endif

static char data_path[17];

static char fake_vm[0x1000];
static char fake_env[0x1000];

int framecap = 0;

int file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

int _newlib_heap_size_user = 128 * 1024 * 1024;

so_module main_mod;

void *__wrap_memcpy(void *dest, const void *src, size_t n) {
	return sceClibMemcpy(dest, src, n);
}

void *__wrap_memmove(void *dest, const void *src, size_t n) {
	return sceClibMemmove(dest, src, n);
}

int ret4() { return 4; }

void *__wrap_memset(void *s, int c, size_t n) {
	return sceClibMemset(s, c, n);
}

char *getcwd_hook(char *buf, size_t size) {
	strcpy(buf, data_path);
	return buf;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
	*memptr = memalign(alignment, size);
	return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[LOG] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_write(int prio, const char *tag, const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[LOGW] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list list) {
#ifdef ENABLE_DEBUG
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[LOGV] %s: %s\n", tag, string);
#endif
	return 0;
}

int ret0(void) {
	return 0;
}

int ret1(void) {
	return 1;
}

#define  MUTEX_TYPE_NORMAL     0x0000
#define  MUTEX_TYPE_RECURSIVE  0x4000
#define  MUTEX_TYPE_ERRORCHECK 0x8000

static void init_static_mutex(pthread_mutex_t **mutex)
{
	pthread_mutex_t *mtxMem = NULL;

	switch ((int)*mutex) {
	case MUTEX_TYPE_NORMAL: {
		pthread_mutex_t initTmpNormal = PTHREAD_MUTEX_INITIALIZER;
		mtxMem = calloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpNormal, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	case MUTEX_TYPE_RECURSIVE: {
		pthread_mutex_t initTmpRec = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
		mtxMem = calloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpRec, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	case MUTEX_TYPE_ERRORCHECK: {
		pthread_mutex_t initTmpErr = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
		mtxMem = calloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpErr, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	default:
		break;
	}
}

static void init_static_cond(pthread_cond_t **cond)
{
	if (*cond == NULL) {
		pthread_cond_t initTmp = PTHREAD_COND_INITIALIZER;
		pthread_cond_t *condMem = calloc(1, sizeof(pthread_cond_t));
		sceClibMemcpy(condMem, &initTmp, sizeof(pthread_cond_t));
		*cond = condMem;
	}
}

int pthread_attr_destroy_soloader(pthread_attr_t **attr)
{
	int ret = pthread_attr_destroy(*attr);
	free(*attr);
	return ret;
}

int pthread_attr_getstack_soloader(const pthread_attr_t **attr,
				   void **stackaddr,
				   size_t *stacksize)
{
	return pthread_attr_getstack(*attr, stackaddr, stacksize);
}

__attribute__((unused)) int pthread_condattr_init_soloader(pthread_condattr_t **attr)
{
	*attr = calloc(1, sizeof(pthread_condattr_t));

	return pthread_condattr_init(*attr);
}

__attribute__((unused)) int pthread_condattr_destroy_soloader(pthread_condattr_t **attr)
{
	int ret = pthread_condattr_destroy(*attr);
	free(*attr);
	return ret;
}

int pthread_cond_init_soloader(pthread_cond_t **cond,
				   const pthread_condattr_t **attr)
{
	*cond = calloc(1, sizeof(pthread_cond_t));

	if (attr != NULL)
		return pthread_cond_init(*cond, *attr);
	else
		return pthread_cond_init(*cond, NULL);
}

int pthread_cond_destroy_soloader(pthread_cond_t **cond)
{
	int ret = pthread_cond_destroy(*cond);
	free(*cond);
	return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t **cond)
{
	init_static_cond(cond);
	return pthread_cond_signal(*cond);
}

int pthread_cond_timedwait_soloader(pthread_cond_t **cond,
					pthread_mutex_t **mutex,
					struct timespec *abstime)
{
	init_static_cond(cond);
	init_static_mutex(mutex);
	return pthread_cond_timedwait(*cond, *mutex, abstime);
}

int pthread_create_soloader(pthread_t **thread,
				pthread_attr_t **attr,
				void *(*start)(void *),
				void *param)
{
	*thread = calloc(1, sizeof(pthread_t));

	if (attr != NULL) {
		pthread_attr_setstacksize(*attr, 512 * 1024);
		return pthread_create(*thread, *attr, start, param);
	} else {
		pthread_attr_t attrr;
		pthread_attr_init(&attrr);
		pthread_attr_setstacksize(&attrr, 512 * 1024);
		return pthread_create(*thread, &attrr, start, param);
	}

}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t **attr)
{
	*attr = calloc(1, sizeof(pthread_mutexattr_t));

	return pthread_mutexattr_init(*attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t **attr, int type)
{
	return pthread_mutexattr_settype(*attr, type);
}

int pthread_mutexattr_setpshared_soloader(pthread_mutexattr_t **attr, int pshared)
{
	return pthread_mutexattr_setpshared(*attr, pshared);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t **attr)
{
	int ret = pthread_mutexattr_destroy(*attr);
	free(*attr);
	return ret;
}

int pthread_mutex_destroy_soloader(pthread_mutex_t **mutex)
{
	int ret = pthread_mutex_destroy(*mutex);
	free(*mutex);
	return ret;
}

int pthread_mutex_init_soloader(pthread_mutex_t **mutex,
				const pthread_mutexattr_t **attr)
{
	*mutex = calloc(1, sizeof(pthread_mutex_t));

	if (attr != NULL)
		return pthread_mutex_init(*mutex, *attr);
	else
		return pthread_mutex_init(*mutex, NULL);
}

int pthread_mutex_lock_soloader(pthread_mutex_t **mutex)
{
	init_static_mutex(mutex);
	return pthread_mutex_lock(*mutex);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t **mutex)
{
	init_static_mutex(mutex);
	return pthread_mutex_trylock(*mutex);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t **mutex)
{
	return pthread_mutex_unlock(*mutex);
}

int pthread_join_soloader(const pthread_t *thread, void **value_ptr)
{
	return pthread_join(*thread, value_ptr);
}

int pthread_cond_wait_soloader(pthread_cond_t **cond, pthread_mutex_t **mutex)
{
	return pthread_cond_wait(*cond, *mutex);
}

int pthread_cond_broadcast_soloader(pthread_cond_t **cond)
{
	return pthread_cond_broadcast(*cond);
}

int pthread_attr_init_soloader(pthread_attr_t **attr)
{
	*attr = calloc(1, sizeof(pthread_attr_t));

	return pthread_attr_init(*attr);
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t **attr, int state)
{
	return pthread_attr_setdetachstate(*attr, !state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t **attr, size_t stacksize)
{
	return pthread_attr_setstacksize(*attr, stacksize);
}

int pthread_attr_getstacksize_soloader(pthread_attr_t **attr, size_t *stacksize)
{
	return pthread_attr_getstacksize(*attr, stacksize);
}

int pthread_attr_setschedparam_soloader(pthread_attr_t **attr,
					const struct sched_param *param)
{
	return pthread_attr_setschedparam(*attr, param);
}

int pthread_attr_getschedparam_soloader(pthread_attr_t **attr,
					struct sched_param *param)
{
	return pthread_attr_getschedparam(*attr, param);
}

int pthread_attr_setstack_soloader(pthread_attr_t **attr,
				   void *stackaddr,
				   size_t stacksize)
{
	return pthread_attr_setstack(*attr, stackaddr, stacksize);
}

int pthread_setschedparam_soloader(const pthread_t *thread, int policy,
				   const struct sched_param *param)
{
	return pthread_setschedparam(*thread, policy, param);
}

int pthread_getschedparam_soloader(const pthread_t *thread, int *policy,
				   struct sched_param *param)
{
	return pthread_getschedparam(*thread, policy, param);
}

int pthread_detach_soloader(const pthread_t *thread)
{
	return pthread_detach(*thread);
}

int pthread_getattr_np_soloader(pthread_t* thread, pthread_attr_t *attr) {
	fprintf(stderr, "[WARNING!] Not implemented: pthread_getattr_np\n");
	return 0;
}

int pthread_equal_soloader(const pthread_t *t1, const pthread_t *t2)
{
	if (t1 == t2)
		return 1;
	if (!t1 || !t2)
		return 0;
	return pthread_equal(*t1, *t2);
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(const pthread_t *thread, const char* thread_name) {
	if (thread == 0 || thread_name == NULL) {
		return EINVAL;
	}
	size_t thread_name_len = strlen(thread_name);
	if (thread_name_len >= MAX_TASK_COMM_LEN) {
		return ERANGE;
	}

	// TODO: Implement the actual name setting if possible
	fprintf(stderr, "PTHR: pthread_setname_np with name %s\n", thread_name);

	return 0;
}

int clock_gettime_hook(int clk_id, struct timespec *t) {
	struct timeval now;
	int rv = gettimeofday(&now, NULL);
	if (rv)
		return rv;
	t->tv_sec = now.tv_sec;
	t->tv_nsec = now.tv_usec * 1000;

	return 0;
}


int GetCurrentThreadId(void) {
	return sceKernelGetThreadId();
}

int GetEnv(void *vm, void **env, int r2) {
	*env = fake_env;
	return 0;
}

void throw_exc(char **str, void *a, int b) {
	dlog("throwing %s\n", *str);
}

FILE *fopen_hook(char *fname, char *mode) {
	FILE *f;
	char real_fname[256];
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "ux0:data/naught2/%s", fname);
		f = fopen(real_fname, mode);
	} else {
		f = fopen(fname, mode);
	}
	return f;
}

int open_hook(const char *fname, int flags, mode_t mode) {
	int f;
	char real_fname[256];
	dlog("open(%s)\n", fname);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "ux0:data/naught2/%s", fname);
		f = open(real_fname, flags, mode);
	} else {
		f = open(fname, flags, mode);
	}
	return f;
}

extern void *__aeabi_atexit;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_unwind_cpp_pr0;
extern void *__aeabi_unwind_cpp_pr1;
extern void *__cxa_atexit;
extern void *__cxa_call_unexpected;
extern void *__cxa_finalize;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__gnu_unwind_frame;
extern void *__srget;
extern void *__stack_chk_fail;
extern void *_ZdaPv;
extern void *_ZdlPv;
extern void *_Znaj;
extern void *_Znwj;

static int __stack_chk_guard_fake = 0x42424242;

static FILE __sF_fake[0x1000][3];

typedef struct __attribute__((__packed__)) stat64_bionic {
	unsigned long long st_dev;
	unsigned char __pad0[4];
	unsigned long st_ino;
	unsigned int st_mode;
	unsigned int st_nlink;
	unsigned long st_uid;
	unsigned long st_gid;
	unsigned long long st_rdev;
	unsigned char __pad3[4];
	unsigned long st_size;
	unsigned long st_blksize;
	unsigned long st_blocks;
	unsigned long st_atime;
	unsigned long st_atime_nsec;
	unsigned long st_mtime;
	unsigned long st_mtime_nsec;
	unsigned long st_ctime;
	unsigned long st_ctime_nsec;
	unsigned long long __pad4;
} stat64_bionic;

int lstat_hook(const char *pathname, stat64_bionic *statbuf) {
	dlog("lstat(%s)\n", pathname);
	int res;
	struct stat st;
	if (strncmp(pathname, "ux0:", 4)) {
		char fname[256];
		sprintf(fname, "ux0:data/naught2/%s", pathname);
		dlog("lstat(%s) fixed\n", fname);
		res = stat(fname, &st);
	} else {
		res = stat(pathname, &st);
	}
	if (res == 0) {
		if (!statbuf) {
			statbuf = malloc(sizeof(stat64_bionic));
		}
		statbuf->st_dev = st.st_dev;
		statbuf->st_ino = st.st_ino;
		statbuf->st_mode = st.st_mode;
		statbuf->st_nlink = st.st_nlink;
		statbuf->st_uid = st.st_uid;
		statbuf->st_gid = st.st_gid;
		statbuf->st_rdev = st.st_rdev;
		statbuf->st_size = st.st_size;
		statbuf->st_blksize = st.st_blksize;
		statbuf->st_blocks = st.st_blocks;
		statbuf->st_atime = st.st_atime;
		statbuf->st_atime_nsec = 0;
		statbuf->st_mtime = st.st_mtime;
		statbuf->st_mtime_nsec = 0;
		statbuf->st_ctime = st.st_ctime;
		statbuf->st_ctime_nsec = 0;
	}
	return res;
}

int stat_hook(const char *pathname, stat64_bionic *statbuf) {
	dlog("stat(%s)\n", pathname);
	int res;
	struct stat st;
	if (strncmp(pathname, "ux0:", 4)) {
		char fname[256];
		sprintf(fname, "ux0:data/naught2/%s", pathname);
		dlog("stat(%s) fixed\n", fname);
		res = stat(fname, &st);
	} else {
		res = stat(pathname, &st);
	}
	if (res == 0) {
		if (!statbuf) {
			statbuf = malloc(sizeof(stat64_bionic));
		}
		statbuf->st_dev = st.st_dev;
		statbuf->st_ino = st.st_ino;
		statbuf->st_mode = st.st_mode;
		statbuf->st_nlink = st.st_nlink;
		statbuf->st_uid = st.st_uid;
		statbuf->st_gid = st.st_gid;
		statbuf->st_rdev = st.st_rdev;
		statbuf->st_size = st.st_size;
		statbuf->st_blksize = st.st_blksize;
		statbuf->st_blocks = st.st_blocks;
		statbuf->st_atime = st.st_atime;
		statbuf->st_atime_nsec = 0;
		statbuf->st_mtime = st.st_mtime;
		statbuf->st_mtime_nsec = 0;
		statbuf->st_ctime = st.st_ctime;
		statbuf->st_ctime_nsec = 0;
	}
	return res;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	return memalign(length, 0x1000);
}

int munmap(void *addr, size_t length) {
	free(addr);
	return 0;
}

int fstat_hook(int fd, void *statbuf) {
	struct stat st;
	int res = fstat(fd, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

void *sceClibMemclr(void *dst, SceSize len) {
	if (!dst) {
		printf("memclr on NULL\n");
		return NULL;
	}
	return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch) {
	return sceClibMemset(dst, ch, len);
}

void *Android_JNI_GetEnv() {
	return fake_env;
}

void abort_hook() {
	dlog("abort called from %p\n", __builtin_return_address(0));
	uint8_t *p = NULL;
	p[0] = 1;
}

int ret99() {
	return 99;
}

int chdir_hook(const char *path) {
	return 0;
}

#define SCE_ERRNO_MASK 0xFF

#define DT_DIR 4
#define DT_REG 8

struct android_dirent {
	char pad[18];
	unsigned char d_type;
	char d_name[256];
};

typedef struct {
	SceUID uid;
	struct android_dirent dir;
} android_DIR;

int closedir_fake(android_DIR *dirp) {
	if (!dirp || dirp->uid < 0) {
		errno = EBADF;
		return -1;
	}

	int res = sceIoDclose(dirp->uid);
	dirp->uid = -1;

	free(dirp);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return -1;
	}

	errno = 0;
	return 0;
}

android_DIR *opendir_fake(const char *dirname) {
	dlog("opendir(%s)\n", dirname);
	SceUID uid;
	if (strncmp(dirname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/naught2/%s", dirname);
		uid = sceIoDopen(real_fname);
	} else {
		uid = sceIoDopen(dirname);
	}

	if (uid < 0) {
		errno = uid & SCE_ERRNO_MASK;
		return NULL;
	}

	android_DIR *dirp = calloc(1, sizeof(android_DIR));

	if (!dirp) {
		sceIoDclose(uid);
		errno = ENOMEM;
		return NULL;
	}

	dirp->uid = uid;

	errno = 0;
	return dirp;
}

struct android_dirent *readdir_fake(android_DIR *dirp) {
	if (!dirp) {
		errno = EBADF;
		return NULL;
	}

	SceIoDirent sce_dir;
	int res = sceIoDread(dirp->uid, &sce_dir);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return NULL;
	}

	if (res == 0) {
		errno = 0;
		return NULL;
	}

	dirp->dir.d_type = SCE_S_ISDIR(sce_dir.d_stat.st_mode) ? DT_DIR : DT_REG;
	strcpy(dirp->dir.d_name, sce_dir.d_name);
	return &dirp->dir;
}

size_t __strlen_chk(const char *s, size_t s_len) {
	return strlen(s);
}

uint64_t lseek64(int fd, uint64_t offset, int whence) {
	return lseek(fd, offset, whence);
}

void __assert2(const char *file, int line, const char *func, const char *expr) {
	dlog("assertion failed:\n%s:%d (%s): %s\n", file, line, func, expr);
	sceKernelExitProcess(0);
}

void *dlsym_hook( void *handle, const char *symbol) {
	//dlog("dlsym %s\n", symbol);
	return vglGetProcAddress(symbol);
}

int strerror_r_hook(int errnum, char *buf, size_t buflen) {
	strerror_r(errnum, buf, buflen);
	dlog("Error %d: %s\n",errnum, buf);
	return 0;
}

uint32_t fake_stdout;

int access_hook(const char *pathname, int mode) {
	dlog("access(%s)\n", pathname);
	if (strncmp(pathname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/naught2/%s", pathname);
		return access(real_fname, mode);
	}

	return access(pathname, mode);
}

int mkdir_hook(const char *pathname, int mode) {
	dlog("mkdir(%s)\n", pathname);
	if (strncmp(pathname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/naught2/%s", pathname);
		return mkdir(real_fname, mode);
	}

	return mkdir(pathname, mode);
}

FILE *AAssetManager_open(void *mgr, const char *fname, int mode) {
	char full_fname[256];
	sprintf(full_fname, "ux0:data/naught2/%s", fname);
	dlog("AAssetManager_open %s\n", full_fname);
	return fopen(full_fname, "rb");
}

int AAsset_close(FILE *f) {
	return fclose(f);
}

size_t AAsset_getLength(FILE *f) {
	size_t p = ftell(f);
	fseek(f, 0, SEEK_END);
	size_t res = ftell(f);
	fseek(f, p, SEEK_SET);
	return res;
}

size_t AAsset_read(FILE *f, void *buf, size_t count) {
	return fread(buf, 1, count, f);
}

size_t AAsset_seek(FILE *f, size_t offs, int whence) {
	fseek(f, offs, whence);
	return ftell(f);
}

int rmdir_hook(const char *pathname) {
	dlog("rmdir(%s)\n", pathname);
	if (strncmp(pathname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/naught2/%s", pathname);
		return rmdir(real_fname);
	}

	return rmdir(pathname);
}

int unlink_hook(const char *pathname) {
	dlog("unlink(%s)\n", pathname);
	if (strncmp(pathname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/naught2/%s", pathname);
		return sceIoRemove(real_fname);
	}

	return sceIoRemove(pathname);
}

int remove_hook(const char *pathname) {
	dlog("unlink(%s)\n", pathname);
	if (strncmp(pathname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/naught2/%s", pathname);
		return sceIoRemove(real_fname);
	}

	return sceIoRemove(pathname);
}

DIR *AAssetManager_openDir(void *mgr, const char *fname) {
	dlog("AAssetManager_opendir(%s)\n", fname);
	if (strncmp(fname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/naught2/%s", fname);
		return opendir(real_fname);
	}

	return opendir(fname);
}

const char *AAssetDir_getNextFileName(DIR *assetDir) {
	struct dirent *ent = readdir(assetDir);
	if (ent) {
		return ent->d_name;
	}
	return NULL;
}

void AAssetDir_close(DIR *assetDir) {
	closedir(assetDir);
}

int rename_hook(const char *old_filename, const char *new_filename) {
	dlog("rename %s -> %s\n", old_filename, new_filename);
	char real_old[256], real_new[256];
	if (strncmp(old_filename, "ux0:", 4)) {
		sprintf(real_old, "ux0:data/naught2/%s", old_filename);
	} else {
		strcpy(real_old, old_filename);
	}
	if (strncmp(new_filename, "ux0:", 4)) {
		sprintf(real_new, "ux0:data/naught2/%s", new_filename);
	} else {
		strcpy(real_new, new_filename);
	}
	return sceIoRename(real_old, real_new);
}

int nanosleep_hook(const struct timespec *req, struct timespec *rem) {
	const uint32_t usec = req->tv_sec * 1000 * 1000 + req->tv_nsec / 1000;
	return sceKernelDelayThreadCB(usec);
}

int sem_destroy_soloader(int * uid) {
	if (sceKernelDeleteSema(*uid) < 0)
		return -1;
	return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
	SceKernelSemaInfo info;
	info.size = sizeof(SceKernelSemaInfo);

	if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
	if (!sval) sval = malloc(sizeof(int32_t));
	*sval = info.currentCount;
	return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
	*uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
	if (*uid < 0)
		return -1;
	return 0;
}

int sem_post_soloader (int * uid) {
	if (sceKernelSignalSema(*uid, 1) < 0)
		return -1;
	return 0;
}

uint64_t current_timestamp_ms() {
	struct timeval te;
	gettimeofday(&te, NULL);
	return (te.tv_sec*1000LL + te.tv_usec/1000);
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
	uint timeout = 1000;
	if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
		return 0;
	if (!abstime) return -1;
	long long now = (long long) current_timestamp_ms() * 1000; // us
	long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
	if (_timeout-now >= 0) return -1;
	uint timeout_real = _timeout - now;
	if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0)
		return -1;
	return 0;
}

int sem_trywait_soloader (int * uid) {
	uint timeout = 1000;
	if (sceKernelWaitSema(*uid, 1, &timeout) < 0)
		return -1;
	return 0;
}

int sem_wait_soloader (int * uid) {
	if (sceKernelWaitSema(*uid, 1, NULL) < 0)
		return -1;
	return 0;
}

int uname_fake(void *buf) {
	strcpy(buf + 195, "1.0");
	return 0;
}

int scandir_hook(const char *dir, struct android_dirent ***namelist,
	int (*selector) (const struct dirent *),
	int (*compar) (const struct dirent **, const struct dirent **))
{
	DIR *dp = opendir(dir);
	struct dirent *current;
	struct android_dirent d;
	struct android_dirent *android_current = &d;
	struct android_dirent **names = NULL;
	size_t names_size = 0, pos;

	if (dp == NULL)
		return -1;

	pos = 0;
	while ((current = readdir (dp)) != NULL) {
		int use_it = selector == NULL;

		sceClibMemcpy(android_current->d_name, current->d_name, 256);
		android_current->d_type = SCE_S_ISDIR(current->d_stat.st_mode) ? 4 : 8;

		if (! use_it) {
			use_it = (*selector) (android_current);
		}
		if (use_it) {
			struct android_dirent *vnew;
			size_t dsize;

			if (pos == names_size)
			{
				struct android_dirent **new;
				if (names_size == 0)
					names_size = 10;
				else
					names_size *= 2;
				new = (struct android_dirent **) vglRealloc (names, names_size * sizeof (struct android_dirent *));
				if (new == NULL)
					break;
				names = new;
			}

			dsize = &android_current->d_name[256+1] - (char*)android_current;
			vnew = (struct android_dirent *) vglMalloc (dsize);
			if (vnew == NULL)
				break;

			names[pos++] = (struct android_dirent *) sceClibMemcpy (vnew, android_current, dsize);
		}
	}

	if (errno != 0) {
		closedir (dp);
		while (pos > 0)
			vglFree (names[--pos]);
		vglFree (names);
		return -1;
	}

	closedir (dp);

	/* Sort the list if we have a comparison function to sort with.  */
	if (compar != NULL)
		qsort (names, pos, sizeof (struct android_dirent *), (__compar_fn_t) compar);
	*namelist = names;
	return pos;
}

static so_default_dynlib default_dynlib[] = {
	// Runtime / ABI / others
	{ "abort", (uintptr_t)&abort_hook },
	{ "exit", (uintptr_t)&exit },
	{ "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
	{ "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
	{ "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
	{ "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
	{ "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
	{ "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
	{ "__aeabi_unwind_cpp_pr0", (uintptr_t)&__aeabi_unwind_cpp_pr0 },
	{ "__aeabi_unwind_cpp_pr1", (uintptr_t)&__aeabi_unwind_cpp_pr1 },
	{ "__cxa_atexit", (uintptr_t)&__cxa_atexit },
	{ "__cxa_finalize", (uintptr_t)&__cxa_finalize },
	{ "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
	{ "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
	{ "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
	{ "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
	{ "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
	{ "__android_log_print", (uintptr_t)&__android_log_print },
	{ "__errno", (uintptr_t)&__errno },
	{ "getenv", (uintptr_t)&ret0 },
	{ "bsd_signal", (uintptr_t)&ret0 },
	{ "dlclose", (uintptr_t)&ret0 },
	{ "dlerror", (uintptr_t)&ret0 },
	{ "dlopen", (uintptr_t)&ret0 },
	{ "dlsym", (uintptr_t)&dlsym_hook },
	{ "time", (uintptr_t)&time },
	{ "usleep", (uintptr_t)&usleep },
	{ "gettimeofday", (uintptr_t)&gettimeofday },
	{ "localtime", (uintptr_t)&localtime },
	{ "setjmp", (uintptr_t)&setjmp },
	{ "longjmp", (uintptr_t)&longjmp },
	{ "malloc", (uintptr_t)&malloc },
	{ "free", (uintptr_t)&free },
	{ "calloc", (uintptr_t)&calloc },
	{ "realloc", (uintptr_t)&realloc },
	{ "qsort", (uintptr_t)&qsort },
	{ "__sF", (uintptr_t)&__sF_fake },
	{ "__srget", (uintptr_t)&__srget },
	{ "uname", (uintptr_t)&uname_fake },
	// Parsing C standard
	{ "atoi", (uintptr_t)&atoi },
	{ "atol", (uintptr_t)&atol },
	{ "strtod", (uintptr_t)&strtod },
	{ "strtol", (uintptr_t)&strtol },
	{ "strtoul", (uintptr_t)&strtoul },
	// Math
	{ "acos", (uintptr_t)&acos },
	{ "acosf", (uintptr_t)&acosf },
	{ "asinf", (uintptr_t)&asinf },
	{ "atan", (uintptr_t)&atan },
	{ "atanf", (uintptr_t)&atanf },
	{ "atan2f", (uintptr_t)&atan2f },
	{ "ceil", (uintptr_t)&ceil },
	{ "ceilf", (uintptr_t)&ceilf },
	{ "cos", (uintptr_t)&cos },
	{ "cosf", (uintptr_t)&cosf },
	{ "exp", (uintptr_t)&exp },
	{ "expf", (uintptr_t)&expf },
	{ "floor", (uintptr_t)&floor },
	{ "floorf", (uintptr_t)&floorf },
	{ "fmaf", (uintptr_t)&fmaf },
	{ "fmaxf", (uintptr_t)&fmaxf },
	{ "fminf", (uintptr_t)&fminf },
	{ "fmodf", (uintptr_t)&fmodf },
	{ "frexp", (uintptr_t)&frexp },
	{ "ldexp", (uintptr_t)&ldexp },
	{ "log", (uintptr_t)&log },
	{ "logf", (uintptr_t)&logf },
	{ "log10f", (uintptr_t)&log10f },
	{ "pow", (uintptr_t)&pow },
	{ "powf", (uintptr_t)&powf },
	{ "rint", (uintptr_t)&rint },
	{ "sin", (uintptr_t)&sin },
	{ "sinf", (uintptr_t)&sinf },
	{ "sqrt", (uintptr_t)&sqrt },
	{ "sqrtf", (uintptr_t)&sqrtf },
	{ "tanf", (uintptr_t)&tanf },
	{ "nextafterf", (uintptr_t)&nextafterf },
	// I/O
	{ "fopen", (uintptr_t)&fopen_hook },
	{ "fclose", (uintptr_t)&fclose },
	{ "fread", (uintptr_t)&fread },
	{ "fwrite", (uintptr_t)&fwrite },
	{ "fseek", (uintptr_t)&fseek },
	{ "ftell", (uintptr_t)&ftell },
	{ "fdopen", (uintptr_t)&fdopen },
	{ "fflush", (uintptr_t)&ret0 },
	{ "fgetc", (uintptr_t)&fgetc },
	{ "fputc", (uintptr_t)&ret0 },
	{ "fputs", (uintptr_t)&ret0 },
	{ "fprintf", (uintptr_t)&fprintf },
	{ "vfprintf", (uintptr_t)&vfprintf },
	{ "vfscanf", (uintptr_t)&vfscanf },
	{ "sprintf", (uintptr_t)&sprintf },
	{ "vsprintf", (uintptr_t)&vsprintf },
	{ "sscanf", (uintptr_t)&sscanf },
	{ "perror", (uintptr_t)&perror },
	{ "open", (uintptr_t)&open_hook },
	{ "close", (uintptr_t)&close },
	{ "read", (uintptr_t)&read },
	{ "dup", (uintptr_t)&dup },
	{ "remove", (uintptr_t)&remove_hook },
	{ "mkdir", (uintptr_t)&mkdir_hook },
	{ "stat", (uintptr_t)&stat_hook },
	{ "opendir", (uintptr_t)&opendir_fake },
	{ "closedir", (uintptr_t)&closedir_fake },
	{ "scandir", (uintptr_t)&scandir_hook },
	{ "alphasort", (uintptr_t)&alphasort },
	{ "ungetc", (uintptr_t)&ungetc },
	{ "ioctl", (uintptr_t)&ret0 },
	// String
	{ "strcat", (uintptr_t)&strcat },
	{ "strncat", (uintptr_t)&strncat },
	{ "strcpy", (uintptr_t)&strcpy },
	{ "strncpy", (uintptr_t)&strncpy },
	{ "strcmp", (uintptr_t)&strcmp },
	{ "strncmp", (uintptr_t)&strncmp },
	{ "strlen", (uintptr_t)&strlen },
	//{ "strnlen", (uintptr_t)&strnlen },
	{ "strchr", (uintptr_t)&strchr },
	{ "strrchr", (uintptr_t)&strrchr },
	{ "strstr", (uintptr_t)&strstr },
	{ "strpbrk", (uintptr_t)&strpbrk },
	{ "strcspn", (uintptr_t)&strcspn },
	{ "strcoll", (uintptr_t)&strcoll },
	{ "strerror", (uintptr_t)&strerror },
	{ "memchr", (uintptr_t)&sceClibMemchr },
	{ "memcmp", (uintptr_t)&memcmp },
	{ "memcpy", (uintptr_t)&sceClibMemcpy },
	{ "memmove", (uintptr_t)&memmove },
	{ "memset", (uintptr_t)&sceClibMemset },
	{ "memmem", (uintptr_t)&memmem },
	{ "isalnum", (uintptr_t)&isalnum },
	{ "isalpha", (uintptr_t)&isalpha },
	{ "iscntrl", (uintptr_t)&iscntrl },
	{ "islower", (uintptr_t)&islower },
	{ "ispunct", (uintptr_t)&ispunct },
	{ "isspace", (uintptr_t)&isspace },
	{ "isupper", (uintptr_t)&isupper },
	{ "isxdigit", (uintptr_t)&isxdigit },
	{ "tolower", (uintptr_t)&tolower },
	{ "toupper", (uintptr_t)&toupper },
	// Network / Sockets
	{ "accept", (uintptr_t)&accept },
	{ "bind", (uintptr_t)&bind },
	{ "connect", (uintptr_t)&connect },
	{ "listen", (uintptr_t)&listen },
	{ "socket", (uintptr_t)&socket },
	{ "send", (uintptr_t)&send },
	{ "recv", (uintptr_t)&recv },
	{ "shutdown", (uintptr_t)&shutdown },
	{ "setsockopt", (uintptr_t)&setsockopt },
	{ "select", (uintptr_t)&select },
	{ "gethostbyname", (uintptr_t)&gethostbyname },
	{ "gethostname", (uintptr_t)&gethostname },
	{ "inet_addr", (uintptr_t)&inet_addr },
	{ "inet_ntoa", (uintptr_t)&inet_ntoa },
	// OpenGL / GLES2
	{ "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
	{ "glActiveTexture", (uintptr_t)&glActiveTexture },
	{ "glAttachShader", (uintptr_t)&glAttachShader },
	{ "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
	{ "glBindBuffer", (uintptr_t)&glBindBuffer },
	{ "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
	{ "glBindRenderbuffer", (uintptr_t)&ret0 },
	{ "glBindTexture", (uintptr_t)&glBindTexture },
	{ "glBlendFunc", (uintptr_t)&glBlendFunc },
	{ "glBufferData", (uintptr_t)&glBufferData },
	{ "glBufferSubData", (uintptr_t)&glBufferSubData },
	{ "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
	{ "glClear", (uintptr_t)&glClear },
	{ "glClearColor", (uintptr_t)&glClearColor },
	{ "glClearDepthf", (uintptr_t)&glClearDepthf },
	{ "glClearStencil", (uintptr_t)&glClearStencil },
	{ "glColorMask", (uintptr_t)&glColorMask },
	{ "glCompileShader", (uintptr_t)&glCompileShader },
	{ "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
	{ "glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D },
	{ "glCopyTexSubImage2D", (uintptr_t)&glCopyTexSubImage2D },
	{ "glCreateProgram", (uintptr_t)&glCreateProgram },
	{ "glCreateShader", (uintptr_t)&glCreateShader },
	{ "glCullFace", (uintptr_t)&glCullFace },
	{ "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
	{ "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
	{ "glDeleteProgram", (uintptr_t)&glDeleteProgram },
	{ "glDeleteRenderbuffers", (uintptr_t)&ret0 },
	{ "glDeleteShader", (uintptr_t)&glDeleteShader },
	{ "glDeleteTextures", (uintptr_t)&glDeleteTextures },
	{ "glDepthFunc", (uintptr_t)&glDepthFunc },
	{ "glDepthMask", (uintptr_t)&glDepthMask },
	{ "glDisable", (uintptr_t)&glDisable },
	{ "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
	{ "glDrawArrays", (uintptr_t)&glDrawArrays },
	{ "glDrawElements", (uintptr_t)&glDrawElements },
	{ "glEnable", (uintptr_t)&glEnable },
	{ "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
	{ "glFinish", (uintptr_t)&glFinish },
	{ "glFlush", (uintptr_t)&glFlush },
	{ "glFramebufferRenderbuffer", (uintptr_t)&ret0 },
	{ "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
	{ "glGenBuffers", (uintptr_t)&glGenBuffers },
	{ "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
	{ "glGenRenderbuffers", (uintptr_t)&ret0 },
	{ "glGenTextures", (uintptr_t)&glGenTextures },
	{ "glGetFloatv", (uintptr_t)&glGetFloatv },
	{ "glGetIntegerv", (uintptr_t)&glGetIntegerv },
	{ "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
	{ "glGetProgramiv", (uintptr_t)&glGetProgramiv },
	{ "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
	{ "glGetShaderiv", (uintptr_t)&glGetShaderiv },
	{ "glGetString", (uintptr_t)&glGetString },
	{ "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
	{ "glLinkProgram", (uintptr_t)&glLinkProgram },
	{ "glPixelStorei", (uintptr_t)&glPixelStorei },
	{ "glPolygonOffset", (uintptr_t)&glPolygonOffset },
	{ "glReadPixels", (uintptr_t)&glReadPixels },
	{ "glRenderbufferStorage", (uintptr_t)&ret0 },
	{ "glScissor", (uintptr_t)&glScissor },
	{ "glShaderSource", (uintptr_t)&glShaderSource },
	{ "glStencilFunc", (uintptr_t)&glStencilFunc },
	{ "glStencilMask", (uintptr_t)&glStencilMask },
	{ "glStencilOp", (uintptr_t)&glStencilOp },
	{ "glTexImage2D", (uintptr_t)&glTexImage2D },
	{ "glTexParameteri", (uintptr_t)&glTexParameteri },
	{ "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
	{ "glUniform1i", (uintptr_t)&glUniform1i },
	{ "glUniform4f", (uintptr_t)&glUniform4f },
	{ "glUniform4fv", (uintptr_t)&glUniform4fv },
	{ "glUseProgram", (uintptr_t)&glUseProgram },
	{ "glValidateProgram", (uintptr_t)&ret0 },
	{ "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
	{ "glViewport", (uintptr_t)&glViewport },
	// OpenAL
	{ "alBufferData", (uintptr_t)&alBufferData },
	{ "alDeleteBuffers", (uintptr_t)&alDeleteBuffers },
	{ "alDeleteSources", (uintptr_t)&alDeleteSources },
	{ "alGenBuffers", (uintptr_t)&alGenBuffers },
	{ "alGenSources", (uintptr_t)&alGenSources },
	{ "alGetBufferi", (uintptr_t)&alGetBufferi },
	{ "alGetError", (uintptr_t)&alGetError },
	{ "alGetProcAddress", (uintptr_t)&alGetProcAddress },
	{ "alGetSourcei", (uintptr_t)&alGetSourcei },
	{ "alGetString", (uintptr_t)&alGetString },
	{ "alIsBuffer", (uintptr_t)&alIsBuffer },
	{ "alIsExtensionPresent", (uintptr_t)&alIsExtensionPresent },
	{ "alListener3f", (uintptr_t)&alListener3f },
	{ "alListenerf", (uintptr_t)&alListenerf },
	{ "alListenerfv", (uintptr_t)&alListenerfv },
	{ "alSource3f", (uintptr_t)&alSource3f },
	{ "alSource3i", (uintptr_t)&alSource3i },
	{ "alSourcef", (uintptr_t)&alSourcef },
	{ "alSourcefv", (uintptr_t)&alSourcefv },
	{ "alSourcei", (uintptr_t)&alSourcei },
	{ "alSourcePause", (uintptr_t)&alSourcePause },
	{ "alSourcePlay", (uintptr_t)&alSourcePlay },
	{ "alSourceQueueBuffers", (uintptr_t)&alSourceQueueBuffers },
	{ "alSourceRewind", (uintptr_t)&alSourceRewind },
	{ "alSourceStop", (uintptr_t)&alSourceStop },
	{ "alSourceUnqueueBuffers", (uintptr_t)&alSourceUnqueueBuffers },
	{ "alcCaptureCloseDevice", (uintptr_t)&alcCaptureCloseDevice },
	{ "alcCaptureOpenDevice", (uintptr_t)&alcCaptureOpenDevice },
	{ "alcCaptureSamples", (uintptr_t)&alcCaptureSamples },
	{ "alcCaptureStart", (uintptr_t)&alcCaptureStart },
	{ "alcCaptureStop", (uintptr_t)&alcCaptureStop },
	{ "alcCloseDevice", (uintptr_t)&alcCloseDevice },
	{ "alcCreateContext", (uintptr_t)&alcCreateContext },
	{ "alcDestroyContext", (uintptr_t)&alcDestroyContext },
	{ "alcGetContextsDevice", (uintptr_t)&alcGetContextsDevice },
	{ "alcGetCurrentContext", (uintptr_t)&alcGetCurrentContext },
	{ "alcGetError", (uintptr_t)&alcGetError },
	{ "alcGetIntegerv", (uintptr_t)&alcGetIntegerv },
	{ "alcGetString", (uintptr_t)&ret0 }, // FIXME
	{ "alcIsExtensionPresent", (uintptr_t)&alcIsExtensionPresent },
	{ "alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrent },
	{ "alcOpenDevice", (uintptr_t)&alcOpenDevice },
	{ "alcProcessContext", (uintptr_t)&alcProcessContext },
	{ "alcSuspendContext", (uintptr_t)&alcSuspendContext },
	// SSL / Crypto
	{ "BIO_ctrl", (uintptr_t)&BIO_ctrl },
	{ "BIO_free", (uintptr_t)&BIO_free },
	{ "BIO_new_socket", (uintptr_t)&BIO_new_socket },
	{ "BIO_new", (uintptr_t)&BIO_new },
	{ "BIO_puts", (uintptr_t)&BIO_puts },
	{ "BIO_s_mem", (uintptr_t)&BIO_s_mem },
	{ "ERR_print_errors", (uintptr_t)&ret0 },
	{ "EVP_DigestInit", (uintptr_t)&EVP_DigestInit },
	{ "EVP_DigestUpdate", (uintptr_t)&EVP_DigestUpdate },
	{ "EVP_MD_CTX_cleanup", (uintptr_t)&EVP_MD_CTX_cleanup },
	{ "EVP_PKEY_free", (uintptr_t)&EVP_PKEY_free },
	{ "EVP_sha1", (uintptr_t)&EVP_sha1 },
	{ "EVP_VerifyFinal", (uintptr_t)&EVP_VerifyFinal },
	{ "OPENSSL_add_all_algorithms_noconf", (uintptr_t)&OPENSSL_add_all_algorithms_noconf },
	{ "PEM_read_bio_X509", (uintptr_t)&PEM_read_bio_X509 },
	{ "SSL_connect", (uintptr_t)&SSL_connect },
	{ "SSL_CTX_free", (uintptr_t)&SSL_CTX_free },
	{ "SSL_CTX_new", (uintptr_t)&SSL_CTX_new },
	{ "SSL_CTX_set_verify", (uintptr_t)&SSL_CTX_set_verify },
	{ "SSL_free", (uintptr_t)&SSL_free },
	{ "SSL_get_error", (uintptr_t)&SSL_get_error },
	{ "SSL_get_peer_certificate", (uintptr_t)&SSL_get_peer_certificate },
	{ "SSL_get_verify_result", (uintptr_t)&SSL_get_verify_result },
	{ "SSL_library_init", (uintptr_t)&SSL_library_init },
	{ "SSL_new", (uintptr_t)&SSL_new },
	{ "SSL_read", (uintptr_t)&SSL_read },
	{ "SSL_set_bio", (uintptr_t)&SSL_set_bio },
	{ "SSL_shutdown", (uintptr_t)&SSL_shutdown },
	{ "SSLv23_client_method", (uintptr_t)&SSLv23_client_method },
	{ "SSL_write", (uintptr_t)&SSL_write },
	{ "X509_free", (uintptr_t)&X509_free },
	{ "X509_get_pubkey", (uintptr_t)&X509_get_pubkey },
	{ "X509_print", (uintptr_t)&X509_print },
	{ "X509_STORE_add_cert", (uintptr_t)&X509_STORE_add_cert },
	{ "X509_STORE_CTX_cleanup", (uintptr_t)&X509_STORE_CTX_cleanup },
	{ "X509_STORE_CTX_get_error", (uintptr_t)&X509_STORE_CTX_get_error },
	{ "X509_STORE_CTX_init", (uintptr_t)&X509_STORE_CTX_init },
	{ "X509_STORE_CTX_new", (uintptr_t)&X509_STORE_CTX_new },
	{ "X509_STORE_free", (uintptr_t)&X509_STORE_free },
	{ "X509_STORE_new", (uintptr_t)&X509_STORE_new },
	{ "X509_verify_cert_error_string", (uintptr_t)&X509_verify_cert_error_string },
	{ "X509_verify_cert", (uintptr_t)&X509_verify_cert },
	// Threads
	{ "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
	{ "pthread_attr_init", (uintptr_t)&pthread_attr_init_soloader },
	{ "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_soloader },
	{ "pthread_create", (uintptr_t)&pthread_create_soloader },
	{ "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
	{ "pthread_cond_init", (uintptr_t)&pthread_cond_init_soloader },
	{ "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
	{ "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },
	{ "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_soloader },
	{ "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_soloader },
	{ "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_soloader },
	{ "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_soloader },
	{ "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_soloader },
	{ "pthread_once", (uintptr_t)&pthread_once },
	{ "pthread_self", (uintptr_t)&pthread_self },
	// RNG
	{ "lrand48", (uintptr_t)&lrand48 },
	{ "srand48", (uintptr_t)&srand48 },
	// C++ allocation
	{ "_ZdaPv", (uintptr_t)&_ZdaPv },
	{ "_ZdlPv", (uintptr_t)&_ZdlPv },
	{ "_Znaj", (uintptr_t)&_Znaj },
	{ "_Znwj", (uintptr_t)&_Znwj }
};


int check_kubridge(void) {
	int search_unk[2];
	return _vshKernelSearchModuleByName("kubridge", search_unk);
}

enum MethodIDs {
	UNKNOWN = 0,
	INIT
} MethodIDs;

typedef struct {
	char *name;
	enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
	{ "<init>", INIT }
};

int GetMethodID(void *env, void *class, const char *name, const char *sig) {
	dlog("GetMethodID: %s\n", name);
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0) {
			return name_to_method_ids[i].id;
		}
	}

	return UNKNOWN;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig) {
	dlog("GetStaticMethodID: %s\n", name);
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0)
			return name_to_method_ids[i].id;
	}

	return UNKNOWN;
}

void CallStaticVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
		break;
	}
}

int CallStaticBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

int CallStaticIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

int64_t CallStaticLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

uint64_t CallLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return -1;
}

void *FindClass(void) {
	return (void *)0x41414141;
}

void *NewGlobalRef(void *env, char *str) {
	return (void *)0x42424242;
}

void DeleteGlobalRef(void *env, char *str) {
}

void *NewObjectV(void *env, void *clazz, int methodID, uintptr_t args) {
	return (void *)0x43434343;
}

void *GetObjectClass(void *env, void *obj) {
	return (void *)0x44444444;
}

char *NewStringUTF(void *env, char *bytes) {
	return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy) {
	return string;
}

size_t GetStringUTFLength(void *env, char *string) {
	return strlen(string);
}

int GetJavaVM(void *env, void **vm) {
	*vm = fake_vm;
	return 0;
}

int GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

int GetBooleanField(void *env, void *obj, int fieldID) {
	return 1;
}

void *GetObjectArrayElement(void *env, uint8_t *obj, int idx) {
	return NULL;
}

int CallBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

void *CallObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return NULL;
	}
}

int CallIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

void CallVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		break;
	}
}

int GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

void *GetStaticObjectField(void *env, void *clazz, int fieldID) {
	switch (fieldID) {
	default:
		return NULL;
	}
}

void GetStringUTFRegion(void *env, char *str, size_t start, size_t len, char *buf) {
	sceClibMemcpy(buf, &str[start], len);
	buf[len] = 0;
}

void *CallStaticObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return NULL;
}

int GetIntField(void *env, void *obj, int fieldID) { return 0; }

float GetFloatField(void *env, void *obj, int fieldID) {
	switch (fieldID) {
	default:
		return 0.0f;
	}
}

float CallStaticFloatMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN) {
			dlog("CallStaticDoubleMethodV(%d)\n", methodID);
		}
		return 0;
	}
}

int GetArrayLength(void *env, void *array) {
	dlog("GetArrayLength returned %d\n", *(int *)array);
	return *(int *)array;
}

const char* pstr = NULL;
int prefix = 1;
const char* (* lua50_tostring)(void *L, int index);
void (* lua50_pushstring)(void *L, const char *s);
void (* lua50_pushlstring)(void *L, const char *s, int len);

int nextSegment(void *L) {
	const char *str = lua50_tostring(L, 1);
	if (!str) {
		if (pstr && strncmp(pstr, "hud.b", 5) == 0) {
			const char *p = strrchr(pstr, 'b');
			lua50_pushstring(L, p + 1);
			return 1;
		}
		return 0;
	}

	pstr = str;

	const char *p = strchr(str, '$');
	if (!p) {
		return 0;
	}

	if (prefix) {
		lua50_pushlstring(L, str, p - str);
	} else {
		lua50_pushstring(L, p + 1);
	}
	prefix = !prefix;
	return 1;
}

void patch_game(void) {
	hook_addr(so_symbol(&main_mod, "S3DClient_InstallCurrentUserEventHook"), (uintptr_t)&ret0);

	lua50_tostring = (void *)so_symbol(&main_mod, "lua50_tostring");
	lua50_pushstring = (void *)so_symbol(&main_mod, "lua50_pushstring");
	lua50_pushlstring = (void *)so_symbol(&main_mod, "lua50_pushlstring");

	hook_addr(main_mod.text_base + 0x27CEC4, (uintptr_t)&nextSegment);
}

void SplashRender() {
	GLuint splash_tex;

	splash_img img = get_splashscreen();
	glGenTextures(1, &splash_tex);
	glBindTexture(GL_TEXTURE_2D, splash_tex	);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.buf);
	free(img.buf);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glOrtho(0.0f, 960.0f, 544.0f, 0.0f, 0.0f, 1.0f);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();

	float verts[] = {0.0f, 0.0f, 0.0f, 544.0f, 960.0f, 0.0f, 960.0, 544.0f};
	float texs[] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f};

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, verts);
	glTexCoordPointer(2, GL_FLOAT, 0, texs);
	glBindTexture(GL_TEXTURE_2D, splash_tex);
	glEnable(GL_TEXTURE_2D);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	vglSwapBuffers(GL_FALSE);
}

void *pthread_main(void *arg) {
	int (* JNI_OnLoad) (void *vm) = (void *)so_symbol(&main_mod, "JNI_OnLoad");
	int (* engineInitialize) (void *env) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineInitialize");
	int (* engineRunOneFrame) (void *env) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineRunOneFrame");
	int (* engineDidPassFirstFrame) (void *env) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineDidPassFirstFrame");
	int (* engineSetSystemVersion) (void *env, void *obj, char *ver) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineSetSystemVersion");
	int (* engineSetDirectories) (void *env, void *obj, char *cache_path, char *home_path, char *pack_path) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineSetDirectories");
	int (* enginePause) (void *env, void *obj, int pause) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_enginePause");
	int (* engineTouch) (void *env, void *obj, int state, float x, float y, int state2, float x2, float y2, int state3, float x3, float y3, int state4, float x4, float y4, int state5, float x5, float y5) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineOnTouchesChange");
	int (* engineSurfaceCreated) () = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineOnSurfaceCreated");
	int (* engineSurfaceChanged) (void *env, void *obj, int width, int height) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineOnSurfaceChanged");
	int (* engineOnMouseMove) (void *env, void *obj, float x, float y) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineOnMouseMove");
	int (* engineOnMouseUp) (void *env, void *obj, float x, float y) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineOnMouseButtonUp");
	int (* engineOnMouseDown) (void *env, void *obj, float x, float y) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineOnMouseButtonDown");
	int (* engineOnDeviceMove) (void *env, void *obj, float x, float y, float z) = (void *)so_symbol(&main_mod, "Java_com_blueshadowgames_naught2_S3DRenderer_engineOnDeviceMove");

	sceClibPrintf("JNI_OnLoad\n");
	JNI_OnLoad(fake_vm);

	engineSetSystemVersion(fake_env, NULL, "v.1.0-vita");
	engineSetDirectories(fake_env, NULL, "ux0:data/naught2", "ux0:data/naught2", "ux0:data/naught2");

	engineSurfaceCreated();
	engineSurfaceChanged(fake_env, NULL, SCREEN_W, SCREEN_H);

	sceClibPrintf("engineInitialize\n");
	engineInitialize(fake_env);

	enginePause(fake_env, NULL, 0);

	SplashRender();

	sceClibPrintf("Entering main loop\n");

	int lastX = -1, lastY = -1;

	sceMotionStartSampling();
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
	sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);

	for (;;) {
		SceTouchData touch;
		sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

		int ts[5];
		float tx[5];
		float ty[5];

		for (int j = 0; j < 6; j++) {
			if (j == 0) {
				if (touch.reportNum) {
					int x = (int)(touch.report[0].x * 0.5f);
					int y = (int)(touch.report[0].y * 0.5f);
					if (lastX == -1 || lastY == -1) {
						engineOnMouseDown(fake_env, NULL, (float)x, (float)y);
					} else {
						engineOnMouseMove(fake_env, NULL, (float)x, (float)y);
					}
					lastX = x;
					lastY = y;
				} else {
					engineOnMouseUp(fake_env, NULL, (float)lastX, (float)lastY);
					lastX = lastY = -1;
				}
			} else {
				int i = j - 1;
				if (i < touch.reportNum) {
					ts[i] = 1;
					tx[i] = touch.report[i].x * 0.5f;
					ty[i] = touch.report[i].y * 0.5f;
				} else {
					ts[i] = 0;
					tx[i] = 0.0f;
					ty[i] = 0.0f;
				}
			}
		}
		engineTouch(fake_env, NULL, ts[0], tx[0], ty[0], ts[1], tx[1], ty[1], ts[2], tx[2], ty[2], ts[3], tx[3], ty[3], ts[4], tx[4], ty[4]);

		SceMotionState motion;
		sceMotionGetState(&motion);
		engineOnDeviceMove(fake_env, NULL, -1 * motion.acceleration.x, -1 * motion.acceleration.y, motion.acceleration.z);

		if (engineRunOneFrame(fake_env)) {
			if (engineDidPassFirstFrame(fake_env)) {
				vglSwapBuffers(GL_FALSE);
			}
		} else {
			exit(0);
			break;
		}
	}

	return NULL;
}

int main(int argc, char *argv[]) {
	SceAppUtilInitParam init_param;
	SceAppUtilBootParam boot_param;
	memset(&init_param, 0, sizeof(SceAppUtilInitParam));
	memset(&boot_param, 0, sizeof(SceAppUtilBootParam));
	sceAppUtilInit(&init_param, &boot_param);

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	if (check_kubridge() < 0) {
		fatal_error("Error kubridge.skprx is not installed.");
	}

	if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx")) {
		fatal_error("Error libshacccg.suprx is not installed.");
	}

	char fname[33];
	sprintf(data_path, "ux0:data/naught2");

	sceClibPrintf("Loading libS3DClient\n");
	sprintf(fname, "%s/libS3DClient.so", data_path);
	if (so_load(&main_mod, fname, LOAD_ADDRESS) < 0) {
		fatal_error("Error could not load %s.", fname);
	}
	so_relocate(&main_mod);
	so_resolve(&main_mod, default_dynlib, sizeof(default_dynlib), 0);

	vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
	vglSetParamBufferSize(6 * 1024 * 1024);
	vglInitWithCustomThreshold(0, SCREEN_W, SCREEN_H, 6 * 1024 * 1024, 0, 0, 0, SCE_GXM_MULTISAMPLE_4X);

	patch_game();
	so_flush_caches(&main_mod);
	so_initialize(&main_mod);

	memset(fake_vm, 'A', sizeof(fake_vm));
	*(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
	*(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;

	memset(fake_env, 'A', sizeof(fake_env));
	*(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
	*(uintptr_t *)(fake_env + 0x4C) = (uintptr_t)ret0; // PushLocalFrame
	*(uintptr_t *)(fake_env + 0x50) = (uintptr_t)ret0; // PopLocalFrame
	*(uintptr_t *)(fake_env + 0x54) = (uintptr_t)NewGlobalRef;
	*(uintptr_t *)(fake_env + 0x58) = (uintptr_t)DeleteGlobalRef;
	*(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)ret0; // DeleteLocalRef
	*(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
	*(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
	*(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
	*(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
	*(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
	*(uintptr_t *)(fake_env + 0xC8) = (uintptr_t)CallIntMethodV;
	*(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
	*(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
	*(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)GetBooleanField;
	*(uintptr_t *)(fake_env + 0x190) = (uintptr_t)GetIntField;
	*(uintptr_t *)(fake_env + 0x198) = (uintptr_t)GetFloatField;
	*(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
	*(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
	*(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
	*(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
	*(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallStaticLongMethodV;
	*(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallStaticFloatMethodV;
	*(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
	*(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
	*(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
	*(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
	*(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
	*(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
	*(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0; // ReleaseStringUTFChars
	*(uintptr_t *)(fake_env + 0x2AC) = (uintptr_t)GetArrayLength;
	*(uintptr_t *)(fake_env + 0x2B4) = (uintptr_t)GetObjectArrayElement;
	*(uintptr_t *)(fake_env + 0x35C) = (uintptr_t)ret0; // RegisterNatives
	*(uintptr_t *)(fake_env + 0x36C) = (uintptr_t)GetJavaVM;
	*(uintptr_t *)(fake_env + 0x374) = (uintptr_t)GetStringUTFRegion;

	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 512 * 1024);
	pthread_create(&t, &attr, pthread_main, NULL);

	return sceKernelExitDeleteThread(0);
}
