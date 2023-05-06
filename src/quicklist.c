/* quicklist.c - A doubly linked list of listpacks
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must start the above copyright notice,
 *     this quicklist of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this quicklist of conditions and the following disclaimer in the
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

#include <stdio.h>
#include <string.h> /* for memcpy */
#include <limits.h>
#include "quicklist.h"
#include "zmalloc.h"
#include "config.h"
#include "listpack.h"
#include "util.h" /* for ll2string */
#include "lzf.h"
#include "redisassert.h"

#ifndef REDIS_STATIC
#define REDIS_STATIC static
#endif

/* Optimization levels for size-based filling.
 * Note that the largest possible limit is 64k, so even if each record takes
 * just one byte, it still won't overflow the 16 bit count field. */
static const size_t optimization_level[] = {4096, 8192, 16384, 32768, 65536};

/* packed_threshold is initialized to 1gb*/
static size_t packed_threshold = (1 << 30);

/* set threshold for PLAIN nodes, the real limit is 4gb */
#define isLargeElement(size) ((size) >= packed_threshold)

int quicklistisSetPackedThreshold(size_t sz) {
    /* Don't allow threshold to be set above or even slightly below 4GB */
    if (sz > (1ull<<32) - (1<<20)) {
        return 0;
    } else if (sz == 0) { /* 0 means restore threshold */
        sz = (1 << 30);
    }
    packed_threshold = sz;
    return 1;
}

/* Maximum size in bytes of any multi-element listpack.
 * Larger values will live in their own isolated listpacks.
 * This is used only if we're limited by record count. when we're limited by
 * size, the maximum limit is bigger, but still safe.
 * 8k is a recommended / default size limit */
#define SIZE_SAFETY_LIMIT 8192

/* Maximum estimate of the listpack entry overhead.
 * Although in the worst case(sz < 64), we will waste 6 bytes in one
 * quicklistNode, but can avoid memory waste due to internal fragmentation
 * when the listpack exceeds the size limit by a few bytes (e.g. being 16388). */
#define SIZE_ESTIMATE_OVERHEAD 8

/* Minimum listpack size in bytes for attempting compression. */
#define MIN_COMPRESS_BYTES 48

/* Minimum size reduction in bytes to store compressed quicklistNode data.
 * This also prevents us from storing compression if the compression
 * resulted in a larger size than the original data. */
#define MIN_COMPRESS_IMPROVE 8

/* If not verbose testing, remove all debug printing. */
#ifndef REDIS_TEST_VERBOSE
#define D(...)
#else
#define D(...)                                                                 \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __func__, __LINE__);                   \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
    } while (0)
#endif

/* Bookmarks forward declarations */
#define QL_MAX_BM ((1 << QL_BM_BITS)-1)
quicklistBookmark *_quicklistBookmarkFindByName(quicklist *ql, const char *name);
quicklistBookmark *_quicklistBookmarkFindByNode(quicklist *ql, quicklistNode *node);
void _quicklistBookmarkDelete(quicklist *ql, quicklistBookmark *bm);

/* Simple way to give quicklistEntry structs default values with one call. */
#define initEntry(e)                                                           \		// 初始化一个quicklistentry
    do {                                                                       \
        (e)->zi = (e)->value = NULL;                                           \
        (e)->longval = -123456789;                                             \
        (e)->quicklist = NULL;                                                 \
        (e)->node = NULL;                                                      \
        (e)->offset = 123456789;                                               \
        (e)->sz = 0;                                                           \
    } while (0)

/* Reset the quicklistIter to prevent it from being used again after
 * insert, replace, or other against quicklist operation. */
#define resetIterator(iter)                                                    \
    do {                                                                       \
        (iter)->current = NULL;                                                \
        (iter)->zi = NULL;                                                     \
    } while (0)

/* Create a new quicklist.
 * Free with quicklistRelease(). */
quicklist *quicklistCreate(void) {					// 初始化一个quicklist
    struct quicklist *quicklist;

    quicklist = zmalloc(sizeof(*quicklist));
    quicklist->head = quicklist->tail = NULL;
    quicklist->len = 0;
    quicklist->count = 0;
    quicklist->compress = 0;
    quicklist->fill = -2;
    quicklist->bookmark_count = 0;
    return quicklist;
}

#define COMPRESS_MAX ((1 << QL_COMP_BITS)-1)
void quicklistSetCompressDepth(quicklist *quicklist, int compress) {						// 设置压缩深度
    if (compress > COMPRESS_MAX) {
        compress = COMPRESS_MAX;
    } else if (compress < 0) {
        compress = 0;
    }
    quicklist->compress = compress;
}

#define FILL_MAX ((1 << (QL_FILL_BITS-1))-1)
void quicklistSetFill(quicklist *quicklist, int fill) {										// 设置fill
    if (fill > FILL_MAX) {
        fill = FILL_MAX;
    } else if (fill < -5) {
        fill = -5;
    }
    quicklist->fill = fill;
}

void quicklistSetOptions(quicklist *quicklist, int fill, int depth) {							// 设置depth与fill
    quicklistSetFill(quicklist, fill);
    quicklistSetCompressDepth(quicklist, depth);
}

/* Create a new quicklist with some default parameters. */
quicklist *quicklistNew(int fill, int compress) {												// 新建一个quicklist
    quicklist *quicklist = quicklistCreate();
    quicklistSetOptions(quicklist, fill, compress);
    return quicklist;
}

REDIS_STATIC quicklistNode *quicklistCreateNode(void) {											// 新建一个quickListNode
    quicklistNode *node;
    node = zmalloc(sizeof(*node));
    node->entry = NULL;
    node->count = 0;
    node->sz = 0;
    node->next = node->prev = NULL;
    node->encoding = QUICKLIST_NODE_ENCODING_RAW;
    node->container = QUICKLIST_NODE_CONTAINER_PACKED;
    node->recompress = 0;
    node->dont_compress = 0;
    return node;
}

/* Return cached quicklist count */
unsigned long quicklistCount(const quicklist *ql) { return ql->count; }						// 返回ql的数据项数量

/* Free entire quicklist. */
void quicklistRelease(quicklist *quicklist) {												// 释放一个quicklist
    unsigned long len;
    quicklistNode *current, *next;

    current = quicklist->head;
    len = quicklist->len;
    while (len--) {					// 删除quicklist每一个节点
        next = current->next;

        zfree(current->entry);		// 释放节点内容
        quicklist->count -= current->count;

        zfree(current);				// 释放节点本体

        quicklist->len--;
        current = next;
    }
    quicklistBookmarksClear(quicklist);		// 释放quicklist本体
    zfree(quicklist);
}

/* Compress the listpack in 'node' and update encoding details.
 * Returns 1 if listpack compressed successfully.
 * Returns 0 if compression failed or if listpack too small to compress. */
REDIS_STATIC int __quicklistCompressNode(quicklistNode *node) {								// 压缩节点
#ifdef REDIS_TEST
    node->attempted_compress = 1;
#endif
    if (node->dont_compress) return 0;

    /* validate that the node is neither
     * tail nor head (it has prev and next)*/
    assert(node->prev && node->next);

    node->recompress = 0;
    /* Don't bother compressing small values */
    if (node->sz < MIN_COMPRESS_BYTES)
        return 0;

    quicklistLZF *lzf = zmalloc(sizeof(*lzf) + node->sz);

    /* Cancel if compression fails or doesn't compress small enough */
    if (((lzf->sz = lzf_compress(node->entry, node->sz, lzf->compressed,
                                 node->sz)) == 0) ||
        lzf->sz + MIN_COMPRESS_IMPROVE >= node->sz) {
        /* lzf_compress aborts/rejects compression if value not compressible. */
        zfree(lzf);
        return 0;
    }
    lzf = zrealloc(lzf, sizeof(*lzf) + lzf->sz);
    zfree(node->entry);
    node->entry = (unsigned char *)lzf;
    node->encoding = QUICKLIST_NODE_ENCODING_LZF;
    return 1;
}

/* Compress only uncompressed nodes. */
#define quicklistCompressNode(_node)                                           \
    do {                                                                       \
        if ((_node) && (_node)->encoding == QUICKLIST_NODE_ENCODING_RAW) {     \		// 如果类型允许的话，对node进行lzf压缩
            __quicklistCompressNode((_node));                                  \
        }                                                                      \
    } while (0)

/* Uncompress the listpack in 'node' and update encoding details.
 * Returns 1 on successful decode, 0 on failure to decode. */
REDIS_STATIC int __quicklistDecompressNode(quicklistNode *node) {							// 解压缩节点
#ifdef REDIS_TEST
    node->attempted_compress = 0;
#endif
    node->recompress = 0;

    void *decompressed = zmalloc(node->sz);
    quicklistLZF *lzf = (quicklistLZF *)node->entry;
    if (lzf_decompress(lzf->compressed, lzf->sz, decompressed, node->sz) == 0) {
        /* Someone requested decompress, but we can't decompress.  Not good. */
        zfree(decompressed);
        return 0;
    }
    zfree(lzf);
    node->entry = decompressed;
    node->encoding = QUICKLIST_NODE_ENCODING_RAW;
    return 1;
}

/* Decompress only compressed nodes. */
#define quicklistDecompressNode(_node)                                         \			// 如果条件合适的话，解压缩节点
    do {                                                                       \
        if ((_node) && (_node)->encoding == QUICKLIST_NODE_ENCODING_LZF) {     \
            __quicklistDecompressNode((_node));                                \
        }                                                                      \
    } while (0)

/* Force node to not be immediately re-compressible */
#define quicklistDecompressNodeForUse(_node)                                   \			// 暂时解压缩节点
    do {                                                                       \
        if ((_node) && (_node)->encoding == QUICKLIST_NODE_ENCODING_LZF) {     \
            __quicklistDecompressNode((_node));                                \
            (_node)->recompress = 1;                                           \
        }                                                                      \
    } while (0)

/* Extract the raw LZF data from this quicklistNode.
 * Pointer to LZF data is assigned to '*data'.
 * Return value is the length of compressed LZF data. */
size_t quicklistGetLzf(const quicklistNode *node, void **data) {									// 获取压缩之后的节点内容，并返回其大小
    quicklistLZF *lzf = (quicklistLZF *)node->entry;
    *data = lzf->compressed;
    return lzf->sz;
}

#define quicklistAllowsCompression(_ql) ((_ql)->compress != 0)

/* Force 'quicklist' to meet compression guidelines set by compress depth.
 * The only way to guarantee interior nodes get compressed is to iterate
 * to our "interior" compress depth then compress the next node we find.
 * If compress depth is larger than the entire list, we return immediately. */
REDIS_STATIC void __quicklistCompress(const quicklist *quicklist,				// 如果符合压缩条件的话，对quiclist的node进行压缩，压缩深度为node->depth
                                      quicklistNode *node) {
    if (quicklist->len == 0) return;

    /* The head and tail should never be compressed (we should not attempt to recompress them) */
    assert(quicklist->head->recompress == 0 && quicklist->tail->recompress == 0);

    /* If length is less than our compress depth (from both sides),
     * we can't compress anything. */
    if (!quicklistAllowsCompression(quicklist) ||
        quicklist->len < (unsigned int)(quicklist->compress * 2))
        return;

#if 0
    /* Optimized cases for small depth counts */
    if (quicklist->compress == 1) {
        quicklistNode *h = quicklist->head, *t = quicklist->tail;
        quicklistDecompressNode(h);
        quicklistDecompressNode(t);
        if (h != node && t != node)
            quicklistCompressNode(node);
        return;
    } else if (quicklist->compress == 2) {
        quicklistNode *h = quicklist->head, *hn = h->next, *hnn = hn->next;
        quicklistNode *t = quicklist->tail, *tp = t->prev, *tpp = tp->prev;
        quicklistDecompressNode(h);
        quicklistDecompressNode(hn);
        quicklistDecompressNode(t);
        quicklistDecompressNode(tp);
        if (h != node && hn != node && t != node && tp != node) {
            quicklistCompressNode(node);
        }
        if (hnn != t) {
            quicklistCompressNode(hnn);
        }
        if (tpp != h) {
            quicklistCompressNode(tpp);
        }
        return;
    }
#endif

    /* Iterate until we reach compress depth for both sides of the list.a
     * Note: because we do length checks at the *top* of this function,
     *       we can skip explicit null checks below. Everything exists. */
    quicklistNode *forward = quicklist->head;
    quicklistNode *reverse = quicklist->tail;
    int depth = 0;
    int in_depth = 0;
    while (depth++ < quicklist->compress) {
        quicklistDecompressNode(forward);
        quicklistDecompressNode(reverse);

        if (forward == node || reverse == node)
            in_depth = 1;

        /* We passed into compress depth of opposite side of the quicklist
         * so there's no need to compress anything and we can exit. */
        if (forward == reverse || forward->next == reverse)
            return;

        forward = forward->next;
        reverse = reverse->prev;
    }

    if (!in_depth)
        quicklistCompressNode(node);

    /* At this point, forward and reverse are one node beyond depth */
    quicklistCompressNode(forward);
    quicklistCompressNode(reverse);
}

#define quicklistCompress(_ql, _node)                                          \		// 压缩_ql的_node节点
    do {                                                                       \
        if ((_node)->recompress)                                               \		// 暂时被解压缩了的话，重新压缩压缩__node
            quicklistCompressNode((_node));                                    \
        else                                                                   \
            __quicklistCompress((_ql), (_node));                               \		// 如果符合压缩条件的话，对_node进行压缩
    } while (0)

/* If we previously used quicklistDecompressNodeForUse(), just recompress. */
#define quicklistRecompressOnly(_node)                                         \
    do {                                                                       \
        if ((_node)->recompress)                                               \
            quicklistCompressNode((_node));                                    \
    } while (0)

/* Insert 'new_node' after 'old_node' if 'after' is 1.
 * Insert 'new_node' before 'old_node' if 'after' is 0.
 * Note: 'new_node' is *always* uncompressed, so if we assign it to
 *       head or tail, we do not need to uncompress it. */
REDIS_STATIC void __quicklistInsertNode(quicklist *quicklist,							// 在old_node的after/before插入old_node
                                        quicklistNode *old_node,
                                        quicklistNode *new_node, int after) {
    if (after) {			// 在old_node之后插入old_node
        new_node->prev = old_node;
        if (old_node) {
            new_node->next = old_node->next;
            if (old_node->next)
                old_node->next->prev = new_node;
            old_node->next = new_node;
        }
        if (quicklist->tail == old_node)		// 如果old之前是尾节点的话更新尾节点
            quicklist->tail = new_node;
    } else {				// 在old_node之前插入new_node
        new_node->next = old_node;
        if (old_node) {
            new_node->prev = old_node->prev;
            if (old_node->prev)
                old_node->prev->next = new_node;
            old_node->prev = new_node;
        }
        if (quicklist->head == old_node)		// 如果old之前是首节点的话更新首节点
            quicklist->head = new_node;
    }
    /* If this insert creates the only element so far, initialize head/tail. */
    if (quicklist->len == 0) {					// 更新头节点与尾节点
        quicklist->head = quicklist->tail = new_node;
    }

    /* Update len first, so in __quicklistCompress we know exactly len */
    quicklist->len++;

    if (old_node)								// 压缩old/new节点
        quicklistCompress(quicklist, old_node);

    quicklistCompress(quicklist, new_node);
}

/* Wrappers for node inserting around existing node. */
REDIS_STATIC void _quicklistInsertNodeBefore(quicklist *quicklist,					// 在old_node之前插入new_node
                                             quicklistNode *old_node,
                                             quicklistNode *new_node) {
    __quicklistInsertNode(quicklist, old_node, new_node, 0);
}

REDIS_STATIC void _quicklistInsertNodeAfter(quicklist *quicklist,				// 在old_node之后插入new_node
                                            quicklistNode *old_node,
                                            quicklistNode *new_node) {
    __quicklistInsertNode(quicklist, old_node, new_node, 1);
}

#define sizeMeetsSafetyLimit(sz) ((sz) <= SIZE_SAFETY_LIMIT)

 /* Calculate the size limit or length limit of the quicklist node
  * based on 'fill', and is also used to limit list listpack. */
void quicklistNodeLimit(int fill, size_t *size, unsigned int *count) {				// 根据fill计算quicklist的大小限制（count：quicklist可以存入的数据项的个数， size：quicklist可以承受的最大内存数）
    *size = SIZE_MAX;
    *count = UINT_MAX;

    if (fill >= 0) {
        /* Ensure that one node have at least one entry */
        *count = (fill == 0) ? 1 : fill;
    } else {
        size_t offset = (-fill) - 1;
        size_t max_level = sizeof(optimization_level) / sizeof(*optimization_level);
        if (offset >= max_level) offset = max_level - 1;
        *size = optimization_level[offset];
    }
}

/* Check if the limit of the quicklist node has been reached to determine if
 * insertions, merges or other operations that would increase the size of
 * the node can be performed.
 * Return 1 if exceeds the limit, otherwise 0. */
int quicklistNodeExceedsLimit(int fill, size_t new_sz, unsigned int new_count) {			// 给出fill/new_sz/new_count，判断是否满足fill的限制）
    size_t sz_limit;
    unsigned int count_limit;
    quicklistNodeLimit(fill, &sz_limit, &count_limit);

    if (likely(sz_limit != SIZE_MAX)) {
        return new_sz > sz_limit;
    } else if (count_limit != UINT_MAX) {
        /* when we reach here we know that the limit is a size limit (which is
         * safe, see comments next to optimization_level and SIZE_SAFETY_LIMIT) */
        if (!sizeMeetsSafetyLimit(new_sz)) return 1;
        return new_count > count_limit;
    }

    redis_unreachable();			// 出错了
}

REDIS_STATIC int _quicklistNodeAllowInsert(const quicklistNode *node,							// 看node是否允许再插入一个节点
                                           const int fill, const size_t sz) {
    if (unlikely(!node))
        return 0;

    if (unlikely(QL_NODE_IS_PLAIN(node) || isLargeElement(sz)))
        return 0;

    /* Estimate how many bytes will be added to the listpack by this one entry.
     * We prefer an overestimation, which would at worse lead to a few bytes
     * below the lowest limit of 4k (see optimization_level).
     * Note: No need to check for overflow below since both `node->sz` and
     * `sz` are to be less than 1GB after the plain/large element check above. */
    size_t new_sz = node->sz + sz + SIZE_ESTIMATE_OVERHEAD;				// 插入节点之后的链表长度
    if (unlikely(quicklistNodeExceedsLimit(fill, new_sz, node->count + 1)))			// 看插入之后是否超出了限制
        return 0;
    return 1;
}

REDIS_STATIC int _quicklistNodeAllowMerge(const quicklistNode *a,					// 看a与b合并之后，是否超出了限制
                                          const quicklistNode *b,
                                          const int fill) {
    if (!a || !b)
        return 0;

    if (unlikely(QL_NODE_IS_PLAIN(a) || QL_NODE_IS_PLAIN(b)))
        return 0;

    /* approximate merged listpack size (- 7 to remove one listpack
     * header/trailer, see LP_HDR_SIZE and LP_EOF) */
    unsigned int merge_sz = a->sz + b->sz - 7;			// 合并之后的字节数
    if (unlikely(quicklistNodeExceedsLimit(fill, merge_sz, a->count + b->count)))			// 看合并之后的状态是否超出了限制
        return 0;
    return 1;
}

#define quicklistNodeUpdateSz(node)                                            \			// 更新node的内容大小
    do {                                                                       \
        (node)->sz = lpBytes((node)->entry);                                   \
    } while (0)

static quicklistNode* __quicklistCreatePlainNode(void *value, size_t sz) {				// 根据sz大小的value创建一个plainNode
    quicklistNode *new_node = quicklistCreateNode();
    new_node->entry = zmalloc(sz);
    new_node->container = QUICKLIST_NODE_CONTAINER_PLAIN;
    memcpy(new_node->entry, value, sz);
    new_node->sz = sz;
    new_node->count++;
    return new_node;
}

static void __quicklistInsertPlainNode(quicklist *quicklist, quicklistNode *old_node,				// 再old_node的where位置插入一个new_node
                                       void *value, size_t sz, int after) {
    __quicklistInsertNode(quicklist, old_node, __quicklistCreatePlainNode(value, sz), after);
    quicklist->count++;
}

/* Add new entry to head node of quicklist.
 *
 * Returns 0 if used existing head.
 * Returns 1 if new head created. */
int quicklistPushHead(quicklist *quicklist, void *value, size_t sz) {						// 再quicklist的首部插入一个sz大小的value数据
    quicklistNode *orig_head = quicklist->head;

    if (unlikely(isLargeElement(sz))) {					// sz不会太大的话，调用函数插入节点
        __quicklistInsertPlainNode(quicklist, quicklist->head, value, sz, 0);
        return 1;
    }

    if (likely(
            _quicklistNodeAllowInsert(quicklist->head, quicklist->fill, sz))) {			// 这一个链表足够装下这个sz大小的数据：调用lpPrepend函数直接插入数据
        quicklist->head->entry = lpPrepend(quicklist->head->entry, value, sz);
        quicklistNodeUpdateSz(quicklist->head);
    } else {																			// 链表不够大小的话：创建一个新的quicklListNode，将数据插入到这个新的链表中，并将这个链表插入到quicklist中
        quicklistNode *node = quicklistCreateNode();
        node->entry = lpPrepend(lpNew(0), value, sz);

        quicklistNodeUpdateSz(node);
        _quicklistInsertNodeBefore(quicklist, quicklist->head, node);
    }
    quicklist->count++;						// 更新数据信息
    quicklist->head->count++;
    return (orig_head != quicklist->head);
}

/* Add new entry to tail node of quicklist.
 *
 * Returns 0 if used existing tail.
 * Returns 1 if new tail created. */
int quicklistPushTail(quicklist *quicklist, void *value, size_t sz) {				// 再quicklist的尾部插入一个sz大小的value数据
    quicklistNode *orig_tail = quicklist->tail;
    if (unlikely(isLargeElement(sz))) {			// 数据较小的话，直接调用函数
        __quicklistInsertPlainNode(quicklist, quicklist->tail, value, sz, 1);
        return 1;
    }

    if (likely(
            _quicklistNodeAllowInsert(quicklist->tail, quicklist->fill, sz))) {			// 链表空间足够的话，直接插入
        quicklist->tail->entry = lpAppend(quicklist->tail->entry, value, sz);
        quicklistNodeUpdateSz(quicklist->tail);
    } else {																			// 链表空间不足的话，创建一个新的链表，并插入元素与加入到quicklist
        quicklistNode *node = quicklistCreateNode();
        node->entry = lpAppend(lpNew(0), value, sz);

        quicklistNodeUpdateSz(node);
        _quicklistInsertNodeAfter(quicklist, quicklist->tail, node);
    }
    quicklist->count++;					// 更新quicklist信息
    quicklist->tail->count++;
    return (orig_tail != quicklist->tail);
}

/* Create new node consisting of a pre-formed listpack.
 * Used for loading RDBs where entire listpacks have been stored
 * to be retrieved later. */
void quicklistAppendListpack(quicklist *quicklist, unsigned char *zl) {		// 将内存包装成quickListNode，将它插入到quicklist的尾部
    quicklistNode *node = quicklistCreateNode();

    node->entry = zl;
    node->count = lpLength(node->entry);
    node->sz = lpBytes(zl);

    _quicklistInsertNodeAfter(quicklist, quicklist->tail, node);
    quicklist->count += node->count;
}

/* Create new node consisting of a pre-formed plain node.
 * Used for loading RDBs where entire plain node has been stored
 * to be retrieved later.
 * data - the data to add (pointer becomes the responsibility of quicklist) */
void quicklistAppendPlainNode(quicklist *quicklist, unsigned char *data, size_t sz) {							// 在quicklist的尾部插入一个链表，链表内容是sz大小的data（要用quicklistnode封装一下data）
    quicklistNode *node = quicklistCreateNode();

    node->entry = data;
    node->count = 1;
    node->sz = sz;
    node->container = QUICKLIST_NODE_CONTAINER_PLAIN;

    _quicklistInsertNodeAfter(quicklist, quicklist->tail, node);
    quicklist->count += node->count;
}

#define quicklistDeleteIfEmpty(ql, n)                                          \				// ql
    do {                                                                       \
        if ((n)->count == 0) {                                                 \
            __quicklistDelNode((ql), (n));                                     \
            (n) = NULL;                                                        \
        }                                                                      \
    } while (0)

REDIS_STATIC void __quicklistDelNode(quicklist *quicklist,							// 根据node地址，删除该链表（存在的话更新书签）
                                     quicklistNode *node) {
    /* Update the bookmark if any */
    quicklistBookmark *bm = _quicklistBookmarkFindByNode(quicklist, node);				// 如果有关于这个Node的书签的话，更新这个书签指向下一个Node
    if (bm) {
        bm->node = node->next;
        /* if the bookmark was to the last node, delete it. */
        if (!bm->node)
            _quicklistBookmarkDelete(quicklist, bm);
    }

    if (node->next)							// 改变链表指向
        node->next->prev = node->prev;
    if (node->prev)
        node->prev->next = node->next;

    if (node == quicklist->tail) {			// 有必要的话，更新头尾节点
        quicklist->tail = node->prev;
    }

    if (node == quicklist->head) {
        quicklist->head = node->next;
    }

    /* Update len first, so in __quicklistCompress we know exactly len */
    quicklist->len--;						// 更新quicklist信息
    quicklist->count -= node->count;

    /* If we deleted a node within our compress depth, we
     * now have compressed nodes needing to be decompressed. */
    __quicklistCompress(quicklist, NULL);			// 该数据在解压的状态下，需要先解压对应的内容

    zfree(node->entry);								// 是否节点内存
    zfree(node);
}

/* Delete one entry from list given the node for the entry and a pointer
 * to the entry in the node.
 *
 * Note: quicklistDelIndex() *requires* uncompressed nodes because you
 *       already had to get *p from an uncompressed node somewhere.
 *
 * Returns 1 if the entire node was deleted, 0 if node still exists.
 * Also updates in/out param 'p' with the next offset in the listpack. */
REDIS_STATIC int quicklistDelIndex(quicklist *quicklist, quicklistNode *node,				// 删除node里的p节点，删除之后node空了的话删除node，返回是否删错了node
                                   unsigned char **p) {
    int gone = 0;

    if (unlikely(QL_NODE_IS_PLAIN(node))) {			// node只有一个元素的话，直接删除该node
        __quicklistDelNode(quicklist, node);
        return 1;
    }
    node->entry = lpDelete(node->entry, *p, p);			// 删除node中的p
    node->count--;
    if (node->count == 0) {			// 如果删除node里的p之后，node里没有节点的话，删除这个空node
        gone = 1;
        __quicklistDelNode(quicklist, node);
    } else {
        quicklistNodeUpdateSz(node);		// 更新node数量
    }
    quicklist->count--;						// 更新quicklist信息
    /* If we deleted the node, the original node is no longer valid */
    return gone ? 1 : 0;			// 是否删了node
}

/* Delete one element represented by 'entry'
 *
 * 'entry' stores enough metadata to delete the proper position in
 * the correct listpack in the correct quicklist node. */
void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry) {						// 删除entry的node里的zi节点（注意要更新iter的指向）
    quicklistNode *prev = entry->node->prev;
    quicklistNode *next = entry->node->next;
    int deleted_node = quicklistDelIndex((quicklist *)entry->quicklist,
                                         entry->node, &entry->zi);

    /* after delete, the zi is now invalid for any future usage. */
    iter->zi = NULL;

    /* If current node is deleted, we must update iterator node and offset. */
    if (deleted_node) {			// 如果删除了node的话，更新iter的指向
        if (iter->direction == AL_START_HEAD) {
            iter->current = next;
            iter->offset = 0;
        } else if (iter->direction == AL_START_TAIL) {
            iter->current = prev;
            iter->offset = -1;
        }
    }
    /* else if (!deleted_node), no changes needed.
     * we already reset iter->zi above, and the existing iter->offset
     * doesn't move again because:
     *   - [1, 2, 3] => delete offset 1 => [1, 3]: next element still offset 1
     *   - [1, 2, 3] => delete offset 0 => [2, 3]: next element still offset 0
     *  if we deleted the last element at offset N and now
     *  length of this listpack is N-1, the next call into
     *  quicklistNext() will jump to the next node. */
}

/* Replace quicklist entry by 'data' with length 'sz'. */
void quicklistReplaceEntry(quicklistIter *iter, quicklistEntry *entry,
                           void *data, size_t sz)
{
    quicklist* quicklist = iter->quicklist;

    if (likely(!QL_NODE_IS_PLAIN(entry->node) && !isLargeElement(sz))) {
        entry->node->entry = lpReplace(entry->node->entry, &entry->zi, data, sz);
        quicklistNodeUpdateSz(entry->node);
        /* quicklistNext() and quicklistGetIteratorEntryAtIdx() provide an uncompressed node */
        quicklistCompress(quicklist, entry->node);
    } else if (QL_NODE_IS_PLAIN(entry->node)) {
        if (isLargeElement(sz)) {
            zfree(entry->node->entry);
            entry->node->entry = zmalloc(sz);
            entry->node->sz = sz;
            memcpy(entry->node->entry, data, sz);
            quicklistCompress(quicklist, entry->node);
        } else {
            quicklistInsertAfter(iter, entry, data, sz);
            __quicklistDelNode(quicklist, entry->node);
        }
    } else {
        entry->node->dont_compress = 1; /* Prevent compression in quicklistInsertAfter() */
        quicklistInsertAfter(iter, entry, data, sz);
        if (entry->node->count == 1) {
            __quicklistDelNode(quicklist, entry->node);
        } else {
            unsigned char *p = lpSeek(entry->node->entry, -1);
            quicklistDelIndex(quicklist, entry->node, &p);
            entry->node->dont_compress = 0; /* Re-enable compression */
            quicklistCompress(quicklist, entry->node);
            quicklistCompress(quicklist, entry->node->next);
        }
    }

    /* In any case, we reset iterator to forbid use of iterator after insert.
     * Notice: iter->current has been compressed above. */
    resetIterator(iter);
}

/* Replace quicklist entry at offset 'index' by 'data' with length 'sz'.
 *
 * Returns 1 if replace happened.
 * Returns 0 if replace failed and no changes happened. */
int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data,
                            size_t sz) {
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(quicklist, index, &entry);
    if (likely(iter)) {
        quicklistReplaceEntry(iter, &entry, data, sz);
        quicklistReleaseIterator(iter);
        return 1;
    } else {
        return 0;
    }
}

/* Given two nodes, try to merge their listpacks.
 *
 * This helps us not have a quicklist with 3 element listpacks if
 * our fill factor can handle much higher levels.
 *
 * Note: 'a' must be to the LEFT of 'b'.
 *
 * After calling this function, both 'a' and 'b' should be considered
 * unusable.  The return value from this function must be used
 * instead of re-using any of the quicklistNode input arguments.
 *
 * Returns the input node picked to merge against or NULL if
 * merging was not possible. */
REDIS_STATIC quicklistNode *_quicklistListpackMerge(quicklist *quicklist,
                                                    quicklistNode *a,
                                                    quicklistNode *b) {
    D("Requested merge (a,b) (%u, %u)", a->count, b->count);

    quicklistDecompressNode(a);
    quicklistDecompressNode(b);
    if ((lpMerge(&a->entry, &b->entry))) {
        /* We merged listpacks! Now remove the unused quicklistNode. */
        quicklistNode *keep = NULL, *nokeep = NULL;
        if (!a->entry) {
            nokeep = a;
            keep = b;
        } else if (!b->entry) {
            nokeep = b;
            keep = a;
        }
        keep->count = lpLength(keep->entry);
        quicklistNodeUpdateSz(keep);

        nokeep->count = 0;
        __quicklistDelNode(quicklist, nokeep);
        quicklistCompress(quicklist, keep);
        return keep;
    } else {
        /* else, the merge returned NULL and nothing changed. */
        return NULL;
    }
}

/* Attempt to merge listpacks within two nodes on either side of 'center'.
 *
 * We attempt to merge:
 *   - (center->prev->prev, center->prev)
 *   - (center->next, center->next->next)
 *   - (center->prev, center)
 *   - (center, center->next)
 */
REDIS_STATIC void _quicklistMergeNodes(quicklist *quicklist,
                                       quicklistNode *center) {
    int fill = quicklist->fill;
    quicklistNode *prev, *prev_prev, *next, *next_next, *target;
    prev = prev_prev = next = next_next = target = NULL;

    if (center->prev) {
        prev = center->prev;
        if (center->prev->prev)
            prev_prev = center->prev->prev;
    }

    if (center->next) {
        next = center->next;
        if (center->next->next)
            next_next = center->next->next;
    }

    /* Try to merge prev_prev and prev */
    if (_quicklistNodeAllowMerge(prev, prev_prev, fill)) {
        _quicklistListpackMerge(quicklist, prev_prev, prev);
        prev_prev = prev = NULL; /* they could have moved, invalidate them. */
    }

    /* Try to merge next and next_next */
    if (_quicklistNodeAllowMerge(next, next_next, fill)) {
        _quicklistListpackMerge(quicklist, next, next_next);
        next = next_next = NULL; /* they could have moved, invalidate them. */
    }

    /* Try to merge center node and previous node */
    if (_quicklistNodeAllowMerge(center, center->prev, fill)) {
        target = _quicklistListpackMerge(quicklist, center->prev, center);
        center = NULL; /* center could have been deleted, invalidate it. */
    } else {
        /* else, we didn't merge here, but target needs to be valid below. */
        target = center;
    }

    /* Use result of center merge (or original) to merge with next node. */
    if (_quicklistNodeAllowMerge(target, target->next, fill)) {
        _quicklistListpackMerge(quicklist, target, target->next);
    }
}

/* Split 'node' into two parts, parameterized by 'offset' and 'after'.
 *
 * The 'after' argument controls which quicklistNode gets returned.
 * If 'after'==1, returned node has elements after 'offset'.
 *                input node keeps elements up to 'offset', including 'offset'.
 * If 'after'==0, returned node has elements up to 'offset'.
 *                input node keeps elements after 'offset', including 'offset'.
 *
 * Or in other words:
 * If 'after'==1, returned node will have elements after 'offset'.
 *                The returned node will have elements [OFFSET+1, END].
 *                The input node keeps elements [0, OFFSET].
 * If 'after'==0, returned node will keep elements up to but not including 'offset'.
 *                The returned node will have elements [0, OFFSET-1].
 *                The input node keeps elements [OFFSET, END].
 *
 * The input node keeps all elements not taken by the returned node.
 *
 * Returns newly created node or NULL if split not possible. */
REDIS_STATIC quicklistNode *_quicklistSplitNode(quicklistNode *node, int offset,
                                                int after) {
    size_t zl_sz = node->sz;

    quicklistNode *new_node = quicklistCreateNode();
    new_node->entry = zmalloc(zl_sz);

    /* Copy original listpack so we can split it */
    memcpy(new_node->entry, node->entry, zl_sz);

    /* Need positive offset for calculating extent below. */
    if (offset < 0) offset = node->count + offset;

    /* Ranges to be trimmed: -1 here means "continue deleting until the list ends" */
    int orig_start = after ? offset + 1 : 0;
    int orig_extent = after ? -1 : offset;
    int new_start = after ? 0 : offset;
    int new_extent = after ? offset + 1 : -1;

    D("After %d (%d); ranges: [%d, %d], [%d, %d]", after, offset, orig_start,
      orig_extent, new_start, new_extent);

    node->entry = lpDeleteRange(node->entry, orig_start, orig_extent);
    node->count = lpLength(node->entry);
    quicklistNodeUpdateSz(node);

    new_node->entry = lpDeleteRange(new_node->entry, new_start, new_extent);
    new_node->count = lpLength(new_node->entry);
    quicklistNodeUpdateSz(new_node);

    D("After split lengths: orig (%d), new (%d)", node->count, new_node->count);
    return new_node;
}

/* Insert a new entry before or after existing entry 'entry'.
 *
 * If after==1, the new value is inserted after 'entry', otherwise
 * the new value is inserted before 'entry'. */
REDIS_STATIC void _quicklistInsert(quicklistIter *iter, quicklistEntry *entry,
                                   void *value, const size_t sz, int after)
{
    quicklist *quicklist = iter->quicklist;
    int full = 0, at_tail = 0, at_head = 0, avail_next = 0, avail_prev = 0;
    int fill = quicklist->fill;
    quicklistNode *node = entry->node;
    quicklistNode *new_node = NULL;

    if (!node) {
        /* we have no reference node, so let's create only node in the list */
        D("No node given!");
        if (unlikely(isLargeElement(sz))) {
            __quicklistInsertPlainNode(quicklist, quicklist->tail, value, sz, after);
            return;
        }
        new_node = quicklistCreateNode();
        new_node->entry = lpPrepend(lpNew(0), value, sz);
        __quicklistInsertNode(quicklist, NULL, new_node, after);
        new_node->count++;
        quicklist->count++;
        return;
    }

    /* Populate accounting flags for easier boolean checks later */
    if (!_quicklistNodeAllowInsert(node, fill, sz)) {
        D("Current node is full with count %d with requested fill %d",
          node->count, fill);
        full = 1;
    }

    if (after && (entry->offset == node->count - 1 || entry->offset == -1)) {
        D("At Tail of current listpack");
        at_tail = 1;
        if (_quicklistNodeAllowInsert(node->next, fill, sz)) {
            D("Next node is available.");
            avail_next = 1;
        }
    }

    if (!after && (entry->offset == 0 || entry->offset == -(node->count))) {
        D("At Head");
        at_head = 1;
        if (_quicklistNodeAllowInsert(node->prev, fill, sz)) {
            D("Prev node is available.");
            avail_prev = 1;
        }
    }

    if (unlikely(isLargeElement(sz))) {
        if (QL_NODE_IS_PLAIN(node) || (at_tail && after) || (at_head && !after)) {
            __quicklistInsertPlainNode(quicklist, node, value, sz, after);
        } else {
            quicklistDecompressNodeForUse(node);
            new_node = _quicklistSplitNode(node, entry->offset, after);
            quicklistNode *entry_node = __quicklistCreatePlainNode(value, sz);
            __quicklistInsertNode(quicklist, node, entry_node, after);
            __quicklistInsertNode(quicklist, entry_node, new_node, after);
            quicklist->count++;
        }
        return;
    }

    /* Now determine where and how to insert the new element */
    if (!full && after) {
        D("Not full, inserting after current position.");
        quicklistDecompressNodeForUse(node);
        node->entry = lpInsertString(node->entry, value, sz, entry->zi, LP_AFTER, NULL);
        node->count++;
        quicklistNodeUpdateSz(node);
        quicklistRecompressOnly(node);
    } else if (!full && !after) {
        D("Not full, inserting before current position.");
        quicklistDecompressNodeForUse(node);
        node->entry = lpInsertString(node->entry, value, sz, entry->zi, LP_BEFORE, NULL);
        node->count++;
        quicklistNodeUpdateSz(node);
        quicklistRecompressOnly(node);
    } else if (full && at_tail && avail_next && after) {
        /* If we are: at tail, next has free space, and inserting after:
         *   - insert entry at head of next node. */
        D("Full and tail, but next isn't full; inserting next node head");
        new_node = node->next;
        quicklistDecompressNodeForUse(new_node);
        new_node->entry = lpPrepend(new_node->entry, value, sz);
        new_node->count++;
        quicklistNodeUpdateSz(new_node);
        quicklistRecompressOnly(new_node);
        quicklistRecompressOnly(node);
    } else if (full && at_head && avail_prev && !after) {
        /* If we are: at head, previous has free space, and inserting before:
         *   - insert entry at tail of previous node. */
        D("Full and head, but prev isn't full, inserting prev node tail");
        new_node = node->prev;
        quicklistDecompressNodeForUse(new_node);
        new_node->entry = lpAppend(new_node->entry, value, sz);
        new_node->count++;
        quicklistNodeUpdateSz(new_node);
        quicklistRecompressOnly(new_node);
        quicklistRecompressOnly(node);
    } else if (full && ((at_tail && !avail_next && after) ||
                        (at_head && !avail_prev && !after))) {
        /* If we are: full, and our prev/next has no available space, then:
         *   - create new node and attach to quicklist */
        D("\tprovisioning new node...");
        new_node = quicklistCreateNode();
        new_node->entry = lpPrepend(lpNew(0), value, sz);
        new_node->count++;
        quicklistNodeUpdateSz(new_node);
        __quicklistInsertNode(quicklist, node, new_node, after);
    } else if (full) {
        /* else, node is full we need to split it. */
        /* covers both after and !after cases */
        D("\tsplitting node...");
        quicklistDecompressNodeForUse(node);
        new_node = _quicklistSplitNode(node, entry->offset, after);
        if (after)
            new_node->entry = lpPrepend(new_node->entry, value, sz);
        else
            new_node->entry = lpAppend(new_node->entry, value, sz);
        new_node->count++;
        quicklistNodeUpdateSz(new_node);
        __quicklistInsertNode(quicklist, node, new_node, after);
        _quicklistMergeNodes(quicklist, node);
    }

    quicklist->count++;

    /* In any case, we reset iterator to forbid use of iterator after insert.
     * Notice: iter->current has been compressed in _quicklistInsert(). */
    resetIterator(iter); 
}

void quicklistInsertBefore(quicklistIter *iter, quicklistEntry *entry,
                           void *value, const size_t sz)
{
    _quicklistInsert(iter, entry, value, sz, 0);
}

void quicklistInsertAfter(quicklistIter *iter, quicklistEntry *entry,
                          void *value, const size_t sz)
{
    _quicklistInsert(iter, entry, value, sz, 1);
}

/* Delete a range of elements from the quicklist.
 *
 * elements may span across multiple quicklistNodes, so we
 * have to be careful about tracking where we start and end.
 *
 * Returns 1 if entries were deleted, 0 if nothing was deleted. */
int quicklistDelRange(quicklist *quicklist, const long start,				// 删除qiucklist的start开始的count个元素
                      const long count) {
    if (count <= 0)
        return 0;

    unsigned long extent = count; /* range is inclusive of start position */

    if (start >= 0 && extent > (quicklist->count - start)) {			// 剩余数量不够，缩减要删的元素个数
        /* if requesting delete more elements than exist, limit to list size. */
        extent = quicklist->count - start;
    } else if (start < 0 && extent > (unsigned long)(-start)) {
        /* else, if at negative offset, limit max size to rest of list. */
        extent = -start; /* c.f. LREM -29 29; just delete until end. */
    }

    quicklistIter *iter = quicklistGetIteratorAtIdx(quicklist, AL_START_TAIL, start);
    if (!iter)
        return 0;

    D("Quicklist delete request for start %ld, count %ld, extent: %ld", start,
      count, extent);
    quicklistNode *node = iter->current;
    long offset = iter->offset;
    quicklistReleaseIterator(iter);

    /* iterate over next nodes until everything is deleted. */
    while (extent) {
        quicklistNode *next = node->next;

        unsigned long del;
        int delete_entire_node = 0;
        if (offset == 0 && extent >= node->count) {
            /* If we are deleting more than the count of this node, we
             * can just delete the entire node without listpack math. */
            delete_entire_node = 1;
            del = node->count;
        } else if (offset >= 0 && extent + offset >= node->count) {
            /* If deleting more nodes after this one, calculate delete based
             * on size of current node. */
            del = node->count - offset;
        } else if (offset < 0) {
            /* If offset is negative, we are in the first run of this loop
             * and we are deleting the entire range
             * from this start offset to end of list.  Since the Negative
             * offset is the number of elements until the tail of the list,
             * just use it directly as the deletion count. */
            del = -offset;

            /* If the positive offset is greater than the remaining extent,
             * we only delete the remaining extent, not the entire offset.
             */
            if (del > extent)
                del = extent;
        } else {
            /* else, we are deleting less than the extent of this node, so
             * use extent directly. */
            del = extent;
        }

        D("[%ld]: asking to del: %ld because offset: %d; (ENTIRE NODE: %d), "
          "node count: %u",
          extent, del, offset, delete_entire_node, node->count);

        if (delete_entire_node || QL_NODE_IS_PLAIN(node)) {
            __quicklistDelNode(quicklist, node);
        } else {
            quicklistDecompressNodeForUse(node);
            node->entry = lpDeleteRange(node->entry, offset, del);
            quicklistNodeUpdateSz(node);
            node->count -= del;
            quicklist->count -= del;
            quicklistDeleteIfEmpty(quicklist, node);
            if (node)
                quicklistRecompressOnly(node);
        }

        extent -= del;

        node = next;

        offset = 0;
    }
    return 1;
}

/* compare between a two entries */
int quicklistCompare(quicklistEntry* entry, unsigned char *p2, const size_t p2_len) {					// 比较entry的内容与p2是否相同
    if (unlikely(QL_NODE_IS_PLAIN(entry->node))) {				// 如果是一条链的quicklist，直接比对该节点的内容
        return ((entry->sz == p2_len) && (memcmp(entry->value, p2, p2_len) == 0));
    }
    return lpCompare(entry->zi, p2, p2_len);					// 否则直接比对节点
}

/* Returns a quicklist iterator 'iter'. After the initialization every
 * call to quicklistNext() will return the next element of the quicklist. */
quicklistIter *quicklistGetIterator(quicklist *quicklist, int direction) {					// 创建一个quicklist的迭代器
    quicklistIter *iter;

    iter = zmalloc(sizeof(*iter));

    if (direction == AL_START_HEAD) {
        iter->current = quicklist->head;
        iter->offset = 0;
    } else if (direction == AL_START_TAIL) {
        iter->current = quicklist->tail;
        iter->offset = -1;
    }

    iter->direction = direction;
    iter->quicklist = quicklist;

    iter->zi = NULL;

    return iter;
}

/* Initialize an iterator at a specific offset 'idx' and make the iterator
 * return nodes in 'direction' direction. */
quicklistIter *quicklistGetIteratorAtIdx(quicklist *quicklist,					// 创建一个迭代器，定位着quicklist的index个节点
                                         const int direction,
                                         const long long idx)
{
    quicklistNode *n;
    unsigned long long accum = 0;
    unsigned long long index;
    int forward = idx < 0 ? 0 : 1; /* < 0 -> reverse, 0+ -> forward */			// 是否正向遍历

    index = forward ? idx : (-idx) - 1;				// 需遍历的次数（前->后/后->前）
    if (index >= quicklist->count)
        return NULL;

    /* Seek in the other direction if that way is shorter. */
    int seek_forward = forward;
    unsigned long long seek_index = index;
    if (index > (quicklist->count - 1) / 2) {			// 如果节点在后半部分则方向遍历
        seek_forward = !forward;
        seek_index = quicklist->count - 1 - index;
    }

    n = seek_forward ? quicklist->head : quicklist->tail;
    while (likely(n)) {				// 确定目标节点在quicklist的那个节点上
        if ((accum + n->count) > seek_index) {
            break;
        } else {
            D("Skipping over (%p) %u at accum %lld", (void *)n, n->count,
              accum);
            accum += n->count;
            n = seek_forward ? n->next : n->prev;
        }
    }

    if (!n)
        return NULL;

    /* Fix accum so it looks like we seeked in the other direction. */
    if (seek_forward != forward) accum = quicklist->count - n->count - accum;

    D("Found node: %p at accum %llu, idx %llu, sub+ %llu, sub- %llu", (void *)n,
      accum, index, index - accum, (-index) - 1 + accum);

    quicklistIter *iter = quicklistGetIterator(quicklist, direction);		// 创建一个新的迭代器
    iter->current = n;				// 迭代器的current指向正在访问的节点的所在直链
    if (forward) {													// 设置偏移量
        /* forward = normal head-to-tail offset. */
        iter->offset = index - accum;			
    } else {
        /* reverse = need negative offset for tail-to-head, so undo
         * the result of the original index = (-idx) - 1 above. */
        iter->offset = (-index) - 1 + accum;
    }

    return iter;
}

/* Release iterator.
 * If we still have a valid current node, then re-encode current node. */
void quicklistReleaseIterator(quicklistIter *iter) {						// 释放quicklist的迭代器（释放之前，对内容进行压缩 
    if (!iter) return;
    if (iter->current)
        quicklistCompress(iter->quicklist, iter->current);			// 释放迭代器时，对节点进行重新压缩

    zfree(iter);
}

/* Get next element in iterator.
 *
 * Note: You must NOT insert into the list while iterating over it.
 * You *may* delete from the list while iterating using the
 * quicklistDelEntry() function.
 * If you insert into the quicklist while iterating, you should
 * re-create the iterator after your addition.
 *
 * iter = quicklistGetIterator(quicklist,<direction>);
 * quicklistEntry entry;
 * while (quicklistNext(iter, &entry)) {
 *     if (entry.value)
 *          [[ use entry.value with entry.sz ]]
 *     else
 *          [[ use entry.longval ]]
 * }
 *
 * Populates 'entry' with values for this iteration.
 * Returns 0 when iteration is complete or if iteration not possible.
 * If return value is 0, the contents of 'entry' are not valid.
 */
int quicklistNext(quicklistIter *iter, quicklistEntry *entry) {						// 将iter下一个节点的信息填入到entry中
    initEntry(entry);			// 初始化entry

    if (!iter) {
        D("Returning because no iter!");
        return 0;
    }

    entry->quicklist = iter->quicklist;
    entry->node = iter->current;

    if (!iter->current) {
        D("Returning because current node is NULL");
        return 0;
    }

    unsigned char *(*nextFn)(unsigned char *, unsigned char *) = NULL;
    int offset_update = 0;

    int plain = QL_NODE_IS_PLAIN(iter->current);				// iter->current是否是plant
    if (!iter->zi) {
        /* If !zi, use current index. */
        quicklistDecompressNodeForUse(iter->current);		// 先解压，并且取出对应下标的节点
        if (unlikely(plain))			// 是listpack
            iter->zi = iter->current->entry;
        else							// 是quicklist
            iter->zi = lpSeek(iter->current->entry, iter->offset);
    } else if (unlikely(plain)) {
        iter->zi = NULL;
    } else {
        /* else, use existing iterator offset and get prev/next as necessary. */
        if (iter->direction == AL_START_HEAD) {
            nextFn = lpNext;
            offset_update = 1;
        } else if (iter->direction == AL_START_TAIL) {
            nextFn = lpPrev;
            offset_update = -1;
        }
        iter->zi = nextFn(iter->current->entry, iter->zi);
        iter->offset += offset_update;
    }

    entry->zi = iter->zi;
    entry->offset = iter->offset;

    if (iter->zi) {				// 取到了值：直接赋值
        if (unlikely(plain)) {
            entry->value = entry->node->entry;
            entry->sz = entry->node->sz;
            return 1;
        }
        /* Populate value from existing listpack position */
        unsigned int sz = 0;
        entry->value = lpGetValue(entry->zi, &sz, &entry->longval);
        entry->sz = sz;
        return 1;
    } else {				// listpack用尽了，重新从边界开始遍历
        /* We ran out of listpack entries.
         * Pick next node, update offset, then re-run retrieval. */
        quicklistCompress(iter->quicklist, iter->current);
        if (iter->direction == AL_START_HEAD) {
            /* Forward traversal */
            D("Jumping to start of next node");			// 打日志
            iter->current = iter->current->next;
            iter->offset = 0;
        } else if (iter->direction == AL_START_TAIL) {
            /* Reverse traversal */
            D("Jumping to end of previous node");
            iter->current = iter->current->prev;
            iter->offset = -1;
        }
        iter->zi = NULL;
        return quicklistNext(iter, entry);
    }
}

/* Sets the direction of a quicklist iterator. */
void quicklistSetDirection(quicklistIter *iter, int direction) {						// 设置quicklist的direction
    iter->direction = direction;
}

/* Duplicate the quicklist.
 * On success a copy of the original quicklist is returned.
 *
 * The original quicklist both on success or error is never modified.
 *
 * Returns newly allocated quicklist. */
quicklist *quicklistDup(quicklist *orig) {												// 复制一份orig并返回
    quicklist *copy;

    copy = quicklistNew(orig->fill, orig->compress);

    for (quicklistNode *current = orig->head; current;			// 复制每一个节点
         current = current->next) {
        quicklistNode *node = quicklistCreateNode();

        if (current->encoding == QUICKLIST_NODE_ENCODING_LZF) {			// 节点压缩了：使用quicklistlzf接收
            quicklistLZF *lzf = (quicklistLZF *)current->entry;
            size_t lzf_sz = sizeof(*lzf) + lzf->sz;
            node->entry = zmalloc(lzf_sz);
            memcpy(node->entry, current->entry, lzf_sz);
        } else if (current->encoding == QUICKLIST_NODE_ENCODING_RAW) {			// 节点没被压缩：直接使用listpack
            node->entry = zmalloc(current->sz);
            memcpy(node->entry, current->entry, current->sz);
        }

        node->count = current->count;			// 复制节点信息
        copy->count += node->count;
        node->sz = current->sz;
        node->encoding = current->encoding;
        node->container = current->container;

        _quicklistInsertNodeAfter(copy, copy->tail, node);		// 将复制的节点插入到新的quicklist后面
    }

    /* copy->count must equal orig->count here */
    return copy;			// 返回复制的quicklist
}

/* Populate 'entry' with the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range 0 is returned.
 *
 * Returns an iterator at a specific offset 'idx' if element found
 * Returns NULL if element not found */
quicklistIter *quicklistGetIteratorEntryAtIdx(quicklist *quicklist, const long long idx,					// 根据下标，获取这个下标的内容，并返回迭代器
                                              quicklistEntry *entry)
{
    quicklistIter *iter = quicklistGetIteratorAtIdx(quicklist, AL_START_TAIL, idx);			// 创建一个指向quicklist的idx节点的迭代器
    if (!iter) return NULL;
    assert(quicklistNext(iter, entry));				// 湖区迭代器的信息，装入到entry中
    return iter;
}

static void quicklistRotatePlain(quicklist *quicklist) {							// 将quick list的尾节点变为头节点，次尾节点变为尾节点
    quicklistNode *new_head = quicklist->tail;
    quicklistNode *new_tail = quicklist->tail->prev;
    quicklist->head->prev = new_head;
    new_tail->next = NULL;
    new_head->next = quicklist->head;
    new_head->prev = NULL;
    quicklist->head = new_head;
    quicklist->tail = new_tail;
}

/* Rotate quicklist by moving the tail element to the head. */
void quicklistRotate(quicklist *quicklist) {						// 将quicklist的尾节点移到头节点位置
    if (quicklist->count <= 1)				// 没必要转
        return;

    if (unlikely(QL_NODE_IS_PLAIN(quicklist->tail))) {				// 如果是plain的化，直接调用函数
        quicklistRotatePlain(quicklist);
        return;
    }

    /* First, get the tail entry */
    unsigned char *p = lpSeek(quicklist->tail->entry, -1);
    unsigned char *value, *tmp;
    long long longval;
    unsigned int sz;
    char longstr[32] = {0};
    tmp = lpGetValue(p, &sz, &longval);				// 获取尾节点的内容

    /* If value found is NULL, then lpGet populated longval instead */
    if (!tmp) {			// 是整数：
        /* Write the longval as a string so we can re-add it */
        sz = ll2string(longstr, sizeof(longstr), longval);
        value = (unsigned char *)longstr;
    } else if (quicklist->len == 1) {			// 是字符串，但是quicklist的节点链表只有一条
        /* Copy buffer since there could be a memory overlap when move
         * entity from tail to head in the same listpack. */
        value = zmalloc(sz);
        memcpy(value, tmp, sz);
    } else {									// 是字符串
        value = tmp;
    }

    /* Add tail entry to head (must happen before tail is deleted). */
    quicklistPushHead(quicklist, value, sz);				// 将新的尾节点插入到quicklist的头部

    /* If quicklist has only one node, the head listpack is also the
     * tail listpack and PushHead() could have reallocated our single listpack,
     * which would make our pre-existing 'p' unusable. */
    if (quicklist->len == 1) {
        p = lpSeek(quicklist->tail->entry, -1);
    }

    /* Remove tail entry. */
    quicklistDelIndex(quicklist, quicklist->tail, &p);				// 清除节点p
    if (value != (unsigned char*)longstr && value != tmp)			// 释放value
        zfree(value);
}

/* pop from quicklist and return result in 'data' ptr.  Value of 'data'
 * is the return value of 'saver' function pointer if the data is NOT a number.
 *
 * If the quicklist element is a long long, then the return value is returned in
 * 'sval'.
 *
 * Return value of 0 means no elements available.
 * Return value of 1 means check 'data' and 'sval' for values.
 * If 'data' is set, use 'data' and 'sz'.  Otherwise, use 'sval'. */
int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data,				// 删除quicklist的where的节点（HEAD：第一条节点的首节点， TAIL：最后一条节点的尾节点），并将数据存储在data/sz/sval中；且对删除的元素做saver回调处理
                       size_t *sz, long long *sval,
                       void *(*saver)(unsigned char *data, size_t sz)) {
    unsigned char *p;
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    int pos = (where == QUICKLIST_HEAD) ? 0 : -1;

    if (quicklist->count == 0)
        return 0;

    if (data)
        *data = NULL;
    if (sz)
        *sz = 0;
    if (sval)
        *sval = -123456789;

    quicklistNode *node;
    if (where == QUICKLIST_HEAD && quicklist->head) {
        node = quicklist->head;
    } else if (where == QUICKLIST_TAIL && quicklist->tail) {
        node = quicklist->tail;
    } else {
        return 0;
    }

    /* The head and tail should never be compressed */
    assert(node->encoding != QUICKLIST_NODE_ENCODING_LZF);

    if (unlikely(QL_NODE_IS_PLAIN(node))) {					// 取出待操作的节点
        if (data)
            *data = saver(node->entry, node->sz);
        if (sz)
            *sz = node->sz;
        quicklistDelIndex(quicklist, node, NULL);
        return 1;
    }

    p = lpSeek(node->entry, pos);					// 将位置移动到待操作的位置
    vstr = lpGetValue(p, &vlen, &vlong);			// 获取待操作的内容
    if (vstr) {				// 保存字符串信息
        if (data)
            *data = saver(vstr, vlen);
        if (sz)
            *sz = vlen;
    } else {				// 保存整数信息
        if (data)
            *data = NULL;
        if (sval)
            *sval = vlong;
    }
    quicklistDelIndex(quicklist, node, &p);				// 获取完值之后删除该节点
    return 1;
}

/* Return a malloc'd copy of data passed in */
REDIS_STATIC void *_quicklistSaver(unsigned char *data, size_t sz) {				// 复制并返回sz长度的data
    unsigned char *vstr;
    if (data) {
        vstr = zmalloc(sz);
        memcpy(vstr, data, sz);
        return vstr;
    }
    return NULL;
}

/* Default pop function
 *
 * Returns malloc'd value from quicklist */
int quicklistPop(quicklist *quicklist, int where, unsigned char **data,						// 弹出quicklist的where的元素，并且保存删掉的元素的内容至对应位置
                 size_t *sz, long long *slong) {
    unsigned char *vstr = NULL;
    size_t vlen = 0;
    long long vlong = 0;
    if (quicklist->count == 0)
        return 0;
    int ret = quicklistPopCustom(quicklist, where, &vstr, &vlen, &vlong,			// 删除where的元素并调用saver回调
                                 _quicklistSaver);
    if (data)			// 把偶才能相应数据
        *data = vstr;
    if (slong)
        *slong = vlong;
    if (sz)
        *sz = vlen;
    return ret;
}

/* Wrapper to allow argument-based switching between HEAD/TAIL pop */
void quicklistPush(quicklist *quicklist, void *value, const size_t sz,					// 在quicklist的where位置插入一个sz长度的value元素
                   int where) {
    /* The head and tail should never be compressed (we don't attempt to decompress them) */
    if (quicklist->head)				// 首/尾节点不能被压缩
        assert(quicklist->head->encoding != QUICKLIST_NODE_ENCODING_LZF);
    if (quicklist->tail)
        assert(quicklist->tail->encoding != QUICKLIST_NODE_ENCODING_LZF);

    if (where == QUICKLIST_HEAD) {			// 在对应位置插入元素
        quicklistPushHead(quicklist, value, sz);
    } else if (where == QUICKLIST_TAIL) {
        quicklistPushTail(quicklist, value, sz);
    }
}

/* Print info of quicklist which is used in debugCommand. */
void quicklistRepr(unsigned char *ql, int full) {			// 打印quick list的全部信息（full：是否打印内部元素信息（调用与元素匹配的repr函数）
    int i = 0;
    quicklist *quicklist  = (struct quicklist*) ql;
    printf("{count : %ld}\n", quicklist->count);
    printf("{len : %ld}\n", quicklist->len);
    printf("{fill : %d}\n", quicklist->fill);
    printf("{compress : %d}\n", quicklist->compress);
    printf("{bookmark_count : %d}\n", quicklist->bookmark_count);
    quicklistNode* node = quicklist->head;

    while(node != NULL) {
        printf("{quicklist node(%d)\n", i++);
        printf("{container : %s, encoding: %s, size: %zu, count: %d, recompress: %d, attempted_compress: %d}\n",
               QL_NODE_IS_PLAIN(node) ? "PLAIN": "PACKED",
               (node->encoding == QUICKLIST_NODE_ENCODING_RAW) ? "RAW": "LZF",
               node->sz,
               node->count,
               node->recompress,
               node->attempted_compress);

        if (full) {
            quicklistDecompressNode(node);
            if (node->container == QUICKLIST_NODE_CONTAINER_PACKED) {
                printf("{ listpack:\n");
                lpRepr(node->entry);
                printf("}\n");

            } else if (QL_NODE_IS_PLAIN(node)) {
                printf("{ entry : %s }\n", node->entry);
            }
            printf("}\n");
            quicklistRecompressOnly(node);
        }
        node = node->next;
    }
}

/* Create or update a bookmark in the list which will be updated to the next node
 * automatically when the one referenced gets deleted.
 * Returns 1 on success (creation of new bookmark or override of an existing one).
 * Returns 0 on failure (reached the maximum supported number of bookmarks).
 * NOTE: use short simple names, so that string compare on find is quick.
 * NOTE: bookmark creation may re-allocate the quicklist, so the input pointer
         may change and it's the caller responsibility to update the reference.
 */
int quicklistBookmarkCreate(quicklist **ql_ref, const char *name, quicklistNode *node) {						// 为ql_ref的node节点制作一个名为name的书签
    quicklist *ql = *ql_ref;
    if (ql->bookmark_count >= QL_MAX_BM)			// 书签数量超出限制
        return 0;
    quicklistBookmark *bm = _quicklistBookmarkFindByName(ql, name);			// 如果书签已经存在，则更新书签内容
    if (bm) {
        bm->node = node;
        return 1;
    }
    ql = zrealloc(ql, sizeof(quicklist) + (ql->bookmark_count+1) * sizeof(quicklistBookmark));			// 书签不存在则创建一个新的书签
    *ql_ref = ql;
    ql->bookmarks[ql->bookmark_count].node = node;
    ql->bookmarks[ql->bookmark_count].name = zstrdup(name);
    ql->bookmark_count++;			// 书签数量加一
    return 1;
}

/* Find the quicklist node referenced by a named bookmark.
 * When the bookmarked node is deleted the bookmark is updated to the next node,
 * and if that's the last node, the bookmark is deleted (so find returns NULL). */
quicklistNode *quicklistBookmarkFind(quicklist *ql, const char *name) {								// 根据name查找书签并返回其指向的node
    quicklistBookmark *bm = _quicklistBookmarkFindByName(ql, name);
    if (!bm) return NULL;
    return bm->node;
}

/* Delete a named bookmark.
 * returns 0 if bookmark was not found, and 1 if deleted.
 * Note that the bookmark memory is not freed yet, and is kept for future use. */
int quicklistBookmarkDelete(quicklist *ql, const char *name) {										// 根据name删除该书签
    quicklistBookmark *bm = _quicklistBookmarkFindByName(ql, name);
    if (!bm)
        return 0;
    _quicklistBookmarkDelete(ql, bm);
    return 1;
}

quicklistBookmark *_quicklistBookmarkFindByName(quicklist *ql, const char *name) {					// 根据name查找是否有该书签
    unsigned i;
    for (i=0; i<ql->bookmark_count; i++) {
        if (!strcmp(ql->bookmarks[i].name, name)) {
            return &ql->bookmarks[i];
        }
    }
    return NULL;
}

quicklistBookmark *_quicklistBookmarkFindByNode(quicklist *ql, quicklistNode *node) {				// 根据node查找是否有该书签
    unsigned i;
    for (i=0; i<ql->bookmark_count; i++) {
        if (ql->bookmarks[i].node == node) {
            return &ql->bookmarks[i];
        }
    }
    return NULL;
}

void _quicklistBookmarkDelete(quicklist *ql, quicklistBookmark *bm) {						// 根据书签地址，删掉该书签
    int index = bm - ql->bookmarks;				// 该书签的偏移量
    zfree(bm->name);
    ql->bookmark_count--;
    memmove(bm, bm+1, (ql->bookmark_count - index)* sizeof(*bm));				// 补全空位（删掉的元素的坑）
    /* NOTE: We do not shrink (realloc) the quicklist yet (to avoid resonance,
     * it may be re-used later (a call to realloc may NOP). */
}

void quicklistBookmarksClear(quicklist *ql) {											// 删除所有书签
    while (ql->bookmark_count)
        zfree(ql->bookmarks[--ql->bookmark_count].name);			// 只能删name，node指向的是有效的内容还有用，不能删
    /* NOTE: We do not shrink (realloc) the quick list. main use case for this
     * function is just before releasing the allocation. */
}

/* The rest of this file is test cases and test helpers. */
#ifdef REDIS_TEST
#include <stdint.h>
#include <sys/time.h>
#include "testhelp.h"
#include <stdlib.h>

#define yell(str, ...) printf("ERROR! " str "\n\n", __VA_ARGS__)

#define ERROR                                                                  \
    do {                                                                       \
        printf("\tERROR!\n");                                                  \
        err++;                                                                 \
    } while (0)

#define ERR(x, ...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __func__, __LINE__);                   \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        err++;                                                                 \
    } while (0)

#define TEST(name) printf("test — %s\n", name);
#define TEST_DESC(name, ...) printf("test — " name "\n", __VA_ARGS__);

#define QL_TEST_VERBOSE 0

#define UNUSED(x) (void)(x)
static void ql_info(quicklist *ql) {
#if QL_TEST_VERBOSE
    printf("Container length: %lu\n", ql->len);
    printf("Container size: %lu\n", ql->count);
    if (ql->head)
        printf("\t(zsize head: %lu)\n", lpLength(ql->head->entry));
    if (ql->tail)
        printf("\t(zsize tail: %lu)\n", lpLength(ql->tail->entry));
    printf("\n");
#else
    UNUSED(ql);
#endif
}

/* Return the UNIX time in microseconds */
static long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
static long long mstime(void) { return ustime() / 1000; }

/* Iterate over an entire quicklist.
 * Print the list if 'print' == 1.
 *
 * Returns physical count of elements found by iterating over the list. */
static int _itrprintr(quicklist *ql, int print, int forward) {
    quicklistIter *iter =
        quicklistGetIterator(ql, forward ? AL_START_HEAD : AL_START_TAIL);
    quicklistEntry entry;
    int i = 0;
    int p = 0;
    quicklistNode *prev = NULL;
    while (quicklistNext(iter, &entry)) {
        if (entry.node != prev) {
            /* Count the number of list nodes too */
            p++;
            prev = entry.node;
        }
        if (print) {
            int size = (entry.sz > (1<<20)) ? 1<<20 : entry.sz;
            printf("[%3d (%2d)]: [%.*s] (%lld)\n", i, p, size,
                   (char *)entry.value, entry.longval);
        }
        i++;
    }
    quicklistReleaseIterator(iter);
    return i;
}
static int itrprintr(quicklist *ql, int print) {
    return _itrprintr(ql, print, 1);
}

static int itrprintr_rev(quicklist *ql, int print) {
    return _itrprintr(ql, print, 0);
}

#define ql_verify(a, b, c, d, e)                                               \
    do {                                                                       \
        err += _ql_verify((a), (b), (c), (d), (e));                            \
    } while (0)

static int _ql_verify_compress(quicklist *ql) {
    int errors = 0;
    if (quicklistAllowsCompression(ql)) {
        quicklistNode *node = ql->head;
        unsigned int low_raw = ql->compress;
        unsigned int high_raw = ql->len - ql->compress;

        for (unsigned int at = 0; at < ql->len; at++, node = node->next) {
            if (node && (at < low_raw || at >= high_raw)) {
                if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                    yell("Incorrect compression: node %d is "
                         "compressed at depth %d ((%u, %u); total "
                         "nodes: %lu; size: %zu; recompress: %d)",
                         at, ql->compress, low_raw, high_raw, ql->len, node->sz,
                         node->recompress);
                    errors++;
                }
            } else {
                if (node->encoding != QUICKLIST_NODE_ENCODING_LZF &&
                    !node->attempted_compress) {
                    yell("Incorrect non-compression: node %d is NOT "
                         "compressed at depth %d ((%u, %u); total "
                         "nodes: %lu; size: %zu; recompress: %d; attempted: %d)",
                         at, ql->compress, low_raw, high_raw, ql->len, node->sz,
                         node->recompress, node->attempted_compress);
                    errors++;
                }
            }
        }
    }
    return errors;
}

/* Verify list metadata matches physical list contents. */
static int _ql_verify(quicklist *ql, uint32_t len, uint32_t count,
                      uint32_t head_count, uint32_t tail_count) {
    int errors = 0;

    ql_info(ql);
    if (len != ql->len) {
        yell("quicklist length wrong: expected %d, got %lu", len, ql->len);
        errors++;
    }

    if (count != ql->count) {
        yell("quicklist count wrong: expected %d, got %lu", count, ql->count);
        errors++;
    }

    int loopr = itrprintr(ql, 0);
    if (loopr != (int)ql->count) {
        yell("quicklist cached count not match actual count: expected %lu, got "
             "%d",
             ql->count, loopr);
        errors++;
    }

    int rloopr = itrprintr_rev(ql, 0);
    if (loopr != rloopr) {
        yell("quicklist has different forward count than reverse count!  "
             "Forward count is %d, reverse count is %d.",
             loopr, rloopr);
        errors++;
    }

    if (ql->len == 0 && !errors) {
        return errors;
    }

    if (ql->head && head_count != ql->head->count &&
        head_count != lpLength(ql->head->entry)) {
        yell("quicklist head count wrong: expected %d, "
             "got cached %d vs. actual %lu",
             head_count, ql->head->count, lpLength(ql->head->entry));
        errors++;
    }

    if (ql->tail && tail_count != ql->tail->count &&
        tail_count != lpLength(ql->tail->entry)) {
        yell("quicklist tail count wrong: expected %d, "
             "got cached %u vs. actual %lu",
             tail_count, ql->tail->count, lpLength(ql->tail->entry));
        errors++;
    }

    errors += _ql_verify_compress(ql);
    return errors;
}

/* Release iterator and verify compress correctly. */
static void ql_release_iterator(quicklistIter *iter) {
    quicklist *ql = NULL;
    if (iter) ql = iter->quicklist;
    quicklistReleaseIterator(iter);
    if (ql) assert(!_ql_verify_compress(ql));
}

/* Generate new string concatenating integer i against string 'prefix' */
static char *genstr(char *prefix, int i) {
    static char result[64] = {0};
    snprintf(result, sizeof(result), "%s%d", prefix, i);
    return result;
}

static void randstring(unsigned char *target, size_t sz) {
    size_t p = 0;
    int minval, maxval;
    switch(rand() % 3) {
    case 0:
        minval = 'a';
        maxval = 'z';
    break;
    case 1:
        minval = '0';
        maxval = '9';
    break;
    case 2:
        minval = 'A';
        maxval = 'Z';
    break;
    default:
        assert(NULL);
    }

    while(p < sz)
        target[p++] = minval+rand()%(maxval-minval+1);
}

/* main test, but callable from other files */
int quicklistTest(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);

    int accurate = (flags & REDIS_TEST_ACCURATE);
    unsigned int err = 0;
    int optimize_start =
        -(int)(sizeof(optimization_level) / sizeof(*optimization_level));

    printf("Starting optimization offset at: %d\n", optimize_start);

    int options[] = {0, 1, 2, 3, 4, 5, 6, 10};
    int fills[] = {-5, -4, -3, -2, -1, 0,
                   1, 2, 32, 66, 128, 999};
    size_t option_count = sizeof(options) / sizeof(*options);
    int fill_count = (int)(sizeof(fills) / sizeof(*fills));
    long long runtime[option_count];

    for (int _i = 0; _i < (int)option_count; _i++) {
        printf("Testing Compression option %d\n", options[_i]);
        long long start = mstime();
        quicklistIter *iter;

        TEST("create list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("add to tail of empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "hello", 6);
            /* 1 for head and 1 for tail because 1 node = head = tail */
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST("add to head of empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            /* 1 for head and 1 for tail because 1 node = head = tail */
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST_DESC("add to tail 5x at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                for (int i = 0; i < 5; i++)
                    quicklistPushTail(ql, genstr("hello", i), 32);
                if (ql->count != 5)
                    ERROR;
                if (fills[f] == 32)
                    ql_verify(ql, 1, 5, 5, 5);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("add to head 5x at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                for (int i = 0; i < 5; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                if (ql->count != 5)
                    ERROR;
                if (fills[f] == 32)
                    ql_verify(ql, 1, 5, 5, 5);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("add to tail 500x at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i), 64);
                if (ql->count != 500)
                    ERROR;
                if (fills[f] == 32)
                    ql_verify(ql, 16, 500, 32, 20);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("add to head 500x at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                if (ql->count != 500)
                    ERROR;
                if (fills[f] == 32)
                    ql_verify(ql, 16, 500, 20, 32);
                quicklistRelease(ql);
            }
        }

        TEST("rotate empty") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistRotate(ql);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("Comprassion Plain node") {
            char buf[256];
            quicklistisSetPackedThreshold(1);
            quicklist *ql = quicklistNew(-2, 1);
            for (int i = 0; i < 500; i++) {
                /* Set to 256 to allow the node to be triggered to compress,
                 * if it is less than 48(nocompress), the test will be successful. */
                snprintf(buf, sizeof(buf), "hello%d", i);
                quicklistPushHead(ql, buf, 256);
            }

            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
            quicklistEntry entry;
            int i = 0;
            while (quicklistNext(iter, &entry)) {
                snprintf(buf, sizeof(buf), "hello%d", i);
                if (strcmp((char *)entry.value, buf))
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.value, buf, i);
                i++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        TEST("NEXT plain node")
        {
            packed_threshold = 3;
            quicklist *ql = quicklistNew(-2, options[_i]);
            char *strings[] = {"hello1", "hello2", "h3", "h4", "hello5"};

            for (int i = 0; i < 5; ++i)
                quicklistPushHead(ql, strings[i], strlen(strings[i]));

            quicklistEntry entry;
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
            int j = 0;

            while(quicklistNext(iter, &entry) != 0) {
                assert(strncmp(strings[j], (char *)entry.value, strlen(strings[j])) == 0);
                j++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        TEST("rotate plain node ") {
            unsigned char *data = NULL;
            size_t sz;
            long long lv;
            int i =0;
            packed_threshold = 5;
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello1", 6);
            quicklistPushHead(ql, "hello4", 6);
            quicklistPushHead(ql, "hello3", 6);
            quicklistPushHead(ql, "hello2", 6);
            quicklistRotate(ql);

            for(i = 1 ; i < 5; i++) {
                quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                int temp_char = data[5];
                zfree(data);
                assert(temp_char == ('0' + i));
            }

            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
            packed_threshold = (1 << 30);
        }

        TEST("rotate one val once") {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                quicklistPushHead(ql, "hello", 6);
                quicklistRotate(ql);
                /* Ignore compression verify because listpack is
                 * too small to compress. */
                ql_verify(ql, 1, 1, 1, 1);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("rotate 500 val 5000 times at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                quicklistPushHead(ql, "900", 3);
                quicklistPushHead(ql, "7000", 4);
                quicklistPushHead(ql, "-1200", 5);
                quicklistPushHead(ql, "42", 2);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 64);
                ql_info(ql);
                for (int i = 0; i < 5000; i++) {
                    ql_info(ql);
                    quicklistRotate(ql);
                }
                if (fills[f] == 1)
                    ql_verify(ql, 504, 504, 1, 1);
                else if (fills[f] == 2)
                    ql_verify(ql, 252, 504, 2, 2);
                else if (fills[f] == 32)
                    ql_verify(ql, 16, 504, 32, 24);
                quicklistRelease(ql);
            }
        }

        TEST("pop empty") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPop(ql, QUICKLIST_HEAD, NULL, NULL, NULL);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop 1 string from 1") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            char *populate = genstr("hello", 331);
            quicklistPushHead(ql, populate, 32);
            unsigned char *data;
            size_t sz;
            long long lv;
            ql_info(ql);
            assert(quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv));
            assert(data != NULL);
            assert(sz == 32);
            if (strcmp(populate, (char *)data)) {
                int size = sz;
                ERR("Pop'd value (%.*s) didn't equal original value (%s)", size,
                    data, populate);
            }
            zfree(data);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 1 number from 1") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "55513", 5);
            unsigned char *data;
            size_t sz;
            long long lv;
            ql_info(ql);
            assert(quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv));
            assert(data == NULL);
            assert(lv == 55513);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 500 from 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_info(ql);
            for (int i = 0; i < 500; i++) {
                unsigned char *data;
                size_t sz;
                long long lv;
                int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                assert(ret == 1);
                assert(data != NULL);
                assert(sz == 32);
                if (strcmp(genstr("hello", 499 - i), (char *)data)) {
                    int size = sz;
                    ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                        size, data, genstr("hello", 499 - i));
                }
                zfree(data);
            }
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 5000 from 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            for (int i = 0; i < 5000; i++) {
                unsigned char *data;
                size_t sz;
                long long lv;
                int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                if (i < 500) {
                    assert(ret == 1);
                    assert(data != NULL);
                    assert(sz == 32);
                    if (strcmp(genstr("hello", 499 - i), (char *)data)) {
                        int size = sz;
                        ERR("Pop'd value (%.*s) didn't equal original value "
                            "(%s)",
                            size, data, genstr("hello", 499 - i));
                    }
                    zfree(data);
                } else {
                    assert(ret == 0);
                }
            }
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("iterate forward over 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
            quicklistEntry entry;
            int i = 499, count = 0;
            while (quicklistNext(iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp((char *)entry.value, h))
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.value, h, i);
                i--;
                count++;
            }
            if (count != 500)
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            ql_verify(ql, 16, 500, 20, 32);
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        TEST("iterate reverse over 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
            quicklistEntry entry;
            int i = 0;
            while (quicklistNext(iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp((char *)entry.value, h))
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.value, h, i);
                i++;
            }
            if (i != 500)
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            ql_verify(ql, 16, 500, 20, 32);
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        TEST("insert after 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            quicklistEntry entry;
            iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
            quicklistInsertAfter(iter, &entry, "abc", 4);
            ql_release_iterator(iter);
            ql_verify(ql, 1, 2, 2, 2);

            /* verify results */
            iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
            int sz = entry.sz;
            if (strncmp((char *)entry.value, "hello", 5)) {
                ERR("Value 0 didn't match, instead got: %.*s", sz,
                    entry.value);
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
            sz = entry.sz;
            if (strncmp((char *)entry.value, "abc", 3)) {
                ERR("Value 1 didn't match, instead got: %.*s", sz,
                    entry.value);
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        TEST("insert before 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            quicklistEntry entry;
            iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
            quicklistInsertBefore(iter, &entry, "abc", 4);
            ql_release_iterator(iter);
            ql_verify(ql, 1, 2, 2, 2);

            /* verify results */
            iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
            int sz = entry.sz;
            if (strncmp((char *)entry.value, "abc", 3)) {
                ERR("Value 0 didn't match, instead got: %.*s", sz,
                    entry.value);
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
            sz = entry.sz;
            if (strncmp((char *)entry.value, "hello", 5)) {
                ERR("Value 1 didn't match, instead got: %.*s", sz,
                    entry.value);
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        TEST("insert head while head node is full") {
            quicklist *ql = quicklistNew(4, options[_i]);
            for (int i = 0; i < 10; i++)
                quicklistPushTail(ql, genstr("hello", i), 6);
            quicklistSetFill(ql, -1);
            quicklistEntry entry;
            iter = quicklistGetIteratorEntryAtIdx(ql, -10, &entry);
            char buf[4096] = {0};
            quicklistInsertBefore(iter, &entry, buf, 4096);
            ql_release_iterator(iter);
            ql_verify(ql, 4, 11, 1, 2);
            quicklistRelease(ql);
        }

        TEST("insert tail while tail node is full") {
            quicklist *ql = quicklistNew(4, options[_i]);
            for (int i = 0; i < 10; i++)
                quicklistPushHead(ql, genstr("hello", i), 6);
            quicklistSetFill(ql, -1);
            quicklistEntry entry;
            iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
            char buf[4096] = {0};
            quicklistInsertAfter(iter, &entry, buf, 4096);
            ql_release_iterator(iter);
            ql_verify(ql, 4, 11, 2, 1);
            quicklistRelease(ql);
        }

        TEST_DESC("insert once in elements while iterating at compress %d",
                  options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                quicklistPushTail(ql, "abc", 3);
                quicklistSetFill(ql, 1);
                quicklistPushTail(ql, "def", 3); /* force to unique node */
                quicklistSetFill(ql, f);
                quicklistPushTail(ql, "bob", 3); /* force to reset for +3 */
                quicklistPushTail(ql, "foo", 3);
                quicklistPushTail(ql, "zoo", 3);

                itrprintr(ql, 0);
                /* insert "bar" before "bob" while iterating over list. */
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
                quicklistEntry entry;
                while (quicklistNext(iter, &entry)) {
                    if (!strncmp((char *)entry.value, "bob", 3)) {
                        /* Insert as fill = 1 so it spills into new node. */
                        quicklistInsertBefore(iter, &entry, "bar", 3);
                        break; /* didn't we fix insert-while-iterating? */
                    }
                }
                ql_release_iterator(iter);
                itrprintr(ql, 0);

                /* verify results */
                iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
                int sz = entry.sz;

                if (strncmp((char *)entry.value, "abc", 3))
                    ERR("Value 0 didn't match, instead got: %.*s", sz,
                        entry.value);
                ql_release_iterator(iter);

                iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
                if (strncmp((char *)entry.value, "def", 3))
                    ERR("Value 1 didn't match, instead got: %.*s", sz,
                        entry.value);
                ql_release_iterator(iter);

                iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
                if (strncmp((char *)entry.value, "bar", 3))
                    ERR("Value 2 didn't match, instead got: %.*s", sz,
                        entry.value);
                ql_release_iterator(iter);

                iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
                if (strncmp((char *)entry.value, "bob", 3))
                    ERR("Value 3 didn't match, instead got: %.*s", sz,
                        entry.value);
                ql_release_iterator(iter);

                iter = quicklistGetIteratorEntryAtIdx(ql, 4, &entry);
                if (strncmp((char *)entry.value, "foo", 3))
                    ERR("Value 4 didn't match, instead got: %.*s", sz,
                        entry.value);
                ql_release_iterator(iter);

                iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
                if (strncmp((char *)entry.value, "zoo", 3))
                    ERR("Value 5 didn't match, instead got: %.*s", sz,
                        entry.value);
                ql_release_iterator(iter);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("insert [before] 250 new in middle of 500 elements at compress %d",
                  options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i), 32);
                for (int i = 0; i < 250; i++) {
                    quicklistEntry entry;
                    iter = quicklistGetIteratorEntryAtIdx(ql, 250, &entry);
                    quicklistInsertBefore(iter, &entry, genstr("abc", i), 32);
                    ql_release_iterator(iter);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 25, 750, 32, 20);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("insert [after] 250 new in middle of 500 elements at compress %d",
                  options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                for (int i = 0; i < 250; i++) {
                    quicklistEntry entry;
                    iter = quicklistGetIteratorEntryAtIdx(ql, 250, &entry);
                    quicklistInsertAfter(iter, &entry, genstr("abc", i), 32);
                    ql_release_iterator(iter);
                }

                if (ql->count != 750)
                    ERR("List size not 750, but rather %ld", ql->count);

                if (fills[f] == 32)
                    ql_verify(ql, 26, 750, 20, 32);
                quicklistRelease(ql);
            }
        }

        TEST("duplicate empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            ql_verify(ql, 0, 0, 0, 0);
            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 0, 0, 0, 0);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        TEST("duplicate list of 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, genstr("hello", 3), 32);
            ql_verify(ql, 1, 1, 1, 1);
            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 1, 1, 1, 1);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        TEST("duplicate list of 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 16, 500, 20, 32);

            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 16, 500, 20, 32);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        for (int f = 0; f < fill_count; f++) {
            TEST_DESC("index 1,200 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
                if (strcmp((char *)entry.value, "hello2") != 0)
                    ERR("Value: %s", entry.value);
                ql_release_iterator(iter);

                iter = quicklistGetIteratorEntryAtIdx(ql, 200, &entry);
                if (strcmp((char *)entry.value, "hello201") != 0)
                    ERR("Value: %s", entry.value);
                ql_release_iterator(iter);
                quicklistRelease(ql);
            }

            TEST_DESC("index -1,-2 from 500 list at fill %d at compress %d",
                      fills[f], options[_i]) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
                if (strcmp((char *)entry.value, "hello500") != 0)
                    ERR("Value: %s", entry.value);
                ql_release_iterator(iter);

                iter = quicklistGetIteratorEntryAtIdx(ql, -2, &entry);
                if (strcmp((char *)entry.value, "hello499") != 0)
                    ERR("Value: %s", entry.value);
                ql_release_iterator(iter);
                quicklistRelease(ql);
            }

            TEST_DESC("index -100 from 500 list at fill %d at compress %d",
                      fills[f], options[_i]) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                iter = quicklistGetIteratorEntryAtIdx(ql, -100, &entry);
                if (strcmp((char *)entry.value, "hello401") != 0)
                    ERR("Value: %s", entry.value);
                ql_release_iterator(iter);
                quicklistRelease(ql);
            }

            TEST_DESC("index too big +1 from 50 list at fill %d at compress %d",
                      fills[f], options[_i]) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                for (int i = 0; i < 50; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                int sz = entry.sz;
                iter = quicklistGetIteratorEntryAtIdx(ql, 50, &entry);
                if (iter)
                    ERR("Index found at 50 with 50 list: %.*s", sz,
                        entry.value);
                ql_release_iterator(iter);
                quicklistRelease(ql);
            }
        }

        TEST("delete range empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistDelRange(ql, 5, 20);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete range of entire node in list of one node") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 32; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 1, 32, 32, 32);
            quicklistDelRange(ql, 0, 32);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete range of entire node with overflow counts") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 32; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 1, 32, 32, 32);
            quicklistDelRange(ql, 0, 128);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete middle 100 of 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, 200, 100);
            ql_verify(ql, 14, 400, 32, 20);
            quicklistRelease(ql);
        }

        TEST("delete less than fill but across nodes") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, 60, 10);
            ql_verify(ql, 16, 490, 32, 20);
            quicklistRelease(ql);
        }

        TEST("delete negative 1 from 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, -1, 1);
            ql_verify(ql, 16, 499, 32, 19);
            quicklistRelease(ql);
        }

        TEST("delete negative 1 from 500 list with overflow counts") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, -1, 128);
            ql_verify(ql, 16, 499, 32, 19);
            quicklistRelease(ql);
        }

        TEST("delete negative 100 from 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistDelRange(ql, -100, 100);
            ql_verify(ql, 13, 400, 32, 16);
            quicklistRelease(ql);
        }

        TEST("delete -10 count 5 from 50 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 50; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 2, 50, 32, 18);
            quicklistDelRange(ql, -10, 5);
            ql_verify(ql, 2, 45, 32, 13);
            quicklistRelease(ql);
        }

        TEST("numbers only list read") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "1111", 4);
            quicklistPushTail(ql, "2222", 4);
            quicklistPushTail(ql, "3333", 4);
            quicklistPushTail(ql, "4444", 4);
            ql_verify(ql, 1, 4, 4, 4);
            quicklistEntry entry;
            iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
            if (entry.longval != 1111)
                ERR("Not 1111, %lld", entry.longval);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
            if (entry.longval != 2222)
                ERR("Not 2222, %lld", entry.longval);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
            if (entry.longval != 3333)
                ERR("Not 3333, %lld", entry.longval);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
            if (entry.longval != 4444)
                ERR("Not 4444, %lld", entry.longval);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 4, &entry);
            if (iter)
                ERR("Index past elements: %lld", entry.longval);
            ql_release_iterator(iter);
            
            iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
            if (entry.longval != 4444)
                ERR("Not 4444 (reverse), %lld", entry.longval);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, -2, &entry);
            if (entry.longval != 3333)
                ERR("Not 3333 (reverse), %lld", entry.longval);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, -3, &entry);
            if (entry.longval != 2222)
                ERR("Not 2222 (reverse), %lld", entry.longval);
            ql_release_iterator(iter);
            
            iter = quicklistGetIteratorEntryAtIdx(ql, -4, &entry);
            if (entry.longval != 1111)
                ERR("Not 1111 (reverse), %lld", entry.longval);
            ql_release_iterator(iter);
            
            iter = quicklistGetIteratorEntryAtIdx(ql, -5, &entry);
            if (iter)
                ERR("Index past elements (reverse), %lld", entry.longval);
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        TEST("numbers larger list read") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            char num[32];
            long long nums[5000];
            for (int i = 0; i < 5000; i++) {
                nums[i] = -5157318210846258176 + i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklistPushTail(ql, num, sz);
            }
            quicklistPushTail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
            quicklistEntry entry;
            for (int i = 0; i < 5000; i++) {
                iter = quicklistGetIteratorEntryAtIdx(ql, i, &entry);
                if (entry.longval != nums[i])
                    ERR("[%d] Not longval %lld but rather %lld", i, nums[i],
                        entry.longval);
                entry.longval = 0xdeadbeef;
                ql_release_iterator(iter);
            }
            iter = quicklistGetIteratorEntryAtIdx(ql, 5000, &entry);
            if (strncmp((char *)entry.value, "xxxxxxxxxxxxxxxxxxxx", 20))
                ERR("String val not match: %s", entry.value);
            ql_verify(ql, 157, 5001, 32, 9);
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        TEST("numbers larger list read B") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "99", 2);
            quicklistPushTail(ql, "98", 2);
            quicklistPushTail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
            quicklistPushTail(ql, "96", 2);
            quicklistPushTail(ql, "95", 2);
            quicklistReplaceAtIndex(ql, 1, "foo", 3);
            quicklistReplaceAtIndex(ql, -1, "bar", 3);
            quicklistRelease(ql);
        }

        TEST_DESC("lrem test at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                char *words[] = {"abc", "foo", "bar",  "foobar", "foobared",
                                 "zap", "bar", "test", "foo"};
                char *result[] = {"abc", "foo",  "foobar", "foobared",
                                  "zap", "test", "foo"};
                char *resultB[] = {"abc",      "foo", "foobar",
                                   "foobared", "zap", "test"};
                for (int i = 0; i < 9; i++)
                    quicklistPushTail(ql, words[i], strlen(words[i]));

                /* lrem 0 bar */
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
                quicklistEntry entry;
                int i = 0;
                while (quicklistNext(iter, &entry)) {
                    if (quicklistCompare(&entry, (unsigned char *)"bar", 3)) {
                        quicklistDelEntry(iter, &entry);
                    }
                    i++;
                }
                ql_release_iterator(iter);

                /* check result of lrem 0 bar */
                iter = quicklistGetIterator(ql, AL_START_HEAD);
                i = 0;
                while (quicklistNext(iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    int sz = entry.sz;
                    if (strncmp((char *)entry.value, result[i], entry.sz)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, sz, entry.value, result[i]);
                    }
                    i++;
                }
                ql_release_iterator(iter);

                quicklistPushTail(ql, "foo", 3);

                /* lrem -2 foo */
                iter = quicklistGetIterator(ql, AL_START_TAIL);
                i = 0;
                int del = 2;
                while (quicklistNext(iter, &entry)) {
                    if (quicklistCompare(&entry, (unsigned char *)"foo", 3)) {
                        quicklistDelEntry(iter, &entry);
                        del--;
                    }
                    if (!del)
                        break;
                    i++;
                }
                ql_release_iterator(iter);

                /* check result of lrem -2 foo */
                /* (we're ignoring the '2' part and still deleting all foo
                 * because
                 * we only have two foo) */
                iter = quicklistGetIterator(ql, AL_START_TAIL);
                i = 0;
                size_t resB = sizeof(resultB) / sizeof(*resultB);
                while (quicklistNext(iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    int sz = entry.sz;
                    if (strncmp((char *)entry.value, resultB[resB - 1 - i],
                                sz)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, sz, entry.value, resultB[resB - 1 - i]);
                    }
                    i++;
                }

                ql_release_iterator(iter);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("iterate reverse + delete at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                quicklistPushTail(ql, "abc", 3);
                quicklistPushTail(ql, "def", 3);
                quicklistPushTail(ql, "hij", 3);
                quicklistPushTail(ql, "jkl", 3);
                quicklistPushTail(ql, "oop", 3);

                quicklistEntry entry;
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
                int i = 0;
                while (quicklistNext(iter, &entry)) {
                    if (quicklistCompare(&entry, (unsigned char *)"hij", 3)) {
                        quicklistDelEntry(iter, &entry);
                    }
                    i++;
                }
                ql_release_iterator(iter);

                if (i != 5)
                    ERR("Didn't iterate 5 times, iterated %d times.", i);

                /* Check results after deletion of "hij" */
                iter = quicklistGetIterator(ql, AL_START_HEAD);
                i = 0;
                char *vals[] = {"abc", "def", "jkl", "oop"};
                while (quicklistNext(iter, &entry)) {
                    if (!quicklistCompare(&entry, (unsigned char *)vals[i],
                                          3)) {
                        ERR("Value at %d didn't match %s\n", i, vals[i]);
                    }
                    i++;
                }
                ql_release_iterator(iter);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("iterator at index test at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 760; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }

                quicklistEntry entry;
                quicklistIter *iter =
                    quicklistGetIteratorAtIdx(ql, AL_START_HEAD, 437);
                int i = 437;
                while (quicklistNext(iter, &entry)) {
                    if (entry.longval != nums[i])
                        ERR("Expected %lld, but got %lld", entry.longval,
                            nums[i]);
                    i++;
                }
                ql_release_iterator(iter);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("ltrim test A at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 32; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 1, 32, 32, 32);
                /* ltrim 25 53 (keep [25,32] inclusive = 7 remaining) */
                quicklistDelRange(ql, 0, 25);
                quicklistDelRange(ql, 0, 0);
                quicklistEntry entry;
                for (int i = 0; i < 7; i++) {
                    iter = quicklistGetIteratorEntryAtIdx(ql, i, &entry);
                    if (entry.longval != nums[25 + i])
                        ERR("Deleted invalid range!  Expected %lld but got "
                            "%lld",
                            entry.longval, nums[25 + i]);
                    ql_release_iterator(iter);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 1, 7, 7, 7);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("ltrim test B at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                /* Force-disable compression because our 33 sequential
                 * integers don't compress and the check always fails. */
                quicklist *ql = quicklistNew(fills[f], QUICKLIST_NOCOMPRESS);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                /* ltrim 5 16 (keep [5,16] inclusive = 12 remaining) */
                quicklistDelRange(ql, 0, 5);
                quicklistDelRange(ql, -16, 16);
                if (fills[f] == 32)
                    ql_verify(ql, 1, 12, 12, 12);
                quicklistEntry entry;

                iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
                if (entry.longval != 5)
                    ERR("A: longval not 5, but %lld", entry.longval);
                ql_release_iterator(iter);

                iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
                if (entry.longval != 16)
                    ERR("B! got instead: %lld", entry.longval);
                quicklistPushTail(ql, "bobobob", 7);
                ql_release_iterator(iter);

                iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
                int sz = entry.sz;
                if (strncmp((char *)entry.value, "bobobob", 7))
                    ERR("Tail doesn't match bobobob, it's %.*s instead",
                        sz, entry.value);
                ql_release_iterator(iter);

                for (int i = 0; i < 12; i++) {
                    iter = quicklistGetIteratorEntryAtIdx(ql, i, &entry);
                    if (entry.longval != nums[5 + i])
                        ERR("Deleted invalid range!  Expected %lld but got "
                            "%lld",
                            entry.longval, nums[5 + i]);
                    ql_release_iterator(iter);
                }
                quicklistRelease(ql);
            }
        }

        TEST_DESC("ltrim test C at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                /* ltrim 3 3 (keep [3,3] inclusive = 1 remaining) */
                quicklistDelRange(ql, 0, 3);
                quicklistDelRange(ql, -29,
                                  4000); /* make sure not loop forever */
                if (fills[f] == 32)
                    ql_verify(ql, 1, 1, 1, 1);
                quicklistEntry entry;
                iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
                if (entry.longval != -5157318210846258173)
                    ERROR;
                ql_release_iterator(iter);
                quicklistRelease(ql);
            }
        }

        TEST_DESC("ltrim test D at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                quicklist *ql = quicklistNew(fills[f], options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                quicklistDelRange(ql, -12, 3);
                if (ql->count != 30)
                    ERR("Didn't delete exactly three elements!  Count is: %lu",
                        ql->count);
                quicklistRelease(ql);
            }
        }

        long long stop = mstime();
        runtime[_i] = stop - start;
    }

    /* Run a longer test of compression depth outside of primary test loop. */
    int list_sizes[] = {250, 251, 500, 999, 1000};
    long long start = mstime();
    int list_count = accurate ? (int)(sizeof(list_sizes) / sizeof(*list_sizes)) : 1;
    for (int list = 0; list < list_count; list++) {
        TEST_DESC("verify specific compression of interior nodes with %d list ",
                  list_sizes[list]) {
            for (int f = 0; f < fill_count; f++) {
                for (int depth = 1; depth < 40; depth++) {
                    /* skip over many redundant test cases */
                    quicklist *ql = quicklistNew(fills[f], depth);
                    for (int i = 0; i < list_sizes[list]; i++) {
                        quicklistPushTail(ql, genstr("hello TAIL", i + 1), 64);
                        quicklistPushHead(ql, genstr("hello HEAD", i + 1), 64);
                    }

                    for (int step = 0; step < 2; step++) {
                        /* test remove node */
                        if (step == 1) {
                            for (int i = 0; i < list_sizes[list] / 2; i++) {
                                unsigned char *data;
                                assert(quicklistPop(ql, QUICKLIST_HEAD, &data,
                                                    NULL, NULL));
                                zfree(data);
                                assert(quicklistPop(ql, QUICKLIST_TAIL, &data,
                                                    NULL, NULL));
                                zfree(data);
                            }
                        }
                        quicklistNode *node = ql->head;
                        unsigned int low_raw = ql->compress;
                        unsigned int high_raw = ql->len - ql->compress;

                        for (unsigned int at = 0; at < ql->len;
                            at++, node = node->next) {
                            if (at < low_raw || at >= high_raw) {
                                if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                                    ERR("Incorrect compression: node %d is "
                                        "compressed at depth %d ((%u, %u); total "
                                        "nodes: %lu; size: %zu)",
                                        at, depth, low_raw, high_raw, ql->len,
                                        node->sz);
                                }
                            } else {
                                if (node->encoding != QUICKLIST_NODE_ENCODING_LZF) {
                                    ERR("Incorrect non-compression: node %d is NOT "
                                        "compressed at depth %d ((%u, %u); total "
                                        "nodes: %lu; size: %zu; attempted: %d)",
                                        at, depth, low_raw, high_raw, ql->len,
                                        node->sz, node->attempted_compress);
                                }
                            }
                        }
                    }

                    quicklistRelease(ql);
                }
            }
        }
    }
    long long stop = mstime();

    printf("\n");
    for (size_t i = 0; i < option_count; i++)
        printf("Test Loop %02d: %0.2f seconds.\n", options[i],
               (float)runtime[i] / 1000);
    printf("Compressions: %0.2f seconds.\n", (float)(stop - start) / 1000);
    printf("\n");

    TEST("bookmark get updated to next item") {
        quicklist *ql = quicklistNew(1, 0);
        quicklistPushTail(ql, "1", 1);
        quicklistPushTail(ql, "2", 1);
        quicklistPushTail(ql, "3", 1);
        quicklistPushTail(ql, "4", 1);
        quicklistPushTail(ql, "5", 1);
        assert(ql->len==5);
        /* add two bookmarks, one pointing to the node before the last. */
        assert(quicklistBookmarkCreate(&ql, "_dummy", ql->head->next));
        assert(quicklistBookmarkCreate(&ql, "_test", ql->tail->prev));
        /* test that the bookmark returns the right node, delete it and see that the bookmark points to the last node */
        assert(quicklistBookmarkFind(ql, "_test") == ql->tail->prev);
        assert(quicklistDelRange(ql, -2, 1));
        assert(quicklistBookmarkFind(ql, "_test") == ql->tail);
        /* delete the last node, and see that the bookmark was deleted. */
        assert(quicklistDelRange(ql, -1, 1));
        assert(quicklistBookmarkFind(ql, "_test") == NULL);
        /* test that other bookmarks aren't affected */
        assert(quicklistBookmarkFind(ql, "_dummy") == ql->head->next);
        assert(quicklistBookmarkFind(ql, "_missing") == NULL);
        assert(ql->len==3);
        quicklistBookmarksClear(ql); /* for coverage */
        assert(quicklistBookmarkFind(ql, "_dummy") == NULL);
        quicklistRelease(ql);
    }

    TEST("bookmark limit") {
        int i;
        quicklist *ql = quicklistNew(1, 0);
        quicklistPushHead(ql, "1", 1);
        for (i=0; i<QL_MAX_BM; i++)
            assert(quicklistBookmarkCreate(&ql, genstr("",i), ql->head));
        /* when all bookmarks are used, creation fails */
        assert(!quicklistBookmarkCreate(&ql, "_test", ql->head));
        /* delete one and see that we can now create another */
        assert(quicklistBookmarkDelete(ql, "0"));
        assert(quicklistBookmarkCreate(&ql, "_test", ql->head));
        /* delete one and see that the rest survive */
        assert(quicklistBookmarkDelete(ql, "_test"));
        for (i=1; i<QL_MAX_BM; i++)
            assert(quicklistBookmarkFind(ql, genstr("",i)) == ql->head);
        /* make sure the deleted ones are indeed gone */
        assert(!quicklistBookmarkFind(ql, "0"));
        assert(!quicklistBookmarkFind(ql, "_test"));
        quicklistRelease(ql);
    }

    if (flags & REDIS_TEST_LARGE_MEMORY) {
        TEST("compress and decompress quicklist listpack node") {
            quicklistNode *node = quicklistCreateNode();
            node->entry = lpNew(0);

            /* Just to avoid triggering the assertion in __quicklistCompressNode(),
             * it disables the passing of quicklist head or tail node. */
            node->prev = quicklistCreateNode();
            node->next = quicklistCreateNode();
            
            /* Create a rand string */
            size_t sz = (1 << 25); /* 32MB per one entry */
            unsigned char *s = zmalloc(sz);
            randstring(s, sz);

            /* Keep filling the node, until it reaches 1GB */
            for (int i = 0; i < 32; i++) {
                node->entry = lpAppend(node->entry, s, sz);
                quicklistNodeUpdateSz(node);

                long long start = mstime();
                assert(__quicklistCompressNode(node));
                assert(__quicklistDecompressNode(node));
                printf("Compress and decompress: %zu MB in %.2f seconds.\n",
                       node->sz/1024/1024, (float)(mstime() - start) / 1000);
            }

            zfree(s);
            zfree(node->prev);
            zfree(node->next);
            zfree(node->entry);
            zfree(node);
        }

#if ULONG_MAX >= 0xffffffffffffffff
        TEST("compress and decomress quicklist plain node large than UINT32_MAX") {
            size_t sz = (1ull << 32);
            unsigned char *s = zmalloc(sz);
            randstring(s, sz);
            memcpy(s, "helloworld", 10);
            memcpy(s + sz - 10, "1234567890", 10);

            quicklistNode *node = __quicklistCreatePlainNode(s, sz);

            /* Just to avoid triggering the assertion in __quicklistCompressNode(),
             * it disables the passing of quicklist head or tail node. */
            node->prev = quicklistCreateNode();
            node->next = quicklistCreateNode();

            long long start = mstime();
            assert(__quicklistCompressNode(node));
            assert(__quicklistDecompressNode(node));
            printf("Compress and decompress: %zu MB in %.2f seconds.\n",
                   node->sz/1024/1024, (float)(mstime() - start) / 1000);

            assert(memcmp(node->entry, "helloworld", 10) == 0);
            assert(memcmp(node->entry + sz - 10, "1234567890", 10) == 0);
            zfree(node->prev);
            zfree(node->next);
            zfree(node->entry);
            zfree(node);
        }
#endif
    }

    if (!err)
        printf("ALL TESTS PASSED!\n");
    else
        ERR("Sorry, not all tests passed!  In fact, %d tests failed.", err);

    return err;
}
#endif
