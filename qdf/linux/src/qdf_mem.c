/*
 * Copyright (c) 2014-2017 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/**
 * DOC: qdf_mem
 * This file provides OS dependent memory management APIs
 */

#include "qdf_debugfs.h"
#include "qdf_mem.h"
#include "qdf_nbuf.h"
#include "qdf_lock.h"
#include "qdf_mc_timer.h"
#include "qdf_module.h"
#include <qdf_trace.h>
#include "qdf_atomic.h"
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#ifdef CONFIG_MCL
#include <host_diag_core_event.h>
#else
#define host_log_low_resource_failure(code) do {} while (0)
#endif

#if defined(CONFIG_CNSS)
#include <net/cnss.h>
#endif

#ifdef CONFIG_WCNSS_MEM_PRE_ALLOC
#include <net/cnss_prealloc.h>
#endif

/* Preprocessor Definitions and Constants */
#define QDF_MEM_MAX_MALLOC (1024 * 1024) /* 1MiB */
#define QDF_MEM_WARN_THRESHOLD 300 /* ms */

static enum qdf_mem_domain qdf_mem_current_domain = QDF_MEM_DOMAIN_INIT;

enum qdf_mem_domain qdf_mem_get_domain(void)
{
	return qdf_mem_current_domain;
}

void qdf_mem_set_domain(enum qdf_mem_domain domain)
{
	QDF_BUG(domain >= QDF_MEM_DOMAIN_INIT);
	if (domain < QDF_MEM_DOMAIN_INIT)
		return;

	QDF_BUG(domain < QDF_MEM_DOMAIN_MAX_COUNT);
	if (domain >= QDF_MEM_DOMAIN_MAX_COUNT)
		return;

	qdf_mem_current_domain = domain;
}

const uint8_t *qdf_mem_domain_name(enum qdf_mem_domain domain)
{
	switch (domain) {
	case QDF_MEM_DOMAIN_INIT:
		return "Init";
	case QDF_MEM_DOMAIN_ACTIVE:
		return "Active";
	default:
		return "Invalid";
	}
}

#ifdef MEMORY_DEBUG
#include <qdf_list.h>

static qdf_list_t qdf_mem_domains[QDF_MEM_DOMAIN_MAX_COUNT];
static qdf_spinlock_t qdf_mem_list_lock;

static qdf_list_t qdf_mem_dma_domains[QDF_MEM_DOMAIN_MAX_COUNT];
static qdf_spinlock_t qdf_mem_dma_list_lock;

static uint64_t WLAN_MEM_HEADER = 0x6162636465666768;
static uint64_t WLAN_MEM_TRAILER = 0x8081828384858687;

static inline qdf_list_t *qdf_mem_list(enum qdf_mem_domain domain)
{
	if (domain < QDF_MEM_DOMAIN_INIT || domain >= QDF_MEM_DOMAIN_MAX_COUNT)
		return NULL;

	return &qdf_mem_domains[domain];
}

static inline qdf_list_t *qdf_mem_dma_list(enum qdf_mem_domain domain)
{
	if (domain < QDF_MEM_DOMAIN_INIT || domain >= QDF_MEM_DOMAIN_MAX_COUNT)
		return NULL;

	return &qdf_mem_dma_domains[domain];
}

static inline qdf_list_t *qdf_mem_active_list(void)
{
	return &qdf_mem_domains[qdf_mem_get_domain()];
}

static inline qdf_list_t *qdf_mem_dma_active_list(void)
{
	return &qdf_mem_dma_domains[qdf_mem_get_domain()];
}

/**
 * struct qdf_mem_header - memory object to dubug
 * @node: node to the list
 * @domain: the active memory domain at time of allocation
 * @freed: flag set during free, used to detect double frees
 *	Use uint8_t so we can detect corruption
 * @file: name of the file the allocation was made from
 * @line: line number of the file the allocation was made from
 * @size: size of the allocation in bytes
 * @header: a known value, used to detect out-of-bounds access
 */
struct qdf_mem_header {
	qdf_list_node_t node;
	enum qdf_mem_domain domain;
	uint8_t freed;
	const char *file;
	uint32_t line;
	uint32_t size;
	uint64_t header;
};

static inline struct qdf_mem_header *qdf_mem_get_header(void *ptr)
{
	return (struct qdf_mem_header *)ptr - 1;
}

static inline uint64_t *qdf_mem_get_trailer(struct qdf_mem_header *header)
{
	return (uint64_t *)((void *)(header + 1) + header->size);
}

static inline void *qdf_mem_get_ptr(struct qdf_mem_header *header)
{
	return (void *)(header + 1);
}

/* number of bytes needed for the qdf memory debug information */
#define QDF_MEM_DEBUG_SIZE \
	(sizeof(struct qdf_mem_header) + sizeof(WLAN_MEM_TRAILER))

static void qdf_mem_header_init(struct qdf_mem_header *header, qdf_size_t size,
				const char *file, uint32_t line)
{
	QDF_BUG(header);
	if (!header)
		return;

	header->domain = qdf_mem_get_domain();
	header->freed = false;
	header->file = file;
	header->line = line;
	header->size = size;
	header->header = WLAN_MEM_HEADER;
	*qdf_mem_get_trailer(header) = WLAN_MEM_TRAILER;
}

enum qdf_mem_validation_bitmap {
	QDF_MEM_BAD_HEADER = 1 << 0,
	QDF_MEM_BAD_TRAILER = 1 << 1,
	QDF_MEM_BAD_SIZE = 1 << 2,
	QDF_MEM_DOUBLE_FREE = 1 << 3,
	QDF_MEM_BAD_FREED = 1 << 4,
	QDF_MEM_BAD_NODE = 1 << 5,
	QDF_MEM_BAD_DOMAIN = 1 << 6,
	QDF_MEM_WRONG_DOMAIN = 1 << 7,
};

/**
 * qdf_mem_validate_list_node() - validate that the node is in a list
 * @qdf_node: node to check for being in a list
 *
 * Return: true if the node validly linked in an anchored doubly linked list
 */
static bool qdf_mem_validate_list_node(qdf_list_node_t *qdf_node)
{
	struct list_head *node = qdf_node;

	/*
	 * if the node is an empty list, it is not tied to an anchor node
	 * and must have been removed with list_del_init
	 */
	if (list_empty(node))
		return false;

	if (!node->prev || !node->next)
		return false;

	if (node->prev->next != node || node->next->prev != node)
		return false;

	return true;
}

static enum qdf_mem_validation_bitmap
qdf_mem_header_validate(struct qdf_mem_header *header,
			enum qdf_mem_domain domain)
{
	enum qdf_mem_validation_bitmap error_bitmap = 0;

	if (header->header != WLAN_MEM_HEADER)
		error_bitmap |= QDF_MEM_BAD_HEADER;

	if (header->size > QDF_MEM_MAX_MALLOC)
		error_bitmap |= QDF_MEM_BAD_SIZE;
	else if (*qdf_mem_get_trailer(header) != WLAN_MEM_TRAILER)
		error_bitmap |= QDF_MEM_BAD_TRAILER;

	if (header->freed == true)
		error_bitmap |= QDF_MEM_DOUBLE_FREE;
	else if (header->freed)
		error_bitmap |= QDF_MEM_BAD_FREED;

	if (!qdf_mem_validate_list_node(&header->node))
		error_bitmap |= QDF_MEM_BAD_NODE;

	if (header->domain < QDF_MEM_DOMAIN_INIT ||
	    header->domain >= QDF_MEM_DOMAIN_MAX_COUNT)
		error_bitmap |= QDF_MEM_BAD_DOMAIN;
	else if (header->domain != domain)
		error_bitmap |= QDF_MEM_WRONG_DOMAIN;

	return error_bitmap;
}

static void
qdf_mem_header_assert_valid(struct qdf_mem_header *header,
			    enum qdf_mem_domain current_domain,
			    enum qdf_mem_validation_bitmap error_bitmap,
			    const char *file,
			    uint32_t line)
{
	if (!error_bitmap)
		return;

	if (error_bitmap & QDF_MEM_BAD_HEADER)
		qdf_err("Corrupted memory header 0x%llx (expected 0x%llx)",
			header->header, WLAN_MEM_HEADER);

	if (error_bitmap & QDF_MEM_BAD_SIZE)
		qdf_err("Corrupted memory size %u (expected < %d)",
			header->size, QDF_MEM_MAX_MALLOC);

	if (error_bitmap & QDF_MEM_BAD_TRAILER)
		qdf_err("Corrupted memory trailer 0x%llx (expected 0x%llx)",
			*qdf_mem_get_trailer(header), WLAN_MEM_TRAILER);

	if (error_bitmap & QDF_MEM_DOUBLE_FREE)
		qdf_err("Memory has previously been freed");

	if (error_bitmap & QDF_MEM_BAD_FREED)
		qdf_err("Corrupted memory freed flag 0x%x", header->freed);

	if (error_bitmap & QDF_MEM_BAD_NODE)
		qdf_err("Corrupted memory header node or double free");

	if (error_bitmap & QDF_MEM_BAD_DOMAIN)
		qdf_err("Corrupted memory domain 0x%x", header->domain);

	if (error_bitmap & QDF_MEM_WRONG_DOMAIN)
		qdf_err("Memory domain mismatch; found %s(%d), expected %s(%d)",
			qdf_mem_domain_name(header->domain), header->domain,
			qdf_mem_domain_name(current_domain), current_domain);

	panic("A fatal memory error was detected @ %s:%d",
	      kbasename(file), line);
}
#endif /* MEMORY_DEBUG */

int qdf_dbg_mask;
qdf_declare_param(qdf_dbg_mask, int);
qdf_export_symbol(qdf_dbg_mask);

u_int8_t prealloc_disabled = 1;
qdf_declare_param(prealloc_disabled, byte);
qdf_export_symbol(prealloc_disabled);

#if defined WLAN_DEBUGFS

/* Debugfs root directory for qdf_mem */
static struct dentry *qdf_mem_debugfs_root;

/**
 * struct __qdf_mem_stat - qdf memory statistics
 * @kmalloc:	total kmalloc allocations
 * @dma:	total dma allocations
 * @skb:	total skb allocations
 */
static struct __qdf_mem_stat {
	qdf_atomic_t kmalloc;
	qdf_atomic_t dma;
	qdf_atomic_t skb;
} qdf_mem_stat;

static inline void qdf_mem_kmalloc_inc(qdf_size_t size)
{
	qdf_atomic_add(size, &qdf_mem_stat.kmalloc);
}

static inline void qdf_mem_dma_inc(qdf_size_t size)
{
	qdf_atomic_add(size, &qdf_mem_stat.dma);
}

void qdf_mem_skb_inc(qdf_size_t size)
{
	qdf_atomic_add(size, &qdf_mem_stat.skb);
}

static inline void qdf_mem_kmalloc_dec(qdf_size_t size)
{
	qdf_atomic_sub(size, &qdf_mem_stat.kmalloc);
}

static inline void qdf_mem_dma_dec(qdf_size_t size)
{
	qdf_atomic_sub(size, &qdf_mem_stat.dma);
}

void qdf_mem_skb_dec(qdf_size_t size)
{
	qdf_atomic_sub(size, &qdf_mem_stat.skb);
}

#ifdef MEMORY_DEBUG
static int qdf_err_printer(void *priv, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	QDF_VTRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR, (char *)fmt, args);
	va_end(args);

	return 0;
}

static int seq_printf_printer(void *priv, const char *fmt, ...)
{
	struct seq_file *file = priv;
	va_list args;

	va_start(args, fmt);
	seq_vprintf(file, fmt, args);
	seq_puts(file, "\n");
	va_end(args);

	return 0;
}

/**
 * struct __qdf_mem_info - memory statistics
 * @file: the file which allocated memory
 * @line: the line at which allocation happened
 * @size: the size of allocation
 * @count: how many allocations of same type
 *
 */
struct __qdf_mem_info {
	const char *file;
	uint32_t line;
	uint32_t size;
	uint32_t count;
};

/*
 * The table depth defines the de-duplication proximity scope.
 * A deeper table takes more time, so choose any optimum value.
 */
#define QDF_MEM_STAT_TABLE_SIZE 8

/**
 * qdf_mem_domain_print_header() - memory domain header print logic
 * @print: the print adapter function
 * @print_priv: the private data to be consumed by @print
 *
 * Return: None
 */
static void qdf_mem_domain_print_header(qdf_abstract_print print,
					void *print_priv)
{
	print(print_priv,
	      "--------------------------------------------------------------");
	print(print_priv, " count    size     total    filename");
	print(print_priv,
	      "--------------------------------------------------------------");
}

/**
 * qdf_mem_meta_table_print() - memory metadata table print logic
 * @table: the memory metadata table to print
 * @print: the print adapter function
 * @print_priv: the private data to be consumed by @print
 *
 * Return: None
 */
static void qdf_mem_meta_table_print(struct __qdf_mem_info *table,
				     qdf_abstract_print print,
				     void *print_priv)
{
	int i;

	for (i = 0; i < QDF_MEM_STAT_TABLE_SIZE; i++) {
		if (!table[i].count)
			break;

		print(print_priv,
		      "%6u x %5u = %7uB @ %s:%u",
		      table[i].count,
		      table[i].size,
		      table[i].count * table[i].size,
		      kbasename(table[i].file),
		      table[i].line);
	}
}

/**
 * qdf_mem_meta_table_insert() - insert memory metadata into the given table
 * @table: the memory metadata table to insert into
 * @meta: the memory metadata to insert
 *
 * Return: true if the table is full after inserting, false otherwise
 */
static bool qdf_mem_meta_table_insert(struct __qdf_mem_info *table,
				      struct qdf_mem_header *meta)
{
	int i;

	for (i = 0; i < QDF_MEM_STAT_TABLE_SIZE; i++) {
		if (!table[i].count) {
			table[i].file = meta->file;
			table[i].line = meta->line;
			table[i].size = meta->size;
			table[i].count = 1;
			break;
		}

		if (table[i].file == meta->file &&
		    table[i].line == meta->line &&
		    table[i].size == meta->size) {
			table[i].count++;
			break;
		}
	}

	/* return true if the table is now full */
	return i >= QDF_MEM_STAT_TABLE_SIZE - 1;
}

/**
 * qdf_mem_domain_print() - output agnostic memory domain print logic
 * @domain: the memory domain to print
 * @print: the print adapter function
 * @print_priv: the private data to be consumed by @print
 *
 * Return: None
 */
static void qdf_mem_domain_print(qdf_list_t *domain,
				 qdf_abstract_print print,
				 void *print_priv)
{
	QDF_STATUS status;
	struct __qdf_mem_info table[QDF_MEM_STAT_TABLE_SIZE];
	qdf_list_node_t *node;

	qdf_mem_zero(table, sizeof(table));
	qdf_mem_domain_print_header(print, print_priv);

	/* hold lock while inserting to avoid use-after free of the metadata */
	qdf_spin_lock(&qdf_mem_list_lock);
	status = qdf_list_peek_front(domain, &node);
	while (QDF_IS_STATUS_SUCCESS(status)) {
		struct qdf_mem_header *meta = (struct qdf_mem_header *)node;
		bool is_full = qdf_mem_meta_table_insert(table, meta);

		qdf_spin_unlock(&qdf_mem_list_lock);

		if (is_full) {
			qdf_mem_meta_table_print(table, print, print_priv);
			qdf_mem_zero(table, sizeof(table));
		}

		qdf_spin_lock(&qdf_mem_list_lock);
		status = qdf_list_peek_next(domain, node, &node);
	}
	qdf_spin_unlock(&qdf_mem_list_lock);

	qdf_mem_meta_table_print(table, print, print_priv);
}

/**
 * qdf_mem_seq_start() - sequential callback to start
 * @seq: seq_file handle
 * @pos: The start position of the sequence
 *
 * Return: iterator pointer, or NULL if iteration is complete
 */
static void *qdf_mem_seq_start(struct seq_file *seq, loff_t *pos)
{
	if (*pos < 0 || *pos >= QDF_MEM_DOMAIN_MAX_COUNT)
		return NULL;

	/* just use the current position as our iterator */
	return pos;
}

/**
 * qdf_mem_seq_next() - next sequential callback
 * @seq: seq_file handle
 * @v: the current iterator
 * @pos: the current position
 *
 * Get the next node and release previous node.
 *
 * Return: iterator pointer, or NULL if iteration is complete
 */
static void *qdf_mem_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	++*pos;

	return qdf_mem_seq_start(seq, pos);
}

/**
 * qdf_mem_seq_stop() - stop sequential callback
 * @seq: seq_file handle
 * @v: current iterator
 *
 * Return: None
 */
static void qdf_mem_seq_stop(struct seq_file *seq, void *v) { }

/**
 * qdf_mem_seq_show() - print sequential callback
 * @seq: seq_file handle
 * @v: current iterator
 *
 * Return: 0 - success
 */
static int qdf_mem_seq_show(struct seq_file *seq, void *v)
{
	enum qdf_mem_domain domain_id = *(enum qdf_mem_domain *)v;

	seq_printf(seq, "\n%s Memory Domain (Id %d)\n",
		   qdf_mem_domain_name(domain_id), domain_id);
	qdf_mem_domain_print(&qdf_mem_domains[domain_id],
			     seq_printf_printer, seq);

	return 0;
}

/* sequential file operation table */
static const struct seq_operations qdf_mem_seq_ops = {
	.start = qdf_mem_seq_start,
	.next  = qdf_mem_seq_next,
	.stop  = qdf_mem_seq_stop,
	.show  = qdf_mem_seq_show,
};


static int qdf_mem_debugfs_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &qdf_mem_seq_ops);
}

/* debugfs file operation table */
static const struct file_operations fops_qdf_mem_debugfs = {
	.owner = THIS_MODULE,
	.open = qdf_mem_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static QDF_STATUS qdf_mem_debug_debugfs_init(void)
{
	if (!qdf_mem_debugfs_root)
		return QDF_STATUS_E_FAILURE;

	debugfs_create_file("list",
			    S_IRUSR,
			    qdf_mem_debugfs_root,
			    NULL,
			    &fops_qdf_mem_debugfs);

	return QDF_STATUS_SUCCESS;
}

static QDF_STATUS qdf_mem_debug_debugfs_exit(void)
{
	return QDF_STATUS_SUCCESS;
}

#else /* MEMORY_DEBUG */

static QDF_STATUS qdf_mem_debug_debugfs_init(void)
{
	return QDF_STATUS_E_NOSUPPORT;
}

static QDF_STATUS qdf_mem_debug_debugfs_exit(void)
{
	return QDF_STATUS_E_NOSUPPORT;
}

#endif /* MEMORY_DEBUG */


static void qdf_mem_debugfs_exit(void)
{
	debugfs_remove_recursive(qdf_mem_debugfs_root);
	qdf_mem_debugfs_root = NULL;
}

static QDF_STATUS qdf_mem_debugfs_init(void)
{
	struct dentry *qdf_debugfs_root = qdf_debugfs_get_root();

	if (!qdf_debugfs_root)
		return QDF_STATUS_E_FAILURE;

	qdf_mem_debugfs_root = debugfs_create_dir("mem", qdf_debugfs_root);

	if (!qdf_mem_debugfs_root)
		return QDF_STATUS_E_FAILURE;


	debugfs_create_atomic_t("kmalloc",
				S_IRUSR,
				qdf_mem_debugfs_root,
				&qdf_mem_stat.kmalloc);

	debugfs_create_atomic_t("dma",
				S_IRUSR,
				qdf_mem_debugfs_root,
				&qdf_mem_stat.dma);

	debugfs_create_atomic_t("skb",
				S_IRUSR,
				qdf_mem_debugfs_root,
				&qdf_mem_stat.skb);

	return QDF_STATUS_SUCCESS;
}

#else /* WLAN_DEBUGFS */

static inline void qdf_mem_kmalloc_inc(qdf_size_t size) {}
static inline void qdf_mem_dma_inc(qdf_size_t size) {}
static inline void qdf_mem_kmalloc_dec(qdf_size_t size) {}
static inline void qdf_mem_dma_dec(qdf_size_t size) {}


static QDF_STATUS qdf_mem_debugfs_init(void)
{
	return QDF_STATUS_E_NOSUPPORT;
}
static void qdf_mem_debugfs_exit(void) {}


static QDF_STATUS qdf_mem_debug_debugfs_init(void)
{
	return QDF_STATUS_E_NOSUPPORT;
}

static QDF_STATUS qdf_mem_debug_debugfs_exit(void)
{
	return QDF_STATUS_E_NOSUPPORT;
}

#endif /* WLAN_DEBUGFS */

/**
 * __qdf_mempool_init() - Create and initialize memory pool
 *
 * @osdev: platform device object
 * @pool_addr: address of the pool created
 * @elem_cnt: no. of elements in pool
 * @elem_size: size of each pool element in bytes
 * @flags: flags
 *
 * return: Handle to memory pool or NULL if allocation failed
 */
int __qdf_mempool_init(qdf_device_t osdev, __qdf_mempool_t *pool_addr,
		       int elem_cnt, size_t elem_size, u_int32_t flags)
{
	__qdf_mempool_ctxt_t *new_pool = NULL;
	u_int32_t align = L1_CACHE_BYTES;
	unsigned long aligned_pool_mem;
	int pool_id;
	int i;

	if (prealloc_disabled) {
		/* TBD: We can maintain a list of pools in qdf_device_t
		 * to help debugging
		 * when pre-allocation is not enabled
		 */
		new_pool = (__qdf_mempool_ctxt_t *)
			kmalloc(sizeof(__qdf_mempool_ctxt_t), GFP_KERNEL);
		if (new_pool == NULL)
			return QDF_STATUS_E_NOMEM;

		memset(new_pool, 0, sizeof(*new_pool));
		/* TBD: define flags for zeroing buffers etc */
		new_pool->flags = flags;
		new_pool->elem_size = elem_size;
		new_pool->max_elem = elem_cnt;
		*pool_addr = new_pool;
		return 0;
	}

	for (pool_id = 0; pool_id < MAX_MEM_POOLS; pool_id++) {
		if (osdev->mem_pool[pool_id] == NULL)
			break;
	}

	if (pool_id == MAX_MEM_POOLS)
		return -ENOMEM;

	new_pool = osdev->mem_pool[pool_id] = (__qdf_mempool_ctxt_t *)
		kmalloc(sizeof(__qdf_mempool_ctxt_t), GFP_KERNEL);
	if (new_pool == NULL)
		return -ENOMEM;

	memset(new_pool, 0, sizeof(*new_pool));
	/* TBD: define flags for zeroing buffers etc */
	new_pool->flags = flags;
	new_pool->pool_id = pool_id;

	/* Round up the element size to cacheline */
	new_pool->elem_size = roundup(elem_size, L1_CACHE_BYTES);
	new_pool->mem_size = elem_cnt * new_pool->elem_size +
				((align)?(align - 1):0);

	new_pool->pool_mem = kzalloc(new_pool->mem_size, GFP_KERNEL);
	if (new_pool->pool_mem == NULL) {
			/* TBD: Check if we need get_free_pages above */
		kfree(new_pool);
		osdev->mem_pool[pool_id] = NULL;
		return -ENOMEM;
	}

	spin_lock_init(&new_pool->lock);

	/* Initialize free list */
	aligned_pool_mem = (unsigned long)(new_pool->pool_mem) +
			((align) ? (unsigned long)(new_pool->pool_mem)%align:0);
	STAILQ_INIT(&new_pool->free_list);

	for (i = 0; i < elem_cnt; i++)
		STAILQ_INSERT_TAIL(&(new_pool->free_list),
			(mempool_elem_t *)(aligned_pool_mem +
			(new_pool->elem_size * i)), mempool_entry);


	new_pool->free_cnt = elem_cnt;
	*pool_addr = new_pool;
	return 0;
}
qdf_export_symbol(__qdf_mempool_init);

/**
 * __qdf_mempool_destroy() - Destroy memory pool
 * @osdev: platform device object
 * @Handle: to memory pool
 *
 * Returns: none
 */
void __qdf_mempool_destroy(qdf_device_t osdev, __qdf_mempool_t pool)
{
	int pool_id = 0;

	if (!pool)
		return;

	if (prealloc_disabled) {
		kfree(pool);
		return;
	}

	pool_id = pool->pool_id;

	/* TBD: Check if free count matches elem_cnt if debug is enabled */
	kfree(pool->pool_mem);
	kfree(pool);
	osdev->mem_pool[pool_id] = NULL;
}
qdf_export_symbol(__qdf_mempool_destroy);

/**
 * __qdf_mempool_alloc() - Allocate an element memory pool
 *
 * @osdev: platform device object
 * @Handle: to memory pool
 *
 * Return: Pointer to the allocated element or NULL if the pool is empty
 */
void *__qdf_mempool_alloc(qdf_device_t osdev, __qdf_mempool_t pool)
{
	void *buf = NULL;

	if (!pool)
		return NULL;

	if (prealloc_disabled)
		return  qdf_mem_malloc(pool->elem_size);

	spin_lock_bh(&pool->lock);

	buf = STAILQ_FIRST(&pool->free_list);
	if (buf != NULL) {
		STAILQ_REMOVE_HEAD(&pool->free_list, mempool_entry);
		pool->free_cnt--;
	}

	/* TBD: Update free count if debug is enabled */
	spin_unlock_bh(&pool->lock);

	return buf;
}
qdf_export_symbol(__qdf_mempool_alloc);

/**
 * __qdf_mempool_free() - Free a memory pool element
 * @osdev: Platform device object
 * @pool: Handle to memory pool
 * @buf: Element to be freed
 *
 * Returns: none
 */
void __qdf_mempool_free(qdf_device_t osdev, __qdf_mempool_t pool, void *buf)
{
	if (!pool)
		return;


	if (prealloc_disabled)
		return qdf_mem_free(buf);

	spin_lock_bh(&pool->lock);
	pool->free_cnt++;

	STAILQ_INSERT_TAIL
		(&pool->free_list, (mempool_elem_t *)buf, mempool_entry);
	spin_unlock_bh(&pool->lock);
}
qdf_export_symbol(__qdf_mempool_free);

/**
 * qdf_mem_alloc_outline() - allocation QDF memory
 * @osdev: platform device object
 * @size: Number of bytes of memory to allocate.
 *
 * This function will dynamicallly allocate the specified number of bytes of
 * memory.
 *
 * Return:
 * Upon successful allocate, returns a non-NULL pointer to the allocated
 * memory.  If this function is unable to allocate the amount of memory
 * specified (for any reason) it returns NULL.
 */
void *
qdf_mem_alloc_outline(qdf_device_t osdev, size_t size)
{
	return qdf_mem_malloc(size);
}
qdf_export_symbol(qdf_mem_alloc_outline);

/**
 * qdf_mem_free_outline() - QDF memory free API
 * @ptr: Pointer to the starting address of the memory to be free'd.
 *
 * This function will free the memory pointed to by 'ptr'. It also checks
 * is memory is corrupted or getting double freed and panic.
 *
 * Return: none
 */
void
qdf_mem_free_outline(void *buf)
{
	qdf_mem_free(buf);
}
qdf_export_symbol(qdf_mem_free_outline);

/**
 * qdf_mem_zero_outline() - zero out memory
 * @buf: pointer to memory that will be set to zero
 * @size: number of bytes zero
 *
 * This function sets the memory location to all zeros, essentially clearing
 * the memory.
 *
 * Return: none
 */
void
qdf_mem_zero_outline(void *buf, qdf_size_t size)
{
	qdf_mem_zero(buf, size);
}
qdf_export_symbol(qdf_mem_zero_outline);

#ifdef CONFIG_WCNSS_MEM_PRE_ALLOC
/**
 * qdf_mem_prealloc_get() - conditionally pre-allocate memory
 * @size: the number of bytes to allocate
 *
 * If size if greater than WCNSS_PRE_ALLOC_GET_THRESHOLD, this function returns
 * a chunk of pre-allocated memory. If size if less than or equal to
 * WCNSS_PRE_ALLOC_GET_THRESHOLD, or an error occurs, NULL is returned instead.
 *
 * Return: NULL on failure, non-NULL on success
 */
static void *qdf_mem_prealloc_get(size_t size)
{
	void *ptr;

	if (size <= WCNSS_PRE_ALLOC_GET_THRESHOLD)
		return NULL;

	ptr = wcnss_prealloc_get(size);
	if (!ptr)
		return NULL;

	memset(ptr, 0, size);

	return ptr;
}

static inline bool qdf_mem_prealloc_put(void *ptr)
{
	return wcnss_prealloc_put(ptr);
}
#else
static inline void *qdf_mem_prealloc_get(size_t size)
{
	return NULL;
}

static inline bool qdf_mem_prealloc_put(void *ptr)
{
	return false;
}
#endif /* CONFIG_WCNSS_MEM_PRE_ALLOC */

static int qdf_mem_malloc_flags(void)
{
	if (in_interrupt() || irqs_disabled() || in_atomic())
		return GFP_ATOMIC;

	return GFP_KERNEL;
}

/* External Function implementation */
#ifdef MEMORY_DEBUG

/**
 * qdf_mem_debug_init() - initialize qdf memory debug functionality
 *
 * Return: none
 */
static void qdf_mem_debug_init(void)
{
	int i;

	qdf_mem_current_domain = QDF_MEM_DOMAIN_INIT;

	/* mem */
	for (i = 0; i < QDF_MEM_DOMAIN_MAX_COUNT; ++i)
		qdf_list_create(&qdf_mem_domains[i], 60000);
	qdf_spinlock_create(&qdf_mem_list_lock);

	/* dma */
	for (i = 0; i < QDF_MEM_DOMAIN_MAX_COUNT; ++i)
		qdf_list_create(&qdf_mem_dma_domains[i], 0);
	qdf_spinlock_create(&qdf_mem_dma_list_lock);

	/* skb */
	qdf_net_buf_debug_init();
}

static uint32_t
qdf_mem_domain_check_for_leaks(enum qdf_mem_domain domain, qdf_list_t *mem_list)
{
	if (qdf_list_empty(mem_list))
		return 0;

	qdf_err("Memory leaks detected in %s domain!",
		qdf_mem_domain_name(domain));
	qdf_mem_domain_print(mem_list, qdf_err_printer, NULL);

	return mem_list->count;
}

static void qdf_mem_domain_set_check_for_leaks(qdf_list_t *domains)
{
	uint32_t leak_count = 0;
	int i;

	/* detect and print leaks */
	for (i = 0; i < QDF_MEM_DOMAIN_MAX_COUNT; ++i)
		leak_count += qdf_mem_domain_check_for_leaks(i, domains + i);

	if (leak_count)
		panic("%u fatal memory leaks detected!", leak_count);
}

/**
 * qdf_mem_debug_exit() - exit qdf memory debug functionality
 *
 * Return: none
 */
static void qdf_mem_debug_exit(void)
{
	int i;

	/* skb */
	qdf_net_buf_debug_exit();

	/* mem */
	qdf_mem_domain_set_check_for_leaks(qdf_mem_domains);
	for (i = 0; i < QDF_MEM_DOMAIN_MAX_COUNT; ++i)
		qdf_list_destroy(&qdf_mem_domains[i]);
	qdf_spinlock_destroy(&qdf_mem_list_lock);

	/* dma */
	qdf_mem_domain_set_check_for_leaks(qdf_mem_dma_domains);
	for (i = 0; i < QDF_MEM_DOMAIN_MAX_COUNT; ++i)
		qdf_list_destroy(&qdf_mem_dma_domains[i]);
	qdf_spinlock_destroy(&qdf_mem_dma_list_lock);
}

void *qdf_mem_malloc_debug(size_t size, const char *file, uint32_t line)
{
	QDF_STATUS status;
	qdf_list_t *mem_list = qdf_mem_active_list();
	struct qdf_mem_header *header;
	void *ptr;
	unsigned long start, duration;

	if (!size || size > QDF_MEM_MAX_MALLOC) {
		qdf_err("Cannot malloc %zu bytes @ %s:%d", size, file, line);
		return NULL;
	}

	ptr = qdf_mem_prealloc_get(size);
	if (ptr)
		return ptr;

	start = qdf_mc_timer_get_system_time();
	header = kzalloc(size + QDF_MEM_DEBUG_SIZE, qdf_mem_malloc_flags());
	duration = qdf_mc_timer_get_system_time() - start;

	if (duration > QDF_MEM_WARN_THRESHOLD)
		qdf_warn("Malloc slept; %lums, %zuB @ %s:%d",
			 duration, size, file, line);

	if (!header) {
		qdf_warn("Failed to malloc %zuB @ %s:%d", size, file, line);
		return NULL;
	}

	qdf_mem_header_init(header, size, file, line);
	ptr = qdf_mem_get_ptr(header);

	qdf_spin_lock_irqsave(&qdf_mem_list_lock);
	status = qdf_list_insert_front(mem_list, &header->node);
	qdf_spin_unlock_irqrestore(&qdf_mem_list_lock);
	if (QDF_IS_STATUS_ERROR(status))
		qdf_err("Failed to insert memory header; status %d", status);

	qdf_mem_kmalloc_inc(size);

	return ptr;
}
qdf_export_symbol(qdf_mem_malloc_debug);

/**
 * qdf_mem_free_debug() - QDF memory free API
 * @ptr: Pointer to the starting address of the memory to be free'd.
 *
 * This function will free the memory pointed to by 'ptr'. It also checks
 * is memory is corrupted or getting double freed and panic.
 *
 * Return: none
 */
void qdf_mem_free_debug(void *ptr, const char *file, uint32_t line)
{
	enum qdf_mem_domain domain = qdf_mem_get_domain();
	struct qdf_mem_header *header;
	enum qdf_mem_validation_bitmap error_bitmap;

	/* freeing a null pointer is valid */
	if (qdf_unlikely(!ptr))
		return;

	if (qdf_mem_prealloc_put(ptr))
		return;

	if (qdf_unlikely((qdf_size_t)ptr <= sizeof(*header)))
		panic("Failed to free invalid memory location %pK", ptr);

	qdf_spin_lock_irqsave(&qdf_mem_list_lock);
	header = qdf_mem_get_header(ptr);
	error_bitmap = qdf_mem_header_validate(header, domain);
	if (!error_bitmap) {
		header->freed = true;
		list_del_init(&header->node);
		qdf_mem_list(header->domain)->count--;
	}
	qdf_spin_unlock_irqrestore(&qdf_mem_list_lock);

	qdf_mem_header_assert_valid(header, domain, error_bitmap, file, line);

	qdf_mem_kmalloc_dec(header->size);
	kfree(header);
}
qdf_export_symbol(qdf_mem_free_debug);

void qdf_mem_check_for_leaks(void)
{
	enum qdf_mem_domain domain = qdf_mem_current_domain;
	qdf_list_t *mem_list = &qdf_mem_domains[domain];
	qdf_list_t *dma_list = &qdf_mem_dma_domains[domain];
	uint32_t leaks_count = 0;

	leaks_count += qdf_mem_domain_check_for_leaks(domain, mem_list);
	leaks_count += qdf_mem_domain_check_for_leaks(domain, dma_list);

	if (leaks_count)
		panic("%u fatal memory leaks detected!", leaks_count);
}
#else
static void qdf_mem_debug_init(void) {}

static void qdf_mem_debug_exit(void) {}

/**
 * qdf_mem_malloc() - allocation QDF memory
 * @size: Number of bytes of memory to allocate.
 *
 * This function will dynamicallly allocate the specified number of bytes of
 * memory.
 *
 * Return:
 * Upon successful allocate, returns a non-NULL pointer to the allocated
 * memory.  If this function is unable to allocate the amount of memory
 * specified (for any reason) it returns NULL.
 */
void *qdf_mem_malloc(size_t size)
{
	void *ptr;

	ptr = qdf_mem_prealloc_get(size);
	if (ptr)
		return ptr;

	ptr = kzalloc(size, qdf_mem_malloc_flags());
	if (!ptr)
		return NULL;

	qdf_mem_kmalloc_inc(ksize(ptr));

	return ptr;
}
qdf_export_symbol(qdf_mem_malloc);

/**
 * qdf_mem_free() - free QDF memory
 * @ptr: Pointer to the starting address of the memory to be free'd.
 *
 * This function will free the memory pointed to by 'ptr'.
 *
 * Return: None
 */
void qdf_mem_free(void *ptr)
{
	if (ptr == NULL)
		return;

	if (qdf_mem_prealloc_put(ptr))
		return;

	qdf_mem_kmalloc_dec(ksize(ptr));

	kfree(ptr);
}
qdf_export_symbol(qdf_mem_free);
#endif

/**
 * qdf_mem_multi_pages_alloc() - allocate large size of kernel memory
 * @osdev: OS device handle pointer
 * @pages: Multi page information storage
 * @element_size: Each element size
 * @element_num: Total number of elements should be allocated
 * @memctxt: Memory context
 * @cacheable: Coherent memory or cacheable memory
 *
 * This function will allocate large size of memory over multiple pages.
 * Large size of contiguous memory allocation will fail frequently, then
 * instead of allocate large memory by one shot, allocate through multiple, non
 * contiguous memory and combine pages when actual usage
 *
 * Return: None
 */
void qdf_mem_multi_pages_alloc(qdf_device_t osdev,
			       struct qdf_mem_multi_page_t *pages,
			       size_t element_size, uint16_t element_num,
			       qdf_dma_context_t memctxt, bool cacheable)
{
	uint16_t page_idx;
	struct qdf_mem_dma_page_t *dma_pages;
	void **cacheable_pages = NULL;
	uint16_t i;

	pages->num_element_per_page = PAGE_SIZE / element_size;
	if (!pages->num_element_per_page) {
		qdf_print("Invalid page %d or element size %d",
			  (int)PAGE_SIZE, (int)element_size);
		goto out_fail;
	}

	pages->num_pages = element_num / pages->num_element_per_page;
	if (element_num % pages->num_element_per_page)
		pages->num_pages++;

	if (cacheable) {
		/* Pages information storage */
		pages->cacheable_pages = qdf_mem_malloc(
			pages->num_pages * sizeof(pages->cacheable_pages));
		if (!pages->cacheable_pages) {
			qdf_print("Cacheable page storage alloc fail");
			goto out_fail;
		}

		cacheable_pages = pages->cacheable_pages;
		for (page_idx = 0; page_idx < pages->num_pages; page_idx++) {
			cacheable_pages[page_idx] = qdf_mem_malloc(PAGE_SIZE);
			if (!cacheable_pages[page_idx]) {
				qdf_print("cacheable page alloc fail, pi %d",
					  page_idx);
				goto page_alloc_fail;
			}
		}
		pages->dma_pages = NULL;
	} else {
		pages->dma_pages = qdf_mem_malloc(
			pages->num_pages * sizeof(struct qdf_mem_dma_page_t));
		if (!pages->dma_pages) {
			qdf_print("dmaable page storage alloc fail");
			goto out_fail;
		}

		dma_pages = pages->dma_pages;
		for (page_idx = 0; page_idx < pages->num_pages; page_idx++) {
			dma_pages->page_v_addr_start =
				qdf_mem_alloc_consistent(osdev, osdev->dev,
					 PAGE_SIZE,
					&dma_pages->page_p_addr);
			if (!dma_pages->page_v_addr_start) {
				qdf_print("dmaable page alloc fail pi %d",
					page_idx);
				goto page_alloc_fail;
			}
			dma_pages->page_v_addr_end =
				dma_pages->page_v_addr_start + PAGE_SIZE;
			dma_pages++;
		}
		pages->cacheable_pages = NULL;
	}
	return;

page_alloc_fail:
	if (cacheable) {
		for (i = 0; i < page_idx; i++)
			qdf_mem_free(pages->cacheable_pages[i]);
		qdf_mem_free(pages->cacheable_pages);
	} else {
		dma_pages = pages->dma_pages;
		for (i = 0; i < page_idx; i++) {
			qdf_mem_free_consistent(osdev, osdev->dev, PAGE_SIZE,
				dma_pages->page_v_addr_start,
				dma_pages->page_p_addr, memctxt);
			dma_pages++;
		}
		qdf_mem_free(pages->dma_pages);
	}

out_fail:
	pages->cacheable_pages = NULL;
	pages->dma_pages = NULL;
	pages->num_pages = 0;
	return;
}
qdf_export_symbol(qdf_mem_multi_pages_alloc);

/**
 * qdf_mem_multi_pages_free() - free large size of kernel memory
 * @osdev: OS device handle pointer
 * @pages: Multi page information storage
 * @memctxt: Memory context
 * @cacheable: Coherent memory or cacheable memory
 *
 * This function will free large size of memory over multiple pages.
 *
 * Return: None
 */
void qdf_mem_multi_pages_free(qdf_device_t osdev,
			      struct qdf_mem_multi_page_t *pages,
			      qdf_dma_context_t memctxt, bool cacheable)
{
	unsigned int page_idx;
	struct qdf_mem_dma_page_t *dma_pages;

	if (cacheable) {
		for (page_idx = 0; page_idx < pages->num_pages; page_idx++)
			qdf_mem_free(pages->cacheable_pages[page_idx]);
		qdf_mem_free(pages->cacheable_pages);
	} else {
		dma_pages = pages->dma_pages;
		for (page_idx = 0; page_idx < pages->num_pages; page_idx++) {
			qdf_mem_free_consistent(osdev, osdev->dev, PAGE_SIZE,
				dma_pages->page_v_addr_start,
				dma_pages->page_p_addr, memctxt);
			dma_pages++;
		}
		qdf_mem_free(pages->dma_pages);
	}

	pages->cacheable_pages = NULL;
	pages->dma_pages = NULL;
	pages->num_pages = 0;
	return;
}
qdf_export_symbol(qdf_mem_multi_pages_free);

/**
 * qdf_mem_copy() - copy memory
 * @dst_addr: Pointer to destination memory location (to copy to)
 * @src_addr: Pointer to source memory location (to copy from)
 * @num_bytes: Number of bytes to copy.
 *
 * Copy host memory from one location to another, similar to memcpy in
 * standard C.  Note this function does not specifically handle overlapping
 * source and destination memory locations.  Calling this function with
 * overlapping source and destination memory locations will result in
 * unpredictable results.  Use qdf_mem_move() if the memory locations
 * for the source and destination are overlapping (or could be overlapping!)
 *
 * Return: none
 */
void qdf_mem_copy(void *dst_addr, const void *src_addr, uint32_t num_bytes)
{
	if (0 == num_bytes) {
		/* special case where dst_addr or src_addr can be NULL */
		return;
	}

	if ((dst_addr == NULL) || (src_addr == NULL)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s called with NULL parameter, source:%pK destination:%pK",
			  __func__, src_addr, dst_addr);
		QDF_ASSERT(0);
		return;
	}
	memcpy(dst_addr, src_addr, num_bytes);
}
qdf_export_symbol(qdf_mem_copy);

/**
 * qdf_mem_zero() - zero out memory
 * @ptr: pointer to memory that will be set to zero
 * @num_bytes: number of bytes zero
 *
 * This function sets the memory location to all zeros, essentially clearing
 * the memory.
 *
 * Return: None
 */
void qdf_mem_zero(void *ptr, uint32_t num_bytes)
{
	if (0 == num_bytes) {
		/* special case where ptr can be NULL */
		return;
	}

	if (ptr == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s called with NULL parameter ptr", __func__);
		return;
	}
	memset(ptr, 0, num_bytes);
}
qdf_export_symbol(qdf_mem_zero);

/**
 * qdf_mem_set() - set (fill) memory with a specified byte value.
 * @ptr: Pointer to memory that will be set
 * @num_bytes: Number of bytes to be set
 * @value: Byte set in memory
 *
 * Return: None
 */
void qdf_mem_set(void *ptr, uint32_t num_bytes, uint32_t value)
{
	if (ptr == NULL) {
		qdf_print("%s called with NULL parameter ptr", __func__);
		return;
	}
	memset(ptr, value, num_bytes);
}
qdf_export_symbol(qdf_mem_set);

/**
 * qdf_mem_move() - move memory
 * @dst_addr: pointer to destination memory location (to move to)
 * @src_addr: pointer to source memory location (to move from)
 * @num_bytes: number of bytes to move.
 *
 * Move host memory from one location to another, similar to memmove in
 * standard C.  Note this function *does* handle overlapping
 * source and destination memory locations.

 * Return: None
 */
void qdf_mem_move(void *dst_addr, const void *src_addr, uint32_t num_bytes)
{
	if (0 == num_bytes) {
		/* special case where dst_addr or src_addr can be NULL */
		return;
	}

	if ((dst_addr == NULL) || (src_addr == NULL)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s called with NULL parameter, source:%pK destination:%pK",
			  __func__, src_addr, dst_addr);
		QDF_ASSERT(0);
		return;
	}
	memmove(dst_addr, src_addr, num_bytes);
}
qdf_export_symbol(qdf_mem_move);

#if defined(A_SIMOS_DEVHOST) || defined(HIF_SDIO) || defined(HIF_USB)
static inline void *qdf_mem_dma_alloc(qdf_device_t osdev, void *dev,
					 qdf_size_t size, qdf_dma_addr_t *paddr)
{
	void *vaddr;

	vaddr = kzmalloc(size, qdf_mem_malloc_flags());
	*paddr = (uintptr_t)vaddr;
	/* using this type conversion to suppress "cast from pointer to integer
	 * of different size" warning on some platforms
	 */
	BUILD_BUG_ON(sizeof(*paddr) < sizeof(vaddr));

	return vaddr;
}
#else
static inline void *qdf_mem_dma_alloc(qdf_device_t osdev, void *dev,
					 qdf_size_t size, qdf_dma_addr_t *paddr)
{
	return dma_alloc_coherent(dev, size, paddr, qdf_mem_malloc_flags());
}
#endif

#ifdef MEMORY_DEBUG
void *__qdf_mem_alloc_consistent(qdf_device_t osdev, void *dev,
				 qdf_size_t size, qdf_dma_addr_t *paddr,
				 const char *file, uint32_t line)
{
	QDF_STATUS status;
	qdf_list_t *mem_list = qdf_mem_dma_active_list();
	struct qdf_mem_header *header;
	void *vaddr;
	unsigned long start, duration;

	if (!size || size > QDF_MEM_MAX_MALLOC) {
		qdf_err("Cannot malloc %zu bytes @ %s:%d", size, file, line);
		return NULL;
	}

	start = qdf_mc_timer_get_system_time();
	header = qdf_mem_dma_alloc(osdev, dev, size + QDF_MEM_DEBUG_SIZE,
				   paddr);
	duration = qdf_mc_timer_get_system_time() - start;

	if (duration > QDF_MEM_WARN_THRESHOLD)
		qdf_warn("Malloc slept; %lums, %zuB @ %s:%d",
			 duration, size, file, line);

	if (!header) {
		qdf_warn("Failed to malloc %zuB @ %s:%d", size, file, line);
		return NULL;
	}

	qdf_mem_header_init(header, size, file, line);
	vaddr = qdf_mem_get_ptr(header);
	*paddr += sizeof(*header);

	qdf_spin_lock_irqsave(&qdf_mem_dma_list_lock);
	status = qdf_list_insert_front(mem_list, &header->node);
	qdf_spin_unlock_irqrestore(&qdf_mem_dma_list_lock);
	if (QDF_IS_STATUS_ERROR(status))
		qdf_err("Failed to insert memory header; status %d", status);

	qdf_mem_dma_inc(size);

	return vaddr;
}
#else
void *__qdf_mem_alloc_consistent(qdf_device_t osdev, void *dev,
				 qdf_size_t size, qdf_dma_addr_t *paddr,
				 const char *file, uint32_t line)
{
	void *vaddr = qdf_mem_dma_alloc(osdev, dev, size, paddr);

	if (vaddr)
		qdf_mem_dma_inc(size);

	return vaddr;
}
#endif /* MEMORY_DEBUG */
qdf_export_symbol(__qdf_mem_alloc_consistent);

#if defined(A_SIMOS_DEVHOST) ||  defined(HIF_SDIO) || defined(HIF_USB)
inline void
qdf_mem_dma_free(void *dev, qdf_size_t size, void *vaddr, qdf_dma_addr_t paddr)
{
	kfree(vaddr);
}

#else
inline void
qdf_mem_dma_free(void *dev, qdf_size_t size, void *vaddr, qdf_dma_addr_t paddr)
{
	dma_free_coherent(dev, size, vaddr, paddr);
}
#endif

#ifdef MEMORY_DEBUG
void __qdf_mem_free_consistent(qdf_device_t osdev, void *dev,
			       qdf_size_t size, void *vaddr,
			       qdf_dma_addr_t paddr, qdf_dma_context_t memctx,
			       const char *file, uint32_t line)
{
	enum qdf_mem_domain domain = qdf_mem_get_domain();
	struct qdf_mem_header *header;
	enum qdf_mem_validation_bitmap error_bitmap;

	/* freeing a null pointer is valid */
	if (qdf_unlikely(!vaddr))
		return;

	if (qdf_unlikely((qdf_size_t)vaddr <= sizeof(*header)))
		panic("Failed to free invalid memory location %pK", vaddr);

	qdf_spin_lock_irqsave(&qdf_mem_dma_list_lock);
	header = qdf_mem_get_header(vaddr);
	error_bitmap = qdf_mem_header_validate(header, domain);
	if (!error_bitmap) {
		header->freed = true;
		list_del_init(&header->node);
		qdf_mem_dma_list(header->domain)->count--;
	}
	qdf_spin_unlock_irqrestore(&qdf_mem_dma_list_lock);

	qdf_mem_header_assert_valid(header, domain, error_bitmap, file, line);

	qdf_mem_dma_dec(header->size);
	qdf_mem_dma_free(dev, size + QDF_MEM_DEBUG_SIZE, header,
			 paddr - sizeof(*header));
}
#else
void __qdf_mem_free_consistent(qdf_device_t osdev, void *dev,
			       qdf_size_t size, void *vaddr,
			       qdf_dma_addr_t paddr, qdf_dma_context_t memctx,
			       const char *file, uint32_t line)
{
	qdf_mem_dma_dec(size);
	qdf_mem_dma_free(dev, size, vaddr, paddr);
}
#endif /* MEMORY_DEBUG */
qdf_export_symbol(__qdf_mem_free_consistent);

/**
 * qdf_mem_dma_sync_single_for_device() - assign memory to device
 * @osdev: OS device handle
 * @bus_addr: dma address to give to the device
 * @size: Size of the memory block
 * @direction: direction data will be DMAed
 *
 * Assign memory to the remote device.
 * The cache lines are flushed to ram or invalidated as needed.
 *
 * Return: none
 */
void qdf_mem_dma_sync_single_for_device(qdf_device_t osdev,
					qdf_dma_addr_t bus_addr,
					qdf_size_t size,
					enum dma_data_direction direction)
{
	dma_sync_single_for_device(osdev->dev, bus_addr,  size, direction);
}
qdf_export_symbol(qdf_mem_dma_sync_single_for_device);

/**
 * qdf_mem_dma_sync_single_for_cpu() - assign memory to CPU
 * @osdev: OS device handle
 * @bus_addr: dma address to give to the cpu
 * @size: Size of the memory block
 * @direction: direction data will be DMAed
 *
 * Assign memory to the CPU.
 *
 * Return: none
 */
void qdf_mem_dma_sync_single_for_cpu(qdf_device_t osdev,
				     qdf_dma_addr_t bus_addr,
				     qdf_size_t size,
				     enum dma_data_direction direction)
{
	dma_sync_single_for_cpu(osdev->dev, bus_addr,  size, direction);
}
qdf_export_symbol(qdf_mem_dma_sync_single_for_cpu);

void qdf_mem_init(void)
{
	qdf_mem_debug_init();
	qdf_mem_debugfs_init();
	qdf_mem_debug_debugfs_init();
}
qdf_export_symbol(qdf_mem_init);

void qdf_mem_exit(void)
{
	qdf_mem_debug_debugfs_exit();
	qdf_mem_debugfs_exit();
	qdf_mem_debug_exit();
}
qdf_export_symbol(qdf_mem_exit);
