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
#include <math.h>

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

/* Check the length of a number of objects to see if we need to convert a
 * listpack to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
void hashTypeTryConversion(robj *o, robj **argv, int start, int end) {																			// 看o添加了argv的start-end的内容后（为添加），o的类型是否要变成hashtable（直接变）
    int i;
    size_t sum = 0;

    if (o->encoding != OBJ_ENCODING_LISTPACK) return;

    for (i = start; i <= end; i++) {							// 如果argv中start-end有一个sds的长度过长的话（listpack不够存储），将o的类型转换成ashtable
        if (!sdsEncodedObject(argv[i]))			// argv类型不是sds的话跳过
            continue;
        size_t len = sdslen(argv[i]->ptr);		// 获取atgv的长度
        if (len > server.hash_max_listpack_value) {
            hashTypeConvert(o, OBJ_ENCODING_HT);			// 将o的类型转换成hashtable
            return;
        }
        sum += len;
    }
    if (!lpSafeToAdd(o->ptr, sum))								// 如果添加所有内容后o的容量太大了（影响速率）：就将o的类型转换成hashtable
        hashTypeConvert(o, OBJ_ENCODING_HT);
}

/* Get the value from a listpack encoded hash, identified by field.
 * Returns -1 when the field cannot be found. */
int hashTypeGetFromListpack(robj *o, sds field,																							// 从listpack中获取key=field对应的value，将内容赋值到参数中
                            unsigned char **vstr,
                            unsigned int *vlen,
                            long long *vll)
{
    unsigned char *zl, *fptr = NULL, *vptr = NULL;

    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    zl = o->ptr;
    fptr = lpFirst(zl);
    if (fptr != NULL) {						// 从第一个节点开始查找field（找到的话，将下一个节点获取至vptr中）
        fptr = lpFind(zl, fptr, (unsigned char*)field, sdslen(field), 1);
        if (fptr != NULL) {
            /* Grab pointer to the value (fptr points to the field) */
            vptr = lpNext(zl, fptr);
            serverAssert(vptr != NULL);
        }
    }

    if (vptr != NULL) {						// 取到了vptr的话，将vptr的内容赋值到参数中
        *vstr = lpGetValue(vptr, vlen, vll);
        return 0;
    }

    return -1;
}

/* Get the value from a hash table encoded hash, identified by field.
 * Returns NULL when the field cannot be found, otherwise the SDS value
 * is returned. */
sds hashTypeGetFromHashTable(robj *o, sds field) {																							// 从hashtable中获取并返回key=field对应的value的内容
    dictEntry *de;

    serverAssert(o->encoding == OBJ_ENCODING_HT);

    de = dictFind(o->ptr, field);			// 从o中查找field，找到的话：返回内容，否则返回空
    if (de == NULL) return NULL;
    return dictGetVal(de);
}

/* Higher level function of hashTypeGet*() that returns the hash value
 * associated with the specified field. If the field is found C_OK
 * is returned, otherwise C_ERR. The returned object is returned by
 * reference in either *vstr and *vlen if it's returned in string form,
 * or stored in *vll if it's returned as a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * for C_OK and checking if vll (or vstr) is NULL. */
int hashTypeGetValue(robj *o, sds field, unsigned char **vstr, unsigned int *vlen, long long *vll) {											// 根据encoding，获取o中field对应的value的内容，将内容赋值到参数中
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        *vstr = NULL;
        if (hashTypeGetFromListpack(o, field, vstr, vlen, vll) == 0)
            return C_OK;
    } else if (o->encoding == OBJ_ENCODING_HT) {
        sds value;
        if ((value = hashTypeGetFromHashTable(o, field)) != NULL) {
            *vstr = (unsigned char*) value;
            *vlen = sdslen(value);
            return C_OK;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_ERR;
}

/* Like hashTypeGetValue() but returns a Redis object, which is useful for
 * interaction with the hash type outside t_hash.c.
 * The function returns NULL if the field is not found in the hash. Otherwise
 * a newly allocated string object with the value is returned. */
robj *hashTypeGetValueObject(robj *o, sds field) {																								// 根据encoding，获取并返回field对应的value的内容
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    if (hashTypeGetValue(o,field,&vstr,&vlen,&vll) == C_ERR) return NULL;
    if (vstr) return createStringObject((char*)vstr,vlen);
    else return createStringObjectFromLongLong(vll);		// 创建存着vll的robj，可以以整数保存的话使用整数类型，否则以字符串形式保持
}

/* Higher level function using hashTypeGet*() to return the length of the
 * object associated with the requested field, or 0 if the field does not
 * exist. */
size_t hashTypeGetValueLength(robj *o, sds field) {																								// 获取o中field对应的value的内容长度（str长度或整数的位数）
    size_t len = 0;
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK)
        len = vstr ? vlen : sdigits10(vll);

    return len;
}

/* Test if the specified field exists in the given hash. Returns 1 if the field
 * exists, and 0 when it doesn't. */
int hashTypeExists(robj *o, sds field) {																										// 查看o中是否有field对应的内容
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    return hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK;
}

/* Add a new field, overwrite the old with the new value if it already exists.
 * Return 0 on insert and 1 on update.
 *
 * By default, the key and value SDS strings are copied if needed, so the
 * caller retains ownership of the strings passed. However this behavior
 * can be effected by passing appropriate flags (possibly bitwise OR-ed):
 *
 * HASH_SET_TAKE_FIELD -- The SDS field ownership passes to the function.
 * HASH_SET_TAKE_VALUE -- The SDS value ownership passes to the function.
 *
 * When the flags are used the caller does not need to release the passed
 * SDS string(s). It's up to the function to use the string to create a new
 * entry or to free the SDS string before returning to the caller.
 *
 * HASH_SET_COPY corresponds to no flags passed, and means the default
 * semantics of copying the values if needed.
 *
 */
#define HASH_SET_TAKE_FIELD (1<<0)
#define HASH_SET_TAKE_VALUE (1<<1)
#define HASH_SET_COPY 0
int hashTypeSet(robj *o, sds field, sds value, int flags) {																				// 按照类型将field-value插入到o中（已有的话更新为新的）
    int update = 0;

    /* Check if the field is too long for listpack, and convert before adding the item.
     * This is needed for HINCRBY* case since in other commands this is handled early by
     * hashTypeTryConversion, so this check will be a NOP. */
    if (o->encoding == OBJ_ENCODING_LISTPACK) {						// listpack：如果field或value不能用listpack存储的话，将o转换成hashtable
        if (sdslen(field) > server.hash_max_listpack_value || sdslen(value) > server.hash_max_listpack_value)
            hashTypeConvert(o, OBJ_ENCODING_HT);
    }
    
    if (o->encoding == OBJ_ENCODING_LISTPACK) {		// o类型是listpack且field与value可以用listpack存储：隔一个查找一次（只查找fields）：找到则更新为value，没找到则插入field与value
        unsigned char *zl, *fptr, *vptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char*)field, sdslen(field), 1);		// 每隔一个查找一次，防止查找到values中与field内容相同的值（只查找fields）
            if (fptr != NULL) {				// 如果o中已有field的话：将内容更新为value
                /* Grab pointer to the value (fptr points to the field) */
                vptr = lpNext(zl, fptr);
                serverAssert(vptr != NULL);
                update = 1;

                /* Replace value */
                zl = lpReplace(zl, &vptr, (unsigned char*)value, sdslen(value));
            }
        }

        if (!update) {					// o中没有field：先插入field，再插入value
            /* Push new field/value pair onto the tail of the listpack */
            zl = lpAppend(zl, (unsigned char*)field, sdslen(field));
            zl = lpAppend(zl, (unsigned char*)value, sdslen(value));
        }
        o->ptr = zl;		// 更新o的内容

        /* Check if the listpack needs to be converted to a hash table */
        if (hashTypeLength(o) > server.hash_max_listpack_entries)		// 长度过长的话：转换成hashtable
            hashTypeConvert(o, OBJ_ENCODING_HT);
    } else if (o->encoding == OBJ_ENCODING_HT) {			// o类型是hashtable:查找有否field：有的话删除原来的更新为新的，没有的话插入新的
        dict *ht = o->ptr;
        dictEntry *de, *existing;
        sds v;
        if (flags & HASH_SET_TAKE_VALUE) {			// 直接使用原来的value或者复制一份
            v = value;
            value = NULL;
        } else {
            v = sdsdup(value);
        }
        de = dictAddRaw(ht, field, &existing);			// 向ht中添加一个field，并返回field对应的entry（用于插入value）
        if (de) {						// 插入成功：设置value
            dictSetVal(ht, de, v);
            if (flags & HASH_SET_TAKE_FIELD) {		// 不直接使用field的话：将field替换为复制的一份
                field = NULL;
            } else {
                dictSetKey(ht, de, sdsdup(field));
            }
        } else {
            sdsfree(dictGetVal(existing));				// 先释放掉已存在的value，再设置新的value
            dictSetVal(ht, existing, v);
            update = 1;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }

    /* Free SDS strings we did not referenced elsewhere if the flags
     * want this function to be responsible. */
    if (flags & HASH_SET_TAKE_FIELD && field) sdsfree(field);
    if (flags & HASH_SET_TAKE_VALUE && value) sdsfree(value);
    return update;
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
int hashTypeDelete(robj *o, sds field) {																					// 删除o中key=field的内容
    int deleted = 0;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {		// listpack：查找，找到的话，删除两个节点（key+value）
        unsigned char *zl, *fptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char*)field, sdslen(field), 1);
            if (fptr != NULL) {
                /* Delete both of the key and the value. */
                zl = lpDeleteRangeWithEntry(zl,&fptr,2);
                o->ptr = zl;
                deleted = 1;
            }
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {		// hashtable：直接删除
        if (dictDelete((dict*)o->ptr, field) == C_OK) {
            deleted = 1;

            /* Always check if the dictionary needs a resize after a delete. */
            if (htNeedsResize(o->ptr)) dictResize(o->ptr);		// 有需要的话（长度过长，占有率过低（10%）），resize一下
        }

    } else {
        serverPanic("Unknown hash encoding");
    }
    return deleted;
}

/* Return the number of elements in a hash. */
unsigned long hashTypeLength(const robj *o) {																					// 返回o的元素个数
    unsigned long length = ULONG_MAX;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {			// listpack：返回总长度/2（一半key，一般value）
        length = lpLength(o->ptr) / 2;
    } else if (o->encoding == OBJ_ENCODING_HT) {		// hashtable：直接返回长度
        length = dictSize((const dict*)o->ptr);
    } else {
        serverPanic("Unknown hash encoding");
    }
    return length;
}

hashTypeIterator *hashTypeInitIterator(robj *subject) {																				// 根据encoding，创建一个新的hash iter并返回
    hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));
    hi->subject = subject;
    hi->encoding = subject->encoding;

    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        hi->fptr = NULL;	// 存key
        hi->vptr = NULL;	// 存擦了
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        hi->di = dictGetIterator(subject->ptr);			// di指向hashtable的迭代器
    } else {
        serverPanic("Unknown hash encoding");
    }
    return hi;
}

void hashTypeReleaseIterator(hashTypeIterator *hi) {																				// 释放一个hash iter
    if (hi->encoding == OBJ_ENCODING_HT)
        dictReleaseIterator(hi->di);				// 第指向的hashtable迭代器要释放
    zfree(hi);
}

/* Move to the next entry in the hash. Return C_OK when the next entry
 * could be found and C_ERR when the iterator reaches the end. */
int hashTypeNext(hashTypeIterator *hi) {																							// 取hi的下一个节点
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {		// listpack：取vptr的下两个节点（key+value）或第一个节点，
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        if (fptr == NULL) {				// fptr为空的话：取第一个值，否则取vptr的下一个值
            /* Initialize cursor */
            serverAssert(vptr == NULL);
            fptr = lpFirst(zl);
        } else {
            /* Advance cursor */
            serverAssert(vptr != NULL);
            fptr = lpNext(zl, vptr);		// 取fptr
        }
        if (fptr == NULL) return C_ERR;

        /* Grab pointer to the value (fptr points to the field) */
        vptr = lpNext(zl, fptr);		// 取vptr
        serverAssert(vptr != NULL);

        /* fptr, vptr now point to the first or next pair */
        hi->fptr = fptr;			// 将fptr与vptr赋给hi
        hi->vptr = vptr;
    } else if (hi->encoding == OBJ_ENCODING_HT) {		// hashtable：直接取下一个值
        if ((hi->de = dictNext(hi->di)) == NULL) return C_ERR;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_OK;
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a listpack. Prototype is similar to `hashTypeGetFromListpack`. */
void hashTypeCurrentFromListpack(hashTypeIterator *hi, int what,															// what=key：取hi中fptr的内容；what=value：取hi中vptr的内容
                                 unsigned char **vstr,
                                 unsigned int *vlen,
                                 long long *vll)
{
    serverAssert(hi->encoding == OBJ_ENCODING_LISTPACK);

    if (what & OBJ_HASH_KEY) {
        *vstr = lpGetValue(hi->fptr, vlen, vll);
    } else {
        *vstr = lpGetValue(hi->vptr, vlen, vll);
    }
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a hash table. Prototype is similar to
 * `hashTypeGetFromHashTable`. */
sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what) {																			// 根据what获取hi中的key或value
    serverAssert(hi->encoding == OBJ_ENCODING_HT);

    if (what & OBJ_HASH_KEY) {
        return dictGetKey(hi->de);
    } else {
        return dictGetVal(hi->de);
    }
}

/* Higher level function of hashTypeCurrent*() that returns the hash value
 * at current iterator position.
 *
 * The returned element is returned by reference in either *vstr and *vlen if
 * it's returned in string form, or stored in *vll if it's returned as
 * a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * type checking if vstr == NULL. */
void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll) {						//根据encoding类型，获取hi中的what类型的内容（放在后面的参数中）
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        *vstr = NULL;
        hashTypeCurrentFromListpack(hi, what, vstr, vlen, vll);			// 根据what获取hi中（fptr/vptr）what内容
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        sds ele = hashTypeCurrentFromHashTable(hi, what);				// 根据what获取hi中（de）what的内容
        *vstr = (unsigned char*) ele;
        *vlen = sdslen(ele);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* Return the key or value at the current iterator position as a new
 * SDS string. */
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what) {																		// 获取并返回hi中的what类型的内容
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    hashTypeCurrentObject(hi,what,&vstr,&vlen,&vll);		// 根据encoding获取hi中的what类型的内容
    if (vstr) return sdsnewlen(vstr,vlen);		// 返回hi指向的str内容
    return sdsfromlonglong(vll);
}

robj *hashTypeLookupWriteOrCreate(client *c, robj *key) {																				// 查找是否含有key对应的hash，没有的话创建一个新的（一开始用listpack）
    robj *o = lookupKeyWrite(c->db,key);			// 查找key对应的hashType
    if (checkType(c,o,OBJ_HASH)) return NULL;

    if (o == NULL) {
        o = createHashObject();						// 没找到的话创建一个新的hashObj（一开始使用listpack）
        dbAdd(c->db,key,o);
    }
    return o;
}


void hashTypeConvertListpack(robj *o, int enc) {																						// 将listpack转换成hashtable
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    if (enc == OBJ_ENCODING_LISTPACK) {		// 已经是listpack
        /* Nothing to do... */

    } else if (enc == OBJ_ENCODING_HT) {
        hashTypeIterator *hi;
        dict *dict;
        int ret;

        hi = hashTypeInitIterator(o);
        dict = dictCreate(&hashDictType);

        /* Presize the dict to avoid rehashing */
        dictExpand(dict,hashTypeLength(o));			// 将新的dict扩容至需要的大小

        while (hashTypeNext(hi) != C_ERR) {					// 将hi中的所有内容都存入到dict中
            sds key, value;

            key = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_KEY);					// 获取hi中的key与value（hashtable保存在de中，listpack保存在fptr/vptr中）
            value = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
            ret = dictAdd(dict, key, value);									// 将key-value添加到dict中
            if (ret != DICT_OK) {
                sdsfree(key); sdsfree(value); /* Needed for gcc ASAN */			// 失败的话：释放资源，迭代器，打日志
                hashTypeReleaseIterator(hi);  /* Needed for gcc ASAN */
                serverLogHexDump(LL_WARNING,"listpack with dup elements dump",
                    o->ptr,lpBytes(o->ptr));
                serverPanic("Listpack corruption detected");
            }
        }
        hashTypeReleaseIterator(hi);			// 释放hi
        zfree(o->ptr);						// 改变o指向
        o->encoding = OBJ_ENCODING_HT;
        o->ptr = dict;
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void hashTypeConvert(robj *o, int enc) {																								// 将o的类型转化成hashtable
    if (o->encoding == OBJ_ENCODING_LISTPACK) {			// 类型是listpack的话：转换成hashtable
        hashTypeConvertListpack(o, enc);
    } else if (o->encoding == OBJ_ENCODING_HT) {		// 类型是hashtable：转换个屁
        serverPanic("Not implemented");
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a hash object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *hashTypeDup(robj *o) {																											// 根据encoding，复制一份o并返回
    robj *hobj;
    hashTypeIterator *hi;

    serverAssert(o->type == OBJ_HASH);

    if(o->encoding == OBJ_ENCODING_LISTPACK) {			// listpack：复制一份listpack，创建一个hashType的robj并指向复制的lp
        unsigned char *zl = o->ptr;
        size_t sz = lpBytes(zl);
        unsigned char *new_zl = zmalloc(sz);
        memcpy(new_zl, zl, sz);
        hobj = createObject(OBJ_HASH, new_zl);
        hobj->encoding = OBJ_ENCODING_LISTPACK;
    } else if(o->encoding == OBJ_ENCODING_HT){			// hashtable：创建一个新的dict，扩容至需要的大小，再把原来hashtable着数据犬奴复制一份添加到新dict中
        dict *d = dictCreate(&hashDictType);
        dictExpand(d, dictSize((const dict*)o->ptr));

        hi = hashTypeInitIterator(o);
        while (hashTypeNext(hi) != C_ERR) {
            sds field, value;
            sds newfield, newvalue;
            /* Extract a field-value pair from an original hash object.*/
            field = hashTypeCurrentFromHashTable(hi, OBJ_HASH_KEY);
            value = hashTypeCurrentFromHashTable(hi, OBJ_HASH_VALUE);
            newfield = sdsdup(field);
            newvalue = sdsdup(value);

            /* Add a field-value pair to a new hash object. */
            dictAdd(d,newfield,newvalue);
        }
        hashTypeReleaseIterator(hi);

        hobj = createObject(OBJ_HASH, d);
        hobj->encoding = OBJ_ENCODING_HT;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return hobj;
}

/* Create a new sds string from the listpack entry. */
sds hashSdsFromListpackEntry(listpackEntry *e) {																						// 根据e创建一个sds（整数也转换成sds）
    return e->sval ? sdsnewlen(e->sval, e->slen) : sdsfromlonglong(e->lval);
}

/* Reply with bulk string from the listpack entry. */
void hashReplyFromListpackEntry(client *c, listpackEntry *e) {																			// 向c回复e的内容
    if (e->sval)
        addReplyBulkCBuffer(c, e->sval, e->slen);
    else
        addReplyBulkLongLong(c, e->lval);
}

/* Return random element from a non empty hash.
 * 'key' and 'val' will be set to hold the element.
 * The memory in them is not to be freed or modified by the caller.
 * 'val' can be NULL in which case it's not extracted. */
void hashTypeRandomElement(robj *hashobj, unsigned long hashsize, listpackEntry *key, listpackEntry *val) {								// 在hashsize以内的数据取随机取出一个保存在key与value中
    if (hashobj->encoding == OBJ_ENCODING_HT) {							// hashtable：随机取出一个节点后，将内容保存在key与value中
        dictEntry *de = dictGetFairRandomKey(hashobj->ptr);
        sds s = dictGetKey(de);
        key->sval = (unsigned char*)s;
        key->slen = sdslen(s);
        if (val) {
            sds s = dictGetVal(de);
            val->sval = (unsigned char*)s;
            val->slen = sdslen(s);
        }
    } else if (hashobj->encoding == OBJ_ENCODING_LISTPACK) {			// listpack：随机便宜一个值后，取出key与value
        lpRandomPair(hashobj->ptr, hashsize, key, val);
    } else {
        serverPanic("Unknown hash encoding");
    }
}


/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

void hsetnxCommand(client *c) {																						// 不存在才创建新的hash并添加内容，否则直接返回
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;			// 查找key对应的hash 没有的话创建新的，有的话直接返回

    if (hashTypeExists(o, c->argv[2]->ptr)) {			// field对应的内容 存在：不对劲（一个新的hash怎么会有内容），回复0
        addReply(c, shared.czero);
    } else {											// 不存在field：添加key-value并回复1
        hashTypeTryConversion(o,c->argv,2,3);		// 看是否需要转换成hashtable
        hashTypeSet(o,c->argv[2]->ptr,c->argv[3]->ptr,HASH_SET_COPY);
        addReply(c, shared.cone);
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH,"hset",c->argv[1],c->db->id);
        server.dirty++;
    }
}

void hsetCommand(client *c) {																						// 向hash中添加多对key-value
    int i, created = 0;
    robj *o;

    if ((c->argc % 2) == 1) {		// 参数数量不对
        addReplyErrorArity(c);
        return;
    }

    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;		// 有必要的话创建新的hash，（有重名的robj：直接返回）
    hashTypeTryConversion(o,c->argv,2,c->argc-1);

    for (i = 2; i < c->argc; i += 2)			// 将key-value对添加到o中
        created += !hashTypeSet(o,c->argv[i]->ptr,c->argv[i+1]->ptr,HASH_SET_COPY);

    /* HMSET (deprecated) and HSET return value is different. */
    char *cmdname = c->argv[0]->ptr;
    if (cmdname[1] == 's' || cmdname[1] == 'S') {		// HSET：返回添加了的对数
        /* HSET */
        addReplyLongLong(c, created);
    } else {											// HMSET：回复OK
        /* HMSET */
        addReply(c, shared.ok);
    }
    signalModifiedKey(c,c->db,c->argv[1]);					
    notifyKeyspaceEvent(NOTIFY_HASH,"hset",c->argv[1],c->db->id);
    server.dirty += (c->argc - 2)/2;
}

void hincrbyCommand(client *c) {																					// 将key对应的hash中field对应的value的内容加一
    long long value, incr, oldvalue;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if (getLongLongFromObjectOrReply(c,c->argv[3],&incr,NULL) != C_OK) return;		// 获取inc的数量
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;			// 查找key对应的hash（没有的话创建）
    if (hashTypeGetValue(o,c->argv[2]->ptr,&vstr,&vlen,&value) == C_OK) {			// 获取field对应的value
        if (vstr) {
            if (string2ll((char*)vstr,vlen,&value) == 0) {			// 转换成ll失败：直接回复错误并返回
                addReplyError(c,"hash value is not an integer");
                return;
            }
        } /* Else hashTypeGetValue() already stored it into &value */
    } else {															// 没有对应的value：创建新的值，从0开始加的去
        value = 0;
    }

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {			// 超出范围：回复错误，并返回
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;
    new = sdsfromlonglong(value);			// 新value的sds格式
    hashTypeSet(o,c->argv[2]->ptr,new,HASH_SET_TAKE_VALUE);		// 更新内容或添加内容
    addReplyLongLong(c,value);				// 回复改变之后的值
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH,"hincrby",c->argv[1],c->db->id);
    server.dirty++;
}

void hincrbyfloatCommand(client *c) {																									// 将key对应的hash中field对应的值增加incr
    long double value, incr;
    long long ll;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if (getLongDoubleFromObjectOrReply(c,c->argv[3],&incr,NULL) != C_OK) return;		// 取出argv中的要添加的浮点数incr
    if (isnan(incr) || isinf(incr)) {		// 数据有误：直接返回
        addReplyError(c,"value is NaN or Infinity");
        return;
    }
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;		// 获取key对应的hash
    if (hashTypeGetValue(o,c->argv[2]->ptr,&vstr,&vlen,&ll) == C_OK) {		// 获取field对应的value：有的话转换成float，没的话初始化成0
        if (vstr) {
            if (string2ld((char*)vstr,vlen,&value) == 0) {
                addReplyError(c,"hash value is not a float");
                return;
            }
        } else {
            value = (long double)ll;
        }
    } else {
        value = 0;
    }

    value += incr;			// 增加incr
    if (isnan(value) || isinf(value)) {		// 增加后超出了范围
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }

    char buf[MAX_LONG_DOUBLE_CHARS];			// 用来存储增加后的内容
    int len = ld2string(buf,sizeof(buf),value,LD_STR_HUMAN);
    new = sdsnewlen(buf,len);			// 新的内容的长度
    hashTypeSet(o,c->argv[2]->ptr,new,HASH_SET_TAKE_VALUE);			// 设置新的内容
    addReplyBulkCBuffer(c,buf,len);			// 回复增加后的内容
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH,"hincrbyfloat",c->argv[1],c->db->id);
    server.dirty++;

    /* Always replicate HINCRBYFLOAT as an HSET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    robj *newobj;
    newobj = createRawStringObject(buf,len);
    rewriteClientCommandArgument(c,0,shared.hset);
    rewriteClientCommandArgument(c,3,newobj);
    decrRefCount(newobj);
}

static void addHashFieldToReply(client *c, robj *o, sds field) {																			// 向c回复o中field对应的value的内容
    if (o == NULL) {
        addReplyNull(c);
        return;
    }

    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK) {			// 获取field对应的value内容并向c回复
        if (vstr) {
            addReplyBulkCBuffer(c, vstr, vlen);
        } else {
            addReplyBulkLongLong(c, vll);
        }
    } else {
        addReplyNull(c);
    }
}

void hgetCommand(client *c) {																													// 向c回复key对应的hash中field对应的value的内容
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL ||		// 获取key对应的hash
        checkType(c,o,OBJ_HASH)) return;

    addHashFieldToReply(c, o, c->argv[2]->ptr);			// 向c回复o中field对应的value内容
}

void hmgetCommand(client *c) {																													// 向c回复key对应的hash中多个field对应的value的内容
    robj *o;
    int i;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    o = lookupKeyRead(c->db, c->argv[1]);		// 取出key对应的hash
    if (checkType(c,o,OBJ_HASH)) return;		// 类型不匹配

    addReplyArrayLen(c, c->argc-2);				// 先回复长度
    for (i = 2; i < c->argc; i++) {				// 再回复全部field对应的value内容
        addHashFieldToReply(c, o, c->argv[i]->ptr);
    }
}

void hdelCommand(client *c) {																													// 删除argv中fields对应的values，回复删除了的数量
    robj *o;
    int j, deleted = 0, keyremoved = 0;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||		// 获取key对应的hash
        checkType(c,o,OBJ_HASH)) return;

    for (j = 2; j < c->argc; j++) {						// 将参数中field对应的value全部删除（没有的话不用删除）
        if (hashTypeDelete(o,c->argv[j]->ptr)) {
            deleted++;
            if (hashTypeLength(o) == 0) {		// 删除空了则删除该hash
                dbDelete(c->db,c->argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }
    if (deleted) {									// 删除了元素的话：提醒一下c
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH,"hdel",c->argv[1],c->db->id);
        if (keyremoved)			// 要删除该hash的话：提醒一下server
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);							// 回复删除了的元素数量
}

void hlenCommand(client *c) {																													// 回复key对应的hash的数据数量
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||			// 获取key独有的hash
        checkType(c,o,OBJ_HASH)) return;

    addReplyLongLong(c,hashTypeLength(o));											// 获取hash长度并回复
}

void hstrlenCommand(client *c) {																												// 回复key对应的hash中field对应的value的长度
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||		// 获取key对应的hash
        checkType(c,o,OBJ_HASH)) return;
    addReplyLongLong(c,hashTypeGetValueLength(o,c->argv[2]->ptr));			// 回复hash中field对应的value的数据长度
}

static void addHashIteratorCursorToReply(client *c, hashTypeIterator *hi, int what) {															// 回复hi中what的内容
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromListpack(hi, what, &vstr, &vlen, &vll);			// 获取hi中的what类型的数据，并返回其内容
        if (vstr)
            addReplyBulkCBuffer(c, vstr, vlen);
        else
            addReplyBulkLongLong(c, vll);
    } else if (hi->encoding == OBJ_ENCODING_HT) {					// 获取hi中的what内容，并回复其内容
        sds value = hashTypeCurrentFromHashTable(hi, what);
        addReplyBulkCBuffer(c, value, sdslen(value));
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void genericHgetallCommand(client *c, int flags) {																						// 根据flag（key/value/key+value）将key对应的hash中的数据回复给c
    robj *o;
    hashTypeIterator *hi;
    int length, count = 0;

    robj *emptyResp = (flags & OBJ_HASH_KEY && flags & OBJ_HASH_VALUE) ?		// 创建一个新的robj（有key有value：map，否则数组）
        shared.emptymap[c->resp] : shared.emptyarray;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],emptyResp))			// 查找key对应的hash，并检查类型
        == NULL || checkType(c,o,OBJ_HASH)) return;

    /* We return a map if the user requested keys and values, like in the
     * HGETALL case. Otherwise to use a flat array makes more sense. */
    length = hashTypeLength(o);				// 元素数量
    if (flags & OBJ_HASH_KEY && flags & OBJ_HASH_VALUE) {
        addReplyMapLen(c, length);			// 有key有value：回复map长度
    } else {
        addReplyArrayLen(c, length);		// 否则返回数组长度
    }

    hi = hashTypeInitIterator(o);			// 创建一个hashIter
    while (hashTypeNext(hi) != C_ERR) {			// 将o中的key与value全部回复给c
        if (flags & OBJ_HASH_KEY) {
            addHashIteratorCursorToReply(c, hi, OBJ_HASH_KEY);
            count++;
        }
        if (flags & OBJ_HASH_VALUE) {
            addHashIteratorCursorToReply(c, hi, OBJ_HASH_VALUE);
            count++;
        }
    }

    hashTypeReleaseIterator(hi);		// 是否iter

    /* Make sure we returned the right number of elements. */
    if (flags & OBJ_HASH_KEY && flags & OBJ_HASH_VALUE) count /= 2;		// 有key有value：count需除以2
    serverAssert(count == length);
}

void hkeysCommand(client *c) {																												// 将hash的全部key回复给c
    genericHgetallCommand(c,OBJ_HASH_KEY);
}

void hvalsCommand(client *c) {																												// 将hash的全部value回复给c
    genericHgetallCommand(c,OBJ_HASH_VALUE);
}

void hgetallCommand(client *c) {																											// 将hash的全部key+value回复给c
    genericHgetallCommand(c,OBJ_HASH_KEY|OBJ_HASH_VALUE);
}

void hexistsCommand(client *c) {																											// 回复hash中是否含有field
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||			// 获取key对应的hash
        checkType(c,o,OBJ_HASH)) return;

    addReply(c, hashTypeExists(o,c->argv[2]->ptr) ? shared.cone : shared.czero);		// 看hash中是否含有fiele对应的value
}

void hscanCommand(client *c) {																												// HSCAN命令？？？
    robj *o;
    unsigned long cursor;

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;
    scanGenericCommand(c,o,cursor);
}

static void hrandfieldReplyWithListpack(client *c, unsigned int count, listpackEntry *keys, listpackEntry *vals) {								// 将keys中前count个数据的key与value（如果有的话）数据恢复给c
    for (unsigned long i = 0; i < count; i++) {
        if (vals && c->resp > 2)
            addReplyArrayLen(c,2);
        if (keys[i].sval)
            addReplyBulkCBuffer(c, keys[i].sval, keys[i].slen);
        else
            addReplyBulkLongLong(c, keys[i].lval);
        if (vals) {
            if (vals[i].sval)
                addReplyBulkCBuffer(c, vals[i].sval, vals[i].slen);
            else
                addReplyBulkLongLong(c, vals[i].lval);
        }
    }
}

/* How many times bigger should be the hash compared to the requested size
 * for us to not use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define HRANDFIELD_SUB_STRATEGY_MUL 3

/* If client is trying to ask for a very large number of random elements,
 * queuing may consume an unlimited amount of memory, so we want to limit
 * the number of randoms per time. */
#define HRANDFIELD_RANDOM_SAMPLE_LIMIT 1000

void hrandfieldWithCountCommand(client *c, long l, int withvalues) {																			// 随机发送long个数据给c，（withvalue：是否发送value）优化：当count够大时，把hash内容复制到新的dict中，之后取出dict元素至剩余count个，再把dict的数据全部发送给c
    unsigned long count, size;
    int uniq = 1;
    robj *hash;

    if ((hash = lookupKeyReadOrReply(c,c->argv[1],shared.emptyarray))				// 获取key对应的hash并检查类型
        == NULL || checkType(c,hash,OBJ_HASH)) return;
    size = hashTypeLength(hash);		// 获取hash的数据数量

    if(l >= 0) {		// 解析数量与遍历方向
        count = (unsigned long) l;
    } else {
        count = -l;
        uniq = 0;
    }

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {		// 数量为0：回复错误
        addReply(c,shared.emptyarray);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. This case is the only one that also needs to return the
     * elements in random order. */
    if (!uniq || count == 1) {											// case1：（l<0）直接获取count个节点的内容并回复给c
        if (withvalues && c->resp == 2)		// 需要回复value
            addReplyArrayLen(c, count*2);
        else
            addReplyArrayLen(c, count);
        if (hash->encoding == OBJ_ENCODING_HT) {		// hashtable：随机获取count个节点（一个一个来），并返回其key与value（withvalues=1时回复value）
            sds key, value;
            while (count--) {
                dictEntry *de = dictGetFairRandomKey(hash->ptr);	// 随机获取一个节点的key与value
                key = dictGetKey(de);
                value = dictGetVal(de);
                if (withvalues && c->resp > 2)		// 要恢复value的话：回复数组长度：2
                    addReplyArrayLen(c,2);
                addReplyBulkCBuffer(c, key, sdslen(key));		// 回复key与value
                if (withvalues)
                    addReplyBulkCBuffer(c, value, sdslen(value));
                if (c->flags & CLIENT_CLOSE_ASAP)
                    break;
            }
        } else if (hash->encoding == OBJ_ENCODING_LISTPACK) {		// listpack：一次性获取count个节点的内容，并发送给c
            listpackEntry *keys, *vals = NULL;
            unsigned long limit, sample_count;

            limit = count > HRANDFIELD_RANDOM_SAMPLE_LIMIT ? HRANDFIELD_RANDOM_SAMPLE_LIMIT : count;
            keys = zmalloc(sizeof(listpackEntry)*limit);
            if (withvalues)
                vals = zmalloc(sizeof(listpackEntry)*limit);
            while (count) {
                sample_count = count > limit ? limit : count;
                count -= sample_count;
                lpRandomPairs(hash->ptr, sample_count, keys, vals);		// 一次性获取count个节点，将内容放在keys与values中
                hrandfieldReplyWithListpack(c, sample_count, keys, vals);		// 将keys与values中的数据全部回复给c
                if (c->flags & CLIENT_CLOSE_ASAP)
                    break;
            }
            zfree(keys);	// 释放掉keys与vals
            zfree(vals);
        }
        return;
    }

    /* Initiate reply count, RESP3 responds with nested array, RESP2 with flat one. */
    long reply_size = count < size ? count : size;
    if (withvalues && c->resp == 2)			// 先回复数量
        addReplyArrayLen(c, reply_size*2);
    else
        addReplyArrayLen(c, reply_size);

    /* CASE 2:
    * The number of requested elements is greater than the number of
    * elements inside the hash: simply return the whole hash. */
    if(count >= size) {													// case2：（回复全部）遍历hash并把全部数据全部回复给c
        hashTypeIterator *hi = hashTypeInitIterator(hash);	// 
        while (hashTypeNext(hi) != C_ERR) {
            if (withvalues && c->resp > 2)		// 回复长度：2
                addReplyArrayLen(c,2);
            addHashIteratorCursorToReply(c, hi, OBJ_HASH_KEY);		// 回复keys
            if (withvalues)
                addHashIteratorCursorToReply(c, hi, OBJ_HASH_VALUE);		// 回复values
        }
        hashTypeReleaseIterator(hi);
        return;
    }

    /* CASE 3:
     * The number of elements inside the hash is not greater than
     * HRANDFIELD_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a hash from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the hash, the natural approach
     * used into CASE 4 is highly inefficient. */
    if (count*HRANDFIELD_SUB_STRATEGY_MUL > size) {						// case3：（count过大）复制hash到一个新的dict中，取出元素到剩余count个，再发送dict的全部数据
        dict *d = dictCreate(&sdsReplyDictType);		// 创建一个新的dict并扩容至需要的大小
        dictExpand(d, size);
        hashTypeIterator *hi = hashTypeInitIterator(hash);

        /* Add all the elements into the temporary dictionary. */
        while ((hashTypeNext(hi)) != C_ERR) {			// 将hash的全部数据复制给新的dict
            int ret = DICT_ERR;
            sds key, value = NULL;

            key = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_KEY);
            if (withvalues)
                value = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
            ret = dictAdd(d, key, value);

            serverAssert(ret == DICT_OK);
        }
        serverAssert(dictSize(d) == size);
        hashTypeReleaseIterator(hi);

        /* Remove random elements to reach the right count. */
        while (size > count) {								// 从dict中堆随机弹出到只剩count个数据
            dictEntry *de;
            de = dictGetFairRandomKey(d);
            dictUnlink(d,dictGetKey(de));
            sdsfree(dictGetKey(de));
            sdsfree(dictGetVal(de));
            dictFreeUnlinkedEntry(d,de);
            size--;
        }

        /* Reply with what's in the dict and release memory */
        dictIterator *di;
        dictEntry *de;
        di = dictGetIterator(d);
        while ((de = dictNext(di)) != NULL) {				// 遍历新的dict，将里面的数据全部发送给c
            sds key = dictGetKey(de);
            sds value = dictGetVal(de);
            if (withvalues && c->resp > 2)
                addReplyArrayLen(c,2);		// 先发送长度：2
            addReplyBulkSds(c, key);
            if (withvalues)
                addReplyBulkSds(c, value);
        }

        dictReleaseIterator(di);							// 释放掉di与新的dict
        dictRelease(d);
    }

    /* CASE 4: We have a big hash compared to the requested number of elements.
     * In this case we can simply get random elements from the hash and add
     * to the temporary hash, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {															// case4：（count不够大）取出count个数据，并发送给c
        if (hash->encoding == OBJ_ENCODING_LISTPACK) {		// listpack：一次性取出count个随机数据，把内容发送给c
            /* it is inefficient to repeatedly pick one random element from a
             * listpack. so we use this instead: */
            listpackEntry *keys, *vals = NULL;
            keys = zmalloc(sizeof(listpackEntry)*count);
            if (withvalues)
                vals = zmalloc(sizeof(listpackEntry)*count);
            serverAssert(lpRandomPairsUnique(hash->ptr, count, keys, vals) == count);
            hrandfieldReplyWithListpack(c, count, keys, vals);
            zfree(keys);
            zfree(vals);
            return;
        }

        /* Hashtable encoding (generic implementation) */
        unsigned long added = 0;				// hashtable：一个一个取出count个随机数据，并把数据发送给c
        listpackEntry key, value;
        dict *d = dictCreate(&hashDictType);
        dictExpand(d, count);
        while(added < count) {
            hashTypeRandomElement(hash, size, &key, withvalues? &value : NULL);

            /* Try to add the object to the dictionary. If it already exists
            * free it, otherwise increment the number of objects we have
            * in the result dictionary. */
            sds skey = hashSdsFromListpackEntry(&key);
            if (dictAdd(d,skey,NULL) != DICT_OK) {
                sdsfree(skey);
                continue;
            }
            added++;

            /* We can reply right away, so that we don't need to store the value in the dict. */
            if (withvalues && c->resp > 2)		// 回复数据
                addReplyArrayLen(c,2);
            hashReplyFromListpackEntry(c, &key);
            if (withvalues)
                hashReplyFromListpackEntry(c, &value);
        }

        /* Release memory */
        dictRelease(d);		// 释放掉dict
    }
}

/* HRANDFIELD key [<count> [WITHVALUES]] */
void hrandfieldCommand(client *c) {																								// 向c随机发送key对应hash的l个数据（count默认为1）
    long l;
    int withvalues = 0;
    robj *hash;
    listpackEntry ele;

    if (c->argc >= 3) {			// 有count参数：解析count并调用hrandfieldWithCountCommand()
        if (getRangeLongFromObjectOrReply(c,c->argv[2],-LONG_MAX,LONG_MAX,&l,NULL) != C_OK) return;		// 获取key对应的hash
        if (c->argc > 4 || (c->argc == 4 && strcasecmp(c->argv[3]->ptr,"withvalues"))) {		// 参数有误：直接返回
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        } else if (c->argc == 4) {		// 参数withvalues：发送value
            withvalues = 1;
            if (l < -LONG_MAX/2 || l > LONG_MAX/2) {
                addReplyError(c,"value is out of range");
                return;
            }
        }
        hrandfieldWithCountCommand(c, l, withvalues);
        return;
    }

    /* Handle variant without <count> argument. Reply with simple bulk string */
    if ((hash = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]))== NULL ||			// 没有该key对应的hash或类型不对
        checkType(c,hash,OBJ_HASH)) {
        return;
    }

    hashTypeRandomElement(hash,hashTypeLength(hash),&ele,NULL);			// count默认为1：随机取出一个节点的数据，并发送给c
    hashReplyFromListpackEntry(c, &ele);
}
