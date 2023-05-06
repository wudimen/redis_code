/*
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

#include "server.h"
#include "intset.h"  /* Compact integer set structure */

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,										// 取第一个set与其他set的合集（UNION）或差值（DIFF）
                              robj *dstkey, int op);

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a regular
 * hash table. */
robj *setTypeCreate(sds value) {																			// 根据value的类型，创建一个set（intste/listpack）
    if (isSdsRepresentableAsLongLong(value,NULL) == C_OK)
        return createIntsetObject();
    return createSetListpackObject();
}

/* Return the maximum number of entries to store in an intset. */
static size_t intsetMaxEntries(void) {																		// 返回intset可以存储的最大数量（最大为1<<30）
    size_t max_entries = server.set_max_intset_entries;
    /* limit to 1G entries due to intset internals. */
    if (max_entries >= 1<<30) max_entries = 1<<30;
    return max_entries;
}

/* Converts intset to HT if it contains too many entries. */
static void maybeConvertIntset(robj *subject) {																// 如果数据量过大的话，将结构转换成hashtable
    serverAssert(subject->encoding == OBJ_ENCODING_INTSET);
    if (intsetLen(subject->ptr) > intsetMaxEntries())			// 如果长度过长的话，把subject转换成hashtable
        setTypeConvert(subject,OBJ_ENCODING_HT);
}

/* When you know all set elements are integers, call this to convert the set to
 * an intset. No conversion happens if the set contains too many entries for an
 * intset. */
static void maybeConvertToIntset(robj *set) {																// 尝试将listpack/hashtable转换成intset
    if (set->encoding == OBJ_ENCODING_INTSET) return; /* already intset */
    if (setTypeSize(set) > intsetMaxEntries()) return; /* can't use intset */		// 长度过长，不允许转换
    intset *is = intsetNew();		// 新建一个空的intset
    char *str;
    size_t len;
    int64_t llval;
    setTypeIterator *si = setTypeInitIterator(set);		// 初始化一个迭代器
    while (setTypeNext(si, &str, &len, &llval) != -1) {			// 取出下一个元素，看是否能插入到intset中
        if (str) {
            /* If the element is returned as a string, we may be able to convert
             * it to integer. This happens for OBJ_ENCODING_HT. */
            serverAssert(string2ll(str, len, (long long *)&llval));		// 将str转换成int
        }
        uint8_t success = 0;
        is = intsetAdd(is, llval, &success);		// 插入到intset
        serverAssert(success);
    }
    setTypeReleaseIterator(si);			// 释放迭代器与原来的原来的set
    freeSetObject(set); /* frees the internals but not robj itself */
    set->ptr = is;
    set->encoding = OBJ_ENCODING_INTSET;
}

/* Add the specified sds value into a set.
 *
 * If the value was already member of the set, nothing is done and 0 is
 * returned, otherwise the new element is added and 1 is returned. */
int setTypeAdd(robj *subject, sds value) {																		// 向subject中插入一个sds类型的数据
    return setTypeAddAux(subject, value, sdslen(value), 0, 1);
}

/* Add member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was added and 0 if it was already a member. */
int setTypeAddAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {							// 将len长度的str或整数llval插入到set中（类型随之改变(intset->listpack->hashtable)，str_is_sds：str是否是sds格式）
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET) {			// 是intset的话：直接插入数据，并且返回1
            uint8_t success = 0;
            set->ptr = intsetAdd(set->ptr, llval, &success);
            if (success) maybeConvertIntset(set);			// 数据量过大的话，将类型转换成hashtable
            return success;
        }
        /* Convert int to string. */			// 数据为整数，类型不一定为intset，也可能是listpack的整数格式
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);		// 将整数转换成string，并保存再str中
        str = tmpbuf;
        str_is_sds = 0;
    }

    serverAssert(str);
    if (set->encoding == OBJ_ENCODING_HT) {			// 要插入到hashtable中的话，查找是否已有key，没的话直接插入到对应位置
        /* Avoid duping the string if it is an sds string. */
        sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);		// 先把普通str转换成sds
        dict *ht = set->ptr;
        void *position = dictFindPositionForInsert(ht, sdsval, NULL);		// 查找hashtable中是否已经存在key，不存在的话，返回key对应的链表节点
        if (position) {		// 没有该key
            /* Key doesn't already exist in the set. Add it but dup the key. */
            if (sdsval == str) sdsval = sdsdup(sdsval);		// 需复制一份sds（外部可能还要用）
            dictInsertAtPosition(ht, sdsval, position);		// 再ht中的position位置插入sdsval
        } else if (sdsval != str) {			// 已有key的话，直接返回false，且有必要的话，释放sdsval
            /* String is already a member. Free our temporary sds copy. */
            sdsfree(sdsval);
        }
        return (position != NULL);
    } else if (set->encoding == OBJ_ENCODING_LISTPACK) {			// 插入到listpack的话：不存在则按照类型插入对应的位置
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        if (p != NULL)
            p = lpFind(lp, p, (unsigned char*)str, len, 0);			// 查找与str相等的元素位置
        if (p == NULL) {		// 没有str或lp为空的话：
            /* Not found.  */
            if (lpLength(lp) < server.set_max_listpack_entries &&
                len <= server.set_max_listpack_value &&
                lpSafeToAdd(lp, len))		// 长度允许的话，直接插入
            {
                if (str == tmpbuf) {		// 原来是整数：直接插入整数
                    /* This came in as integer so we can avoid parsing it again.
                     * TODO: Create and use lpFindInteger; don't go via string. */
                    lp = lpAppendInteger(lp, llval);
                } else {					// 原来是字符串：插入字符串
                    lp = lpAppend(lp, (unsigned char*)str, len);
                }
                set->ptr = lp;
            } else {					// 长度过长的话，要转换成hashtable
                /* Size limit is reached. Convert to hashtable and add. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_HT, lpLength(lp) + 1, 1);		// 转换成hahstable
                serverAssert(dictAdd(set->ptr,sdsnewlen(str,len),NULL) == DICT_OK);		// 插入元素
            }
            return 1;
        }
    } else if (set->encoding == OBJ_ENCODING_INTSET) {		// 插入到intset：
        long long value;
        if (string2ll(str, len, &value)) {			// 是整数的话：直接插入
            uint8_t success = 0;
            set->ptr = intsetAdd(set->ptr,value,&success);
            if (success) {			// 插入成功的话，看是否要转换成hashtable
                maybeConvertIntset(set);
                return 1;
            }
        } else {
            /* Check if listpack encoding is safe not to cross any threshold. */
            size_t maxelelen = 0, totsize = 0;
            unsigned long n = intsetLen(set->ptr);
            if (n != 0) {
                size_t elelen1 = sdigits10(intsetMax(set->ptr));
                size_t elelen2 = sdigits10(intsetMin(set->ptr));
                maxelelen = max(elelen1, elelen2);		// 获取intset内容中最大的的位数
                size_t s1 = lpEstimateBytesRepeatedInteger(intsetMax(set->ptr), n);
                size_t s2 = lpEstimateBytesRepeatedInteger(intsetMin(set->ptr), n);
                totsize = max(s1, s2);			// 计算需要的内存
            }
            if (intsetLen((const intset*)set->ptr) < server.set_max_listpack_entries &&
                len <= server.set_max_listpack_value &&
                maxelelen <= server.set_max_listpack_value &&
                lpSafeToAdd(NULL, totsize + len))			// 看是否可以转换成listpack
            {
                /* In the "safe to add" check above we assumed all elements in
                 * the intset are of size maxelelen. This is an upper bound. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_LISTPACK,		// 转换成listpack
                                        intsetLen(set->ptr) + 1, 1);
                unsigned char *lp = set->ptr;				
                lp = lpAppend(lp, (unsigned char *)str, len);		// 插入str
                lp = lpShrinkToFit(lp);			// 请空对于内存
                set->ptr = lp;
                return 1;
            } else {										// 不能转换成listpack的话，只能转换成hashtable
                setTypeConvertAndExpand(set, OBJ_ENCODING_HT,			// 转换成hashtable
                                        intsetLen(set->ptr) + 1, 1);
                /* The set *was* an intset and this value is not integer
                 * encodable, so dictAdd should always work. */
                serverAssert(dictAdd(set->ptr,sdsnewlen(str,len),NULL) == DICT_OK);		// 插入str
                return 1;
            }
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* Deletes a value provided as an sds string from the set. Returns 1 if the
 * value was deleted and 0 if it was not a member of the set. */
int setTypeRemove(robj *setobj, sds value) {																	// 删除setobj中的sds类型元素value
    return setTypeRemoveAux(setobj, value, sdslen(value), 0, 1);
}

/* Remove a member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was deleted and 0 if it was not a member of the set. */
int setTypeRemoveAux(robj *setobj, char *str, size_t len, int64_t llval, int str_is_sds) {						// 将setobj中内容为len长度的str或整数llval的内容删除（str_is_sds：str是否是sds）
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {				// 不是str的话，将其转换成string
        if (setobj->encoding == OBJ_ENCODING_INTSET) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            return success;
        }
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (setobj->encoding == OBJ_ENCODING_HT) {			// 类型是hashtable的话：先将str转换成sds（hashtable的key只能储存sds），再查找删除resize
        sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
        int deleted = (dictDelete(setobj->ptr, sdsval) == DICT_OK);
        if (deleted && htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);		// 需要resize的话，resize一下
        if (sdsval != str) sdsfree(sdsval); /* free temp copy */
        return deleted;
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {				// 类型是listpack的话：从开始地方查找值为str的内容，找到的话直接删除，没有了的话删除该set
        unsigned char *lp = setobj->ptr;
        unsigned char *p = lpFirst(lp);
        if (p == NULL) return 0;
        p = lpFind(lp, p, (unsigned char*)str, len, 0);
        if (p != NULL) {
            lp = lpDelete(lp, p, NULL);
            setobj->ptr = lp;
            return 1;
        }
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {				// 类型是intset的话：先把sds还原成int，再删除该数据
        long long llval;
        if (string2ll(str, len, &llval)) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* Check if an sds string is a member of the set. Returns 1 if the value is a
 * member of the set and 0 if it isn't. */
int setTypeIsMember(robj *subject, sds value) {																	// 查看sds类型元素是否是subject的内容
    return setTypeIsMemberAux(subject, value, sdslen(value), 0, 1);
}

/* Membership checking optimized for the different encodings. The value can be
 * provided as an sds string (indicated by passing str_is_sds = 1), as string
 * and length (str_is_sds = 0) or as an integer in which case str is set to NULL
 * and llval is provided instead.
 *
 * Returns 1 if the value is a member of the set and 0 if it isn't. */
int setTypeIsMemberAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {						// 根据类型，查找set中是否含有len长度的str或整数llval
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {		// 先把任何类型的数据转换成sds，用str_is_sds标识该数据是否是sds
        if (set->encoding == OBJ_ENCODING_INTSET)
            return intsetFind(set->ptr, llval);
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (set->encoding == OBJ_ENCODING_LISTPACK) {		
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        return p && lpFind(lp, p, (unsigned char*)str, len, 0);		// 从开始找，看是否找到str
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        return string2ll(str, len, &llval) && intsetFind(set->ptr, llval);		// 不能转换成整数或该整数不存在与set中：则返回不存在
    } else if (set->encoding == OBJ_ENCODING_HT && str_is_sds) {		// 直接查找
        return dictFind(set->ptr, (sds)str) != NULL;
    } else if (set->encoding == OBJ_ENCODING_HT) {						// 先转换成sds再查找
        sds sdsval = sdsnewlen(str, len);
        int result = dictFind(set->ptr, sdsval) != NULL;
        sdsfree(sdsval);
        return result;
    } else {
        serverPanic("Unknown set encoding");
    }
}

setTypeIterator *setTypeInitIterator(robj *subject) {															// 创建一个setIterator
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
    if (si->encoding == OBJ_ENCODING_HT) {					// 根据subject的类型，为iter的参数赋值
        si->di = dictGetIterator(subject->ptr);
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        si->ii = 0;
    } else if (si->encoding == OBJ_ENCODING_LISTPACK) {
        si->lpi = NULL;
    } else {
        serverPanic("Unknown set encoding");
    }
    return si;
}

void setTypeReleaseIterator(setTypeIterator *si) {																// 释放setIter
    if (si->encoding == OBJ_ENCODING_HT)
        dictReleaseIterator(si->di);
    zfree(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position, as a string or as an integer.
 *
 * Since set elements can be internally be stored as SDS strings, char buffers or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointers
 * (str and len) or (llele) depending on whether the value is stored as a string
 * or as an integer internally.
 *
 * If OBJ_ENCODING_HT is returned, then str points to an sds string and can be
 * used as such. If OBJ_ENCODING_INTSET, then llele is populated and str is
 * pointed to NULL. If OBJ_ENCODING_LISTPACK is returned, the value can be
 * either a string or an integer. If *str is not NULL, then str and len are
 * populated with the string content and length. Otherwise, llele populated with
 * an integer value.
 *
 * Note that str, len and llele pointers should all be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused.
 *
 * When there are no more elements -1 is returned. */
int setTypeNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele) {									// 取出si代表的节点的下一个节点，是str的话：str保存内容，len保存内容长度；是整数的话：llele保存整数；
    if (si->encoding == OBJ_ENCODING_HT) {						// hashtable：取下一个节点，且为参数赋值
        dictEntry *de = dictNext(si->di);
        if (de == NULL) return -1;
        *str = dictGetKey(de);
        *len = sdslen(*str);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (si->encoding == OBJ_ENCODING_INTSET) {			// intste：直接取出下标为ii+1的元素
        if (!intsetGet(si->subject->ptr,si->ii++,llele))
            return -1;
        *str = NULL;
    } else if (si->encoding == OBJ_ENCODING_LISTPACK) {			// listpack：取lp的下一个节点，且为参数赋值
        unsigned char *lp = si->subject->ptr;
        unsigned char *lpi = si->lpi;
        if (lpi == NULL) {				// 从第一个节点开始，取下一个节点
            lpi = lpFirst(lp);
        } else {
            lpi = lpNext(lp, lpi);
        }
        if (lpi == NULL) return -1;			// 取到了结尾
        si->lpi = lpi;
        unsigned int l;
        *str = (char *)lpGetValue(lpi, &l, (long long *)llele);		// 为参数赋值
        *len = (size_t)l;
    } else {
        serverPanic("Wrong set encoding in setTypeNext");
    }
    return si->encoding;
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new SDS
 * strings. So if you don't retain a pointer to this object you should call
 * sdsfree() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue. */
sds setTypeNextObject(setTypeIterator *si) {																	// 取出下一个节点的内容的sds格式
    int64_t intele;
    char *str;
    size_t len;

    if (setTypeNext(si, &str, &len, &intele) == -1) return NULL;		// 取出失败
    if (str != NULL) return sdsnewlen(str, len);			// 将str与知识转换成sds并返回
    return sdsfromlonglong(intele);
}

/* Return random element from a non empty set.
 * The returned element can be an int64_t value if the set is encoded
 * as an "intset" blob of integers, or an string.
 *
 * The caller provides three pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and can be used by the caller to check if the
 * int64_t pointer or the str and len pointers were populated, as for
 * setTypeNext. If OBJ_ENCODING_HT is returned, str is pointed to a
 * string which is actually an sds string and it can be used as such.
 *
 * Note that both the str, len and llele pointers should be passed and cannot
 * be NULL. If str is set to NULL, the value is an integer stored in llele. */
int setTypeRandomElement(robj *setobj, char **str, size_t *len, int64_t *llele) {								// 按照类型，随机获取某一个节点的内容
    if (setobj->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictGetFairRandomKey(setobj->ptr);		// 随机返回一个setobj里面的节点
        *str = dictGetKey(de);				// 获取随机节点的内容
        *len = sdslen(*str);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        *llele = intsetRandom(setobj->ptr);			// 随机获取一个intste的内容
        *str = NULL; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = setobj->ptr;		// 获取listpack
        int r = rand() % lpLength(lp);			// 获取len以内的随机数
        unsigned char *p = lpSeek(lp, r);		// 偏移至随机数位置
        unsigned int l;
        *str = (char *)lpGetValue(p, &l, (long long *)llele);		// 取出随机位置的内容
        *len = (size_t)l;
    } else {
        serverPanic("Unknown set encoding");
    }
    return setobj->encoding;
}

/* Pops a random element and returns it as an object. */
robj *setTypePopRandom(robj *set) {																				// 随机删除一个系欸但，并返回该节点的内容
    robj *obj;
    if (set->encoding == OBJ_ENCODING_LISTPACK) {			// listpakc的话：随机指向一个后面的节点，获取内容后删除
        /* Find random and delete it without re-seeking the listpack. */
        unsigned int i = 0;
        unsigned char *p = lpNextRandom(set->ptr, lpFirst(set->ptr), &i, 1, 0);			// 将p指向一个后面随机的节点
        unsigned int len = 0; /* initialize to silence warning */
        long long llele = 0; /* initialize to silence warning */
        char *str = (char *)lpGetValue(p, &len, &llele);		// 获取随即节点的内容
        if (str)
            obj = createStringObject(str, len);
        else
            obj = createStringObjectFromLongLong(llele);
        set->ptr = lpDelete(set->ptr, p, NULL);					// 删除随机节点
    } else {												// intset/hashtable的话：
        char *str;
        size_t len = 0;
        int64_t llele = 0;
        int encoding = setTypeRandomElement(set, &str, &len, &llele);		// 随机获取一个节点的内容
        if (str)
            obj = createStringObject(str, len);
        else
            obj = createStringObjectFromLongLong(llele);
        setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);	// 删除该节点
    }
    return obj;
}

unsigned long setTypeSize(const robj *subject) {																// 获取subject的元素个数
    if (subject->encoding == OBJ_ENCODING_HT) {
        return dictSize((const dict*)subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        return intsetLen((const intset*)subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        return lpLength((unsigned char *)subject->ptr);
    } else {
        serverPanic("Unknown set encoding");
    }
}

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *setobj, int enc) {																	// 将setobj转换成enc类型的结构
    setTypeConvertAndExpand(setobj, enc, setTypeSize(setobj), 1);
}

/* Converts a set to the specified encoding, pre-sizing it for 'cap' elements.
 * The 'panic' argument controls whether to panic on OOM (panic=1) or return
 * C_ERR on OOM (panic=0). If panic=1 is given, this function always returns
 * C_OK. */
int setTypeConvertAndExpand(robj *setobj, int enc, unsigned long cap, int panic) {								// 将setobj转换成enc类型的数据结构（cap：数据量）
    setTypeIterator *si;
    serverAssertWithInfo(NULL,setobj,setobj->type == OBJ_SET &&
                             setobj->encoding != enc);

    if (enc == OBJ_ENCODING_HT) {																	// intset转换成hashtable（一个一个拿出插入）
        dict *d = dictCreate(&setDictType);		// 创建一个新的hashtable
        sds element;

        /* Presize the dict to avoid rehashing */
        if (panic) {				// 扩容值合适的大小
            dictExpand(d, cap);
        } else if (dictTryExpand(d, cap) != DICT_OK) {
            dictRelease(d);
            return C_ERR;
        }

        /* To add the elements we extract integers and create redis objects */
        si = setTypeInitIterator(setobj);		// 创建迭代器
        while ((element = setTypeNextObject(si)) != NULL) {
            serverAssert(dictAdd(d,element,NULL) == DICT_OK);			// 一个一个将intset的内容以sds格式插入到hashtable中
        }
        setTypeReleaseIterator(si);				// 释放迭代器

        freeSetObject(setobj); /* frees the internals but not setobj itself */			// 释放原intset
        setobj->encoding = OBJ_ENCODING_HT;			// 更改encoding
        setobj->ptr = d;							// 改变内容指向
    } else if (enc == OBJ_ENCODING_LISTPACK) {						// intset转换成listpack：取出数据，以数据类型插入至listpack
        /* Preallocate the minimum two bytes per element (enc/value + backlen) */
        size_t estcap = cap * 2;
        if (setobj->encoding == OBJ_ENCODING_INTSET && setTypeSize(setobj) > 0) {			// 计算储存cap个数据最少需要多少内存
            /* If we're converting from intset, we have a better estimate. */
            size_t s1 = lpEstimateBytesRepeatedInteger(intsetMin(setobj->ptr), cap);
            size_t s2 = lpEstimateBytesRepeatedInteger(intsetMax(setobj->ptr), cap);
            estcap = max(s1, s2);
        }
        unsigned char *lp = lpNew(estcap);			// 创建一个新的listpack
        char *str;
        size_t len;
        int64_t llele;
        si = setTypeInitIterator(setobj);
        while (setTypeNext(si, &str, &len, &llele) != -1) {			// 取出数据，按照类型向listpack中插入数据
            if (str != NULL)
                lp = lpAppend(lp, (unsigned char *)str, len);
            else
                lp = lpAppendInteger(lp, llele);
        }
        setTypeReleaseIterator(si);

        freeSetObject(setobj); /* frees the internals but not setobj itself */		// 释放原intset并修改原robj的内容
        setobj->encoding = OBJ_ENCODING_LISTPACK;
        setobj->ptr = lp;
    } else {
        serverPanic("Unsupported set conversion");
    }
    return C_OK;
}

/* This is a helper function for the COPY command.
 * Duplicate a set object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *setTypeDup(robj *o) {																						// 复制一份o并返回
    robj *set;
    setTypeIterator *si;

    serverAssert(o->type == OBJ_SET);

    /* Create a new set object that have the same encoding as the original object's encoding */
    if (o->encoding == OBJ_ENCODING_INTSET) {			// intset的话：直接复制内存
        intset *is = o->ptr;
        size_t size = intsetBlobLen(is);
        intset *newis = zmalloc(size);
        memcpy(newis,is,size);
        set = createObject(OBJ_SET, newis);
        set->encoding = OBJ_ENCODING_INTSET;
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {	// listpack的话：直接复制内存
        unsigned char *lp = o->ptr;
        size_t sz = lpBytes(lp);
        unsigned char *new_lp = zmalloc(sz);
        memcpy(new_lp, lp, sz);
        set = createObject(OBJ_SET, new_lp);
        set->encoding = OBJ_ENCODING_LISTPACK;
    } else if (o->encoding == OBJ_ENCODING_HT) {		// hahsatble的话：新建一个新的setobj，之后一个一个取出添加
        set = createSetObject();
        dict *d = o->ptr;
        dictExpand(set->ptr, dictSize(d));		// 先改变容量
        si = setTypeInitIterator(o);
        char *str;
        size_t len;
        int64_t intobj;
        while (setTypeNext(si, &str, &len, &intobj) != -1) {
            setTypeAdd(set, (sds)str);		// hashtable返回的一定是str
        }
        setTypeReleaseIterator(si);
    } else {
        serverPanic("Unknown set encoding");
    }
    return set;
}

void saddCommand(client *c) {																					// 在key对应的set中添加元素（可多个），返回添加元素的个数
    robj *set;
    int j, added = 0;

    set = lookupKeyWrite(c->db,c->argv[1]);		// 获取对应的set
    if (checkType(c,set,OBJ_SET)) return;		// 判断类型
    
    if (set == NULL) {							// 无对应set的话，先创建一个新的
        set = setTypeCreate(c->argv[2]->ptr);
        dbAdd(c->db,c->argv[1],set);
    }

    for (j = 2; j < c->argc; j++) {				// 连续插入参数中的元素
        if (setTypeAdd(set,c->argv[j]->ptr)) added++;
    }
    if (added) {								// 添加了元素的话：提醒一下c
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->argv[1],c->db->id);
    }
    server.dirty += added;
    addReplyLongLong(c,added);					// 回复添加了的元素数量
}

void sremCommand(client *c) {																					// 删除key对应的set中的元素（可多个），返回删除元素的个数
    robj *set;
    int j, deleted = 0, keyremoved = 0;

    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||		// 获取对应的set（没有的话直接返回，没有set删除个屁）
        checkType(c,set,OBJ_SET)) return;

    for (j = 2; j < c->argc; j++) {		// 将参数中的元素全部删除
        if (setTypeRemove(set,c->argv[j]->ptr)) {	// 删除成功：
            deleted++;
            if (setTypeSize(set) == 0) {			// 删除到没有元素的话，删除该set
                dbDelete(c->db,c->argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }	
    if (deleted) {									// 删除了元素的话：提醒一下c
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET,"srem",c->argv[1],c->db->id);
        if (keyremoved)
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);					// 回复删除了的元素个数
}

void smoveCommand(client *c) {																					// 将set1中的elem元素移动到set2中
    robj *srcset, *dstset, *ele;
    srcset = lookupKeyWrite(c->db,c->argv[1]);		// 获取源set
    dstset = lookupKeyWrite(c->db,c->argv[2]);		// 获取目标set
    ele = c->argv[3];

    /* If the source key does not exist return 0 */
    if (srcset == NULL) {		// 源set不存在：返回移动了0个
        addReply(c,shared.czero);
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. */
    if (checkType(c,srcset,OBJ_SET) ||			// 检测类型
        checkType(c,dstset,OBJ_SET)) return;

    /* If srcset and dstset are equal, SMOVE is a no-op */
    if (srcset == dstset) {		// 源set与目标set是同一个：直接返回移动了0/1个
        addReply(c,setTypeIsMember(srcset,ele->ptr) ?
            shared.cone : shared.czero);
        return;
    }

    /* If the element cannot be removed from the src set, return 0. */
    if (!setTypeRemove(srcset,ele->ptr)) {		// 源set没有该元素：直接返回移动了0个
        addReply(c,shared.czero);
        return;
    }
    notifyKeyspaceEvent(NOTIFY_SET,"srem",c->argv[1],c->db->id);

    /* Remove the src set from the database when empty */
    if (setTypeSize(srcset) == 0) {				// 删除空了的话，删除源set
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    /* Create the destination set when it doesn't exist */
    if (!dstset) {								// 目标set不存在的话：先创建
        dstset = setTypeCreate(ele->ptr);
        dbAdd(c->db,c->argv[2],dstset);
    }

    signalModifiedKey(c,c->db,c->argv[1]);		// 告诉c创建了一个新的set
    server.dirty++;

    /* An extra key has changed when ele was successfully added to dstset */
    if (setTypeAdd(dstset,ele->ptr)) {			// 添加成功了
        server.dirty++;
        signalModifiedKey(c,c->db,c->argv[2]);
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->argv[2],c->db->id);
    }
    addReply(c,shared.cone);					 // 回复移动了一个元素
}

void sismemberCommand(client *c) {																				// 判断elem是否是set中的元素
    robj *set;

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||		// 查找key对应的set
        checkType(c,set,OBJ_SET)) return;

    if (setTypeIsMember(set,c->argv[2]->ptr))									// 查看元素是否是set里的元素，是的话，返回一，否则返回零
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}

void smismemberCommand(client *c) {																				// 判断多个数据是否是set中的元素
    robj *set;
    int j;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * sets, where SMISMEMBER should respond with a series of zeros. */
    set = lookupKeyRead(c->db,c->argv[1]);			// 查找key对应的value
    if (set && checkType(c,set,OBJ_SET)) return;	// 检查类型

    addReplyArrayLen(c,c->argc - 2);				// 回复需判断的总长度

    for (j = 2; j < c->argc; j++) {					// 连续判断是否是set内的元素
        if (set && setTypeIsMember(set,c->argv[j]->ptr))
            addReply(c,shared.cone);
        else
            addReply(c,shared.czero);
    }
}

void scardCommand(client *c) {																					// 查看set的元素个数
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||		// 查找key对应的set
        checkType(c,o,OBJ_SET)) return;

    addReplyLongLong(c,setTypeSize(o));			// 回复set的元素个数
}

/* Handle the "SPOP key <count>" variant. The normal version of the
 * command is handled by the spopCommand() function itself. */

/* How many times bigger should be the set compared to the remaining size
 * for us to use the "create new set" strategy? Read later in the
 * implementation for more info. */
#define SPOP_MOVE_STRATEGY_MUL 5

void spopWithCountCommand(client *c) {																			// 随机弹出value元素后面的count个元素（优化：剩余元素较少的时候：将剩余元素取出到另一个newset中，删除元素留在原set中）
    long l;
    unsigned long count, size;
    robj *set;

    /* Get the count argument */
    if (getPositiveLongFromObjectOrReply(c,c->argv[2],&l,NULL) != C_OK) return;		// 获取count
    count = (unsigned long) l;

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set. Otherwise, return nil */
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.emptyset[c->resp]))		// 获取key对应的set
        == NULL || checkType(c,set,OBJ_SET)) return;

    /* If count is zero, serve an empty set ASAP to avoid special
     * cases later. */
    if (count == 0) {										// pop零个：直接返回0
        addReply(c,shared.emptyset[c->resp]);
        return;
    }

    size = setTypeSize(set);								// 获取set的元素数量

    /* Generate an SPOP keyspace notification */
    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->argv[1],c->db->id);
    server.dirty += (count >= size) ? size : count;

    /* CASE 1:
     * The number of requested elements is greater than or equal to
     * the number of elements inside the set: simply return the whole set. */
    if (count >= size) {			// pop全部元素
        /* We just return the entire set */
        sunionDiffGenericCommand(c,c->argv+1,1,NULL,SET_OP_UNION);			// 返回全部元素

        /* Delete the set as it is now empty */
        dbDelete(c->db,c->argv[1]);							// 删除该set
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);

        /* todo: Move the spop notification to be executed after the command logic. */

        /* Propagate this command as a DEL operation */
        rewriteClientCommandVector(c,2,shared.del,c->argv[1]);			// 重写参数
        signalModifiedKey(c,c->db,c->argv[1]);
        return;
    }

    /* Case 2 and 3 require to replicate SPOP as a set of SREM commands.
     * Prepare our replication argument vector. Also send the array length
     * which is common to both the code paths. */
    robj *propargv[3];
    propargv[0] = shared.srem;
    propargv[1] = c->argv[1];
    addReplySetLen(c,count);						// 相应set长度

    /* Common iteration vars. */
    robj *objele;
    char *str;
    size_t len;
    int64_t llele;
    unsigned long remaining = size-count; /* Elements left after SPOP. */		// 原set剩余长度

    /* If we are here, the number of requested elements is less than the
     * number of elements inside the set. Also we are sure that count < size.
     * Use two different strategies.
     *
     * CASE 2: The number of elements to return is small compared to the
     * set size. We can just extract random elements and return them to
     * the set. */
    if (remaining*SPOP_MOVE_STRATEGY_MUL > count &&									// set是listpack的话，且剩余长度较多
        set->encoding == OBJ_ENCODING_LISTPACK)
    {
        /* Specialized case for listpack. Traverse it only once. */
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        unsigned int index = 0;
        unsigned char **ps = zmalloc(sizeof(char *) * count);
        for (unsigned long i = 0; i < count; i++) {				// 取出p后面的随机count个节点，回复给c并保存在ps中
            p = lpNextRandom(lp, p, &index, count - i, 0);
            unsigned int len;
            str = (char *)lpGetValue(p, &len, (long long *)&llele);

            if (str) {
                addReplyBulkCBuffer(c, str, len);
                objele = createStringObject(str, len);
            } else {
                addReplyBulkLongLong(c, llele);
                objele = createStringObjectFromLongLong(llele);
            }

            /* Replicate/AOF this command as an SREM operation */
            propargv[2] = objele;
            alsoPropagate(c->db->id,propargv,3,PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(objele);

            /* Store pointer for later deletion and move to next. */
            ps[i] = p;
            p = lpNext(lp, p);
            index++;
        }
        lp = lpBatchDelete(lp, ps, count);						// 删除这些保存在ps的节点
        zfree(ps);
        set->ptr = lp;
    } else if (remaining*SPOP_MOVE_STRATEGY_MUL > count) {	// 剩余节点数较多但是不是listpack
        while(count--) {						// 随机弹出set中的count个元素
            objele = setTypePopRandom(set);
            addReplyBulk(c, objele);

            /* Replicate/AOF this command as an SREM operation */
            propargv[2] = objele;
            alsoPropagate(c->db->id,propargv,3,PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(objele);
        }
    } else {																					// 剩余节点较少
    /* CASE 3: The number of elements to return is very big, approaching
     * the size of the set itself. After some time extracting random elements
     * from such a set becomes computationally expensive, so we use
     * a different strategy, we extract random elements that we don't
     * want to return (the elements that will remain part of the set),
     * creating a new set as we do this (that will be stored as the original
     * set). Then we return the elements left in the original set and
     * release it. */
        robj *newset = NULL;

        /* Create a new set with just the remaining elements. */		// 操作这个if之后：newset保存着剩余元素，set保存着删除元素
        if (set->encoding == OBJ_ENCODING_LISTPACK) {			// set是listpack：
            /* Specialized case for listpack. Traverse it only once. */
            newset = createSetListpackObject();
            unsigned char *lp = set->ptr;
            unsigned char *p = lpFirst(lp);
            unsigned int index = 0;
            unsigned char **ps = zmalloc(sizeof(char *) * remaining);
            for (unsigned long i = 0; i < remaining; i++) {			// 随机取出剩余数量个内容添加到newset中，要删除的数据保留在原set中
                p = lpNextRandom(lp, p, &index, remaining - i, 0);
                unsigned int len;
                str = (char *)lpGetValue(p, &len, (long long *)&llele);
                setTypeAddAux(newset, str, len, llele, 0);
                ps[i] = p;
                p = lpNext(lp, p);
                index++;
            }
            lp = lpBatchDelete(lp, ps, remaining);		// 删除剩余的元素
            zfree(ps);
            set->ptr = lp;
        } else {			//  set不是listpack：随机挑出剩余数量个元素，添加到newset，从set中删除
            while(remaining--) {
                int encoding = setTypeRandomElement(set, &str, &len, &llele);
                if (!newset) {
                    newset = str ? createSetListpackObject() : createIntsetObject();
                }
                setTypeAddAux(newset, str, len, llele, encoding == OBJ_ENCODING_HT);
                setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);
            }
        }

        /* Transfer the old set to the client. */
        setTypeIterator *si;
        si = setTypeInitIterator(set);
        while (setTypeNext(si, &str, &len, &llele) != -1) {				// 将待删除的元素回复给c
            if (str == NULL) {
                addReplyBulkLongLong(c,llele);
                objele = createStringObjectFromLongLong(llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
                objele = createStringObject(str, len);
            }

            /* Replicate/AOF this command as an SREM operation */
            propargv[2] = objele;
            alsoPropagate(c->db->id,propargv,3,PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(objele);
        }
        setTypeReleaseIterator(si);

        /* Assign the new set as the key value. */
        dbReplaceValue(c->db,c->argv[1],newset);
    }

    /* Don't propagate the command itself even if we incremented the
     * dirty counter. We don't want to propagate an SPOP command since
     * we propagated the command as a set of SREMs operations using
     * the alsoPropagate() API. */
    preventCommandPropagation(c);
    signalModifiedKey(c,c->db,c->argv[1]);
}

void spopCommand(client *c) {																						// 随机弹出value后面的count个元素（count没有的话为1）
    robj *set, *ele;

    if (c->argc == 3) {		// 参数数量正常
        spopWithCountCommand(c);
        return;
    } else if (c->argc > 3) {		// 参数数量不正常
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set */
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]))			// 取出set
         == NULL || checkType(c,set,OBJ_SET)) return;

    /* Pop a random element from the set */
    ele = setTypePopRandom(set);				// 随机pop一个元素

    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->argv[1],c->db->id);

    /* Replicate/AOF this command as an SREM operation */
    rewriteClientCommandVector(c,3,shared.srem,c->argv[1],ele);

    /* Add the element to the reply */
    addReplyBulk(c, ele);			// 回复给c删除的元素
    decrRefCount(ele);

    /* Delete the set if it's empty */
    if (setTypeSize(set) == 0) {			// set剩余元素数量为0：删除该set
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    /* Set has been modified */
    signalModifiedKey(c,c->db,c->argv[1]);
    server.dirty++;
}

/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself. */

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

/* If client is trying to ask for a very large number of random elements,
 * queuing may consume an unlimited amount of memory, so we want to limit
 * the number of randoms per time. */
#define SRANDFIELD_RANDOM_SAMPLE_LIMIT 1000

void srandmemberWithCountCommand(client *c) {																			// 从set中随机取出（不删除）count个元素
    long l;
    unsigned long count, size;
    int uniq = 1;
    robj *set;
    char *str;
    size_t len;
    int64_t llele;

    dict *d;

    if (getRangeLongFromObjectOrReply(c,c->argv[2],-LONG_MAX,LONG_MAX,&l,NULL) != C_OK) return;		// 获取count
    if (l >= 0) {
        count = (unsigned long) l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyarray))					// 取出set
        == NULL || checkType(c,set,OBJ_SET)) return;
    size = setTypeSize(set);							// 获取set元素个数

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        addReply(c,shared.emptyarray);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. This case is the only one that also needs to return the
     * elements in random order. */
    if (!uniq || count == 1) {					// case1：count为负：直接回复set中的随机count个元素
        addReplyArrayLen(c,count);

        if (set->encoding == OBJ_ENCODING_LISTPACK && count > 1) {		// set是listpack的话：
            /* Specialized case for listpack, traversing it only once. */
            unsigned long limit, sample_count;
            limit = count > SRANDFIELD_RANDOM_SAMPLE_LIMIT ? SRANDFIELD_RANDOM_SAMPLE_LIMIT : count;
            listpackEntry *entries = zmalloc(limit * sizeof(listpackEntry));
            while (count) {				// 使用lpRandomEntries随机取出count个元素并回复给c
                sample_count = count > limit ? limit : count;
                count -= sample_count;
                lpRandomEntries(set->ptr, sample_count, entries);
                for (unsigned long i = 0; i < sample_count; i++) {
                    if (entries[i].sval)
                        addReplyBulkCBuffer(c, entries[i].sval, entries[i].slen);
                    else
                        addReplyBulkLongLong(c, entries[i].lval);
                }
                if (c->flags & CLIENT_CLOSE_ASAP)
                    break;
            }
            zfree(entries);
            return;
        }

        while(count--) {
            setTypeRandomElement(set, &str, &len, &llele);		// 随机取出set中的某个元素并回复内容，连续count次
            if (str == NULL) {
                addReplyBulkLongLong(c,llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            if (c->flags & CLIENT_CLOSE_ASAP)
                break;
        }
        return;
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set. */
    if (count >= size) {						// case2：返回全部元素：
        setTypeIterator *si;
        addReplyArrayLen(c,size);		// 回复长度
        si = setTypeInitIterator(set);
        while (setTypeNext(si, &str, &len, &llele) != -1) {		// 以此取出全部元素并回复给c
            if (str == NULL) {
                addReplyBulkLongLong(c,llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            size--;
        }
        setTypeReleaseIterator(si);
        serverAssert(size==0);
        return;
    }

    /* CASE 2.5 listpack only. Sampling unique elements, in non-random order.
     * Listpack encoded sets are meant to be relatively small, so
     * SRANDMEMBER_SUB_STRATEGY_MUL isn't necessary and we rather not make
     * copies of the entries. Instead, we emit them directly to the output
     * buffer. */
    if (set->encoding == OBJ_ENCODING_LISTPACK) {		// case2.5：set是listpack时：以此取出count个元素并返回元素内容
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        unsigned int i = 0;
        addReplyArrayLen(c, count);		// 回复长度
        while (count) {
            p = lpNextRandom(lp, p, &i, count--, 0);		// 取随机节点
            unsigned int len;
            str = (char *)lpGetValue(p, &len, (long long *)&llele);
            if (str == NULL) {			// 回复元素内容
                addReplyBulkLongLong(c, llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            p = lpNext(lp, p);
            i++;
        }
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    d = dictCreate(&sdsReplyDictType);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 4 is highly inefficient. */
     // 执行完这个if之后：d存着要删除的元素，set中存着set本来的元素
    if (count*SRANDMEMBER_SUB_STRATEGY_MUL > size) {			// case3：要取出的元素较多：
        setTypeIterator *si;

        /* Add all the elements into the temporary dictionary. */
        si = setTypeInitIterator(set);
        dictExpand(d, size);			// 删除多余的容量
        while (setTypeNext(si, &str, &len, &llele) != -1) {		// 将set中的元素全部加入到d中
            int retval = DICT_ERR;

            if (str == NULL) {
                retval = dictAdd(d,sdsfromlonglong(llele),NULL);
            } else {
                retval = dictAdd(d, sdsnewlen(str, len), NULL);
            }
            serverAssert(retval == DICT_OK);
        }
        setTypeReleaseIterator(si);
        serverAssert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        while (size > count) {				// 从d中取出剩余数量个元素
            dictEntry *de;
            de = dictGetFairRandomKey(d);
            dictUnlink(d,dictGetKey(de));
            sdsfree(dictGetKey(de));
            dictFreeUnlinkedEntry(d,de);
            size--;
        }
    }

    /* CASE 4: We have a big set compared to the requested number of elements.
     * In this case we can simply get random elements from the set and add
     * to the temporary set, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {														// case4：要取出的元素较少
        unsigned long added = 0;
        sds sdsele;

        dictExpand(d, count);
        while (added < count) {				// 向d中随机添加count个元素
            setTypeRandomElement(set, &str, &len, &llele);
            if (str == NULL) {
                sdsele = sdsfromlonglong(llele);
            } else {
                sdsele = sdsnewlen(str, len);
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            if (dictAdd(d,sdsele,NULL) == DICT_OK)
                added++;
            else
                sdsfree(sdsele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    {
        dictIterator *di;
        dictEntry *de;

        addReplyArrayLen(c,count);		// 向c回复长度和d中的元素
        di = dictGetIterator(d);
        while((de = dictNext(di)) != NULL)
            addReplyBulkSds(c,dictGetKey(de));
        dictReleaseIterator(di);
        dictRelease(d);
    }
}

/* SRANDMEMBER <key> [<count>] */
void srandmemberCommand(client *c) {																								// 从set中随机取出（不删除）count个元素（count没有的话为1）
    robj *set;
    char *str;
    size_t len;
    int64_t llele;

    if (c->argc == 3) {				// 参数足够：取出count个元素
        srandmemberWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Handle variant without <count> argument. Reply with simple bulk string */
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]))		// 取出set
        == NULL || checkType(c,set,OBJ_SET)) return;

    setTypeRandomElement(set, &str, &len, &llele);		// 随机取出一个元素
    if (str == NULL) {					// 回复取出的元素
        addReplyBulkLongLong(c,llele);
    } else {
        addReplyBulkCBuffer(c, str, len);
    }
}

int qsortCompareSetsByCardinality(const void *s1, const void *s2) {																// 比较set1与set2那个的元素多（元素多的放后面）
    if (setTypeSize(*(robj**)s1) > setTypeSize(*(robj**)s2)) return 1;
    if (setTypeSize(*(robj**)s1) < setTypeSize(*(robj**)s2)) return -1;
    return 0;
}

/* This is used by SDIFF and in this case we can receive NULL that should
 * be handled as empty sets. */
int qsortCompareSetsByRevCardinality(const void *s1, const void *s2) {															// 比较set1与set2谁元素多（s1与s2可以为空）
    robj *o1 = *(robj**)s1, *o2 = *(robj**)s2;
    unsigned long first = o1 ? setTypeSize(o1) : 0;
    unsigned long second = o2 ? setTypeSize(o2) : 0;

    if (first < second) return 1;
    if (first > second) return -1;
    return 0;
}

/* SINTER / SMEMBERS / SINTERSTORE / SINTERCARD
 *
 * 'cardinality_only' work for SINTERCARD, only return the cardinality
 * with minimum processing and memory overheads.
 *
 * 'limit' work for SINTERCARD, stop searching after reaching the limit.
 * Passing a 0 means unlimited.
 */
void sinterGenericCommand(client *c, robj **setkeys,																			// 取所有set的交集（setkeys：所有的setkey，dstkey：结果保存在dstkey中没有的话：直接返回结果，cardinality_only：是否只返回长度，limit：交集最大长度）（优化：将元素最少的放在最前面，查找后面的是否都含有第一个集合中的元素）
                          unsigned long setnum, robj *dstkey,
                          int cardinality_only, unsigned long limit) {
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *dstset = NULL;
    char *str;
    size_t len;
    int64_t intobj;
    void *replylen = NULL;
    unsigned long j, cardinality = 0;
    int encoding, empty = 0;

    for (j = 0; j < setnum; j++) {							// 取出全部的sets
        robj *setobj = lookupKeyRead(c->db, setkeys[j]);
        if (!setobj) {
            /* A NULL is considered an empty set */
            empty += 1;
            sets[j] = NULL;
            continue;
        }
        if (checkType(c,setobj,OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }

    /* Set intersection with an empty set always results in an empty set.
     * Return ASAP if there is an empty set. */
    if (empty > 0) {		// 有空set的话，交集一定为空（a&0=0）
        zfree(sets);
        if (dstkey) {		// 有dstkey则删除该set
		    if (dbDelete(c->db,dstkey)) {
                signalModifiedKey(c,c->db,dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
                server.dirty++;
            }
            addReply(c,shared.czero);
        } else if (cardinality_only) {		// 回复交集为0
            addReplyLongLong(c,cardinality);
        } else {
            addReply(c,shared.emptyset[c->resp]);
        }
        return;
    }

    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    qsort(sets,setnum,sizeof(robj*),qsortCompareSetsByCardinality);			// 将元素多的集合放前面

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (dstkey) {							// 有dstkey的话，根据encoding创建不同的set
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        if (sets[0]->encoding == OBJ_ENCODING_INTSET) {
            /* The first set is an intset, so the result is an intset too. The
             * elements are inserted in ascending order which is efficient in an
             * intset. */
            dstset = createIntsetObject();
        } else if (sets[0]->encoding == OBJ_ENCODING_LISTPACK) {
            /* To avoid many reallocs, we estimate that the result is a listpack
             * of approximately the same size as the first set. Then we shrink
             * it or possibly convert it to intset it in the end. */
            unsigned char *lp = lpNew(lpBytes(sets[0]->ptr));
            dstset = createObject(OBJ_SET, lp);
            dstset->encoding = OBJ_ENCODING_LISTPACK;
        } else {
            /* We start off with a listpack, since it's more efficient to append
             * to than an intset. Later we can convert it to intset or a
             * hashtable. */
            dstset = createSetListpackObject();
        }
    } else if (!cardinality_only) {
        replylen = addReplyDeferredLen(c);
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    int only_integers = 1;
    si = setTypeInitIterator(sets[0]);
    while((encoding = setTypeNext(si, &str, &len, &intobj)) != -1) {
        for (j = 1; j < setnum; j++) {		// 只要有一个不含有该str就破出循环（不加入到dstset中）
            if (sets[j] == sets[0]) continue;
            if (!setTypeIsMemberAux(sets[j], str, len, intobj,
                                    encoding == OBJ_ENCODING_HT))
                break;
        }

        /* Only take action when all sets contain the member */
        if (j == setnum) {				// 每个set中都含有该str元素
            if (cardinality_only) {		// 只返回共有元素个数：只cardinality加一
                cardinality++;

                /* We stop the searching after reaching the limit. */
                if (limit && cardinality >= limit)
                    break;
            } else if (!dstkey) {		// 没dstset：回复共有元素内容
                if (str != NULL)
                    addReplyBulkCBuffer(c, str, len);
                else
                    addReplyBulkLongLong(c,intobj);
                cardinality++;
            } else {					// 有dstset：将元素按照类型加入到dstset中
                if (str && only_integers) {		// 看结果是否全部是int，是的话：后面尝试转换成intset
                    /* It may be an integer although we got it as a string. */
                    if (encoding == OBJ_ENCODING_HT &&
                        string2ll(str, len, (long long *)&intobj))
                    {
                        if (dstset->encoding == OBJ_ENCODING_LISTPACK ||
                            dstset->encoding == OBJ_ENCODING_INTSET)
                        {
                            /* Adding it as an integer is more efficient. */
                            str = NULL;
                        }
                    } else {
                        /* It's not an integer */
                        only_integers = 0;
                    }
                }
                setTypeAddAux(dstset, str, len, intobj, encoding == OBJ_ENCODING_HT);		// 加入到dstset中
            }
        }
    }
    setTypeReleaseIterator(si);		// 释放iter

    if (cardinality_only) {		// 只回复长度的话：回复长度
        addReplyLongLong(c,cardinality);
    } else if (dstkey) {		// 有dstset的话：尝试将dstset转换成intset
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        if (setTypeSize(dstset) > 0) {
            if (only_integers) maybeConvertToIntset(dstset);
            if (dstset->encoding == OBJ_ENCODING_LISTPACK) {
                /* We allocated too much memory when we created it to avoid
                 * frequent reallocs. Therefore, we shrink it now. */
                dstset->ptr = lpShrinkToFit(dstset->ptr);		// 清空lp多余内存
            }
            setKey(c,c->db,dstkey,dstset,0);				// 创建dstset
            addReplyLongLong(c,setTypeSize(dstset));		// 回复集合set长度
            notifyKeyspaceEvent(NOTIFY_SET,"sinterstore",
                dstkey,c->db->id);
            server.dirty++;
        } else {		// set为空：删除该set
            addReply(c,shared.czero);
            if (dbDelete(c->db,dstkey)) {
                server.dirty++;
                signalModifiedKey(c,c->db,dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
            }
        }
        decrRefCount(dstset);
    } else {		// 无dstset的话：返回长度
        setDeferredSetLen(c,replylen,cardinality);
    }
    zfree(sets);
}

/* SINTER key [key ...] */
void sinterCommand(client *c) {																										// 取多个set的交集
    sinterGenericCommand(c, c->argv+1,  c->argc-1, NULL, 0, 0);
}

/* SINTERCARD numkeys key [key ...] [LIMIT limit] */
void sinterCardCommand(client *c) {																									// 返回多个sets的交集数量（最大为limit）
    long j;
    long numkeys = 0; /* Number of keys. */
    long limit = 0;   /* 0 means not limit. */

    if (getRangeLongFromObjectOrReply(c, c->argv[1], 1, LONG_MAX,					// 将argv[1]转换成整数类型
                                      &numkeys, "numkeys should be greater than 0") != C_OK)
        return;
    if (numkeys > (c->argc - 2)) {			// key数量不满足要求
        addReplyError(c, "Number of keys can't be greater than number of args");
        return;
    }

    for (j = 2 + numkeys; j < c->argc; j++) {			// 取出limit的参数
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc - 1) - j;

        if (!strcasecmp(opt, "LIMIT") && moreargs) {
            j++;
            if (getPositiveLongFromObjectOrReply(c, c->argv[j], &limit,		// limit参数不是整数
                                                 "LIMIT can't be negative") != C_OK)
                return;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    sinterGenericCommand(c, c->argv+2, numkeys, NULL, 1, limit);			// 返回多个sets的交集数量
}

/* SINTERSTORE destination key [key ...] */
void sinterstoreCommand(client *c) {																								// 将后面的sets的交集移入到dstset中
    sinterGenericCommand(c, c->argv+2, c->argc-2, c->argv[1], 0, 0);
}

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,																// 取第一个set与其他sets的并集或差集
                              robj *dstkey, int op) {			// 取sets[0]与其他sets的差值（DIFF）或者集合（UNION）（DIFF有两种方法）
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *dstset = NULL;
    char *str;
    size_t len;
    int64_t llval;
    int encoding;
    int j, cardinality = 0;
    int diff_algo = 1;
    int sameset = 0; 

    for (j = 0; j < setnum; j++) {						// 取出setkeys对应的values，放在sets中，如果有与sets[0]相同的value：sameset置为1
        robj *setobj = lookupKeyRead(c->db, setkeys[j]);
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
        if (checkType(c,setobj,OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
        if (j > 0 && sets[0] == sets[j]) {
            sameset = 1; 
        }
    }

    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M) where N is the size of the element first set
     * and M the total number of sets.
     *
     * Algorithm 2 is O(N) where N is the total number of elements in all
     * the sets.
     *
     * We compute what is the best bet with the current input here. */
    if (op == SET_OP_DIFF && sets[0] && !sameset) {		// 要取差值且没有相同的set（一定要遍历全部set）的话：判断用那种方法好（其余sets数据较多：diff=1；较少：diff=2）
        long long algo_one_work = 0, algo_two_work = 0;

        for (j = 0; j < setnum; j++) {
            if (sets[j] == NULL) continue;

            algo_one_work += setTypeSize(sets[0]);			// setnum个sets[0]长度之和
            algo_two_work += setTypeSize(sets[j]);			// sets内容长度的和
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        algo_one_work /= 2;			// setnum个sets[0]长度之和的一半
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;			// 所有内容长度的和小于sets[0]的和的一半的话，diff_algo置为2

        if (diff_algo == 1 && setnum > 1) {			// diff_algo为1且长度不为1的话：将sets除了0以外的数据排序
            /* With algorithm 1 it is better to order the sets to subtract
             * by decreasing size, so that we are more likely to find
             * duplicated elements ASAP. */
            qsort(sets+1,setnum-1,sizeof(robj*),	
                qsortCompareSetsByRevCardinality);
        }
    }

    /* We need a temp set object to store our union. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    dstset = createIntsetObject();			// 创建一个intset

    if (op == SET_OP_UNION) {
        /* Union is trivial, just add every element of every set to the
         * temporary set. */
        for (j = 0; j < setnum; j++) {																									// UNION：将所有set的内容都插入到dstset中
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            while ((encoding = setTypeNext(si, &str, &len, &llval)) != -1) {		// 取出下一个节点的内容
                cardinality += setTypeAddAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HT);			// 将取出来的内容插入到dstset中
            }
            setTypeReleaseIterator(si);		// 是否迭代器
        }
    } else if (op == SET_OP_DIFF && sameset) {			// DIFF：有原来set的差值一定为空（a-a=0）
        /* At least one of the sets is the same one (same key) as the first one, result must be empty. */
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 1) {		// 其他元素较多的话：把原set中不存在于其他set的元素插入到dstset中
        /* DIFF Algorithm 1:
         *
         * We perform the diff by iterating all the elements of the first set,
         * and only adding it to the target set if the element does not exist
         * into all the other sets.
         *
         * This way we perform at max N*M operations, where N is the size of
         * the first set, and M the number of sets. */
        si = setTypeInitIterator(sets[0]);
        while ((encoding = setTypeNext(si, &str, &len, &llval)) != -1) {
            for (j = 1; j < setnum; j++) {
                if (!sets[j]) continue; /* no key is an empty set. */
                if (sets[j] == sets[0]) break; /* same set! */
                if (setTypeIsMemberAux(sets[j], str, len, llval,		// 判断这个set中是否含有该数据
                                       encoding == OBJ_ENCODING_HT))
                    break;
            }
            if (j == setnum) {			// 改元素不存在与其他set中：插入到dstset中
                /* There is no other set with this element. Add it. */
                cardinality += setTypeAddAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HT);
            }
        }
        setTypeReleaseIterator(si);
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 2) {		// 其他set元素较少的话：把原set的元素全部插入到dstset中，再将其他set的元素从dstset中删除
        /* DIFF Algorithm 2:
         *
         * Add all the elements of the first set to the auxiliary set.
         * Then remove all the elements of all the next sets from it.
         *
         * This is O(N) where N is the sum of all the elements in every
         * set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            while((encoding = setTypeNext(si, &str, &len, &llval)) != -1) {
                if (j == 0) {
                    cardinality += setTypeAddAux(dstset, str, len, llval,
                                                 encoding == OBJ_ENCODING_HT);			// 把原set中的数据插入到dstset中	
                } else {
                    cardinality -= setTypeRemoveAux(dstset, str, len, llval,
                                                    encoding == OBJ_ENCODING_HT);		// 将其他set的元素从dstset中删除
                }
            }
            setTypeReleaseIterator(si);

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. */
            if (cardinality == 0) break;			// 减为零了则直接退出（差值最少为零）
        }
    }

    /* Output the content of the resulting set, if not in STORE mode */
    if (!dstkey) {			// 没有dstkey的话，把dstset的内容一个一个回复给c
        addReplySetLen(c,cardinality);
        si = setTypeInitIterator(dstset);
        while (setTypeNext(si, &str, &len, &llval) != -1) {
            if (str)
                addReplyBulkCBuffer(c, str, len);
            else
                addReplyBulkLongLong(c, llval);
        }
        setTypeReleaseIterator(si);
        server.lazyfree_lazy_server_del ? freeObjAsync(NULL, dstset, -1) :
                                          decrRefCount(dstset);
    } else {				// 有dstkey的话：
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        if (setTypeSize(dstset) > 0) {		// dstset有内容的话：
            setKey(c,c->db,dstkey,dstset,0);		// （新建）一个set
            addReplyLongLong(c,setTypeSize(dstset));		// 回复dstset的长度给c
            notifyKeyspaceEvent(NOTIFY_SET,
                op == SET_OP_UNION ? "sunionstore" : "sdiffstore",
                dstkey,c->db->id);
            server.dirty++;
        } else {		// dstset为空
            addReply(c,shared.czero);		// 回复结果为null
            if (dbDelete(c->db,dstkey)) {	// 删除dstkey对应的set
                server.dirty++;
                signalModifiedKey(c,c->db,dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
            }
        }
        decrRefCount(dstset);
    }
    zfree(sets);
}

/* SUNION key [key ...] */
void sunionCommand(client *c) {																					// 返回set1与set2的合集
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,SET_OP_UNION);
}

/* SUNIONSTORE destination key [key ...] */
void sunionstoreCommand(client *c) {																			// 将set2与set3的合集保存在set1中
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],SET_OP_UNION);
}

/* SDIFF key [key ...] */
void sdiffCommand(client *c) {																					// 返回set1与set2的差集
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,SET_OP_DIFF);
}

/* SDIFFSTORE destination key [key ...] */
void sdiffstoreCommand(client *c) {																				// 将set2与set3的差集保存在set1中
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],SET_OP_DIFF);
}

void sscanCommand(client *c) {																					// SCAN命令？？？
    robj *set;
    unsigned long cursor;

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;		// 获取key2对应的set
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||		// 获取key1对应的set
        checkType(c,set,OBJ_SET)) return;
    scanGenericCommand(c,set,cursor);
}
