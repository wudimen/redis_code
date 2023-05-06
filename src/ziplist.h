/*
	ziplist（压缩链表）：
		可以储存字符串与整形；
		可能会产生连锁更新：
			因为zlentry中储存上一个数据的时候小于254时使用1字节，大于等于时使用5字节，若当前数据长度全是253且要插入一个大于254的数据的话，
			因为这个数大于254了因此要重新申请一块5字节的内存，后面的储存的长度也会变成257（253+4），大于了254，又要重新申请内存，因此后面全部的内存都要重新申请！
	zlEnrtry：
		用于存储数据，主要包含encoding与prevlen；
			用prevlen表示前一个内容的长度，因此可以从后往前遍历（ziplist中可以获得最后数据的地址）；
				如何储存前一节点的长度：
					若长度小于254，则使用1字节存储，否则使用5字节存储（若使用5字节，第一字节始终为254）
			用encoding的前两位来区分是什么类型（11xxxxxx是字符串，其余为数字）；
				encoding编码方式：
				前两位是11：代表内容是整数（有6种编码方式）：
					1.整数为uint16_t：1100 0000 + 2字节的整数
					2.整数为uint32_t：1101 0000 + 4字节的整数
					3.整数为uint64_t：1110 0000 + 8字节的整数
					4.整数为3字节的整数：1111 0000 + 2字节的整数
					5.整数为1字节的整数：1111 1110 + 1字节的整数
					6.整数为0-12：1111 0001 ~ 1111 1101
				前两位是00~10：代表内容是字符串（有3种编码方式）：
					1.长度小于6比特的字符串：00xx xxxx（6比特的字符串长度） + 字符串
					2.长度小于14比特的字符串：01xx xxxx             xxxx xxxx（14比特的字符串长度） + 字符串
					3.长度大于14比特的字符串：1000 0000 + 4字节的字符串长度 + 字符串


*/


/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef _ZIPLIST_H
#define _ZIPLIST_H

#define ZIPLIST_HEAD 0
#define ZIPLIST_TAIL 1		/*默认从尾部开始插入*/

/* Each entry in the ziplist is either a string or an integer. */
typedef struct {
    /* When string is used, it is provided with the length (slen). */
    unsigned char *sval;
    unsigned int slen;
    /* When integer is used, 'sval' is NULL, and lval holds the value. */
    long long lval;
} ziplistEntry;									// 可以储存带长度的char* 或 long long类型的数据

unsigned char *ziplistNew(void);																			// 申请内存/初始化并返回
unsigned char *ziplistMerge(unsigned char **first, unsigned char **second);									// 将first与second合并（存在着某种规则----？----）										
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where);				// 将slen长度的s插入到z1的where处															
unsigned char *ziplistIndex(unsigned char *zl, int index);													// 返回第index个节点的地址（负数代表倒数第index个）						
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p);											// 将p移到下一节点								
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p);											// 将p移到前一节点								
unsigned int ziplistGet(unsigned char *p, unsigned char **sval, unsigned int *slen, long long *lval);		// 获取p中所含的内容																	
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen);					// 将slen长度的s插入到z1的p位置上														
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p);											// 删除z1中p后面的一个节点								
unsigned char *ziplistDeleteRange(unsigned char *zl, int index, unsigned int num);							// 删除z1中s后面的enum个节点												
unsigned char *ziplistReplace(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen);			// 将p的内容替换成s（entry容器也要变化）																
unsigned int ziplistCompare(unsigned char *p, unsigned char *s, unsigned int slen);							// 判断p与s的内容是否相等												
unsigned char *ziplistFind(unsigned char *zl, unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip);				// 每隔skip个节点比对一下，查找值为vstr的节点并返回															
unsigned int ziplistLen(unsigned char *zl);																	// 获取z1的内容数		
size_t ziplistBlobLen(unsigned char *zl);																	// 获取z1占用的字节数		
void ziplistRepr(unsigned char *zl);																		// 打印z1的信息 以及 所有节点信息	
typedef int (*ziplistValidateEntryCB)(unsigned char* p, unsigned int head_count, void* userdata);																			
int ziplistValidateIntegrity(unsigned char *zl, size_t size, int deep,										// 验证z1是否正常（deep为1时还要验证里面的entry是否符合规则）									
                             ziplistValidateEntryCB entry_cb, void *cb_userdata);																			
void ziplistRandomPair(unsigned char *zl, unsigned long total_count, ziplistEntry *key, ziplistEntry *val);				// 随机获取小于2*total_count的连续的两个节点的内容															
void ziplistRandomPairs(unsigned char *zl, unsigned int count, ziplistEntry *keys, ziplistEntry *vals);					// 随机生成count个可重复的偶数节点，并把key与val保存在keys与vals中（因为要取一对，下一个直接取这个加一的值就行了）														
unsigned int ziplistRandomPairsUnique(unsigned char *zl, unsigned int count, ziplistEntry *keys, ziplistEntry *vals);					// 随机生成count个不可重复的偶数节点，并把key与val保存在keys与vals中														
int ziplistSafeToAdd(unsigned char* zl, size_t add);														// 查看添加add大小的内容后z1是否满足条件					


#ifdef REDIS_TEST
int ziplistTest(int argc, char *argv[], int flags);
#endif

#endif /* _ZIPLIST_H */
