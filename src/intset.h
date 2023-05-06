/*
	intset使一个存储int型数据的'有序'存储结构，intset的每个单元的容量会随着is内容的需要而扩增

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

#ifndef __INTSET_H
#define __INTSET_H
#include <stdint.h>

typedef struct intset {
    uint32_t encoding;			// 存储单元大小
    uint32_t length;			// 存储数据的数量
    int8_t contents[];			// 零长数组：用于存储可能有可能无的数据
} intset;

intset *intsetNew(void);																	// 申请一块intset的内存，初始化并返回
intset *intsetAdd(intset *is, int64_t value, uint8_t *success);								// 	将value按照升序位置存入到is中（encoding不够则扩大encoding）							
intset *intsetRemove(intset *is, int64_t value, int *success);								// 如果有的话，删除value数据								
uint8_t intsetFind(intset *is, int64_t value);												// 查看is中是否有value				
int64_t intsetRandom(intset *is);															// 随机返回is中的一个数据	
int64_t intsetMax(intset *is);																// 返回is的最大值
int64_t intsetMin(intset *is);																// 返回is的最小值
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value);								// 将is中第pos个数据存入value中，返回pos是否有效								
uint32_t intsetLen(const intset *is);														// 返回is的数据个数		
size_t intsetBlobLen(intset *is);															// 返回is所消耗的内存量	
int intsetValidateIntegrity(const unsigned char *is, size_t size, int deep);				// 给出is消耗的总内存，验证is是否符合size大小的要求（deep==0：只验证is大小是否符合size要求； deep==1：在前面的基础上验证is的顺序有没有乱）											

#ifdef REDIS_TEST
int intsetTest(int argc, char *argv[], int flags);
#endif

#endif // __INTSET_H
