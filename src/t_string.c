/*
	有关string的命令的实现

*/



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
#include <math.h> /* isnan(), isinf() */

/* Forward declarations */
int getGenericCommand(client *c);

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/

static int checkStringLength(client *c, long long size, long long append) {				// 返回是否响应该请求
    if (mustObeyClient(c))		// 不管长度如何，这个信息都不能被拒绝
        return C_OK;
    /* 'uint64_t' cast is there just to prevent undefined behavior on overflow */
    long long total = (uint64_t)size + append;
    /* Test configured max-bulk-len represending a limit of the biggest string object,
     * and also test for overflow. */
    if (total > server.proto_max_bulk_len || total < size || total < append) {		// 长度不符合要求的话（长度过长，子长度为负），拒绝该请求
        addReplyError(c,"string exceeds maximum allowed size (proto-max-bulk-len)");
        return C_ERR;
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX, GETSET.
 *
 * 'flags' changes the behavior of the command (NX, XX or GET, see below).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */

#define OBJ_NO_FLAGS 0
#define OBJ_SET_NX (1<<0)          /* Set if key not exists. */
#define OBJ_SET_XX (1<<1)          /* Set if key exists. */
#define OBJ_EX (1<<2)              /* Set if time in seconds is given */
#define OBJ_PX (1<<3)              /* Set if time in ms in given */
#define OBJ_KEEPTTL (1<<4)         /* Set and keep the ttl */
#define OBJ_SET_GET (1<<5)         /* Set if want to get key before set */
#define OBJ_EXAT (1<<6)            /* Set if timestamp in second is given */
#define OBJ_PXAT (1<<7)            /* Set if timestamp in ms is given */
#define OBJ_PERSIST (1<<8)         /* Set if we need to remove the ttl */

/* Forward declaration */
static int getExpireMillisecondsOrReply(client *c, robj *expire, int flags, int unit, long long *milliseconds);

void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {				// 设置一个key-value信息（expire：过期时间， unit：expire单位， ok_reply/abort_reply：成功或出错时响应的信息）
    long long milliseconds = 0; /* initialized to avoid any harmness warning */
    int found = 0;
    int setkey_flags = 0;

    if (expire && getExpireMillisecondsOrReply(c, expire, flags, unit, &milliseconds) != C_OK) {		// 数据过期了
        return;
    }

    if (flags & OBJ_SET_GET) {								// set之前get一下
        if (getGenericCommand(c) == C_ERR) return;			// 没有key对应的value
    }

    found = (lookupKeyWrite(c->db,key) != NULL);			// 是否查找到了key对应的值

    if ((flags & OBJ_SET_NX && found) ||
        (flags & OBJ_SET_XX && !found))
    {
        if (!(flags & OBJ_SET_GET)) {
            addReply(c, abort_reply ? abort_reply : shared.null[c->resp]);		// set之前不要get的话，响应一下abort_reply
        }
        return;
    }

    /* When expire is not NULL, we avoid deleting the TTL so it can be updated later instead of being deleted and then created again. */
    setkey_flags |= ((flags & OBJ_KEEPTTL) || expire) ? SETKEY_KEEPTTL : 0;			// 是否设置过期时间
    setkey_flags |= found ? SETKEY_ALREADY_EXIST : SETKEY_DOESNT_EXIST;				// key是否已经存在对应的value
	
    setKey(c,c->db,key,val,setkey_flags);		// 设置key-value
    server.dirty++;
    notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id);		// 向服务器通知一下准备set

    if (expire) {
        setExpire(c,c->db,key,milliseconds);			// 设置超时时间
        /* Propagate as SET Key Value PXAT millisecond-timestamp if there is
         * EX/PX/EXAT/PXAT flag. */
        robj *milliseconds_obj = createStringObjectFromLongLong(milliseconds);
        rewriteClientCommandVector(c, 5, shared.set, key, val, shared.pxat, milliseconds_obj);			// 重设c的argv参数为key与他的value + 超时时间
        decrRefCount(milliseconds_obj);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"expire",key,c->db->id);
    }

    if (!(flags & OBJ_SET_GET)) {
        addReply(c, ok_reply ? ok_reply : shared.ok);		// 响应一个ok_reply
    }

    /* Propagate without the GET argument (Isn't needed if we had expire since in that case we completely re-written the command argv) */
    if ((flags & OBJ_SET_GET) && !expire) {		// 将c的argv信息去掉"get"后重新保存一份（传播时不带get参数）
        int argc = 0;
        int j;
        robj **argv = zmalloc((c->argc-1)*sizeof(robj*));
        for (j=0; j < c->argc; j++) {
            char *a = c->argv[j]->ptr;
            /* Skip GET which may be repeated multiple times. */
            if (j >= 3 &&
                (a[0] == 'g' || a[0] == 'G') &&
                (a[1] == 'e' || a[1] == 'E') &&
                (a[2] == 't' || a[2] == 'T') && a[3] == '\0')	/* 跳过GET */
                continue;
            argv[argc++] = c->argv[j];
            incrRefCount(c->argv[j]);
        }
        replaceClientCommandVector(c, argc, argv);
    }
}

/*
 * Extract the `expire` argument of a given GET/SET command as an absolute timestamp in milliseconds.
 *
 * "client" is the client that sent the `expire` argument.
 * "expire" is the `expire` argument to be extracted.
 * "flags" represents the behavior of the command (e.g. PX or EX).
 * "unit" is the original unit of the given `expire` argument (e.g. UNIT_SECONDS).
 * "milliseconds" is output argument.
 *
 * If return C_OK, "milliseconds" output argument will be set to the resulting absolute timestamp.
 * If return C_ERR, an error reply has been added to the given client.
 */
static int getExpireMillisecondsOrReply(client *c, robj *expire, int flags, int unit, long long *milliseconds) {				// 查看数据是否没有过期（过期的话通知client
    int ret = getLongLongFromObjectOrReply(c, expire, milliseconds, NULL);		// 获取过期时间到milliseconds中
    if (ret != C_OK) {
        return ret;
    }

    if (*milliseconds <= 0 || (unit == UNIT_SECONDS && *milliseconds > LLONG_MAX / 1000)) {		// milliseconds超出范围的话返回错误
        /* Negative value provided or multiplication is gonna overflow. */
        addReplyErrorExpireTime(c);
        return C_ERR;
    }

    if (unit == UNIT_SECONDS) *milliseconds *= 1000;

    if ((flags & OBJ_PX) || (flags & OBJ_EX)) {
        *milliseconds += commandTimeSnapshot();
    }

    if (*milliseconds <= 0) {		// 过期了
        /* Overflow detected. */
        addReplyErrorExpireTime(c);
        return C_ERR;
    }

    return C_OK;
}

#define COMMAND_GET 0
#define COMMAND_SET 1
/*
 * The parseExtendedStringArgumentsOrReply() function performs the common validation for extended
 * string arguments used in SET and GET command.
 *
 * Get specific commands - PERSIST/DEL
 * Set specific commands - XX/NX/GET
 * Common commands - EX/EXAT/PX/PXAT/KEEPTTL
 *
 * Function takes pointers to client, flags, unit, pointer to pointer of expire obj if needed
 * to be determined and command_type which can be COMMAND_GET or COMMAND_SET.
 *
 * If there are any syntax violations C_ERR is returned else C_OK is returned.
 *
 * Input flags are updated upon parsing the arguments. Unit and expire are updated if there are any
 * EX/EXAT/PX/PXAT arguments. Unit is updated to millisecond if PX/PXAT is set.
 */
int parseExtendedStringArgumentsOrReply(client *c, int *flags, int *unit, robj **expire, int command_type) {			// 为flags加上c的argv对应的标签

    int j = command_type == COMMAND_GET ? 2 : 3;
    for (; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1];

        if ((opt[0] == 'n' || opt[0] == 'N') &&						// 为flags加上对应的标签
            (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' &&
            !(*flags & OBJ_SET_XX) && (command_type == COMMAND_SET))
        {
            *flags |= OBJ_SET_NX;
        } else if ((opt[0] == 'x' || opt[0] == 'X') &&
                   (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' &&
                   !(*flags & OBJ_SET_NX) && (command_type == COMMAND_SET))
        {
            *flags |= OBJ_SET_XX;
        } else if ((opt[0] == 'g' || opt[0] == 'G') &&
                   (opt[1] == 'e' || opt[1] == 'E') &&
                   (opt[2] == 't' || opt[2] == 'T') && opt[3] == '\0' &&
                   (command_type == COMMAND_SET))
        {
            *flags |= OBJ_SET_GET;
        } else if (!strcasecmp(opt, "KEEPTTL") && !(*flags & OBJ_PERSIST) &&
            !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) &&
            !(*flags & OBJ_PX) && !(*flags & OBJ_PXAT) && (command_type == COMMAND_SET))
        {
            *flags |= OBJ_KEEPTTL;
        } else if (!strcasecmp(opt,"PERSIST") && (command_type == COMMAND_GET) &&
               !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) &&
               !(*flags & OBJ_PX) && !(*flags & OBJ_PXAT) &&
               !(*flags & OBJ_KEEPTTL))
        {
            *flags |= OBJ_PERSIST;
        } else if ((opt[0] == 'e' || opt[0] == 'E') &&
                   (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' &&
                   !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) &&
                   !(*flags & OBJ_EXAT) && !(*flags & OBJ_PX) &&
                   !(*flags & OBJ_PXAT) && next)
        {
            *flags |= OBJ_EX;
            *expire = next;
            j++;
        } else if ((opt[0] == 'p' || opt[0] == 'P') &&
                   (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' &&
                   !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) &&
                   !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) &&
                   !(*flags & OBJ_PXAT) && next)
        {
            *flags |= OBJ_PX;
            *unit = UNIT_MILLISECONDS;
            *expire = next;
            j++;
        } else if ((opt[0] == 'e' || opt[0] == 'E') &&
                   (opt[1] == 'x' || opt[1] == 'X') &&
                   (opt[2] == 'a' || opt[2] == 'A') &&
                   (opt[3] == 't' || opt[3] == 'T') && opt[4] == '\0' &&
                   !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) &&
                   !(*flags & OBJ_EX) && !(*flags & OBJ_PX) &&
                   !(*flags & OBJ_PXAT) && next)
        {
            *flags |= OBJ_EXAT;
            *expire = next;
            j++;
        } else if ((opt[0] == 'p' || opt[0] == 'P') &&
                   (opt[1] == 'x' || opt[1] == 'X') &&
                   (opt[2] == 'a' || opt[2] == 'A') &&
                   (opt[3] == 't' || opt[3] == 'T') && opt[4] == '\0' &&
                   !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) &&
                   !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) &&
                   !(*flags & OBJ_PX) && next)
        {
            *flags |= OBJ_PXAT;
            *unit = UNIT_MILLISECONDS;
            *expire = next;
            j++;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return C_ERR;
        }
    }
    return C_OK;
}

/* SET key value [NX] [XX] [KEEPTTL] [GET] [EX <seconds>] [PX <milliseconds>]
 *     [EXAT <seconds-timestamp>][PXAT <milliseconds-timestamp>] */
void setCommand(client *c) {			// 执行set命令前，先解析一下参数以及对value进行重新编码（省内存）
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = OBJ_NO_FLAGS;

    if (parseExtendedStringArgumentsOrReply(c,&flags,&unit,&expire,COMMAND_SET) != C_OK) {		// 将c中argv的参数转换成flags的内容（解析参数）
        return;
    }

    c->argv[2] = tryObjectEncoding(c->argv[2]);			// 对参数而进行重新编码
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);			// 执行色set指令
}

void setnxCommand(client *c) {					// 执行key不存在对应value时的set命令
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,OBJ_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

void setexCommand(client *c) {					// 执行有过期时间的set命令（过期时间单位是秒）
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_EX,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

void psetexCommand(client *c) {					// 执行有过期时间的set命令（过期时间单位是ms）
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_PX,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}

int getGenericCommand(client *c) {					// 向c发送c的argv[i]对应的value的bulk类型信息 ---- 查找get后面参数对应的value并响应（get命令）
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL)			// 没有查找到key对应的value ---- 查找get后面参数对应的value，没有的话响应空
        return C_OK;

    if (checkType(c,o,OBJ_STRING)) {			// 与预期的类型不一致  			----》  如果不是string的话报错
        return C_ERR;
    }

    addReplyBulk(c,o);							// 响应值，并返回ok
    return C_OK;
}

void getCommand(client *c) {					// 执行get命令
    getGenericCommand(c);
}

/*
 * GETEX <key> [PERSIST][EX seconds][PX milliseconds][EXAT seconds-timestamp][PXAT milliseconds-timestamp]
 *
 * The getexCommand() function implements extended options and variants of the GET command. Unlike GET
 * command this command is not read-only.
 *
 * The default behavior when no options are specified is same as GET and does not alter any TTL.
 *
 * Only one of the below options can be used at a given time.
 *
 * 1. PERSIST removes any TTL associated with the key.
 * 2. EX Set expiry TTL in seconds.
 * 3. PX Set expiry TTL in milliseconds.
 * 4. EXAT Same like EX instead of specifying the number of seconds representing the TTL
 *      (time to live), it takes an absolute Unix timestamp
 * 5. PXAT Same like PX instead of specifying the number of milliseconds representing the TTL
 *      (time to live), it takes an absolute Unix timestamp
 *
 * Command would either return the bulk string, error or nil.
 */
void getexCommand(client *c) {
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = OBJ_NO_FLAGS;

    if (parseExtendedStringArgumentsOrReply(c,&flags,&unit,&expire,COMMAND_GET) != C_OK) {			// 解析argv参数
        return;
    }

    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL)		// 没找到key对应的value
        return;

    if (checkType(c,o,OBJ_STRING)) {				// o的类型是string
        return;
    }

    /* Validate the expiration time value first */
    long long milliseconds = 0;
    if (expire && getExpireMillisecondsOrReply(c, expire, flags, unit, &milliseconds) != C_OK) {		// 时间已过期
        return;
    }

    /* We need to do this before we expire the key or delete it */
    addReplyBulk(c,o);														// 向client返回o

    /* This command is never propagated as is. It is either propagated as PEXPIRE[AT],DEL,UNLINK or PERSIST.
     * This why it doesn't need special handling in feedAppendOnlyFile to convert relative expire time to absolute one. */
    if (((flags & OBJ_PXAT) || (flags & OBJ_EXAT)) && checkAlreadyExpired(milliseconds)) {			// 时间戳未过期
        /* When PXAT/EXAT absolute timestamp is specified, there can be a chance that timestamp
         * has already elapsed so delete the key in that case. */
        int deleted = server.lazyfree_lazy_expire ? dbAsyncDelete(c->db, c->argv[1]) :
                      dbSyncDelete(c->db, c->argv[1]);
        serverAssert(deleted);
        robj *aux = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
        rewriteClientCommandVector(c,2,aux,c->argv[1]);			// 重新写入c的参数
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty++;
    } else if (expire) {			// 设置过期时间
        setExpire(c,c->db,c->argv[1],milliseconds);
        /* Propagate as PXEXPIREAT millisecond-timestamp if there is
         * EX/PX/EXAT/PXAT flag and the key has not expired. */
        robj *milliseconds_obj = createStringObjectFromLongLong(milliseconds);
        rewriteClientCommandVector(c,3,shared.pexpireat,c->argv[1],milliseconds_obj);
        decrRefCount(milliseconds_obj);
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"expire",c->argv[1],c->db->id);
        server.dirty++;
    } else if (flags & OBJ_PERSIST) {			// 删除过期时间
        if (removeExpire(c->db, c->argv[1])) {
            signalModifiedKey(c, c->db, c->argv[1]);
            rewriteClientCommandVector(c, 2, shared.persist, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"persist",c->argv[1],c->db->id);
            server.dirty++;
        }
    }
}

void getdelCommand(client *c) {						// 执行执行删除命令（先执行get并返回key对应的值）
    if (getGenericCommand(c) == C_ERR) return;			// 执行get argv[1]命令失败
    if (dbSyncDelete(c->db, c->argv[1])) {				// 异步删除成功
        /* Propagate as DEL command */
        rewriteClientCommandVector(c,2,shared.del,c->argv[1]);
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty++;
    }
}

void getsetCommand(client *c) {						// 执行get_set命令
    if (getGenericCommand(c) == C_ERR) return;		// 先get并响应
    c->argv[2] = tryObjectEncoding(c->argv[2]);		// 重新编码value
    setKey(c,c->db,c->argv[1],c->argv[2],0);		// 设置key-value
    notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[1],c->db->id);
    server.dirty++;

    /* Propagate as SET command */
    rewriteClientCommandArgument(c,0,shared.set);
}

void setrangeCommand(client *c) {			// set range命令：向key对应的val后面追加长度为offset 的 value字符串
    robj *o;
    long offset;
    sds value = c->argv[3]->ptr;

    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != C_OK)		// 获取偏移量
        return;

    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }

    o = lookupKeyWrite(c->db,c->argv[1]);			// 查找key对应的value
    if (o == NULL) {			// 没有对应的value的话，新建value并添加
        /* Return 0 when setting nothing on a non-existing string */
        if (sdslen(value) == 0) {
            addReply(c,shared.czero);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c,offset,sdslen(value)) != C_OK)
            return;

        o = createObject(OBJ_STRING,sdsnewlen(NULL, offset+sdslen(value)));			// 先新建一个长度合适的obj，后面填充内容
        dbAdd(c->db,c->argv[1],o);			
    } else {					// 有对应的value的话，
        size_t olen;	

        /* Key exists, check type */
        if (checkType(c,o,OBJ_STRING))		// value已存在且类型匹配
            return;

        /* Return existing string length when setting nothing */
        olen = stringObjectLen(o);
        if (sdslen(value) == 0) {			// value长度未0
            addReplyLongLong(c,olen);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c,offset,sdslen(value)) != C_OK)		// 无需回应
            return;

        /* Create a copy when the object is shared or encoded. */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    if (sdslen(value) > 0) {		// 向o的内容后面追加value内容
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));
        memcpy((char*)o->ptr+offset,value,sdslen(value));
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);
        server.dirty++;
    }
    addReplyLongLong(c,sdslen(o->ptr));			// 响应长度
}

void getrangeCommand(client *c) {				// 执行getrange 命令（截取key内容的satrt-end的部分）
    robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;

    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK)		// 获取start
        return;
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)		// 获取end
        return;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||		// 获取key对应的value
        checkType(c,o,OBJ_STRING)) return;

    if (o->encoding == OBJ_ENCODING_INT) {		// 获取内容的长度与内容（str类型的）
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }

    /* Convert negative indexes */
    if (start < 0 && end < 0 && start > end) {
        addReply(c,shared.emptybulk);
        return;
    }
    if (start < 0) start = strlen+start;		// 负数代表倒序遍历
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((unsigned long long)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
        addReply(c,shared.emptybulk);
    } else {				// 响应key对应的val的start-end的内容
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

void mgetCommand(client *c) {				// mget命令（get多个值）
    int j;

    addReplyArrayLen(c,c->argc-1);
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
            addReplyNull(c);
        } else {
            if (o->type != OBJ_STRING) {
                addReplyNull(c);
            } else {
                addReplyBulk(c,o);
            }
        }
    }
}

void msetGenericCommand(client *c, int nx) {			// 设置多个值（nx：当key不存在时才设置；仅在所有key不存在时设置成功）
    int j;

    if ((c->argc % 2) == 0) {			// 参数数量不对
        addReplyErrorArity(c);
        return;
    }

    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set anything if at least one key already exists. */
    if (nx) {			// not exists：若有的值已经存在，则全部都不执行
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                addReply(c, shared.czero);
                return;
            }
        }
    }

    for (j = 1; j < c->argc; j += 2) {
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);			// 重新编码（省内存）
        setKey(c, c->db, c->argv[j], c->argv[j + 1], 0);			// set值
        notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[j],c->db->id);
    }
    server.dirty += (c->argc-1)/2;
    addReply(c, nx ? shared.cone : shared.ok);
}

void msetCommand(client *c) {			// set多个
    msetGenericCommand(c,0);
}

void msetnxCommand(client *c) {			// 多余key不存在才设置全部
    msetGenericCommand(c,1);
}

void incrDecrCommand(client *c, long long incr) {					// incr命令
    long long value, oldvalue;
    robj *o, *new;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (checkType(c,o,OBJ_STRING)) return;
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return;

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {			// 增加后超出了边界
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;

    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)			// 修改后的值在long范围内的话，使用long类型存储
    {
        new = o;
        o->ptr = (void*)((long)value);
    } else {
        new = createStringObjectFromLongLongForValue(value);		// 长度在ll的话，使用string存储
        if (o) {
            dbReplaceValue(c->db,c->argv[1],new);
        } else {
            dbAdd(c->db,c->argv[1],new);
        }
    }
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    server.dirty++;
    addReplyLongLong(c, value);			// 响应修改后的长度
}

void incrCommand(client *c) {			// 变量值增加一（变量在参数中）
    incrDecrCommand(c,1);
}

void decrCommand(client *c) {			// 变量值减少一（变量在参数中）
    incrDecrCommand(c,-1);
}

void incrbyCommand(client *c) {				// incrby命令（变量与数值在c的参数后面）
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,incr);
}

void decrbyCommand(client *c) {					// decrby命令（变量与数值在c的参数后面）
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    /* Overflow check: negating LLONG_MIN will cause an overflow */
    if (incr == LLONG_MIN) {
        addReplyError(c, "decrement would overflow");
        return;
    }
    incrDecrCommand(c,-incr);
}

void incrbyfloatCommand(client *c) {				// incrby float 的命令
    long double incr, value;
    robj *o, *new;

    o = lookupKeyWrite(c->db,c->argv[1]);			// 要修改的变量内容
    if (checkType(c,o,OBJ_STRING)) return;
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != C_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != C_OK)		// 要修改的值或者增加的值不是数字
        return;

    value += incr;
    if (isnan(value) || isinf(value)) {			// 变量内容数字不是正常数字
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }
    new = createStringObjectFromLongDouble(value,1);			// 以简化方式创建一个value的字符串对象
    if (o)
        dbReplaceValue(c->db,c->argv[1],new);					// 原来有值：增加；原来没值：新增。
    else
        dbAdd(c->db,c->argv[1],new);
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);
    server.dirty++;
    addReplyBulk(c,new);							// 响应修改后的值

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    rewriteClientCommandArgument(c,0,shared.set);			// 以set命令保存该命令（防止aof备份时产生不一样的结果
    rewriteClientCommandArgument(c,2,new);
    rewriteClientCommandArgument(c,3,shared.keepttl);
}

void appendCommand(client *c) {			// append key val：append操作
    size_t totlen;
    robj *o, *append;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {				// key不存在的话，新建一个对象
        /* Create the key */
        c->argv[2] = tryObjectEncoding(c->argv[2]);		// 重新编码
        dbAdd(c->db,c->argv[1],c->argv[2]);				// 新增内容
        incrRefCount(c->argv[2]);						// 增加引用计数
        totlen = stringObjectLen(c->argv[2]);			// 获取修改后的内容长度
    } else {
        /* Key exists, check type */
        if (checkType(c,o,OBJ_STRING))
            return;

        /* "append" is an argument, so always an sds */
        append = c->argv[2];
        if (checkStringLength(c,stringObjectLen(o),sdslen(append->ptr)) != C_OK)		// 长度不符
            return;

        /* Append the value */
        o = dbUnshareStringValue(c->db,c->argv[1],o);		// 获取key的value
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));		// append操作
        totlen = sdslen(o->ptr);							// 修改后的长度
    }
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id);
    server.dirty++;
    addReplyLongLong(c,totlen);				// 响应修改后的长度
}

void strlenCommand(client *c) {					// strlen key ：strlen操作
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;			// 获取key对应的value
    addReplyLongLong(c,stringObjectLen(o));
}

/* LCS key1 key2 [LEN] [IDX] [MINMATCHLEN <len>] [WITHMATCHLEN] */
// 获取ley1 key2 的最长公共子序列
void lcsCommand(client *c) {
    uint32_t i, j;
    long long minmatchlen = 0;
    sds a = NULL, b = NULL;
    int getlen = 0, getidx = 0, withmatchlen = 0;
    robj *obja = NULL, *objb = NULL;

    obja = lookupKeyRead(c->db,c->argv[1]);		/* key1对应的value */
    objb = lookupKeyRead(c->db,c->argv[2]);		/* key2对应的value */
    if ((obja && obja->type != OBJ_STRING) ||
        (objb && objb->type != OBJ_STRING))
    {											// 类型不对
        addReplyError(c,
            "The specified keys must contain string values");
        /* Don't cleanup the objects, we need to do that
         * only after calling getDecodedObject(). */
        obja = NULL;
        objb = NULL;
        goto cleanup;
    }
    obja = obja ? getDecodedObject(obja) : createStringObject("",0);			// 获取两个值对应的sds格式的robj
    objb = objb ? getDecodedObject(objb) : createStringObject("",0);
    a = obja->ptr;			// 获取两个对象的字符串
    b = objb->ptr;

    for (j = 3; j < (uint32_t)c->argc; j++) {			// 分析后续参数
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1) - j;

        if (!strcasecmp(opt,"IDX")) {
            getidx = 1;
        } else if (!strcasecmp(opt,"LEN")) {
            getlen = 1;
        } else if (!strcasecmp(opt,"WITHMATCHLEN")) {
            withmatchlen = 1;
        } else if (!strcasecmp(opt,"MINMATCHLEN") && moreargs) {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&minmatchlen,NULL)
                != C_OK) goto cleanup;
            if (minmatchlen < 0) minmatchlen = 0;
            j++;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Complain if the user passed ambiguous parameters. */
    if (getlen && getidx) {			// 不能同时获取长度与下标
        addReplyError(c,
            "If you want both the length and indexes, please just use IDX.");
        goto cleanup;
    }

    /* Detect string truncation or later overflows. */
    if (sdslen(a) >= UINT32_MAX-1 || sdslen(b) >= UINT32_MAX-1) {			// 长度过长
        addReplyError(c, "String too long for LCS");
        goto cleanup;
    }

    /* Compute the LCS using the vanilla dynamic programming technique of
     * building a table of LCS(x,y) substrings. */
    uint32_t alen = sdslen(a);			// 两个字符串的长度
    uint32_t blen = sdslen(b);

    /* Setup an uint32_t array to store at LCS[i,j] the length of the
     * LCS A0..i-1, B0..j-1. Note that we have a linear array here, so
     * we index it as LCS[j+(blen+1)*i] */
    #define LCS(A,B) lcs[(B)+((A)*(blen+1))]			// 二维数组的第a行第b列的下标

    /* Try to allocate the LCS table, and abort on overflow or insufficient memory. */
    unsigned long long lcssize = (unsigned long long)(alen+1)*(blen+1); /* Can't overflow due to the size limits above. */
    unsigned long long lcsalloc = lcssize * sizeof(uint32_t);
    uint32_t *lcs = NULL;
    if (lcsalloc < SIZE_MAX && lcsalloc / lcssize == sizeof(uint32_t)) {
        if (lcsalloc > (size_t)server.proto_max_bulk_len) {
            addReplyError(c, "Insufficient memory, transient memory for LCS exceeds proto-max-bulk-len");
            goto cleanup;
        }
        lcs = ztrymalloc(lcsalloc);
    }
    if (!lcs) {
        addReplyError(c, "Insufficient memory, failed allocating transient memory for LCS");
        goto cleanup;
    }


	// lcs算法

    /* Start building the LCS table. */
    for (uint32_t i = 0; i <= alen; i++) {
        for (uint32_t j = 0; j <= blen; j++) {
            if (i == 0 || j == 0) {
                /* If one substring has length of zero, the
                 * LCS length is zero. */
                LCS(i,j) = 0;
            } else if (a[i-1] == b[j-1]) {
                /* The len LCS (and the LCS itself) of two
                 * sequences with the same final character, is the
                 * LCS of the two sequences without the last char
                 * plus that last char. */
                LCS(i,j) = LCS(i-1,j-1)+1;
            } else {
                /* If the last character is different, take the longest
                 * between the LCS of the first string and the second
                 * minus the last char, and the reverse. */
                uint32_t lcs1 = LCS(i-1,j);
                uint32_t lcs2 = LCS(i,j-1);
                LCS(i,j) = lcs1 > lcs2 ? lcs1 : lcs2;
            }
        }
    }

    /* Store the actual LCS string in "result" if needed. We create
     * it backward, but the length is already known, we store it into idx. */
    uint32_t idx = LCS(alen,blen);
    sds result = NULL;        /* Resulting LCS string. */
    void *arraylenptr = NULL; /* Deferred length of the array for IDX. */
    uint32_t arange_start = alen, /* alen signals that values are not set. */
             arange_end = 0,
             brange_start = 0,
             brange_end = 0;

    /* Do we need to compute the actual LCS string? Allocate it in that case. */
    int computelcs = getidx || !getlen;
    if (computelcs) result = sdsnewlen(SDS_NOINIT,idx);

    /* Start with a deferred array if we have to emit the ranges. */
    uint32_t arraylen = 0;  /* Number of ranges emitted in the array. */
    if (getidx) {
        addReplyMapLen(c,2);
        addReplyBulkCString(c,"matches");
        arraylenptr = addReplyDeferredLen(c);
    }

    i = alen, j = blen;
    while (computelcs && i > 0 && j > 0) {
        int emit_range = 0;
        if (a[i-1] == b[j-1]) {
            /* If there is a match, store the character and reduce
             * the indexes to look for a new match. */
            result[idx-1] = a[i-1];

            /* Track the current range. */
            if (arange_start == alen) {
                arange_start = i-1;
                arange_end = i-1;
                brange_start = j-1;
                brange_end = j-1;
            } else {
                /* Let's see if we can extend the range backward since
                 * it is contiguous. */
                if (arange_start == i && brange_start == j) {
                    arange_start--;
                    brange_start--;
                } else {
                    emit_range = 1;
                }
            }
            /* Emit the range if we matched with the first byte of
             * one of the two strings. We'll exit the loop ASAP. */
            if (arange_start == 0 || brange_start == 0) emit_range = 1;
            idx--; i--; j--;
        } else {
            /* Otherwise reduce i and j depending on the largest
             * LCS between, to understand what direction we need to go. */
            uint32_t lcs1 = LCS(i-1,j);
            uint32_t lcs2 = LCS(i,j-1);
            if (lcs1 > lcs2)
                i--;
            else
                j--;
            if (arange_start != alen) emit_range = 1;
        }

        /* Emit the current range if needed. */
        uint32_t match_len = arange_end - arange_start + 1;
        if (emit_range) {
            if (minmatchlen == 0 || match_len >= minmatchlen) {
                if (arraylenptr) {
                    addReplyArrayLen(c,2+withmatchlen);
                    addReplyArrayLen(c,2);
                    addReplyLongLong(c,arange_start);
                    addReplyLongLong(c,arange_end);
                    addReplyArrayLen(c,2);
                    addReplyLongLong(c,brange_start);
                    addReplyLongLong(c,brange_end);
                    if (withmatchlen) addReplyLongLong(c,match_len);
                    arraylen++;
                }
            }
            arange_start = alen; /* Restart at the next match. */
        }
    }

    /* Signal modified key, increment dirty, ... */

    /* Reply depending on the given options. */
    if (arraylenptr) {		// 响应
        addReplyBulkCString(c,"len");
        addReplyLongLong(c,LCS(alen,blen));
        setDeferredArrayLen(c,arraylenptr,arraylen);
    } else if (getlen) {
        addReplyLongLong(c,LCS(alen,blen));
    } else {
        addReplyBulkSds(c,result);
        result = NULL;
    }

    /* Cleanup. */
    sdsfree(result);
    zfree(lcs);

cleanup:
    if (obja) decrRefCount(obja);
    if (objb) decrRefCount(objb);
    return;
}

