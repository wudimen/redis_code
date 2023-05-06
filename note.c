/*

	*：重要内容
	#：redis主要知识点
	&：新知识



*/一：reds自创数据结构部分：
adlist.h:
	L37:
		// 用于构建链表节点的结构体
		typedef struct listNode {
		    struct listNode *prev;
		    struct listNode *next;
		    void *value;
		} listNode;
		
	L58:
		// 使用宏定义便于获取一些常用的量（要用上()，防止出现错误）
		#define listLength(l) ((l)->len)
		
	L65:
		// 将一些简单的函数使用宏实现，增加程序效率
		#define listSetDupMethod(l,m) ((l)->dup = (m))
		
	L54:
		// 如果内容中可能有指针类型的变量，要注意释放！！
		typedef struct list {
	    	void (*free)(void *ptr);		// 是否需要释放节点（如果是指针的话就要释放，不释放会造成内存泄漏）
		} list;
		
	L75 - L93:
		// 函数命名方式：类名 + 函数作用（eg：listAddNodeHead）
		
*	L50：
		// 使用void*表示内容的指针时，要注意内容是否可以是指针（指向另一块内存），可能的话，就要设置一些函数来表示针对于他们的操作，防止要深拷贝的时候发生了浅拷贝；
		//  		eg：void *(*dup)(void *ptr);		用于复制list的内容
		//				void (*free)(void *ptr);	用于是否list的内容
		// 				int (*match)(void *ptr, void *key);			用于比较list的内容
		typedef struct list {
		    listNode *head;
		    listNode *tail;
		    void *(*dup)(void *ptr);		// list中要复制内容的话，如果内容是个指针，则要深拷贝，这里装的就是拷贝内容的方式（默认为空：直接复制）
		    void (*free)(void *ptr);		// 是否需要释放节点（如果是指针的话就要释放，不释放会造成内存泄漏）
		    int (*match)(void *ptr, void *key);		// 比较函数，比较list中的两个值是否相等
		    unsigned long len;
		} list;


sds.h:
&	/*  __attribute__ :：设置struct或union的属性（针对编译器）
			 __attribute__  ((aligned (8))) ： // 指定字节对齐格式为：采用8字节对齐格式（不指定数量的话编译器会使用最大收益的对齐方式）
			 __attribute__ ((__packed__)) ： // 使用一字节对齐格式（每个变量之间不相隔一字节，总字节数=全部的内容字节数，不添加字节来对齐）
			 __attribute__((at(0x0800F000)))： // 绝对定位，将内存定位在Flush或RAM的指定地址（不能在函数中使用：函数储存在栈地址中，栈地址由MDK自动分配，不能指定地址）
			__attribute__((section("section_name"))) ： // 将作用的函数或数据放在指定名字为"section_name"对于的段中
				// 在ARM编译器编译之后，代码被划分为不同的段，
				//			RO Section(ReadOnly)中存放代码段和常量，
				//			RW Section(ReadWrite)中存放可读写静态变量和全局变量，
				//			ZI Section(ZeroInit)是存放在RW段中初始化为0的变量。
			（可多个同时使用）
	/*

	
&	零长数组：
		1.只能在gnu c（gcc）下使用；
		2.在编译时统计内存，且内存为0；
		3.要在结构体中使用的话，一定要放在结构体最后（否则为他申请内存时会覆盖后面的元素）
		4.若要通过零长数组的内存取得整体的内存，要使用 __attribute__ ((__packed__)) 来通知编译器不要内存对齐
		5.当结构体中有一个时有时无的数据时，可以使用零长数组来实现；



dict.h:
**		redis数据库的总结构就是使用的dict：
			RedisDb保存着16个dict，每个dict保存着2个dickht（用于rehashing），每个dictht中含有x（为2^n个，因为key取hash函数映射后还要取模，而x % 2^n可以简化成x & (2^n-1)， 可以提高性能）个dictEntry（每个entry代表一个表：mylist/myhash...），
			每个entry的key就是表的名字（mylist），entry的value就是robj（通过type区分类别(list/strng/set/zset/hash)，通过encoding区分类型(list：ziplist/quicklist ，string：raw/embstr，
																			zset：ziplist/skiplist，hash：ziplist/hashtable，set：intset/hashtable)
			RedisDb -> dict[16] -> dictht[2] -> dictEntry[2^n] (key：sds， value：robj)
## 		map/hash储存数据注意点：
			hash/map是使用下标进行排序与储存数据的，要对齐进行改变容量时要对所有的数据进行rehash才能放在新的hash/map中

**		单线程处理rehash的方式：dictrehash时将table[1]作为新的容器（一般时使用table[0]），开始rehash时从0开始一个一个rehash（可中断），
   								将rehash后的值装进新的容器(table[1])里，等到全部的table[0]的值都rehash完成，就把table[1]的地址复制到table[0]中并重置table[1]，完成复制


二：内存编码数据结构部分：

intset.h：
		intset.c:L176：算法思路：
		if (prepend)					// 要使用扩容的方式插入数据，说明这个数已经大于原范围可存下的最大值/小于最小值了，因此直接放在最前面或者最后面！！！
			_intsetSet(is,0,value);
		else
			_intsetSet(is,intrev32ifbe(is->length),value);


##		is的特点：
			用于存储整形数据；
			数据是有序的；
			整个容器的容量大小会随着插入数据即时改变；
			每个数据的容量大小会随着所含数据的需求而改变（足以容下每一个数据）；



ziplist.h：
**		ziplist（压缩链表）：
			可以储存字符串与整形；
			可能会产生连锁更新：
				因为zlentry中储存上一个数据的时候小于254时使用1字节，大于等于时使用5字节，若当前数据长度全是253且要插入一个大于254的数据的话，
				因为这个数大于254了因此要重新申请一块5字节的内存，后面的储存的长度也会变成257（253+4），大于了254，又要重新申请内存，因此后面全部的内存都要重新申请！
*		zlEnrtry：
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
						2.长度小于14比特的字符串：01xx xxxx			   xxxx xxxx（14比特的字符串长度） + 字符串
						3.长度大于14比特的字符串：1000 0000 + 4字节的字符串长度 + 字符串

listpack.h：
	.c：L1257：
		要访问的数据在整个数组的后半部分，则可以从后面开始遍历
*	.c：L1182
		要将一块内存追加到另一块内存后面，需要将地址小的内存复制到地址大的内存后面（防止覆盖地址小的内存的尾部信息）；
		并且如果待追加的内存在后面的话，可以先将后面的内容移到后面（为被追加的内存腾出地方来），在把被追加的内存复制过去
		
	listpack存储格式：totalbyte+count+  		entry（encoding+data+len：encoding：保存数据用的编码格式，data：保存的数据，len：encod+data用的长度，用于向前遍历）
	encoding编码方式：（代码所在：编码方式：listpack.c:L55-L99；编码整数：lpEncodeIntegerGetType()；编码str：宏：LP_ENCODING_6/12/32BIT_STR_LEN，解码内容：lpGetWithSize()）
		特定：（根据内容使用的频率，将较常用的类型用较少的编码字节，增加了可使用的字节：较小的整数：用0编码，可用7字节， 较短的str，用01编码，可用6字节）
		<int>	：（xxxx存着整数内容）
			7位整数     0-127 ：					0xxx xxxx
			13位整数 -4096-4095 ：					110x xxxx     xxxx xxxx
			16位整数 -32768-32767 ：				1111 0001     xxxx xxxx  xxxx xxxx
			24位整数 -8388608-8388607 ：			1111 0010     xxxx ... xxxx(24位)
			32位整数 -2147483648-21473647 ： 		1111 0011 	  xxxx ... xxxx(32位)
			64位整数 其他 ： 							1111 0100     xxxx ... xxxx(64位)
		<str>	：（xxxx存储的是字符串长度）
			str长度小于6位：							10xx xxxx + 字符串
			str长度小于12位：							1110 xxxx + 字符串
			str长度大于12位小于32位：					1111 0000 + xxxx ... xxxx(4字节) + 字符串
	len编码方式：（代码所在：lpEncodeBacklen()+lpDecodeBacklen()）
		<varint编码>		：使用每个字节的第一位存储是否是内容开头（0：开头，1：不是开头）；
		<eg>				：整数512：1000 0000 ----> 编码过程：7位7位拆分：000 0001  000 0000----开始的字节第一位使用0，其余使用1：0000 0001  1000 0000---->编码结果：0000 0001  1000 0000
		<好处>			：可以从结尾找到不固定长度的数据的开始点，使得listpack可以向前遍历
  	listpackEntry
  		<问题>	：（typedef struct { unsigned char *sval;  uint32_t slen;  long long lval; } listpackEntry;）：这个结构并不符合实际listpack节点的结构？
  		<解答>	：它只是用来存储listpackEntry用的，并不参与到listpack添加数据的过程中（代码所在：liInsert()）,插入数据/获取数据都是直接操作内存，根据encoding的编码方式获取信息，从而获取节点信息的内容或向前/后遍历，
  					listpackEntry的作用仅仅是保存listpack节点信息（甚至可以保存ziplist的节点信息），不参与到listpack的结构中！
	listpack如何实现向后遍历：（代码所在：lpSkip()）
		根据encoding获取存储类型：
			<int>	数据保存在了encoding中，根据encoding可直接获得encod+data用的字节数，再计算保存encoding+data长度用的字节数(len的字节数)，就是整个节点用的字节数了，加上就可跳至下一节点；
			<str>	数据的长度保存在encoding中，根据encoding可获得str的长度，加上str长度，在加上len长度(根据encoding+strlen计算得到)，就是当前字节的长度，加上就可跳至下一节点；
	listpack如何实现向前遍历：（代码所在：lpPrev()）
		<更改编码原因>		  listpack不同于ziplist，ziplist保存着前一个节点的内容长度，因此可能会造成连锁更新，
		<出现的问题>  	   	  因此listpack不保存前一个节点的长度，而只保存当前节点的长度，由于只保存当前节点长度且保存该长度的字节数不确定，因此无法定位到保存长度的起始点，
		<解决问题>  		  所以listpack使用的varint的编码格式（第一位用来保存内容是否结束，后面7位用来存储数据），因此可以从最后一个字节开始找到保存长度的起始字节(第一位为0的就是起始位置)，也就可以向前遍历了

skiplist（server.h+zset.c）：
	结构：
		zset：
			dict dict：包含着zsl全部元素的ele
			skiplist zsl：保存着全部ele按照score的排名顺序
		skiplist：
			skipListNode header（ZSKIPLIST_MAXLEVEL个），初始状态下指向NULL，后面：第i个指向第一个拥有i的Node；
			skipListNode tail（初始状态为NULL，后面指向sl的最后一个元素（与level->backward合作，实现向前遍历）
			length：zsl的元素个数
			level：zsl的最高level

		skipListNode：
			ele：实际元素内容
			score：实际元素分数
			skipListNode backward：该节点前面的那个元素
			levels（level个）：
				skipListNode forward：指向该层下一个元素（最后指向NULL，与header类似）
				span：距离下一个元素的跨度
	随机level法：
		如果每次插入都维护每层的跨度一样，那么每次插入都要更新每一层后面的所有元素，时间复杂度很高，不太理想；
		根据每层之间元素数的比例（3:2:1=4:2:1），随机产生level，最后形成的skiplist的状态也会符合（3:2:1=4:2:1），遍历时时间复杂度也是Onlog2n，且插入时间复杂度仅为On！，达到了要求；
	如何插入：
		先找出每一层的待插入元素前面那个元素，且记录跨度之和
		之后将待插入元素创建出来并插入到对应位置，依据 前面记录的前面跨度之和与后面元素的跨度与总长度 算出距离后面元素的跨度，更新插入元素的跨度与后面元素的跨度
		最后更新x->backard/zsl->lenth/zsl->tail

三。《数据结构类型》

object.c（redis对象类型）：
&	位域：以bit分配内存的结构体，域内的变量大小以bit为单位，并且不能跨越两个字节（一个变量最大为8bit，若要从下一个字节开始，可以使用空变量名进行填充）；
		格式：	struct xx {				// 一个名为xx的位域，其中包含abc三个变量
					int a:8;		// 占8bit
					int b:2;		// 占2bit
					int c:6;		// 占6bit
				};
&	do {}while(0); 的作用：
		1.使用在高级的宏定义中，防止引用出错，增强代码的健壮性（使用宏引用两个函数，不使用{}可能会导致引用失败，使用{}：因为习惯加;导致储存，因此使用do{}while(0)代替函数，可以将宏函数当作普通函数使用；
		2.减少重复代码，且不需用到goto：当有很多重复代码时，可使用goto解决，也可以使用do{}while(0);, 将重复代码放在while之后，要调用时级打破循环，以达到goto的效果；



L993

t_string.c（字符串键的实现）：《√》
*	string保存方式：（内容小于44字节：embstr；内容大于44字节：raw）
		raw：robj的ptr中保存sds字符串的地址；
		embstr：sds字符串直接内嵌在robj后面；
*	区分原因：
		cpu一次从缓存中读取64字节的内存，robj中使用了16字节，sds中使用了4字节，还剩44字节的空余，如果内容小于44字节的话，可以内嵌在robj中，这样就可以一次性读取出要用的数据，不用再次访问缓存

	
	


t_list.c（列表键的实现）：《√》
*	list保存方式：（list的数据量大于阈值：quicklist；list数据量小于阈值：ziplist/listpack）
		quicklist：一条quicklistNode链表，一个quicklist保存了链表的头节点与尾节点（便于顺序与倒序遍历），quicklistNode中保存了ziplist/listpack的地址（就相当于多条ziplist的集合）
				可以对中间的Node进行压缩：因为访问list的时候一般都是访问两端的数据，因此中间的数据可以进行压缩（链表的节点在内存中分布不均匀，会产生很多内存碎片，压缩可以集中这些碎片，以便于回收）
											通过设置deep来进行压缩设置：-1：仅不压缩头尾节点（×√√√√√×），-2：仅不压缩头尾两个节点（××√√√××）……以此类推
											通过设置max_len决定ziplist数量超过多少才进行分链
		ziplist（前）：头部保存着 总字节数+尾部的地址偏移量+数据数据 +entrys+ 结束标志；entry中保存着： 前一个内容的字节数(可用1/5字节表示：因此会造成连锁更新)+当前内容的字节数+内容
				可顺序遍历：从头部开始，每次增加当前内容长度；						也可倒序遍历：头部+尾部的偏移量=尾部地址， 每次减去前一个内容的长度就可访问到前一个内容；
		listpack（后）：解决了ziplist连锁更新的问题
*	区分原因：
		ziplist每次在中间增加或删除元素的时候，都要将后面全部的内容向后/前移动，当内容很多的时候就会十分消耗时间，因此要把ziplist的长度固定在一个区间内；
		因此当ziplist的数据量达到某个阈值时，就会改变储存形式为quicklist（quicklist的链数缩减为一时又会变成ziplist），因此可以保证ziplist的数据量不会太大一保证性能；
		
	quicklist与listpack互转：
		quicklist只剩一条链的时候，将退化成listpack；listpack的剩余空间不够接下来要插入的元素时，要进化成quicklist才足以符合要求；
		listTypeTryConversionRaw()函数


t_hash.c（哈希键的实现）：
	
*	hash保存方式：
		ziplist：
			将hash的key与value分开变成两个对象，一次保存在ziplist中（key1--value1--key2--value2--....keyn--valuen）
		hashtable（dict）：
			有两个hashtable（当容量改变的时候进行rehash用的），通过key进行hash函数进行映射在hashtable中（hashtable的容量始终为2^n，可以通过（x & (2^n)-1）来取余数），将key与value保存在dickEntry中，
			并且dickEntry首尾相连新形成链表放在hashtable的对应的节点上（每次rehashing会rehash一个节点上面链表的所有键值对）
	区分原因：
		ziplist在数据量多的时候性能很差，需改为hashtable


*	.c：L248：如果对象中保存的是指针内容的话，当替换或删除时，要释放掉原来的内容，才设置新的内容，否则会造成内存泄漏

t_set.c（集合键的实现）：《√》
*	set保存方式：
		intset：（数据全是数字且数据量小于阈值时使用）
			包含： encoding(每个数据占用了多少内存)+length(元素个数)+数据；
			每个数据使用的空间是固定的，以内容需要的最大的内存决定，若需要添加一个内存比现有内存还大的数据，则需全部扩容后再添加元素；
			intset是一个有序的整数集合，因此查找时可以用二分查找
			因为每次插入元素都需要扩容，因此只适合储存数据少的场景
		hashtable（dict）：
			使用key存储内容，value为空，其余就是hashtable的使用；
	区分原因：
		intset只适合少数据与全部整数的场景；


t_zset.c（有序集合键的实现）：
*	zset保存方式：
		ziplist：与存储hash时类似；
		skiplist（前）：
		listpack（后）：
	区分原因：
		zilist只适合少数据；

主要框架：
	server.h/c：
&		UNUSED(d)：外界传入了某个参数，但是我们用不上，不用又会报警告，因此用UNUSED宏来“使用”一次这个参数
&		atomic原理：（atomicvar.c:L140-142）
			#define atomicSet(var,value) do { \
			    while(!__sync_bool_compare_and_swap(&var,var,value)); \
			} while(0)
&		结构体初始化/赋值方式：（server.c：L441-449）
			dictType setDictType = {																							// set hashType：key：sds		value：无value，只用key（例如set使用的就是无value的dict——
			    dictSdsHash,             
			    NULL,                    
			    NULL,                    
			    dictSdsKeyCompare,       
			    dictSdsDestructor,       
			    .no_value = 1,           
			    .keys_are_odd = 1        
			};
& 		缓存驱逐算法：
			LRU：最近未使用删除：
				原理：链表长度固定，将使用过的放在链表头部，要删除时删除链表尾部的数据；
				使用：
					add：若链表中没有该节点的话：删除链表尾部数据，将该节点添加至链表头部；已有该节点的话：将该节点移到头部（先删除在插入到头部）；
				优化：
					要查找队列中是否含有该节点，可以使用map<节点数据,节点地址>，以快速找到是否含有该数据与对应的节点地址；
				缺点：
					常用的数据可能被后面要使用的数据挤出队列；
			CLOCK：时间轮轮转：（LRU的升级版）
				原理：给每个节点添加一个ref参数，访问之后ref置true，要删除时，轮转一圈，将ref为true的置为false，将ref为false的删除
				使用：
					add：若已有该节点：将ref置为true；若没有该节点则插入：若还有空位置：直接插入节点；没有空位置：执行clock轮转函数，删除一个节点后插入节点；
				优化： (与lru类似)
				缺点：（与lru类似）
			LRF：最不常使用删除：
				原理：维护一个二维链表，一维标识节点的使用频率，频率高的在链表尾部，要删除时就从频率低的链表中以lru算法选出要删除的节点（结合了lru算法）
				使用：
					add：若已有节点，则将该节点从当前链表移到频率高一的链表中；若没有节点：则将节点插入到频率最低的链表尾部；
				优化：使用map查找节点位置；
				缺点：
					可能会将刚插入的节点删除（，由于内存的局部性原理，可能马上又会使用到该内存，原来经常使用的节点要通过很多次的迭代才会被删除）
&		CPU有关知识！！：
			setcpuaffinity.c--L73-setcpuaffinity()
&		设置当前计算机的位数
			server.arch_bits = (sizeof(long) == 8) ? 64 : 32;							// 当前计算机的位数
&hs		函数：int access(const char* pathname, int mode)：
			作用：判断pathname是否存在/有某种权限
			mode：
				F_OK 值为0，判断文件是否存在
				X_OK 值为1，判断对文件是可执行权限
				W_OK 值为2，判断对文件是否有写权限
				R_OK 值为4，判断对文件是否有读权限
				后三种可以使用或“|”的方式，一起使用，如W_OK|R_OK
			返回值：
				符合mode条件：返回0，不满足：返回-1
&hs		函数：		int getrlimit(int resource, struct rlimit *rlim);
				int setrlimit(int resource, const struct rlimit *rlim);	 
			作用：获取或设置资源使用限制
&hs 	函数：	int sysctl(const int *name,u_int namelen,void *oldp,size_t *oldlenp,const void *newp,size_t newlen);
			作用：检索系统信息和允许适当的进程设置系统信息
			怎么样我也不知（网上很少）
&hs		函数：	int pthread_setcancelstate(int state,   int *oldstate);
			作用：设置当前线程收到CANCEL信号时的反应；
			state：
				PTHREAD_CANCEL_ENABLE （缺省）：收到后取消该线程
				PTHREAD_CANCEL_DISABLE 	： 忽略该信号
		函数：int pthread_setcanceltype(type, old_type);
			作用：设置当前线程的销毁方式
			type：
				PTHREAD_CANCEL_ASYNCHRONOUS, NULL); //异步取消、 
            	pthread_setcanceltype (PTHREAD_CANCEL_DEFERRED, NULL); //同步取消、 
            	pthread_setcanceltype (PTHREAD_CANCEL_DISABLE
		
		

			


	networking.c
&		L4236-L4251：I/O线程池的实现


