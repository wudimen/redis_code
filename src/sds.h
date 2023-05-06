/*
	sds为一个redis自定义的字符串类型（不是用char*）；
	sds的外部结构有 sdshdr(8-64) ， 里面包含了len（sds已使用的长度），alloc（sds的总长度）， flags（标识sdshdr的类型）， buf（sds本体）；
	sdshdr使用了 __attribute__ ((__packed__))    来设置sdshdr的属性，让编译器不要为其内存对齐，因此flag一定在sds本体的前一个字节（可以通过s[-1]来获取sdshdr的类型，从而获得其hdr指针，进而进行操作
	
	sds的优点：
		1.可减少为char*扩容的次数（sds后面的字符被削减后不会立即释放，而是变为剩余可用内存）
		2.可直接获得字符串的长度（sdshdr包装了len）
		3.二进制安全（char*根据'\0'判断字符串是否结束，因此不能用于储存二进制数据（因为要是遇到了'\0'就会被截断），而sds是根据len来判断字符串是否结束的，因此可以用来储存二进制文件；

	sds得以储存变长字符长的基础是 零长数组 （可以用char*替代）；
	零长数组：
		1.只能在gnu c（gcc）下使用；
		2.在编译时统计内存，且内存为0；
		3.要在结构体中使用的话，一定要放在结构体最后（否则为他申请内存时会覆盖后面的元素）
		4.当结构体中有一个时有时无的数据时，可以使用零长数组来实现；
*/


/* SDSLib 2.0 -- A C dynamic strings library
 *
 * Copyright (c) 2006-2015, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2015, Oran Agra
 * Copyright (c) 2015, Redis Labs, Inc
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __SDS_H
#define __SDS_H

#define SDS_MAX_PREALLOC (1024*1024)
extern const char *SDS_NOINIT;

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

typedef char *sds;

/* Note: sdshdr5 is never used, we just access the flags byte directly.
 * However is here to document the layout of type 5 SDS strings. */
struct __attribute__ ((__packed__)) sdshdr5 {				// __attribute__ ((__packed__)) ： 告诉编译器不要字节对齐
    unsigned char flags; /* 3 lsb of type, and 5 msb of string length */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr8 {
    uint8_t len; /* used */
    uint8_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr16 {
    uint16_t len; /* used */
    uint16_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr32 {
    uint32_t len; /* used */
    uint32_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr64 {
    uint64_t len; /* used */
    uint64_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

#define SDS_TYPE_5  0
#define SDS_TYPE_8  1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4
#define SDS_TYPE_MASK 7			// flag的后3bit用于标识类型，因此用掩码0000 0111可以获得flag对应的类型
#define SDS_TYPE_BITS 3			// flag后3位标识类型
#define SDS_HDR_VAR(T,s) struct sdshdr##T *sh = (void*)((s)-(sizeof(struct sdshdr##T)));		// char buf[]：sizeof不会计算这段内存（----）
#define SDS_HDR(T,s) ((struct sdshdr##T *)((s)-(sizeof(struct sdshdr##T))))
#define SDS_TYPE_5_LEN(f) ((f)>>SDS_TYPE_BITS)


// 从sds的qian1个字节获取sds类型，之后根据类型获得sds的已使用长度
static inline size_t sdslen(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->len;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->len;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->len;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->len;
    }
    return 0;
}

// 根据sds，获得sds的剩余内存
static inline size_t sdsavail(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5: {
            return 0;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            return sh->alloc - sh->len;
        }
    }
    return 0;
}


// 设置sds的长度
static inline void sdssetlen(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len = newlen;
            break;
    }
}

// sds的长度增加inc
static inline void sdsinclen(sds s, size_t inc) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                unsigned char newlen = SDS_TYPE_5_LEN(flags)+inc;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len += inc;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len += inc;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len += inc;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len += inc;
            break;
    }
}


// 获取sds总共的长度
/* sdsalloc() = sdsavail() + sdslen() */
static inline size_t sdsalloc(const sds s) {			// 获取s的alloc
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->alloc;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->alloc;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->alloc;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->alloc;
    }
    return 0;
}

// 设置sds总共的长度
static inline void sdssetalloc(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            /* Nothing to do, this type has no total allocation info. */
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->alloc = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->alloc = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->alloc = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->alloc = newlen;
            break;
    }
}

sds sdsnewlen(const void *init, size_t initlen);			// 申请一块内存，用于保存字符串init与sdshdr（长度为initlen，总长度为zmalloc申请的长度）
sds sdstrynewlen(const void *init, size_t initlen);			// 尝试申请一块内存，用于保存字符串init与sdshdr
sds sdsnew(const char *init);								// 申请一块内存用于储存init字符串（sdshdr格式）
sds sdsempty(void);											// 申请一块空的内存（sdshdr格式，内容为""）
sds sdsdup(const sds s);									// 复制一份s
void sdsfree(sds s);										// 释放sds以及前面的sdshdr内存
sds sdsgrowzero(sds s, size_t len);							// 将s的长度扩充至长度为len（多分配一倍内存）
sds sdscatlen(sds s, const void *t, size_t len);			// 将s的长度扩充len，且扩充的长度用t的前len为填充（多分配一倍内存）
sds sdscat(sds s, const char *t);							// 在s的后面拼接上t
sds sdscatsds(sds s, const sds t);							// 在s的后面拼接上t
sds sdscpylen(sds s, const char *t, size_t len);			// 将s的内容设置为t的前n位
sds sdscpy(sds s, const char *t);							// 将s的内容设置为t

sds sdscatvprintf(sds s, const char *fmt, va_list ap);			// 将后面的参数以fmt的格式写入到s中
#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);				// 将后面的参数以fmt的格式写入到s中
#endif

sds sdscatfmt(sds s, char const *fmt, ...);					// 解析fmt与后面的参数，（%s/%S:char*  %i:int 		   %I:long	  %u:uint	%U:Ulong   %*:*）

sds sdstrim(sds s, const char *cset);						// 以cset截断s（找出最左与最右的cset下标，从这两处截断s）
void sdssubstr(sds s, size_t start, size_t len);			// 将s在start到start+len处截断（将s以start开始，长度为len的字符串移动到s中）
void sdsrange(sds s, ssize_t start, ssize_t end);			// 将s按照start-end截断（负数代表从尾部开始计算）
void sdsupdatelen(sds s);									// 将s的长度更新为s字符串的长度
void sdsclear(sds s);										// 将s赋值为"" 
int sdscmp(const sds s1, const sds s2);		 				// 比较s1与s2的大小
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count);			// 分割字符串s（将长度为len的字符串s中的 长度为seplen的字符串sep 分割出来，最前面的与最后一个后面全部的也压入进去）
void sdsfreesplitres(sds *tokens, int count);				// 释放sds数组的内存
void sdstolower(sds s);										// 将sds中的字母变成小写
void sdstoupper(sds s);										// 将sds中的字母变成大写
sds sdsfromlonglong(long long value);						// 将long long类型的数字转换成char*并装入到sds中
sds sdscatrepr(sds s, const char *p, size_t len);			// 将p的内容全部按照字符写入到s中（\开头， \结尾， 转义字符逆转义，'\n' --> "\\n"） 
sds *sdssplitargs(const char *line, int *argc);				// 将line按照\n,\r,\0进行切割，将结果存入sds*中并返回（遇到“”之间的转义字符需转义）
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen);			// 自定义映射长度为setlen的map（from->to），将s中from字符转换成to对应的字符
sds sdsjoin(char **argv, int argc, char *sep);				// 将argv的内容填入到sds中，用sep间隔，并返回
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen);		// 将argv的内容填入到sds中，用sep间隔，并返回
int sdsneedsrepr(const sds s);								// 查找s中是否含有" \ " \n \r \t \a \b 空格"" 等转义字符或不可打印字符

/* Callback for sdstemplate. The function gets called by sdstemplate
 * every time a variable needs to be expanded. The variable name is
 * provided as variable, and the callback is expected to return a
 * substitution value. Returning a NULL indicates an error.
 */
typedef sds (*sdstemplate_callback_t)(const sds variable, void *arg);
sds sdstemplate(const char *template, sdstemplate_callback_t cb_func, void *cb_arg);		// 将template中的内容添加到sds中（{}内的内容需经过cb_func处理）

/* Low level functions exposed to the user API */
sds sdsMakeRoomFor(sds s, size_t addlen);						// 为s多分配addlen的内存（多分配总内存的一倍）
sds sdsMakeRoomForNonGreedy(sds s, size_t addlen);			// 为s多分配addlen的内存
void sdsIncrLen(sds s, ssize_t incr);							// 增加s的长度。（负数则减短）并在结尾位置加上'\0'
sds sdsRemoveFreeSpace(sds s, int would_regrow);				// 移除sds多余的空间（alloc-len）
sds sdsResize(sds s, size_t size, int would_regrow);			// 将s变成size大小（可变大可变小，type随之变化，would_regroew：是否将sd_type5变成sds_type8)
size_t sdsAllocSize(sds s);										// 返回s要申请的总的内存（包括前置部分）
void *sdsAllocPtr(sds s);										// 返回s前置内存的地址

/* Export the allocator used by SDS to the program using SDS.
 * Sometimes the program SDS is linked to, may use a different set of
 * allocators, but may want to allocate or free things that SDS will
 * respectively free or allocate. */
void *sds_malloc(size_t size);						// 申请size大小的内存并返回
void *sds_realloc(void *ptr, size_t size);			// 为ptr重新申请size大小的内存
void sds_free(void *ptr);							// 释放ptr的内存

#ifdef REDIS_TEST
int sdsTest(int argc, char *argv[], int flags);			// 测试程序
#endif

#endif
