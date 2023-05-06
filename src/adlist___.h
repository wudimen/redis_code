/*
	以listNode的方式存储每一个节点，
			对于内容void*可能是指针的清空：使用了三个对于操作的函数 
				： void *(*dup)(void *ptr); void (*free)(void *ptr); int (*match)(void *ptr, void *key);
			用于操作void*的内容（如果是指针的话，不偏特化这些操作，会以浅拷贝的方式复制这些内容，会造成内存泄漏）
	使用 listIter 装 ListNode，
	list 就是以listNode为基础的链表；
	使用 AL_START_HEAD / AL_START_TAIL 代表遍历链表的方向（从 头往后 或 后往前 ）
	以及一系列对于list的操作函数；
*/


/* adlist.h - A generic doubly linked list implementation
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

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */

// listNode：前指针 / 后指针与 void*形式的内容（用于构建链表节点）
typedef struct listNode {
    struct listNode *prev;
    struct listNode *next;
    void *value;
} listNode;

// ListNode的迭代器，用于存储List的某一个节点（与方向：用于遍历-----）
typedef struct listIter {
    listNode *next;
    int direction;
} listIter;

// 不带头尾节点的链表
typedef struct list {
    listNode *head;
    listNode *tail;
    void *(*dup)(void *ptr);		// list中要复制内容的话，如果内容是个指针，则要深拷贝，这里装的就是拷贝内容的方式（默认为空：直接复制）
    void (*free)(void *ptr);		// 是否需要释放节点（如果是指针的话就要释放，不释放会造成内存泄漏）
    int (*match)(void *ptr, void *key);		// 比较函数，比较list中的两个值是否相等
    unsigned long len;
} list;

/* Functions implemented as macros */
#define listLength(l) ((l)->len)				// 获取list的数据量
#define listFirst(l) ((l)->head)
#define listLast(l) ((l)->tail)
#define listPrevNode(n) ((n)->prev)
#define listNextNode(n) ((n)->next)
#define listNodeValue(n) ((n)->value)

#define listSetDupMethod(l,m) ((l)->dup = (m))
#define listSetFreeMethod(l,m) ((l)->free = (m))
#define listSetMatchMethod(l,m) ((l)->match = (m))

#define listGetDupMethod(l) ((l)->dup)
#define listGetFreeMethod(l) ((l)->free)
#define listGetMatchMethod(l) ((l)->match)

/* Prototypes */
list *listCreate(void);			// 初始化一个空的list
void listRelease(list *list);			// 释放list（listEmpty+ zfree）
void listEmpty(list *list);			// 清空一个list
list *listAddNodeHead(list *list, void *value);			// 使用头插法插入节点
list *listAddNodeTail(list *list, void *value);			// 使用尾插法插入节点
list *listInsertNode(list *list, listNode *old_node, void *value, int after);			// 在list的某一个节点old_node后(after=1)/前(after=0)插入一个值为value的节点；
void listDelNode(list *list, listNode *node);				// 删除list中的一个节点
listIter *listGetIterator(list *list, int direction);				// 获取list中的受节点（根据AL_START_HEAD确定其实节点）
listNode *listNext(listIter *iter);				// 根据AL_START_HEAD获取iter的下一个节点
void listReleaseIterator(listIter *iter);				// 释放iterator的内存
list *listDup(list *orig);				// 使用尾插法将orig的内容复制且返回
listNode *listSearchKey(list *list, void *key);				// 查找list中的key，并返回位置
listNode *listIndex(list *list, long index);				// 返回list中下标为index的内容（负数代表从尾部开始查找）
void listRewind(list *list, listIter *li);					// 获取list的头节点
void listRewindTail(list *list, listIter *li);				// 获取list的尾节点
void listRotateTailToHead(list *list);						// 将list的尾节点变成头节点（只变这一个节点）
void listRotateHeadToTail(list *list);						// 将list的头节点变成尾节点（只变这一个节点）
void listJoin(list *l, list *o);				// 将o的链表接在l链表的后面

/* Directions for iterators */
#define AL_START_HEAD 0			// 从头部开始遍历list
#define AL_START_TAIL 1

#endif /* __ADLIST_H__ */
