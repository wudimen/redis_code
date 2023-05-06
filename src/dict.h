/*
   	dict中储存着	 dictEntry** 的二维数组 ：第一维：key经过hashfunction处理后的64位数据 求余容器长度后	的下标；
   											第二维：		储存着dictentry的地址，以链表的方式储存这与之冲突的数据
   							要使用两个单元存储是因为：容器扩容或缩容时，要分治处理庞大的数据，rehash时需使用备份的单元
   									<--after know-->：	dictrehash时将table[1]作为新的容器（一般时使用table[0]），开始rehash时从0开始一个一个rehash（可中断），
   															将rehash后的值装进新的容器(table[1])里，等到全部的table[0]的值都rehash完成，就把table[1]的地址复制到table[0]中并重置table[1]，完成复制

   	dict扩容与缩容时要对数据进行rehash放在新的dict中，要使用分治来处理数据，
   		因为redis时单线程的，如果一次性将全部数据都rehash的话，就会严重小寒cpu的资源，导致redis-server有一点卡顿，影响性能；
   		
   		解决方法：在增删改查dict时，会顺便rehash数据；
   					在redis-server空闲时，会启用定时器：在1ms时间内对dict进行rehash
*/



/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __DICT_H
#define __DICT_H

#include "mt19937-64.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define DICT_OK 0
#define DICT_ERR 1

typedef struct dictEntry dictEntry; /* opaque */		// 保存着key-value结构与next指针	---->>    dict数据节点

typedef struct dict dict;								// 字典结构（主要内容为两个dictEntry数组）

typedef struct dictType {																		// 处理dict的函数合集
    uint64_t (*hashFunction)(const void *key);								// hash函数
    void *(*keyDup)(dict *d, const void *key);								// 复制key的操作
    void *(*valDup)(dict *d, const void *obj);								// 复制val
    int (*keyCompare)(dict *d, const void *key1, const void *key2);			// 比较两个key是否相等					
    void (*keyDestructor)(dict *d, void *key);								// 释放key
    void (*valDestructor)(dict *d, void *obj);								// 释放val
    int (*expandAllowed)(size_t moreMem, double usedRatio);					// 是否允许拓展		
    /* Flags */
    /* The 'no_value' flag, if set, indicates that values are not used, i.e. the
     * dict is a set. When this flag is set, it's not possible to access the
     * value of a dictEntry and it's also impossible to use dictSetKey(). Entry
     * metadata can also not be used. */
    unsigned int no_value:1;													//这个dict装的是否没有value
    /* If no_value = 1 and all keys are odd (LSB=1), setting keys_are_odd = 1
     * enables one more optimization: to store a key without an allocated
     * dictEntry. */
    unsigned int keys_are_odd:1;												// ---- ？ ----<针对no_value=1的情况>
    /* TODO: Add a 'keys_are_even' flag and use a similar optimization if that
     * flag is set. */

    /* Allow each dict and dictEntry to carry extra caller-defined metadata. The
     * extra memory is initialized to 0 when allocated. */
    size_t (*dictEntryMetadataBytes)(dict *d);									// 获得dictEntryMetadata的大小
    size_t (*dictMetadataBytes)(void);											// 获得dictMetadata的大小
    /* Optional callback called after an entry has been reallocated (due to
     * active defrag). Only called if the entry has metadata. */
    void (*afterReplaceEntry)(dict *d, dictEntry *entry);						// 替换了entry之后执行的操作
} dictType;

#define DICTHT_SIZE(exp) ((exp) == -1 ? 0 : (unsigned long)1<<(exp))		// 1 << exp
#define DICTHT_SIZE_MASK(exp) ((exp) == -1 ? 0 : (DICTHT_SIZE(exp))-1)		// 生成掩码

struct dict {
    dictType *type;																// 处理dict的函数集合

    dictEntry **ht_table[2];													// ** dist的俩个存储单元 2 * &&(key--value)
    																			// 两个单元的目的：当第一个单元rehashing的时候，可以使用第二个继续储存	---->> 第二个作为新的容器，用于增容或缩容
    																			// 每个entry是：一维下标：key按照hashfunction处理后的索引
    																			//        		二维：	key处理后的索引可能一样，以链表的形式来储存相同的索引的值，这里存储的时dictEntry的指针
    unsigned long ht_used[2];													// ht_table中分别储存了的键值对数量

    long rehashidx; /* rehashing not in progress if rehashidx == -1 */			// 是否rehash（-1：不rehash）---->>  rehash完成的数量

    /* Keep small vars at end for optimal (minimal) struct padding */
    int16_t pauserehash; /* If >0 rehashing is paused (<0 indicates coding error) */	// 是否暂停rehash（ >0 ：rehash暂停）	---->> 是否允许rehash
    signed char ht_size_exp[2]; /* exponent of size. (size = 1<<exp) */			// ---- ht_used 中两个数的位数（2的指数）eg：9=1001 --> 4位

    void *metadata[];           /* An arbitrary number of bytes (starting at a		// ----？----
                                 * pointer-aligned address) of size as defined
                                 * by dictType's dictEntryBytes. */
};

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {														// dict迭代器
    dict *d;																// dict本体
    long index;																// 下标
    int table, safe;														// 第几个表       /   dict内容是否安全（是否允许进行除了next的其他操作）
    dictEntry *entry, *nextEntry;											// 当前的地址与下一个地址（下一个地址是为了方便取下一地址）					
    /* unsafe iterator fingerprint for misuse detection. */													
    unsigned long long fingerprint;											// 
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
typedef void *(dictDefragAllocFunction)(void *ptr);
typedef struct {
    dictDefragAllocFunction *defragAlloc; /* Used for entries etc. */
    dictDefragAllocFunction *defragKey;   /* Defrag-realloc keys (optional) */
    dictDefragAllocFunction *defragVal;   /* Defrag-realloc values (optional) */
} dictDefragFunctions;

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_EXP      2
#define DICT_HT_INITIAL_SIZE     (1<<(DICT_HT_INITIAL_EXP))				// dict初始化大小

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) do {                     \				// 释放d（dict）中的 entry（dictEntry）中的value
    if ((d)->type->valDestructor)                      \
        (d)->type->valDestructor((d), dictGetVal(entry)); \
   } while(0)

#define dictFreeKey(d, entry) \											// 释放d中的entry的key
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d), dictGetKey(entry))

#define dictCompareKeys(d, key1, key2) \								// 比较key1与key2是否相同
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d), key1, key2) : \
        (key1) == (key2))

#define dictEntryMetadataSize(d) ((d)->type->dictEntryMetadataBytes     \			// 返回d中的entry的mateData的数量
                                  ? (d)->type->dictEntryMetadataBytes(d) : 0)
#define dictMetadataSize(d) ((d)->type->dictMetadataBytes               \			// 返回d的mateData的数量
                             ? (d)->type->dictMetadataBytes() : 0)

#define dictHashKey(d, key) ((d)->type->hashFunction(key))										// 调用typ中的hashFunction方法	---->>    对key进行hash处理
#define dictSlots(d) (DICTHT_SIZE((d)->ht_size_exp[0])+DICTHT_SIZE((d)->ht_size_exp[1]))		// 计算dict的基数（两个entry的数量最高值之和）
#define dictSize(d) ((d)->ht_used[0]+(d)->ht_used[1])								// 计算dict两个entry储存的数量之和
#define dictIsRehashing(d) ((d)->rehashidx != -1)									// 是否允许rehash	---->>    是否正在rehash
#define dictPauseRehashing(d) ((d)->pauserehash++)									// d中的pauserehash加一
#define dictResumeRehashing(d) ((d)->pauserehash--)									// d中的pauserehash减一

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff
#define randomULong() ((unsigned long) genrand64_int64())
#else
#define randomULong() random()														// 产生随机数
#endif

typedef enum {
    DICT_RESIZE_ENABLE,																// 允许resize
    DICT_RESIZE_AVOID,																// 避免resize
    DICT_RESIZE_FORBID,																// 禁止resize
} dictResizeEnable;															//是否允许resize

/* API */
dict *dictCreate(dictType *type);													// 根据type创建一个dict并返回
int dictExpand(dict *d, unsigned long size);										// 扩大d的容量至大于size的最小2^n（防止多次申请内存）---（不管有没有申请到内存都返回OK	）
int dictTryExpand(dict *d, unsigned long size);										// 扩大d的容量至大于size的最小2^n（防止多次申请内存）----（没有申请到内存就返回ERR）
void *dictMetadata(dict *d);														// 返回d的metadata地址
int dictAdd(dict *d, void *key, void *val);											// 增加key-val到d的对应位置
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);					// 添加key到d的对应位置（existing：是否已有key）								
void *dictFindPositionForInsert(dict *d, const void *key, dictEntry **existing);					// 查找d中是否已经存在key，若存在则把entry地址赋给existing，否则返回可用于储存key对应键值对的entry地址					
dictEntry *dictInsertAtPosition(dict *d, void *key, void *position);								// 在position是添加key			
dictEntry *dictAddOrFind(dict *d, void *key);														// 将key插入到d的对应位置	，如果已经存在的话返回已有的key所在的dictentry地址
int dictReplace(dict *d, void *key, void *val);											// 将key-value插入到d的对应位置，如果已经存在的话，将原始value替换成新的val
int dictDelete(dict *d, const void *key);												// 删除key对应的链表节点
dictEntry *dictUnlink(dict *d, const void *key);										// 仅从d中断开key的连接，不释放该段内存	
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);										// 释放dictentry里的key/value的内存	
dictEntry *dictTwoPhaseUnlinkFind(dict *d, const void *key, dictEntry ***plink, int *table_index);					// 找到key对应的节点位置，并使阻碍rehash的参数加一（plink：节点地址，table_index：节点所在的单元数(优先显示后面单元)	)					
void dictTwoPhaseUnlinkFree(dict *d, dictEntry *he, dictEntry **plink, int table_index);							// 直接释放he里的内容内存，并使阻碍rehash的参数减一
void dictRelease(dict *d);																// 删除dict中的两个dictentry单元的内容，与释放d的内容
dictEntry * dictFind(dict *d, const void *key);											// 返回d中key对应的节点
void *dictFetchValue(dict *d, const void *key);											// 返回d中key对应的节点中的value
int dictResize(dict *d);																// 将dict中dictentry的容量扩大至最小的2^n
void dictSetKey(dict *d, dictEntry* de, void *key);										// 将de的key设置为key	
void dictSetVal(dict *d, dictEntry *de, void *val);										// 将de的val值设置val
void dictSetSignedIntegerVal(dictEntry *de, int64_t val);								// 将de的s64值设置val			
void dictSetUnsignedIntegerVal(dictEntry *de, uint64_t val);							// 将de的v64值设置val					
void dictSetDoubleVal(dictEntry *de, double val);										// 将de的d值设置val		
int64_t dictIncrSignedIntegerVal(dictEntry *de, int64_t val);							// 将de的s64值加val				
uint64_t dictIncrUnsignedIntegerVal(dictEntry *de, uint64_t val);						// 将de的u64值加val					
double dictIncrDoubleVal(dictEntry *de, double val);									// 将de的d值加val		
void *dictEntryMetadata(dictEntry *de);													// 获取de的metadata
void *dictGetVal(const dictEntry *de);													// 获取de的val值
void *dictGetKey(const dictEntry *de);													// 获取de的key
int64_t dictGetSignedIntegerVal(const dictEntry *de);									// 获取de的s64值	
uint64_t dictGetUnsignedIntegerVal(const dictEntry *de);								// 获取de的d64值				
double dictGetDoubleVal(const dictEntry *de);											// 获取de的d值	
double *dictGetDoubleValPtr(dictEntry *de);												// 获取de的d值的地址
size_t dictMemUsage(const dict *d);											// 获取dict中dictentry使用的内存情况
size_t dictEntryMemUsage(void);												// 获取一个dictEntry的内存大小						
dictIterator *dictGetIterator(dict *d);										// 创造一个指向d的迭代器							
dictIterator *dictGetSafeIterator(dict *d);									// 创造一个指向d/safe=1的迭代器							
void dictInitIterator(dictIterator *iter, dict *d);							// 为iter初始化内容										
void dictInitSafeIterator(dictIterator *iter, dict *d);						// 为iter初始化内容	且safe设置为1											
void dictResetIterator(dictIterator *iter);									//初始化迭代器（阻碍rehash的参数-1）								
dictEntry *dictNext(dictIterator *iter);									// 获取iter的下一个对象，如果iter内没有内容的话取第一个表中的第一个值的头节点（如果之前iter没有指向的话pauserehash需加一）								
void dictReleaseIterator(dictIterator *iter);								// 重置iter并释放其内存									
dictEntry *dictGetRandomKey(dict *d);										// 随机返回一个节点							
dictEntry *dictGetFairRandomKey(dict *d);									// 先用 dictGetSomeKeys 再用 dictGetRandomKey 随机返回一个节点								
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);				// 从d中随机取出最多count个可连续节点，返回实际获取的节点												
void dictGetStats(char *buf, size_t bufsize, dict *d);						// 将dict的第一个单元的信息打印到buf中（如果正在rehash的话打印第2个）											
uint64_t dictGenHashFunction(const void *key, size_t len);						// <UNKNOW>											
uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len);						// <UNKNOW>													
void dictEmpty(dict *d, void(callback)(dict*));									// 清除d中两个单元的所有内容								
void dictSetResizeEnabled(dictResizeEnable enable);								// 设置dict_can_resize为enable									
int dictRehash(dict *d, int n);													// 将旧的dictentry找到新的位置并放入到table[1]中				
int dictRehashMilliseconds(dict *d, int ms);									// 持续 dictRehash 1ms								
void dictSetHashFunctionSeed(uint8_t *seed);									// 设置dict_hash_function_seed为seed								
uint8_t *dictGetHashFunctionSeed(void);											// 返回dict_hash_function_seed						
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata);			// 对d中v后面的节点读进行fn操作														
unsigned long dictScanDefrag(dict *d, unsigned long v, dictScanFunction *fn, dictDefragFunctions *defragfns, void *privdata);				// 对d中v后面的节点里的key/value都执行defragfns后，对节点进行fn操作（rehash的话对两个单元都进行操作）															
uint64_t dictGetHash(dict *d, const void *key);									// 获取key的hash值								
dictEntry *dictFindEntryByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);					// 根据hash值与dictEntry的key地址指针查找对应的dictEntry地址												


#ifdef REDIS_TEST
int dictTest(int argc, char *argv[], int flags);																	
#endif

#endif /* __DICT_H */
