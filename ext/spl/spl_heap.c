/*
   +----------------------------------------------------------------------+
   | PHP Version 7                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Etienne Kneuss <colder@php.net>                             |
   +----------------------------------------------------------------------+
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"

#include "php_spl.h"
#include "spl_functions.h"
#include "spl_engine.h"
#include "spl_iterators.h"
#include "spl_heap.h"
#include "spl_exceptions.h"

#define PTR_HEAP_BLOCK_SIZE 64

#define SPL_HEAP_CORRUPTED       0x00000001

#define SPL_PQUEUE_EXTR_MASK     0x00000003
#define SPL_PQUEUE_EXTR_BOTH     0x00000003
#define SPL_PQUEUE_EXTR_DATA     0x00000001
#define SPL_PQUEUE_EXTR_PRIORITY 0x00000002

zend_object_handlers spl_handler_SplHeap;
zend_object_handlers spl_handler_SplPriorityQueue;

PHPAPI zend_class_entry  *spl_ce_SplHeap;
PHPAPI zend_class_entry  *spl_ce_SplMaxHeap;
PHPAPI zend_class_entry  *spl_ce_SplMinHeap;
PHPAPI zend_class_entry  *spl_ce_SplPriorityQueue;


typedef void (*spl_ptr_heap_dtor_func)(zval *);
typedef void (*spl_ptr_heap_ctor_func)(zval *);
typedef int  (*spl_ptr_heap_cmp_func)(zval *, zval *, zval *);

typedef struct _spl_ptr_heap {
	zval                    *elements;
	spl_ptr_heap_ctor_func  ctor;
	spl_ptr_heap_dtor_func  dtor;
	spl_ptr_heap_cmp_func   cmp;
	int                     count;
	int                     max_size;
	int                     flags;
} spl_ptr_heap;

typedef struct _spl_heap_object spl_heap_object;
typedef struct _spl_heap_it spl_heap_it;

struct _spl_heap_object {
	spl_ptr_heap       *heap;
	int                 flags;
	zend_class_entry   *ce_get_iterator;
	zend_function      *fptr_cmp;
	zend_function      *fptr_count;
	zend_object         std;
};

/* define an overloaded iterator structure */
struct _spl_heap_it {
	zend_user_iterator  intern;
	int                 flags;
};

typedef struct _spl_pqueue_elem {
	zval data;
	zval priority;
} spl_pqueue_elem;

static inline spl_heap_object *spl_heap_from_obj(zend_object *obj) /* {{{ */ {
	return (spl_heap_object*)((char*)(obj) - XtOffsetOf(spl_heap_object, std));
}
/* }}} */

#define Z_SPLHEAP_P(zv)  spl_heap_from_obj(Z_OBJ_P((zv)))

static void spl_ptr_heap_zval_dtor(zval *elem) { /* {{{ */
	if (!Z_ISUNDEF_P(elem)) {
		zval_ptr_dtor(elem);
	}
}
/* }}} */

static void spl_ptr_heap_zval_ctor(zval *elem) { /* {{{ */
	Z_TRY_ADDREF_P(elem);
}
/* }}} */

static void spl_ptr_heap_pqueue_elem_dtor(zval *zv) { /* {{{ */
	if (!Z_ISUNDEF_P(zv)) {
		spl_pqueue_elem *elem = Z_PTR_P(zv);
		zval_ptr_dtor(&elem->data);
		zval_ptr_dtor(&elem->priority);
		efree(elem);
	}
}
/* }}} */

static void spl_ptr_heap_pqueue_elem_ctor(zval *zv) { /* {{{ */
	spl_pqueue_elem *old_elem = Z_PTR_P(zv);
	spl_pqueue_elem *new_elem = emalloc(sizeof(spl_pqueue_elem));
	ZVAL_COPY(&new_elem->data, &old_elem->data);
	ZVAL_COPY(&new_elem->priority, &old_elem->priority);
	Z_PTR_P(zv) = new_elem;
}
/* }}} */

static int spl_ptr_heap_cmp_cb_helper(zval *object, spl_heap_object *heap_object, zval *a, zval *b, zend_long *result) { /* {{{ */
	zval zresult;

	zend_call_method_with_2_params(Z_OBJ_P(object), heap_object->std.ce, &heap_object->fptr_cmp, "compare", &zresult, a, b);

	if (EG(exception)) {
		return FAILURE;
	}

	*result = zval_get_long(&zresult);
	zval_ptr_dtor(&zresult);

	return SUCCESS;
}
/* }}} */

static void spl_pqueue_extract_helper(zval *result, zval *value, int flags) /* {{{ */
{
	spl_pqueue_elem *elem = Z_PTR_P(value);
	if ((flags & SPL_PQUEUE_EXTR_BOTH) == SPL_PQUEUE_EXTR_BOTH) {
		array_init(result);
		add_assoc_zval_ex(result, "data", sizeof("data") - 1, &elem->data);
		add_assoc_zval_ex(result, "priority", sizeof("priority") - 1, &elem->priority);
		return;
	}

	if (flags & SPL_PQUEUE_EXTR_DATA) {
		ZVAL_COPY(result, &elem->data);
		return;
	}

	if (flags & SPL_PQUEUE_EXTR_PRIORITY) {
		ZVAL_COPY(result, &elem->priority);
		return;
	}

	ZEND_ASSERT(0);
}
/* }}} */

static int spl_ptr_heap_zval_max_cmp(zval *a, zval *b, zval *object) { /* {{{ */
	zval result;

	if (EG(exception)) {
		return 0;
	}

	if (object) {
		spl_heap_object *heap_object = Z_SPLHEAP_P(object);
		if (heap_object->fptr_cmp) {
			zend_long lval = 0;
			if (spl_ptr_heap_cmp_cb_helper(object, heap_object, a, b, &lval) == FAILURE) {
				/* exception or call failure */
				return 0;
			}
			return ZEND_NORMALIZE_BOOL(lval);
		}
	}

	compare_function(&result, a, b);
	return (int)Z_LVAL(result);
}
/* }}} */

static int spl_ptr_heap_zval_min_cmp(zval *a, zval *b, zval *object) { /* {{{ */
	zval result;

	if (EG(exception)) {
		return 0;
	}

	if (object) {
		spl_heap_object *heap_object = Z_SPLHEAP_P(object);
		if (heap_object->fptr_cmp) {
			zend_long lval = 0;
			if (spl_ptr_heap_cmp_cb_helper(object, heap_object, a, b, &lval) == FAILURE) {
				/* exception or call failure */
				return 0;
			}
			return ZEND_NORMALIZE_BOOL(lval);
		}
	}

	compare_function(&result, b, a);
	return (int)Z_LVAL(result);
}
/* }}} */

static int spl_ptr_pqueue_elem_cmp(zval *a_zv, zval *b_zv, zval *object) { /* {{{ */
	spl_pqueue_elem *a = Z_PTR_P(a_zv);
	spl_pqueue_elem *b = Z_PTR_P(b_zv);
	zval *a_priority_p = &a->priority;
	zval *b_priority_p = &b->priority;
	zval result;

	if (EG(exception)) {
		return 0;
	}

	if (object) {
		spl_heap_object *heap_object = Z_SPLHEAP_P(object);
		if (heap_object->fptr_cmp) {
			zend_long lval = 0;
			if (spl_ptr_heap_cmp_cb_helper(object, heap_object, a_priority_p, b_priority_p, &lval) == FAILURE) {
				/* exception or call failure */
				return 0;
			}
			return ZEND_NORMALIZE_BOOL(lval);
		}
	}

	compare_function(&result, a_priority_p, b_priority_p);
	return (int)Z_LVAL(result);
}
/* }}} */

static spl_ptr_heap *spl_ptr_heap_init(spl_ptr_heap_cmp_func cmp, spl_ptr_heap_ctor_func ctor, spl_ptr_heap_dtor_func dtor) /* {{{ */
{
	spl_ptr_heap *heap = emalloc(sizeof(spl_ptr_heap));

	heap->dtor     = dtor;
	heap->ctor     = ctor;
	heap->cmp      = cmp;
	heap->elements = ecalloc(PTR_HEAP_BLOCK_SIZE, sizeof(zval));
	heap->max_size = PTR_HEAP_BLOCK_SIZE;
	heap->count    = 0;
	heap->flags    = 0;

	return heap;
}
/* }}} */

static void spl_ptr_heap_insert(spl_ptr_heap *heap, zval *elem, void *cmp_userdata) { /* {{{ */
	int i;

	if (heap->count+1 > heap->max_size) {
		/* we need to allocate more memory */
		heap->elements  = erealloc(heap->elements, heap->max_size * 2 * sizeof(zval));
		memset(heap->elements + heap->max_size, 0, heap->max_size * sizeof(zval));
		heap->max_size *= 2;
	}

	/* sifting up */
	for (i = heap->count; i > 0 && heap->cmp(&heap->elements[(i-1)/2], elem, cmp_userdata) < 0; i = (i-1)/2) {
		heap->elements[i] = heap->elements[(i-1)/2];
	}
	heap->count++;

	if (EG(exception)) {
		/* exception thrown during comparison */
		heap->flags |= SPL_HEAP_CORRUPTED;
	}

	ZVAL_COPY_VALUE(&heap->elements[i], elem);
}
/* }}} */

static zval *spl_ptr_heap_top(spl_ptr_heap *heap) { /* {{{ */
	if (heap->count == 0) {
		return NULL;
	}

	return Z_ISUNDEF(heap->elements[0])? NULL : &heap->elements[0];
}
/* }}} */

static void spl_ptr_heap_delete_top(spl_ptr_heap *heap, zval *elem, void *cmp_userdata) { /* {{{ */
	int i, j;
	const int limit = (heap->count-1)/2;
	zval *bottom;

	if (heap->count == 0) {
		ZVAL_UNDEF(elem);
		return;
	}

	ZVAL_COPY_VALUE(elem, &heap->elements[0]);
	bottom = &heap->elements[--heap->count];

	for (i = 0; i < limit; i = j) {
		/* Find smaller child */
		j = i * 2 + 1;
		if(j != heap->count && heap->cmp(&heap->elements[j+1], &heap->elements[j], cmp_userdata) > 0) {
			j++; /* next child is bigger */
		}

		/* swap elements between two levels */
		if(heap->cmp(bottom, &heap->elements[j], cmp_userdata) < 0) {
			heap->elements[i] = heap->elements[j];
		} else {
			break;
		}
	}

	if (EG(exception)) {
		/* exception thrown during comparison */
		heap->flags |= SPL_HEAP_CORRUPTED;
	}

	ZVAL_COPY_VALUE(&heap->elements[i], bottom);
}
/* }}} */

static spl_ptr_heap *spl_ptr_heap_clone(spl_ptr_heap *from) { /* {{{ */
	int i;

	spl_ptr_heap *heap = emalloc(sizeof(spl_ptr_heap));

	heap->dtor     = from->dtor;
	heap->ctor     = from->ctor;
	heap->cmp      = from->cmp;
	heap->max_size = from->max_size;
	heap->count    = from->count;
	heap->flags    = from->flags;

	heap->elements = safe_emalloc(sizeof(zval), from->max_size, 0);
	memcpy(heap->elements, from->elements, sizeof(zval)*from->max_size);

	for (i=0; i < heap->count; ++i) {
		heap->ctor(&heap->elements[i]);
	}

	return heap;
}
/* }}} */

static void spl_ptr_heap_destroy(spl_ptr_heap *heap) { /* {{{ */
	int i;

	for (i=0; i < heap->count; ++i) {
		heap->dtor(&heap->elements[i]);
	}

	efree(heap->elements);
	efree(heap);
}
/* }}} */

static int spl_ptr_heap_count(spl_ptr_heap *heap) { /* {{{ */
	return heap->count;
}
/* }}} */

zend_object_iterator *spl_heap_get_iterator(zend_class_entry *ce, zval *object, int by_ref);

static void spl_heap_object_free_storage(zend_object *object) /* {{{ */
{
	spl_heap_object *intern = spl_heap_from_obj(object);

	zend_object_std_dtor(&intern->std);

	spl_ptr_heap_destroy(intern->heap);
}
/* }}} */

static zend_object *spl_heap_object_new_ex(zend_class_entry *class_type, zend_object *orig, int clone_orig) /* {{{ */
{
	spl_heap_object   *intern;
	zend_class_entry  *parent = class_type;
	int                inherited = 0;

	intern = zend_object_alloc(sizeof(spl_heap_object), parent);

	zend_object_std_init(&intern->std, class_type);
	object_properties_init(&intern->std, class_type);

	intern->flags      = 0;
	intern->fptr_cmp   = NULL;

	if (orig) {
		spl_heap_object *other = spl_heap_from_obj(orig);
		intern->ce_get_iterator = other->ce_get_iterator;

		if (clone_orig) {
			intern->heap = spl_ptr_heap_clone(other->heap);
		} else {
			intern->heap = other->heap;
		}

		intern->flags = other->flags;
	} else {
		intern->heap = spl_ptr_heap_init(spl_ptr_heap_zval_max_cmp, spl_ptr_heap_zval_ctor, spl_ptr_heap_zval_dtor);
	}

	intern->std.handlers = &spl_handler_SplHeap;

	while (parent) {
		if (parent == spl_ce_SplPriorityQueue) {
			intern->heap->cmp = spl_ptr_pqueue_elem_cmp;
			intern->heap->ctor = spl_ptr_heap_pqueue_elem_ctor;
			intern->heap->dtor = spl_ptr_heap_pqueue_elem_dtor;
			intern->flags = SPL_PQUEUE_EXTR_DATA;
			intern->std.handlers = &spl_handler_SplPriorityQueue;
			break;
		}

		if (parent == spl_ce_SplMinHeap) {
			intern->heap->cmp = spl_ptr_heap_zval_min_cmp;
			break;
		}

		if (parent == spl_ce_SplMaxHeap) {
			intern->heap->cmp = spl_ptr_heap_zval_max_cmp;
			break;
		}

		if (parent == spl_ce_SplHeap) {
			break;
		}

		parent = parent->parent;
		inherited = 1;
	}

	if (!parent) { /* this must never happen */
		php_error_docref(NULL, E_COMPILE_ERROR, "Internal compiler error, Class is not child of SplHeap");
	}

	if (inherited) {
		intern->fptr_cmp = zend_hash_str_find_ptr(&class_type->function_table, "compare", sizeof("compare") - 1);
		if (intern->fptr_cmp->common.scope == parent) {
			intern->fptr_cmp = NULL;
		}
		intern->fptr_count = zend_hash_str_find_ptr(&class_type->function_table, "count", sizeof("count") - 1);
		if (intern->fptr_count->common.scope == parent) {
			intern->fptr_count = NULL;
		}
	}

	return &intern->std;
}
/* }}} */

static zend_object *spl_heap_object_new(zend_class_entry *class_type) /* {{{ */
{
	return spl_heap_object_new_ex(class_type, NULL, 0);
}
/* }}} */

static zend_object *spl_heap_object_clone(zend_object *old_object) /* {{{ */
{
	zend_object *new_object = spl_heap_object_new_ex(old_object->ce, old_object, 1);

	zend_objects_clone_members(new_object, old_object);

	return new_object;
}
/* }}} */

static int spl_heap_object_count_elements(zend_object *object, zend_long *count) /* {{{ */
{
	spl_heap_object *intern = spl_heap_from_obj(object);

	if (intern->fptr_count) {
		zval rv;
		zend_call_method_with_0_params(object, intern->std.ce, &intern->fptr_count, "count", &rv);
		if (!Z_ISUNDEF(rv)) {
			*count = zval_get_long(&rv);
			zval_ptr_dtor(&rv);
			return SUCCESS;
		}
		*count = 0;
		return FAILURE;
	}

	*count = spl_ptr_heap_count(intern->heap);

	return SUCCESS;
}
/* }}} */

static HashTable* spl_heap_object_get_debug_info_helper(zend_class_entry *ce, zend_object *obj, int *is_temp) { /* {{{ */
	spl_heap_object *intern = spl_heap_from_obj(obj);
	zval tmp, heap_array;
	zend_string *pnstr;
	HashTable *debug_info;
	int  i;

	*is_temp = 1;

	if (!intern->std.properties) {
		rebuild_object_properties(&intern->std);
	}

	debug_info = zend_new_array(zend_hash_num_elements(intern->std.properties) + 1);
	zend_hash_copy(debug_info, intern->std.properties, (copy_ctor_func_t) zval_add_ref);

	pnstr = spl_gen_private_prop_name(ce, "flags", sizeof("flags")-1);
	ZVAL_LONG(&tmp, intern->flags);
	zend_hash_update(debug_info, pnstr, &tmp);
	zend_string_release_ex(pnstr, 0);

	pnstr = spl_gen_private_prop_name(ce, "isCorrupted", sizeof("isCorrupted")-1);
	ZVAL_BOOL(&tmp, intern->heap->flags&SPL_HEAP_CORRUPTED);
	zend_hash_update(debug_info, pnstr, &tmp);
	zend_string_release_ex(pnstr, 0);

	array_init(&heap_array);

	for (i = 0; i < intern->heap->count; ++i) {
		if (ce == spl_ce_SplPriorityQueue) {
			zval elem;
			spl_pqueue_extract_helper(&elem, &intern->heap->elements[i], SPL_PQUEUE_EXTR_BOTH);
			add_index_zval(&heap_array, i, &elem);
		} else {
			add_index_zval(&heap_array, i, &intern->heap->elements[i]);
			Z_TRY_ADDREF(intern->heap->elements[i]);
		}
	}

	pnstr = spl_gen_private_prop_name(ce, "heap", sizeof("heap")-1);
	zend_hash_update(debug_info, pnstr, &heap_array);
	zend_string_release_ex(pnstr, 0);

	return debug_info;
}
/* }}} */

static HashTable *spl_heap_object_get_gc(zend_object *obj, zval **gc_data, int *gc_data_count) /* {{{ */
{
	spl_heap_object *intern = spl_heap_from_obj(obj);
	*gc_data = intern->heap->elements;
	*gc_data_count = intern->heap->count;

	return zend_std_get_properties(obj);
}
/* }}} */

static HashTable* spl_heap_object_get_debug_info(zend_object *obj, int *is_temp) /* {{{ */
{
	return spl_heap_object_get_debug_info_helper(spl_ce_SplHeap, obj, is_temp);
}
/* }}} */

static HashTable* spl_pqueue_object_get_debug_info(zend_object *obj, int *is_temp) /* {{{ */
{
	return spl_heap_object_get_debug_info_helper(spl_ce_SplPriorityQueue, obj, is_temp);
}
/* }}} */

/* {{{ proto int SplHeap::count()
 Return the number of elements in the heap. */
SPL_METHOD(SplHeap, count)
{
	zend_long count;
	spl_heap_object *intern = Z_SPLHEAP_P(ZEND_THIS);

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	count = spl_ptr_heap_count(intern->heap);
	RETURN_LONG(count);
}
/* }}} */

/* {{{ proto int SplHeap::isEmpty()
 Return true if the heap is empty. */
SPL_METHOD(SplHeap, isEmpty)
{
	spl_heap_object *intern = Z_SPLHEAP_P(ZEND_THIS);

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	RETURN_BOOL(spl_ptr_heap_count(intern->heap) == 0);
}
/* }}} */

/* {{{ proto bool SplHeap::insert(mixed value)
	   Push $value on the heap */
SPL_METHOD(SplHeap, insert)
{
	zval *value;
	spl_heap_object *intern;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &value) == FAILURE) {
		return;
	}

	intern = Z_SPLHEAP_P(ZEND_THIS);

	if (intern->heap->flags & SPL_HEAP_CORRUPTED) {
		zend_throw_exception(spl_ce_RuntimeException, "Heap is corrupted, heap properties are no longer ensured.", 0);
		return;
	}

	Z_TRY_ADDREF_P(value);
	spl_ptr_heap_insert(intern->heap, value, ZEND_THIS);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto mixed SplHeap::extract()
	   extract the element out of the top of the heap */
SPL_METHOD(SplHeap, extract)
{
	spl_heap_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	intern = Z_SPLHEAP_P(ZEND_THIS);

	if (intern->heap->flags & SPL_HEAP_CORRUPTED) {
		zend_throw_exception(spl_ce_RuntimeException, "Heap is corrupted, heap properties are no longer ensured.", 0);
		return;
	}

	spl_ptr_heap_delete_top(intern->heap, return_value, ZEND_THIS);

	if (Z_ISUNDEF_P(return_value)) {
		zend_throw_exception(spl_ce_RuntimeException, "Can't extract from an empty heap", 0);
		return;
	}
}
/* }}} */

/* {{{ proto bool SplPriorityQueue::insert(mixed value, mixed priority)
	   Push $value with the priority $priodiry on the priorityqueue */
SPL_METHOD(SplPriorityQueue, insert)
{
	zval *data, *priority, elem_zv;
	spl_heap_object *intern;
	spl_pqueue_elem *elem;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "zz", &data, &priority) == FAILURE) {
		return;
	}

	intern = Z_SPLHEAP_P(ZEND_THIS);

	if (intern->heap->flags & SPL_HEAP_CORRUPTED) {
		zend_throw_exception(spl_ce_RuntimeException, "Heap is corrupted, heap properties are no longer ensured.", 0);
		return;
	}

	elem = emalloc(sizeof(spl_pqueue_elem));
	ZVAL_COPY(&elem->data, data);
	ZVAL_COPY(&elem->priority, priority);
	ZVAL_PTR(&elem_zv, elem);

	spl_ptr_heap_insert(intern->heap, &elem_zv, ZEND_THIS);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto mixed SplPriorityQueue::extract()
	   extract the element out of the top of the priority queue */
SPL_METHOD(SplPriorityQueue, extract)
{
	zval value;
	spl_heap_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	intern = Z_SPLHEAP_P(ZEND_THIS);

	if (intern->heap->flags & SPL_HEAP_CORRUPTED) {
		zend_throw_exception(spl_ce_RuntimeException, "Heap is corrupted, heap properties are no longer ensured.", 0);
		return;
	}

	spl_ptr_heap_delete_top(intern->heap, &value, ZEND_THIS);

	if (Z_ISUNDEF(value)) {
		zend_throw_exception(spl_ce_RuntimeException, "Can't extract from an empty heap", 0);
		return;
	}

	spl_pqueue_extract_helper(return_value, &value, intern->flags);
	spl_ptr_heap_pqueue_elem_dtor(&value);
}
/* }}} */

/* {{{ proto mixed SplPriorityQueue::top()
	   Peek at the top element of the priority queue */
SPL_METHOD(SplPriorityQueue, top)
{
	zval *value;
	spl_heap_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	intern = Z_SPLHEAP_P(ZEND_THIS);

	if (intern->heap->flags & SPL_HEAP_CORRUPTED) {
		zend_throw_exception(spl_ce_RuntimeException, "Heap is corrupted, heap properties are no longer ensured.", 0);
		return;
	}

	value = spl_ptr_heap_top(intern->heap);

	if (!value) {
		zend_throw_exception(spl_ce_RuntimeException, "Can't peek at an empty heap", 0);
		return;
	}

	spl_pqueue_extract_helper(return_value, value, intern->flags);
}
/* }}} */


/* {{{ proto int SplPriorityQueue::setExtractFlags(int flags)
 Set the flags of extraction*/
SPL_METHOD(SplPriorityQueue, setExtractFlags)
{
	zend_long value;
	spl_heap_object *intern;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &value) == FAILURE) {
		return;
	}

	value &= SPL_PQUEUE_EXTR_MASK;
	if (!value) {
		zend_throw_exception(spl_ce_RuntimeException, "Must specify at least one extract flag", 0);
		return;
	}

	intern = Z_SPLHEAP_P(ZEND_THIS);
	intern->flags = value;
	RETURN_LONG(intern->flags);
}
/* }}} */

/* {{{ proto int SplPriorityQueue::getExtractFlags()
 Get the flags of extraction*/
SPL_METHOD(SplPriorityQueue, getExtractFlags)
{
	spl_heap_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	intern = Z_SPLHEAP_P(ZEND_THIS);

	RETURN_LONG(intern->flags);
}
/* }}} */

/* {{{ proto int SplHeap::recoverFromCorruption()
 Recover from a corrupted state*/
SPL_METHOD(SplHeap, recoverFromCorruption)
{
	spl_heap_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	intern = Z_SPLHEAP_P(ZEND_THIS);

	intern->heap->flags = intern->heap->flags & ~SPL_HEAP_CORRUPTED;

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int SplHeap::isCorrupted()
 Tells if the heap is in a corrupted state*/
SPL_METHOD(SplHeap, isCorrupted)
{
	spl_heap_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	intern = Z_SPLHEAP_P(ZEND_THIS);

	RETURN_BOOL(intern->heap->flags & SPL_HEAP_CORRUPTED);
}
/* }}} */

/* {{{ proto bool SplPriorityQueue::compare(mixed $value1, mixed $value2)
	   compare the priorities */
SPL_METHOD(SplPriorityQueue, compare)
{
	zval *a, *b;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "zz", &a, &b) == FAILURE) {
		return;
	}

	RETURN_LONG(spl_ptr_heap_zval_max_cmp(a, b, NULL));
}
/* }}} */

/* {{{ proto mixed SplHeap::top()
	   Peek at the top element of the heap */
SPL_METHOD(SplHeap, top)
{
	zval *value;
	spl_heap_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	intern = Z_SPLHEAP_P(ZEND_THIS);

	if (intern->heap->flags & SPL_HEAP_CORRUPTED) {
		zend_throw_exception(spl_ce_RuntimeException, "Heap is corrupted, heap properties are no longer ensured.", 0);
		return;
	}

	value = spl_ptr_heap_top(intern->heap);

	if (!value) {
		zend_throw_exception(spl_ce_RuntimeException, "Can't peek at an empty heap", 0);
		return;
	}

	ZVAL_COPY_DEREF(return_value, value);
}
/* }}} */

/* {{{ proto bool SplMinHeap::compare(mixed $value1, mixed $value2)
	   compare the values */
SPL_METHOD(SplMinHeap, compare)
{
	zval *a, *b;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "zz", &a, &b) == FAILURE) {
		return;
	}

	RETURN_LONG(spl_ptr_heap_zval_min_cmp(a, b, NULL));
}
/* }}} */

/* {{{ proto bool SplMaxHeap::compare(mixed $value1, mixed $value2)
	   compare the values */
SPL_METHOD(SplMaxHeap, compare)
{
	zval *a, *b;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "zz", &a, &b) == FAILURE) {
		return;
	}

	RETURN_LONG(spl_ptr_heap_zval_max_cmp(a, b, NULL));
}
/* }}} */

static void spl_heap_it_dtor(zend_object_iterator *iter) /* {{{ */
{
	spl_heap_it *iterator = (spl_heap_it *)iter;

	zend_user_it_invalidate_current(iter);
	zval_ptr_dtor(&iterator->intern.it.data);
}
/* }}} */

static void spl_heap_it_rewind(zend_object_iterator *iter) /* {{{ */
{
	/* do nothing, the iterator always points to the top element */
}
/* }}} */

static int spl_heap_it_valid(zend_object_iterator *iter) /* {{{ */
{
	return ((Z_SPLHEAP_P(&iter->data))->heap->count != 0 ? SUCCESS : FAILURE);
}
/* }}} */

static zval *spl_heap_it_get_current_data(zend_object_iterator *iter) /* {{{ */
{
	spl_heap_object *object = Z_SPLHEAP_P(&iter->data);
	zval *element = &object->heap->elements[0];

	if (object->heap->flags & SPL_HEAP_CORRUPTED) {
		zend_throw_exception(spl_ce_RuntimeException, "Heap is corrupted, heap properties are no longer ensured.", 0);
		return NULL;
	}

	if (object->heap->count == 0 || Z_ISUNDEF_P(element)) {
		return NULL;
	} else {
		return element;
	}
}
/* }}} */

static zval *spl_pqueue_it_get_current_data(zend_object_iterator *iter) /* {{{ */
{
	zend_user_iterator *user_it = (zend_user_iterator *) iter;
	spl_heap_object *object = Z_SPLHEAP_P(&iter->data);
	zval *element = &object->heap->elements[0];

	if (object->heap->flags & SPL_HEAP_CORRUPTED) {
		zend_throw_exception(spl_ce_RuntimeException, "Heap is corrupted, heap properties are no longer ensured.", 0);
		return NULL;
	}

	if (object->heap->count == 0 || Z_ISUNDEF_P(element)) {
		return NULL;
	}
	if (Z_ISUNDEF(user_it->value)) {
		spl_pqueue_extract_helper(&user_it->value, element, object->flags);
	}
	return &user_it->value;
}
/* }}} */

static void spl_heap_it_get_current_key(zend_object_iterator *iter, zval *key) /* {{{ */
{
	spl_heap_object *object = Z_SPLHEAP_P(&iter->data);

	ZVAL_LONG(key, object->heap->count - 1);
}
/* }}} */

static void spl_heap_it_move_forward(zend_object_iterator *iter) /* {{{ */
{
	spl_heap_object *object = Z_SPLHEAP_P(&iter->data);
	zval elem;

	if (object->heap->flags & SPL_HEAP_CORRUPTED) {
		zend_throw_exception(spl_ce_RuntimeException, "Heap is corrupted, heap properties are no longer ensured.", 0);
		return;
	}

	spl_ptr_heap_delete_top(object->heap, &elem, &iter->data);
	object->heap->dtor(&elem);

	zend_user_it_invalidate_current(iter);
}
/* }}} */

/* {{{  proto int SplHeap::key()
   Return current array key */
SPL_METHOD(SplHeap, key)
{
	spl_heap_object *intern = Z_SPLHEAP_P(ZEND_THIS);

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	RETURN_LONG(intern->heap->count - 1);
}
/* }}} */

/* {{{ proto void SplHeap::next()
   Move to next entry */
SPL_METHOD(SplHeap, next)
{
	spl_heap_object *intern = Z_SPLHEAP_P(ZEND_THIS);
	zval elem;

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	spl_ptr_heap_delete_top(intern->heap, &elem, ZEND_THIS);
	intern->heap->dtor(&elem);
}
/* }}} */

/* {{{ proto bool SplHeap::valid()
   Check whether the datastructure contains more entries */
SPL_METHOD(SplHeap, valid)
{
	spl_heap_object *intern = Z_SPLHEAP_P(ZEND_THIS);

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	RETURN_BOOL(intern->heap->count != 0);
}
/* }}} */

/* {{{ proto void SplHeap::rewind()
   Rewind the datastructure back to the start */
SPL_METHOD(SplHeap, rewind)
{
	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}
	/* do nothing, the iterator always points to the top element */
}
/* }}} */

/* {{{ proto mixed|NULL SplHeap::current()
   Return current datastructure entry */
SPL_METHOD(SplHeap, current)
{
	spl_heap_object *intern  = Z_SPLHEAP_P(ZEND_THIS);
	zval *element = &intern->heap->elements[0];

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	if (!intern->heap->count || Z_ISUNDEF_P(element)) {
		RETURN_NULL();
	} else {
		ZVAL_COPY_DEREF(return_value, element);
	}
}
/* }}} */

/* {{{ proto mixed|NULL SplPriorityQueue::current()
   Return current datastructure entry */
SPL_METHOD(SplPriorityQueue, current)
{
	spl_heap_object  *intern  = Z_SPLHEAP_P(ZEND_THIS);
	zval *element = &intern->heap->elements[0];

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	if (!intern->heap->count || Z_ISUNDEF_P(element)) {
		RETURN_NULL();
	} else {
		spl_pqueue_extract_helper(return_value, element, intern->flags);
	}
}
/* }}} */

/* iterator handler table */
static const zend_object_iterator_funcs spl_heap_it_funcs = {
	spl_heap_it_dtor,
	spl_heap_it_valid,
	spl_heap_it_get_current_data,
	spl_heap_it_get_current_key,
	spl_heap_it_move_forward,
	spl_heap_it_rewind,
	NULL
};

static const zend_object_iterator_funcs spl_pqueue_it_funcs = {
	spl_heap_it_dtor,
	spl_heap_it_valid,
	spl_pqueue_it_get_current_data,
	spl_heap_it_get_current_key,
	spl_heap_it_move_forward,
	spl_heap_it_rewind,
	NULL
};

zend_object_iterator *spl_heap_get_iterator(zend_class_entry *ce, zval *object, int by_ref) /* {{{ */
{
	spl_heap_it     *iterator;
	spl_heap_object *heap_object = Z_SPLHEAP_P(object);

	if (by_ref) {
		zend_throw_exception(spl_ce_RuntimeException, "An iterator cannot be used with foreach by reference", 0);
		return NULL;
	}

	iterator = emalloc(sizeof(spl_heap_it));

	zend_iterator_init(&iterator->intern.it);

	Z_ADDREF_P(object);
	ZVAL_OBJ(&iterator->intern.it.data, Z_OBJ_P(object));
	iterator->intern.it.funcs = &spl_heap_it_funcs;
	iterator->intern.ce       = ce;
	iterator->flags           = heap_object->flags;
	ZVAL_UNDEF(&iterator->intern.value);

	return &iterator->intern.it;
}
/* }}} */

zend_object_iterator *spl_pqueue_get_iterator(zend_class_entry *ce, zval *object, int by_ref) /* {{{ */
{
	spl_heap_it     *iterator;
	spl_heap_object *heap_object = Z_SPLHEAP_P(object);

	if (by_ref) {
		zend_throw_exception(spl_ce_RuntimeException, "An iterator cannot be used with foreach by reference", 0);
		return NULL;
	}

	iterator = emalloc(sizeof(spl_heap_it));

	zend_iterator_init((zend_object_iterator*)iterator);

	Z_ADDREF_P(object);
	ZVAL_OBJ(&iterator->intern.it.data, Z_OBJ_P(object));
	iterator->intern.it.funcs = &spl_pqueue_it_funcs;
	iterator->intern.ce       = ce;
	iterator->flags           = heap_object->flags;

	ZVAL_UNDEF(&iterator->intern.value);

	return &iterator->intern.it;
}
/* }}} */

ZEND_BEGIN_ARG_INFO(arginfo_heap_insert, 0)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_heap_compare, 0)
	ZEND_ARG_INFO(0, value1)
	ZEND_ARG_INFO(0, value2)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_pqueue_insert, 0)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, priority)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_pqueue_setflags, 0)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_splheap_void, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry spl_funcs_SplMinHeap[] = {
	SPL_ME(SplMinHeap, compare, arginfo_heap_compare, ZEND_ACC_PROTECTED)
	PHP_FE_END
};
static const zend_function_entry spl_funcs_SplMaxHeap[] = {
	SPL_ME(SplMaxHeap, compare, arginfo_heap_compare, ZEND_ACC_PROTECTED)
	PHP_FE_END
};

static const zend_function_entry spl_funcs_SplPriorityQueue[] = {
	SPL_ME(SplPriorityQueue, compare,               arginfo_heap_compare,    ZEND_ACC_PUBLIC)
	SPL_ME(SplPriorityQueue, insert,                arginfo_pqueue_insert,   ZEND_ACC_PUBLIC)
	SPL_ME(SplPriorityQueue, setExtractFlags,       arginfo_pqueue_setflags, ZEND_ACC_PUBLIC)
	SPL_ME(SplPriorityQueue, getExtractFlags,       arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplPriorityQueue, top,                   arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplPriorityQueue, extract,               arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap,          count,                 arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap,          isEmpty,               arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap,          rewind,                arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplPriorityQueue, current,               arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap,          key,                   arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap,          next,                  arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap,          valid,                 arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap,          recoverFromCorruption, arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap,          isCorrupted,           arginfo_splheap_void,    ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry spl_funcs_SplHeap[] = {
	SPL_ME(SplHeap, extract,               arginfo_splheap_void, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, insert,                arginfo_heap_insert, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, top,                   arginfo_splheap_void, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, count,                 arginfo_splheap_void, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, isEmpty,               arginfo_splheap_void, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, rewind,                arginfo_splheap_void, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, current,               arginfo_splheap_void, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, key,                   arginfo_splheap_void, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, next,                  arginfo_splheap_void, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, valid,                 arginfo_splheap_void, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, recoverFromCorruption, arginfo_splheap_void, ZEND_ACC_PUBLIC)
	SPL_ME(SplHeap, isCorrupted,           arginfo_splheap_void, ZEND_ACC_PUBLIC)
	ZEND_FENTRY(compare, NULL, arginfo_heap_compare, ZEND_ACC_PROTECTED|ZEND_ACC_ABSTRACT)
	PHP_FE_END
};
/* }}} */

PHP_MINIT_FUNCTION(spl_heap) /* {{{ */
{
	REGISTER_SPL_STD_CLASS_EX(SplHeap, spl_heap_object_new, spl_funcs_SplHeap);
	memcpy(&spl_handler_SplHeap, &std_object_handlers, sizeof(zend_object_handlers));

	spl_handler_SplHeap.offset         = XtOffsetOf(spl_heap_object, std);
	spl_handler_SplHeap.clone_obj      = spl_heap_object_clone;
	spl_handler_SplHeap.count_elements = spl_heap_object_count_elements;
	spl_handler_SplHeap.get_debug_info = spl_heap_object_get_debug_info;
	spl_handler_SplHeap.get_gc         = spl_heap_object_get_gc;
	spl_handler_SplHeap.dtor_obj = zend_objects_destroy_object;
	spl_handler_SplHeap.free_obj = spl_heap_object_free_storage;

	REGISTER_SPL_IMPLEMENTS(SplHeap, Iterator);
	REGISTER_SPL_IMPLEMENTS(SplHeap, Countable);

	spl_ce_SplHeap->get_iterator = spl_heap_get_iterator;

	REGISTER_SPL_SUB_CLASS_EX(SplMinHeap,           SplHeap,        spl_heap_object_new, spl_funcs_SplMinHeap);
	REGISTER_SPL_SUB_CLASS_EX(SplMaxHeap,           SplHeap,        spl_heap_object_new, spl_funcs_SplMaxHeap);

	spl_ce_SplMaxHeap->get_iterator = spl_heap_get_iterator;
	spl_ce_SplMinHeap->get_iterator = spl_heap_get_iterator;

	REGISTER_SPL_STD_CLASS_EX(SplPriorityQueue, spl_heap_object_new, spl_funcs_SplPriorityQueue);
	memcpy(&spl_handler_SplPriorityQueue, &std_object_handlers, sizeof(zend_object_handlers));

	spl_handler_SplPriorityQueue.offset         = XtOffsetOf(spl_heap_object, std);
	spl_handler_SplPriorityQueue.clone_obj      = spl_heap_object_clone;
	spl_handler_SplPriorityQueue.count_elements = spl_heap_object_count_elements;
	spl_handler_SplPriorityQueue.get_debug_info = spl_pqueue_object_get_debug_info;
	spl_handler_SplPriorityQueue.get_gc         = spl_heap_object_get_gc;
	spl_handler_SplPriorityQueue.dtor_obj = zend_objects_destroy_object;
	spl_handler_SplPriorityQueue.free_obj = spl_heap_object_free_storage;

	REGISTER_SPL_IMPLEMENTS(SplPriorityQueue, Iterator);
	REGISTER_SPL_IMPLEMENTS(SplPriorityQueue, Countable);

	spl_ce_SplPriorityQueue->get_iterator = spl_pqueue_get_iterator;

	REGISTER_SPL_CLASS_CONST_LONG(SplPriorityQueue, "EXTR_BOTH",      SPL_PQUEUE_EXTR_BOTH);
	REGISTER_SPL_CLASS_CONST_LONG(SplPriorityQueue, "EXTR_PRIORITY",  SPL_PQUEUE_EXTR_PRIORITY);
	REGISTER_SPL_CLASS_CONST_LONG(SplPriorityQueue, "EXTR_DATA",      SPL_PQUEUE_EXTR_DATA);

	return SUCCESS;
}
/* }}} */
